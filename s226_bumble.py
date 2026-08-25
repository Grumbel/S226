#!/usr/bin/env python3
"""S226 reverse-engineering tool using Bumble (HCI, no bluetoothd).

Talks to a USB Bluetooth controller over raw HCI so BlueZ/bluetoothd
cannot inject SMP/HID pairing. Requires exclusive access to the dongle:

  sudo systemctl stop bluetooth
  # grant USB access if needed, e.g.:
  # sudo chmod o+rw /dev/bus/usb/BUS/DEV
  nix run .#s226-bumble -- --transport usb:0

Default flow: scan for S226 → connect → discover GATT → 0xA1 bind →
enable notifications on f0080002 → listen.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import struct
import sys
import time
from datetime import datetime, timezone
from typing import Optional

__version__ = "@S226_VERSION@"

UUID_F008_NOTIFY = "f0080002-0451-4000-b000-000000000000"
UUID_F008_WRITE = "f0080003-0451-4000-b000-000000000000"

log = logging.getLogger("s226")


def hexstr(data: bytes | bytearray) -> str:
    return " ".join(f"{b:02x}" for b in data)


def build_a1_bind_packet(now: Optional[datetime] = None) -> bytes:
    """20-byte Veepoo 0xA1 bind/time packet (from H-Band HCI capture)."""
    if now is None:
        now = datetime.now(timezone.utc).astimezone()
    year = now.year
    # a1 00 00 00 | year_be | mon day hour min sec | profile...
    return bytes(
        [
            0xA1,
            0x00,
            0x00,
            0x00,
            (year >> 8) & 0xFF,
            year & 0xFF,
            now.month,
            now.day,
            now.hour,
            now.minute,
            now.second,
            0x01,  # gender/profile marker seen in capture
            0x01,
            0x04,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
        ]
    )



# Post-bind probes from Moto G54 H-Band HCI (2026-08-25).
PROBE_SEQUENCES = {
    "sync": [
        bytes([0xD8, 0x00]),
        bytes([0xA0, 0x00]),
    ],
    # Mode select then stream type A (heavy 0xD1 notify traffic in capture).
    "stream-a": [
        bytes([0xE0, 0x00]),
        bytes([0xE0, 0x01]),
        bytes([0xE0, 0x02]),
        bytes([0xD1, 0x01, 0x00, 0x00]),
        bytes([0xD1, 0x01, 0x00, 0x01]),
    ],
    # Start/stop type B (0x90 notifies while running in capture).
    "stream-b": [
        bytes([0xE0, 0x00]),
        bytes([0xE0, 0x01]),
        bytes([0xE0, 0x02]),
        bytes([0x90, 0x01, 0x00]),
    ],
    "stream-b-stop": [
        bytes([0x90, 0x00, 0x00]),
    ],
    "d0-on": [bytes([0xD0, 0x01])],
    "d0-off": [bytes([0xD0, 0x00])],
}

OPCODE_NAMES = {
    0xA0: "status",
    0xA1: "bind/time",
    0xA3: "profile-blob",
    0xA7: "settings-dump",
    0xAA: "feature-flags",
    0xAC: "settings",
    0xAD: "status-dump",
    0xB1: "alarm/schedule",
    0xB2: "config",
    0xB8: "status-dump",
    0xB9: "config",
    0xC7: "menu-flags",
    0xD0: "measure-ctrl",
    0xD1: "stream-a",
    0xD3: "history",
    0xD4: "history-data",
    0xD8: "poll",
    0xE0: "mode-select",
    0xE1: "user-profile",
    0x90: "stream-b",
    0xF4: "init",
}


def annotate_payload(value: bytes) -> str:
    if not value:
        return ""
    name = OPCODE_NAMES.get(value[0], "")
    return f"  # {name}" if name else ""


async def write_packets(write_char, packets, delay: float = 0.15) -> None:
    for pkt in packets:
        log.info(
            "  write %s:%s",
            hexstr(pkt),
            annotate_payload(pkt),
        )
        try:
            await write_char.write_value(pkt, with_response=False)
        except Exception as e:
            log.info("    failed: %s: %s", type(e).__name__, e)
        await asyncio.sleep(delay)


def _adv_name(data) -> str | None:
    """Extract local name from Bumble AdvertisingData."""
    from bumble.core import AdvertisingData

    for ad_type in (
        AdvertisingData.COMPLETE_LOCAL_NAME,
        AdvertisingData.SHORTENED_LOCAL_NAME,
    ):
        try:
            val = data.get(ad_type)
        except Exception:
            val = None
        if val is None:
            continue
        if isinstance(val, (bytes, bytearray)):
            try:
                return val.decode("utf-8", errors="replace")
            except Exception:
                return str(val)
        return str(val)
    return None


def _adv_manufacturer_ids(data) -> list[int]:
    from bumble.core import AdvertisingData

    ids: list[int] = []
    try:
        entries = data.get_all(AdvertisingData.MANUFACTURER_SPECIFIC_DATA)
    except Exception:
        entries = None
    if not entries:
        try:
            one = data.get(AdvertisingData.MANUFACTURER_SPECIFIC_DATA)
            entries = [one] if one is not None else []
        except Exception:
            entries = []
    for entry in entries:
        if entry is None:
            continue
        if isinstance(entry, tuple) and len(entry) >= 1:
            ids.append(int(entry[0]))
        elif isinstance(entry, (bytes, bytearray)) and len(entry) >= 2:
            ids.append(int.from_bytes(entry[:2], "little"))
    return ids


def is_s226_adv(advertisement) -> bool:
    """Match S226 by name or manufacturer 0xf8f8 payload."""
    data = advertisement.data
    name = _adv_name(data)
    if name and "S226" in name:
        return True
    if 0xF8F8 in _adv_manufacturer_ids(data):
        return True
    try:
        if str(advertisement.address).upper().startswith("FD:32:EF:97:4A:CD"):
            return True
    except Exception:
        pass
    return False


async def run(args: argparse.Namespace) -> int:
    from bumble.device import Device
    from bumble.hci import Address
    from bumble.transport import open_transport

    log.info("s226-bumble %s", __version__)
    log.info("HCI transport: %s", args.transport)
    log.info(
        "Stop bluetoothd first if the dongle is owned by BlueZ: "
        "sudo systemctl stop bluetooth"
    )

    async with await open_transport(args.transport) as hci_transport:
        device = Device.with_hci(
            "S226-Bumble",
            Address("F0:F1:F2:F3:F4:F5"),
            hci_transport.source,
            hci_transport.sink,
        )
        # Prefer no host-initiated SMP; watch rejects pairing.
        for attr, val in (
            ("smp_enabled", False),
            ("le_simultaneous_enabled", False),
        ):
            if hasattr(device, attr):
                setattr(device, attr, val)
        await device.power_on()
        log.info("Controller powered on")

        target: Optional[Address] = None
        if args.address:
            target = Address(
                args.address,
                Address.RANDOM_DEVICE_ADDRESS
                if args.random_address
                else Address.PUBLIC_DEVICE_ADDRESS,
            )
            log.info("Using address %s", target)
        else:
            found = asyncio.Event()
            seen: dict[str, Address] = {}

            def on_adv(advertisement) -> None:
                nonlocal target
                addr = advertisement.address
                key = str(addr)
                name = _adv_name(advertisement.data)
                if key not in seen:
                    seen[key] = addr
                    log.info(
                        "  adv %s rssi=%s name=%r",
                        addr,
                        advertisement.rssi,
                        name,
                    )
                if not is_s226_adv(advertisement):
                    return
                log.info("=" * 70)
                log.info("S226 FOUND")
                log.info("=" * 70)
                log.info("Address: %s", addr)
                log.info("RSSI:    %s", advertisement.rssi)
                try:
                    log.info(
                        "Adv:     %s",
                        advertisement.data.to_string(" | "),
                    )
                except Exception:
                    pass
                target = addr
                found.set()

            device.on("advertisement", on_adv)
            log.info(
                "Scanning for S226 (timeout=%ss)...", args.scan_timeout
            )
            log.info("Hold the watch near the dongle (~10cm).")
            try:
                await device.start_scanning(
                    filter_duplicates=False,
                    active=True,
                )
            except TypeError:
                await device.start_scanning(filter_duplicates=False)
            try:
                await asyncio.wait_for(found.wait(), timeout=args.scan_timeout)
            except asyncio.TimeoutError:
                log.error("S226 not found")
                return 1
            finally:
                await device.stop_scanning()

        assert target is not None
        log.info("Connecting to %s ...", target)
        connection = await asyncio.wait_for(
            device.connect(target),
            timeout=args.connect_timeout,
        )
        log.info("Connected: %s", connection)

        client = connection.gatt_client
        log.info("Discovering GATT services...")
        await client.discover_services()
        for service in client.services:
            await service.discover_characteristics()
            for char in service.characteristics:
                await char.discover_descriptors()

        log.info("=" * 70)
        log.info("GATT DATABASE")
        log.info("=" * 70)
        for service in client.services:
            log.info("SERVICE %s", service.uuid)
            for char in service.characteristics:
                props = []
                p = char.properties
                # Properties is a flag enum in bumble
                try:
                    props = str(p).replace("Characteristic.Properties.", "")
                except Exception:
                    props = repr(p)
                log.info(
                    "  CHAR %s handle=0x%04x props=%s",
                    char.uuid,
                    char.handle,
                    props,
                )

        def find_char(uuid_str: str):
            u = uuid_str.lower()
            for service in client.services:
                for char in service.characteristics:
                    if str(char.uuid).lower() == u:
                        return char
            return None

        notify_char = find_char(UUID_F008_NOTIFY)
        write_char = find_char(UUID_F008_WRITE)

        if write_char is None:
            log.error("Missing write characteristic %s", UUID_F008_WRITE)
            return 1

        # Optional: enable notify first (phone order) or after A1
        def on_notify(value: bytes) -> None:
            # Bumble passes only the value (decoded bytes).
            now = time.strftime("%H:%M:%S")
            if not isinstance(value, (bytes, bytearray)):
                try:
                    value = bytes(value)
                except Exception:
                    value = bytes(str(value), "utf-8", errors="replace")
            note = annotate_payload(value)
            log.info(
                "[%s] NOTIFY: %s (%d bytes)%s",
                now,
                hexstr(value),
                len(value),
                note,
            )
            if value and value[0] == 0xA1:
                log.info("  ↑ 0xA1 response (bind ACK + MAC)")

        if args.enable_notify and notify_char is not None:
            log.info("=" * 70)
            log.info("ENABLE NOTIFY %s", UUID_F008_NOTIFY)
            log.info("=" * 70)
            try:
                await notify_char.subscribe(on_notify)
                log.info("  subscribed OK")
            except Exception as e:
                log.info("  subscribe failed: %s: %s", type(e).__name__, e)

        if not args.no_auth:
            packet = build_a1_bind_packet()
            log.info("=" * 70)
            log.info("VEEPOO HANDSHAKE (0xA1 bind)")
            log.info("=" * 70)
            log.info("  write %s: %s", write_char.uuid, hexstr(packet))
            try:
                await write_char.write_value(packet, with_response=False)
                log.info("  0xA1 sent (write without response)")
            except Exception as e:
                log.info(
                    "  write without response failed: %s: %s",
                    type(e).__name__,
                    e,
                )
                try:
                    await write_char.write_value(packet, with_response=True)
                    log.info("  0xA1 sent (write with response)")
                except Exception as e2:
                    log.info(
                        "  write with response failed: %s: %s",
                        type(e2).__name__,
                        e2,
                    )

            # If notify was deferred: try after A1 (phone did CCCD before A1,
            # but Linux BlueZ path failed either order)
            if (
                args.enable_notify
                and notify_char is not None
                and args.notify_after_a1
            ):
                try:
                    await notify_char.subscribe(on_notify)
                    log.info("  subscribed after A1 OK")
                except Exception as e:
                    log.info(
                        "  subscribe after A1 failed: %s: %s",
                        type(e).__name__,
                        e,
                    )

        if args.probe:
            log.info("=" * 70)
            log.info("PROBES: %s", ", ".join(args.probe))
            log.info("=" * 70)
            for name in args.probe:
                seq = PROBE_SEQUENCES.get(name)
                if not seq:
                    log.info("  unknown probe %r (skip)", name)
                    continue
                log.info("--- probe %s ---", name)
                await write_packets(write_char, seq)
                await asyncio.sleep(0.5)

        log.info("=" * 70)
        log.info("LISTENING FOR %.1f SECONDS", args.listen)
        log.info("=" * 70)
        log.info(
            "If a measurement was started, leave this running; "
            "stop with stream-b-stop / d0-off probes on next run."
        )
        await asyncio.sleep(args.listen)

        # Auto-stop stream-b if we started it
        if args.probe and "stream-b" in args.probe and "stream-b-stop" not in args.probe:
            log.info("Auto-stopping stream-b (90 00 00)")
            await write_packets(write_char, PROBE_SEQUENCES["stream-b-stop"])
            await asyncio.sleep(0.5)

        try:
            await connection.disconnect()
        except Exception:
            pass
        log.info("Done.")
        return 0


def main() -> None:
    parser = argparse.ArgumentParser(
        description="S226 tool via Bumble (raw HCI, no bluetoothd)"
    )
    parser.add_argument(
        "--version", action="version", version=f"s226-bumble {__version__}"
    )
    parser.add_argument(
        "--transport",
        default="usb:0",
        help="Bumble HCI transport (default: usb:0)",
    )
    parser.add_argument(
        "--address",
        default=None,
        help="Target address (skip scan)",
    )
    parser.add_argument(
        "--random-address",
        action="store_true",
        default=True,
        help="Treat --address as random (default for S226)",
    )
    parser.add_argument(
        "--scan-timeout", type=float, default=30.0, help="Scan timeout seconds"
    )
    parser.add_argument(
        "--connect-timeout",
        type=float,
        default=40.0,
        help="Connect timeout seconds",
    )
    parser.add_argument(
        "--listen",
        type=float,
        default=30.0,
        dest="listen",
        help="Seconds to listen after handshake",
    )
    parser.add_argument(
        "--no-auth",
        action="store_true",
        help="Skip 0xA1 bind packet",
    )
    parser.add_argument(
        "--enable-notify",
        action="store_true",
        default=True,
        help="Subscribe to f0080002 (default: on for Bumble path)",
    )
    parser.add_argument(
        "--no-notify",
        action="store_true",
        help="Do not subscribe to notifications",
    )
    parser.add_argument(
        "--notify-after-a1",
        action="store_true",
        help="Subscribe to notify after 0xA1 instead of before",
    )
    parser.add_argument(
        "--probe",
        action="append",
        default=[],
        metavar="NAME",
        help=(
            "Post-bind probe sequence (repeatable): "
            "sync, stream-a, stream-b, stream-b-stop, d0-on, d0-off. "
            "stream-a/b are candidate HR/BP starts from phone HCI."
        ),
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="Debug logging"
    )
    args = parser.parse_args()
    if args.no_notify:
        args.enable_notify = False

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(message)s",
    )
    # Quiet noisy bumble modules unless -v
    if not args.verbose:
        logging.getLogger("bumble").setLevel(logging.WARNING)

    try:
        raise SystemExit(asyncio.run(run(args)))
    except KeyboardInterrupt:
        log.info("Interrupted.")
        raise SystemExit(130)


if __name__ == "__main__":
    main()
