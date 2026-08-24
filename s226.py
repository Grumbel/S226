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


WATCH_NAME = "S226"
MANUFACTURER_ID = 0xF8F8

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
    "f0080002-0451-4000-b000-000000000000": "f0080002",
    "f0080003-0451-4000-b000-000000000000": "f0080003",
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


async def scan_only(scan_timeout: float) -> int:
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

    scanner = BleakScanner(detection_callback=detection_callback)
    log.info("Scan-only mode: logging advertisements for %ss...", scan_timeout)
    await scanner.start()
    try:
        await asyncio.sleep(scan_timeout)
    finally:
        await scanner.stop()
    log.info("Seen %d S226 advertisement(s).", count)
    return 0 if count else 1


async def connect_address(
    address: str,
    connect_timeout: float,
    connect_retries: int,
) -> Optional[BleakClient]:
    """Connect directly to a known address (no scan)."""
    last_error: Optional[BaseException] = None
    for attempt in range(1, connect_retries + 1):
        log.info("")
        log.info(
            "Direct connect attempt %d/%d to %s (timeout=%ss)...",
            attempt,
            connect_retries,
            address,
            connect_timeout,
        )
        client = BleakClient(address, timeout=connect_timeout)
        try:
            await client.connect()
            services = await client.get_services()
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

    scanner = BleakScanner(detection_callback=detection_callback)
    log.info(
        "Scanning for S226 (timeout=%ss, connect_timeout=%ss, retries=%d)...",
        scan_timeout,
        connect_timeout,
        connect_retries,
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

            client = BleakClient(device, timeout=connect_timeout)
            try:
                await client.connect()
                if not client.is_connected:
                    raise RuntimeError(
                        "connect() returned but is_connected is False"
                    )

                # Force service discovery; avoid the
                # "Service Discovery has not been performed yet" race.
                services = await client.get_services()
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
    client: BleakClient, characteristic: BleakGATTCharacteristic
) -> None:
    log.info("")
    log.info("    Characteristic %s", uuid_label(str(characteristic.uuid)))
    log.info("      handle:     %s", characteristic.handle)
    log.info("      properties: %s", ", ".join(characteristic.properties))

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


async def dump_gatt(client: BleakClient) -> None:
    log.info("")
    log.info("=" * 70)
    log.info("GATT DATABASE")
    log.info("=" * 70)

    for service in client.services:
        log.info("")
        log.info("SERVICE %s", uuid_label(str(service.uuid)))
        log.info("  handle: %s", service.handle)

        for characteristic in service.characteristics:
            await dump_characteristic(client, characteristic)


async def subscribe_notifications(
    client: BleakClient,
) -> list[BleakGATTCharacteristic]:
    log.info("")
    log.info("=" * 70)
    log.info("SUBSCRIBING TO NOTIFICATIONS")
    log.info("=" * 70)

    notify_chars: list[BleakGATTCharacteristic] = []

    for service in client.services:
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
        help="Skip full GATT dump (only subscribe and listen)",
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

    if args.log_file:
        log.info("Logging to %s", args.log_file)
        log.info("Started at %s", datetime.now().isoformat(timespec="seconds"))

    if args.scan_only:
        return await scan_only(args.scan_timeout)

    client: Optional[BleakClient] = None

    try:
        if args.address:
            client = await connect_address(
                args.address,
                connect_timeout=args.connect_timeout,
                connect_retries=args.connect_retries,
            )
        else:
            client = await find_and_connect(
                scan_timeout=args.scan_timeout,
                connect_timeout=args.connect_timeout,
                connect_retries=args.connect_retries,
            )

        if client is None:
            return 1

        log.info("")
        log.info("Connected and services ready.")

        # Brief settle so BlueZ finishes any remaining bookkeeping.
        await asyncio.sleep(0.2)

        if not args.no_gatt_dump:
            await dump_gatt(client)

        subscribed = await subscribe_notifications(client)

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
