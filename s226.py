#!/usr/bin/env python3

import asyncio
import sys
import time
from bleak import BleakScanner, BleakClient


WATCH_NAME = "S226"
WATCH_MAC = None          # e.g. "FD:32:EF:97:4A:CD", or None
LISTEN_SECONDS = 30


def hexstr(data):
    return " ".join(f"{b:02x}" for b in data)


def short_uuid(uuid):
    return uuid


async def find_watch():
    print(f"Scanning for {WATCH_NAME}...")

    devices = await BleakScanner.discover(
        timeout=10,
        return_adv=True,
    )

    # Prefer exact name.
    for address, (device, adv) in devices.items():
        if device.name == WATCH_NAME or adv.local_name == WATCH_NAME:
            print(f"Found: {device.name} {device.address}")
            print(f"  RSSI: {adv.rssi}")
            print(f"  Service UUIDs: {adv.service_uuids}")
            print(f"  Manufacturer data:")
            for k, v in adv.manufacturer_data.items():
                print(f"    {k:#06x}: {hexstr(v)}")
            return device.address

    # Fallback to explicitly supplied address.
    if WATCH_MAC:
        print(f"Didn't find {WATCH_NAME}; trying {WATCH_MAC}")
        return WATCH_MAC

    print("S226 not found.")
    return None


async def dump_descriptor(client, desc):
    print(f"      Descriptor {desc.uuid}")

    try:
        value = await client.read_gatt_descriptor(desc.handle)
        print(f"        value: {hexstr(value)}")
    except Exception as e:
        print(f"        read failed: {e}")


async def dump_characteristic(client, char):
    print(f"    Characteristic {char.uuid}")
    print(f"      handle:     {char.handle}")
    print(f"      properties: {', '.join(char.properties)}")

    # Descriptors first.
    for desc in char.descriptors:
        await dump_descriptor(client, desc)

    if "read" in char.properties:
        try:
            value = await client.read_gatt_char(char)
            print(f"      READ: {hexstr(value)}")
            try:
                print(f"            ASCII: {value.decode(errors='replace')!r}")
            except Exception:
                pass
        except Exception as e:
            print(f"      READ FAILED: {e}")


async def main():
    address = await find_watch()

    if not address:
        return 1

    print()
    print(f"Connecting to {address}...")

    async with BleakClient(address, timeout=10) as client:
        print(f"Connected: {client.is_connected}")
        print()

        print("=" * 70)
        print("GATT DATABASE")
        print("=" * 70)

        for service in client.services:
            print()
            print(f"SERVICE {service.uuid}")
            print(f"  handle: {service.handle}")

            for char in service.characteristics:
                await dump_characteristic(client, char)

        print()
        print("=" * 70)
        print("SUBSCRIBING TO NOTIFICATIONS")
        print("=" * 70)

        notify_chars = []

        for service in client.services:
            for char in service.characteristics:
                if "notify" in char.properties or "indicate" in char.properties:
                    notify_chars.append(char)

        def notification_handler(sender, data):
            now = time.strftime("%H:%M:%S")
            print(
                f"[{now}] NOTIFY {sender}: "
                f"{hexstr(data)}  ({len(data)} bytes)"
            )

        for char in notify_chars:
            try:
                await client.start_notify(char, notification_handler)
                print(f"  subscribed: {char.uuid}")
            except Exception as e:
                print(f"  FAILED: {char.uuid}: {e}")

        print()
        print("=" * 70)
        print(f"LISTENING FOR {LISTEN_SECONDS} SECONDS")
        print("=" * 70)
        print()
        print("Try interacting with the watch while this runs.")
        print("For example: open menus, measure heart rate, etc.")
        print()

        try:
            await asyncio.sleep(LISTEN_SECONDS)
        finally:
            for char in notify_chars:
                try:
                    await client.stop_notify(char)
                except Exception:
                    pass

    print()
    print("Disconnected.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        print("\nInterrupted.")
