#!/usr/bin/env python3
"""S226 BLE reverse engineering tool.

Scan for the S226 fitness tracker, connect immediately on advertisement,
dump the GATT database, subscribe to notifications, and listen for traffic.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys
import time
from datetime import datetime
from typing import Optional

from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic
from bleak.backends.device import BLEDevice
from bleak.backends.scanner import AdvertisementData

# Nix installPhase substitutes the placeholder below with the full
# version (e.g. 0.2.0-dev.15+g98f3a01). Keep exactly one placeholder
# in this file so sed cannot rewrite a fallback comparison.
__version__ = "@S226_VERSION@"
if __version__.startswith("@") and __version__.endswith("@"):
    # Token not substituted: plain source checkout / non-Nix run.
    __version__ = "0.2.0-dev"


WATCH_NAME = "S226"
MANUFACTURER_ID = 0xF8F8

# Veepoo / H-Band vendor service (from phone HCI capture, 2026-08-24)
UUID_F008_SERVICE = "f0080001-0451-4000-b000-000000000000"
UUID_F008_NOTIFY = "f0080002-0451-4000-b000-000000000000"  # watch → phone
UUID_F008_WRITE = "f0080003-0451-4000-b000-000000000000"  # phone → watch
# Handles observed on this S226 unit (may differ on other firmwares):
#   notify value 0x000d, CCCD 0x000e, write value 0x0011

# Well-known BLE UUIDs for nicer output
UUID_NAMES: dict[str, str] = {
    # Services
    "00001800-0000-1000-8000-00805f9b34fb": "Generic Access",
    "00001801-0000-1000-8000-00805f9b34fb": "Generic Attribute",
    "0000180a-0000-1000-8000-00805f9b34fb": "Device Information",
    "0000180f-0000-1000-8000-00805f9b34fb": "Battery Service",
    "00001812-0000-1000-8000-00805f9b34fb": "Human Interface Device",
    "0000fee7-0000-1000-8000-00805f9b34fb": "Tencent Holdings Limited",
    "f0080001-0451-4000-b000-000000000000": "Vendor (f008)",
    "f0020001-0451-4000-b000-000000000000": "Vendor (f002)",
    # Characteristics
    "00002a00-0000-1000-8000-00805f9b34fb": "Device Name",
    "00002a01-0000-1000-8000-00805f9b34fb": "Appearance",
    "00002a04-0000-1000-8000-00805f9b34fb": "Peripheral Preferred Connection Parameters",
    "00002aa6-0000-1000-8000-00805f9b34fb": "Central Address Resolution",
    "00002a19-0000-1000-8000-00805f9b34fb": "Battery Level",
    "00002a29-0000-1000-8000-00805f9b34fb": "Manufacturer Name String",
    "00002a24-0000-1000-8000-00805f9b34fb": "Model Number String",
    "00002a25-0000-1000-8000-00805f9b34fb": "Serial Number String",
    "00002a27-0000-1000-8000-00805f9b34fb": "Hardware Revision String",
    "00002a26-0000-1000-8000-00805f9b34fb": "Firmware Revision String",
    "00002a28-0000-1000-8000-00805f9b34fb": "Software Revision String",
    "00002a4e-0000-1000-8000-00805f9b34fb": "Protocol Mode",
    "00002a4d-0000-1000-8000-00805f9b34fb": "Report",
    "00002a4b-0000-1000-8000-00805f9b34fb": "Report Map",
    "00002a33-0000-1000-8000-00805f9b34fb": "Boot Mouse Input Report",
    "00002a4a-0000-1000-8000-00805f9b34fb": "HID Information",
    "00002a4c-0000-1000-8000-00805f9b34fb": "HID Control Point",
    "0000fea1-0000-1000-8000-00805f9b34fb": "fea1",
    "0000fea2-0000-1000-8000-00805f9b34fb": "fea2",
    "0000fec9-0000-1000-8000-00805f9b34fb": "fec9",
    "f0080002-0451-4000-b000-000000000000": "f0080002 (notify)",
    "f0080003-0451-4000-b000-000000000000": "f0080003 (write)",
    "f0020002-0451-4000-b000-000000000000": "f0020002",
    "f0020003-0451-4000-b000-000000000000": "f0020003",
    # Descriptors
    "00002902-0000-1000-8000-00805f9b34fb": "Client Characteristic Configuration",
    "00002901-0000-1000-8000-00805f9b34fb": "Characteristic User Description",
}


def uuid_label(uuid: str) -> str:
    """Return a human-readable label for a UUID if known."""
    name = UUID_NAMES.get(uuid.lower())
    if name:
        return f"{uuid} ({name})"
    # Also try short form
    short = uuid.lower().lstrip("0").rstrip("0")  # crude
    for k, v in UUID_NAMES.items():
        if k.endswith(uuid.lower()[-8:]) or uuid.lower().endswith(k[-8:]):
            return f"{uuid} ({v})"
    return uuid


def hexstr(data: bytes | bytearray) -> str:
    return " ".join(f"{b:02x}" for b in data)


def is_s226(device: BLEDevice, advertisement_data: AdvertisementData) -> bool:
    """Identify the S226 from its name or manufacturer advertisement."""
    name = device.name or advertisement_data.local_name

    if name == WATCH_NAME:
        return True

    # The S226 we've observed advertises:
    #
    #   manufacturer ID: f8f8
    #   data: CD 4A 97 EF 32 FD
    #
    # which is the BLE address FD:32:EF:97:4A:CD in reverse byte order.
    data = advertisement_data.manufacturer_data.get(MANUFACTURER_ID)

    if data is not None and len(data) == 6:
        try:
            address_bytes = bytes.fromhex(device.address.replace(":", ""))
            if data == address_bytes[::-1]:
                return True
        except ValueError:
            pass

    return False


def setup_logging(log_file: Optional[str]) -> None:
    """Configure logging to console and optionally a file."""
    handlers: list[logging.Handler] = [logging.StreamHandler(sys.stdout)]

    if log_file:
        handlers.append(logging.FileHandler(log_file, encoding="utf-8"))

    logging.basicConfig(
        level=logging.INFO,
        format="%(message)s",
        handlers=handlers,
        force=True,
    )


log = logging.getLogger("s226")


def _print_advertisement(device: BLEDevice, advertisement_data: AdvertisementData) -> None:
    name = device.name or advertisement_data.local_name
    log.info("")
    log.info("=" * 70)
    log.info("S226 FOUND")
    log.info("=" * 70)
    log.info("Name:         %s", name)
    log.info("Address:      %s", device.address)
    log.info("RSSI:         %s", advertisement_data.rssi)
    log.info("Service UUIDs: %s", advertisement_data.service_uuids)

    if advertisement_data.manufacturer_data:
        log.info("Manufacturer data:")
        for manufacturer_id, data in advertisement_data.manufacturer_data.items():
            log.info("  %#06x: %s", manufacturer_id, hexstr(data))

    if advertisement_data.service_data:
        log.info("Service data:")
        for uuid, data in advertisement_data.service_data.items():
            log.info("  %s: %s", uuid, hexstr(data))

    log.info("=" * 70)


async def scan_only(scan_timeout: float, adapter: Optional[str] = None) -> int:
    """Continuously log every S226 advertisement until timeout."""
    count = 0

    def detection_callback(
        device: BLEDevice, advertisement_data: AdvertisementData
    ) -> None:
        nonlocal count
        if not is_s226(device, advertisement_data):
            return
        count += 1
        _print_advertisement(device, advertisement_data)
        log.info("(advertisement #%d)", count)

    scanner = BleakScanner(detection_callback=detection_callback, adapter=adapter)
    log.info(
        "Scan-only mode: logging advertisements for %ss%s...",
        scan_timeout,
        f" (adapter={adapter})" if adapter else "",
    )
    await scanner.start()
    try:
        await asyncio.sleep(scan_timeout)
    finally:
        await scanner.stop()
    log.info("Seen %d S226 advertisement(s).", count)
    return 0 if count else 1


async def ensure_services(client: BleakClient, timeout: float = 10.0):
    """
    Return the GATT service collection once discovery has finished.

    Bleak 3.x removed get_services(); connect() performs discovery and
    results are exposed via the client.services property. Poll briefly
    if the property is not ready yet.
    """
    deadline = asyncio.get_event_loop().time() + timeout
    last_error: Optional[BaseException] = None
    while asyncio.get_event_loop().time() < deadline:
        try:
            services = client.services
            # Force evaluation; may raise if discovery is incomplete.
            items = list(services)
            if items:
                return services
        except Exception as e:
            last_error = e
        if not client.is_connected:
            raise RuntimeError("disconnected while waiting for services") from last_error
        await asyncio.sleep(0.1)
    if last_error is not None:
        raise RuntimeError(
            f"services not ready within {timeout}s"
        ) from last_error
    raise RuntimeError(f"services not ready within {timeout}s (empty)")

async def connect_address(
    address: str,
    connect_timeout: float,
    connect_retries: int,
    adapter: Optional[str] = None,
) -> Optional[BleakClient]:
    """Connect directly to a known address (no scan)."""
    last_error: Optional[BaseException] = None
    for attempt in range(1, connect_retries + 1):
        log.info("")
        log.info(
            "Direct connect attempt %d/%d to %s (timeout=%ss%s)...",
            attempt,
            connect_retries,
            address,
            connect_timeout,
            f", adapter={adapter}" if adapter else "",
        )
        client = BleakClient(address, timeout=connect_timeout, adapter=adapter)
        try:
            await client.connect()
            services = await ensure_services(client)
            n_services = len(list(services))
            n_chars = sum(len(s.characteristics) for s in services)
            log.info(
                "Connected: %s  (services=%d, characteristics=%d)",
                client.is_connected,
                n_services,
                n_chars,
            )
            if n_services == 0:
                raise RuntimeError("connected but zero GATT services discovered")
            return client
        except Exception as e:
            last_error = e
            log.info("  attempt failed: %s: %s", type(e).__name__, e)
            try:
                if client.is_connected:
                    await client.disconnect()
            except Exception:
                pass
            if attempt < connect_retries:
                await asyncio.sleep(0.5)
    if last_error is not None:
        raise last_error
    return None


async def _safe_scanner_stop(scanner: BleakScanner) -> None:
    """Stop scanner without blowing up if BlueZ already tore it down."""
    try:
        await scanner.stop()
    except Exception as e:
        log.info("scanner stop: %s: %s", type(e).__name__, e)


async def find_and_connect(
    scan_timeout: float,
    connect_timeout: float,
    connect_retries: int,
    adapter: Optional[str] = None,
) -> Optional[BleakClient]:
    """
    Scan and connect immediately when the S226 appears.

    The watch often accepts the ACL link and then disconnects during
    GATT service discovery (typical of Veepoo devices that expect an
    immediate app-level password). Between retries we wait for a *fresh*
    advertisement so BlueZ has a live random-address device again.
    """
    found = asyncio.Event()
    result: dict = {}
    lock = asyncio.Lock()

    def detection_callback(
        device: BLEDevice, advertisement_data: AdvertisementData
    ) -> None:
        if not is_s226(device, advertisement_data):
            return
        # Always refresh the latest BLEDevice; do not ignore later ads.
        result["device"] = device
        if not found.is_set():
            _print_advertisement(device, advertisement_data)
            found.set()

    scanner = BleakScanner(detection_callback=detection_callback, adapter=adapter)
    log.info(
        "Scanning for S226 (timeout=%ss, connect_timeout=%ss, retries=%d%s)...",
        scan_timeout,
        connect_timeout,
        connect_retries,
        f", adapter={adapter}" if adapter else "",
    )
    await scanner.start()

    try:
        deadline = asyncio.get_event_loop().time() + scan_timeout
        last_error: Optional[BaseException] = None

        for attempt in range(1, connect_retries + 1):
            remaining = deadline - asyncio.get_event_loop().time()
            if remaining <= 0:
                if last_error is None:
                    log.info("S226 not found.")
                    return None
                break

            # Wait for a (fresh) advertisement before each attempt.
            found.clear()
            log.info(
                "Waiting for advertisement (attempt %d/%d, %.0fs left)...",
                attempt,
                connect_retries,
                remaining,
            )
            try:
                await asyncio.wait_for(found.wait(), timeout=remaining)
            except asyncio.TimeoutError:
                if last_error is None:
                    log.info("S226 not found.")
                    return None
                log.info("No further advertisements before scan timeout.")
                break

            device = result["device"]
            log.info("")
            log.info(
                "Connect attempt %d/%d to %s (timeout=%ss)...",
                attempt,
                connect_retries,
                device.address,
                connect_timeout,
            )

            # Discover the full GATT database (no services= filter). A
            # restricted UUID list has been observed to hang discovery on
            # some BlueZ/Bleak combinations.
            client = BleakClient(device, timeout=connect_timeout, adapter=adapter)
            try:
                await client.connect()
                if not client.is_connected:
                    raise RuntimeError(
                        "connect() returned but is_connected is False"
                    )

                # Force service discovery; avoid the
                # "Service Discovery has not been performed yet" race.
                services = await ensure_services(client)
                n_services = len(list(services))
                n_chars = sum(len(s.characteristics) for s in services)
                log.info(
                    "Connected: %s  (services=%d, characteristics=%d)",
                    client.is_connected,
                    n_services,
                    n_chars,
                )
                if n_services == 0:
                    raise RuntimeError(
                        "connected but zero GATT services discovered"
                    )
                return client
            except Exception as e:
                last_error = e
                log.info("  attempt failed: %s: %s", type(e).__name__, e)
                try:
                    if client.is_connected:
                        await client.disconnect()
                except Exception:
                    pass
                # Small gap so the peripheral can re-enter advertising.
                await asyncio.sleep(0.5)

        if last_error is not None:
            raise last_error
        return None

    finally:
        await _safe_scanner_stop(scanner)


async def dump_descriptor(client: BleakClient, descriptor) -> None:
    log.info("      Descriptor %s", uuid_label(str(descriptor.uuid)))
    log.info("        handle: %s", descriptor.handle)

    try:
        value = await client.read_gatt_descriptor(descriptor.handle)
        log.info("        READ: %s", hexstr(value))
        if value:
            try:
                log.info("        ASCII: %r", value.decode(errors="replace"))
            except Exception:
                pass
    except Exception as e:
        log.info("        READ FAILED: %s", e)


async def dump_characteristic(
    client: BleakClient,
    characteristic: BleakGATTCharacteristic,
    *,
    read_values: bool,
) -> None:
    log.info("")
    log.info("    Characteristic %s", uuid_label(str(characteristic.uuid)))
    log.info("      handle:     %s", characteristic.handle)
    log.info("      properties: %s", ", ".join(characteristic.properties))

    # Structure only: list descriptor UUIDs/handles, no GATT reads.
    for descriptor in characteristic.descriptors:
        log.info(
            "      Descriptor %s  handle=%s",
            uuid_label(str(descriptor.uuid)),
            descriptor.handle,
        )

    if not read_values:
        return

    if not client.is_connected:
        return

    for descriptor in characteristic.descriptors:
        await dump_descriptor(client, descriptor)

    if "read" in characteristic.properties:
        try:
            value = await client.read_gatt_char(characteristic)
            log.info("      READ: %s", hexstr(value))
            if value:
                try:
                    log.info(
                        "            ASCII: %r", value.decode(errors="replace")
                    )
                except Exception:
                    pass
        except Exception as e:
            log.info("      READ FAILED: %s", e)



async def dump_gatt(client: BleakClient, *, read_values: bool) -> None:
    log.info("")
    log.info("=" * 70)
    log.info(
        "GATT DATABASE%s",
        "" if read_values else " (structure only, no reads)",
    )
    log.info("=" * 70)

    # Services were already resolved during connect. Listing is local and
    # fast; avoid GATT reads until the full tree is printed — the watch
    # often drops the link on the first descriptor/value read.
    try:
        services = list(client.services)
    except Exception as e:
        log.info("Cannot access services: %s: %s", type(e).__name__, e)
        return

    for service in services:
        if not client.is_connected:
            log.info("Disconnected during GATT dump; stopping.")
            break
        log.info("")
        log.info("SERVICE %s", uuid_label(str(service.uuid)))
        log.info("  handle: %s", service.handle)

        for characteristic in service.characteristics:
            if not client.is_connected:
                log.info("Disconnected during GATT dump; stopping.")
                return
            try:
                await dump_characteristic(
                    client, characteristic, read_values=read_values
                )
            except Exception as e:
                log.info(
                    "  characteristic dump failed: %s: %s",
                    type(e).__name__,
                    e,
                )



def build_a1_bind_packet(when: Optional[datetime] = None) -> bytes:
    """
    Build the 20-byte 0xA1 bind / auth packet observed from H-Band.

    Capture (btsnoop, H-Band → S226):
      a1 00 00 00 07 ea 08 18 10 18 1d 01 01 04 00 00 00 00 00 00
               ^^^^ year  ^^^^^^^^ datetime     ^^^^^^^^ profile?

    Year is big-endian. Trailing 01 01 04 matches H-Band profile bytes;
    keep them for now until a minimal working subset is known.
    """
    when = when or datetime.now()
    year = when.year
    pkt = bytearray(20)
    pkt[0] = 0xA1
    pkt[1] = 0x00
    pkt[2] = 0x00
    pkt[3] = 0x00
    pkt[4] = (year >> 8) & 0xFF
    pkt[5] = year & 0xFF
    pkt[6] = when.month
    pkt[7] = when.day
    pkt[8] = when.hour
    pkt[9] = when.minute
    pkt[10] = when.second
    pkt[11] = 0x01
    pkt[12] = 0x01
    pkt[13] = 0x04
    # remainder already zero
    return bytes(pkt)


def find_char_by_uuid(
    client: BleakClient, uuid: str
) -> Optional[BleakGATTCharacteristic]:
    target = uuid.lower()
    try:
        services = list(client.services)
    except Exception:
        return None
    for service in services:
        for characteristic in service.characteristics:
            if str(characteristic.uuid).lower() == target:
                return characteristic
    return None




async def veepoo_handshake(client: BleakClient, *, enable_notify: bool = False) -> bool:
    """
    Post-connect Veepoo bind from H-Band HCI capture.

    HCI sequence (no SMP, no MTU exchange):
      Write_Req  CCCD 0x000e = 01 00
      Write_Cmd  handle 0x0011  20-byte 0xA1 packet
      → notifications on 0x000d

    On Linux, 0xA1 write-without-response succeeds and the link stays up.
    start_notify() gets ATT Unlikely Error (0x0e); BlueZ then ends the
    ACL with reason 0x16 (local host). So notify is opt-in via
    enable_notify; default is bind-only so we can dump and probe.
    """
    log.info("")
    log.info("=" * 70)
    log.info("VEEPOO HANDSHAKE (0xA1 bind)")
    log.info("=" * 70)

    notify_char = find_char_by_uuid(client, UUID_F008_NOTIFY)
    write_char = find_char_by_uuid(client, UUID_F008_WRITE)

    if write_char is None:
        log.info("  missing write characteristic %s", UUID_F008_WRITE)
        return False

    def notification_handler(sender: BleakGATTCharacteristic, data: bytearray) -> None:
        now = time.strftime("%H:%M:%S")
        log.info(
            "[%s] NOTIFY %s: %s (%d bytes)",
            now,
            uuid_label(str(sender.uuid)),
            hexstr(data),
            len(data),
        )
        if data and data[0] == 0xA1:
            log.info("  ↑ 0xA1 response (auth/bind status); MAC may follow in payload")

    packet = build_a1_bind_packet()
    log.info("  write %s: %s", uuid_label(str(write_char.uuid)), hexstr(packet))
    write_ok = False
    try:
        await client.write_gatt_char(write_char, packet, response=False)
        log.info("  0xA1 sent (write-without-response)")
        write_ok = True
    except Exception as e:
        log.info("  write-without-response failed: %s: %s", type(e).__name__, e)
        if client.is_connected:
            try:
                await client.write_gatt_char(write_char, packet, response=True)
                log.info("  0xA1 sent (write-with-response)")
                write_ok = True
            except Exception as e2:
                log.info("  write-with-response failed: %s: %s", type(e2).__name__, e2)

    log.info("  after 0xA1 write: connected=%s", client.is_connected)
    if not client.is_connected:
        return write_ok

    notify_ok = False
    if enable_notify and notify_char is not None:
        try:
            await client.start_notify(notify_char, notification_handler)
            log.info("  subscribed: %s", uuid_label(str(notify_char.uuid)))
            notify_ok = True
        except Exception as e:
            log.info("  start_notify failed: %s: %s", type(e).__name__, e)
        log.info("  after start_notify: connected=%s", client.is_connected)
    elif not enable_notify:
        log.info(
            "  skipping start_notify (default; use --enable-notify to try CCCD)"
        )

    # Probe while link is up (responses only visible if notify works).
    if client.is_connected:
        for probe in (bytes([0xD8, 0x00]), bytes([0xA0, 0x00])):
            try:
                await client.write_gatt_char(write_char, probe, response=False)
                log.info("  probe write: %s", hexstr(probe))
            except Exception as e:
                log.info("  probe %s failed: %s: %s", hexstr(probe), type(e).__name__, e)
                break

    log.info("  handshake done (write=%s, notify=%s)", write_ok, notify_ok)
    return write_ok


async def subscribe_notifications(
    client: BleakClient,
) -> list[BleakGATTCharacteristic]:
    log.info("")
    log.info("=" * 70)
    log.info("SUBSCRIBING TO NOTIFICATIONS")
    log.info("=" * 70)

    notify_chars: list[BleakGATTCharacteristic] = []
    try:
        services = list(client.services)
    except Exception as e:
        log.info("Cannot access services for notify: %s: %s", type(e).__name__, e)
        return []

    for service in services:
        for characteristic in service.characteristics:
            properties = characteristic.properties
            if "notify" in properties or "indicate" in properties:
                notify_chars.append(characteristic)

    def notification_handler(sender: BleakGATTCharacteristic, data: bytearray) -> None:
        now = time.strftime("%H:%M:%S")
        log.info(
            "[%s] NOTIFY %s: %s (%d bytes)",
            now,
            uuid_label(str(sender.uuid)),
            hexstr(data),
            len(data),
        )

    subscribed: list[BleakGATTCharacteristic] = []

    for characteristic in notify_chars:
        # Handshake already subscribed to the primary vendor notify char.
        if str(characteristic.uuid).lower() == UUID_F008_NOTIFY:
            log.info(
                "  skip (already subscribed in handshake): %s",
                uuid_label(str(characteristic.uuid)),
            )
            subscribed.append(characteristic)
            continue
        try:
            await client.start_notify(characteristic, notification_handler)
            log.info("  subscribed: %s", uuid_label(str(characteristic.uuid)))
            subscribed.append(characteristic)
        except Exception as e:
            log.info("  FAILED: %s: %s", uuid_label(str(characteristic.uuid)), e)

    return subscribed


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="S226 BLE reverse engineering tool",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    parser.add_argument(
        "--scan-timeout",
        type=float,
        default=30.0,
        help="Seconds to scan for the S226",
    )
    parser.add_argument(
        "--connect-timeout",
        type=float,
        default=40.0,
        help="Seconds to wait for each connection attempt",
    )
    parser.add_argument(
        "--connect-retries",
        type=int,
        default=3,
        help="How many times to retry connect after a timeout",
    )
    parser.add_argument(
        "--listen",
        type=float,
        default=30.0,
        metavar="SECONDS",
        help="Seconds to listen for notifications after connecting",
    )
    parser.add_argument(
        "--log-file",
        type=str,
        default=None,
        help="Also write all output to this file",
    )
    parser.add_argument(
        "--no-gatt-dump",
        action="store_true",
        help="Skip GATT dump (only subscribe and listen)",
    )
    parser.add_argument(
        "--read-values",
        action="store_true",
        help="After structure dump, also read characteristic/descriptor values "
        "(often causes the watch to disconnect)",
    )
    parser.add_argument(
        "--no-auth",
        action="store_true",
        help="Skip the Veepoo 0xA1 bind packet after connect",
    )
    parser.add_argument(
        "--enable-notify",
        action="store_true",
        help="After 0xA1, call start_notify on f0080002 (currently drops the link)",
    )
    parser.add_argument(
        "--adapter",
        default=None,
        metavar="HCI",
        help="Bluetooth adapter name (e.g. hci2). Default: BlueZ default adapter",
    )
    parser.add_argument(
        "--scan-only",
        action="store_true",
        help="Only log advertisements; do not attempt to connect",
    )
    parser.add_argument(
        "--address",
        type=str,
        default=None,
        help="Skip scan and connect directly to this BLE address",
    )
    return parser.parse_args(argv)


async def main(args: argparse.Namespace) -> int:
    setup_logging(args.log_file)

    log.info("s226 %s", __version__)
    if args.log_file:
        log.info("Logging to %s", args.log_file)
        log.info("Started at %s", datetime.now().isoformat(timespec="seconds"))

    if args.scan_only:
        return await scan_only(args.scan_timeout, adapter=args.adapter)

    client: Optional[BleakClient] = None

    try:
        if args.adapter:
            log.info("Using adapter %s", args.adapter)

        if args.address:
            client = await connect_address(
                args.address,
                connect_timeout=args.connect_timeout,
                connect_retries=args.connect_retries,
                adapter=args.adapter,
            )
        else:
            client = await find_and_connect(
                scan_timeout=args.scan_timeout,
                connect_timeout=args.connect_timeout,
                connect_retries=args.connect_retries,
                adapter=args.adapter,
            )

        if client is None:
            return 1

        log.info("")
        log.info("Connected and services ready.")

        # Order matters: the watch drops unauthenticated links within ~1s.
        # 1) Veepoo 0xA1 bind immediately (subscribe f0080002 + write f0080003)
        # 2) Structure dump (local)
        # 3) Subscribe remaining notify characteristics
        # 4) Optional value reads last
        if not args.no_auth and client.is_connected:
            await veepoo_handshake(client, enable_notify=args.enable_notify)
        elif args.no_auth:
            log.info("Skipping Veepoo 0xA1 bind (--no-auth)")

        if not args.no_gatt_dump and client.is_connected:
            await dump_gatt(client, read_values=False)

        subscribed = []
        if client.is_connected:
            subscribed = await subscribe_notifications(client)

        if (
            not args.no_gatt_dump
            and args.read_values
            and client.is_connected
        ):
            log.info("")
            log.info("Reading characteristic/descriptor values...")
            await dump_gatt(client, read_values=True)

        log.info("")
        log.info("=" * 70)
        log.info("LISTENING FOR %s SECONDS", args.listen)
        log.info("=" * 70)
        log.info("")
        log.info("Interact with the watch while this is running.")
        log.info("Try menus, heart-rate measurement, activity tracking, etc.")
        log.info("")

        try:
            await asyncio.sleep(args.listen)
        except KeyboardInterrupt:
            pass

        for characteristic in subscribed:
            try:
                await client.stop_notify(characteristic)
            except Exception:
                pass

    except Exception as e:
        log.info("")
        log.info("=" * 70)
        log.info("ERROR")
        log.info("=" * 70)
        log.info("%s: %s", type(e).__name__, e)
        return 1

    finally:
        if client is not None:
            try:
                if client.is_connected:
                    log.info("")
                    log.info("Disconnecting...")
                    await client.disconnect()
            except Exception:
                pass

    log.info("")
    log.info("Done.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main(parse_args())))
    except KeyboardInterrupt:
        print()
        print("Interrupted.")
        sys.exit(130)
