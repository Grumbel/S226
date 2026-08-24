#!/usr/bin/env python3

import asyncio
import sys
import time

from bleak import BleakScanner, BleakClient


WATCH_NAME = "S226"
MANUFACTURER_ID = 0xF8F8

SCAN_TIMEOUT = 30
CONNECT_TIMEOUT = 10
LISTEN_SECONDS = 30


def hexstr(data):
    return " ".join(f"{b:02x}" for b in data)


def is_s226(device, advertisement_data):
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
            address_bytes = bytes.fromhex(
                device.address.replace(":", "")
            )

            if data == address_bytes[::-1]:
                return True
        except ValueError:
            pass

    return False


async def find_and_connect():
    """
    Scan and connect immediately when the S226 appears.

    This is deliberately done from the scanner callback. The watch
    appears to advertise only briefly, so doing a separate
    discover() followed by connect() introduces a race.
    """

    found = asyncio.Event()
    result = {}

    def detection_callback(device, advertisement_data):
        if not is_s226(device, advertisement_data):
            return

        if found.is_set():
            return

        name = device.name or advertisement_data.local_name

        print()
        print("=" * 70)
        print("S226 FOUND")
        print("=" * 70)
        print(f"Name:         {name}")
        print(f"Address:      {device.address}")
        print(f"RSSI:         {advertisement_data.rssi}")
        print(f"Service UUIDs: {advertisement_data.service_uuids}")

        if advertisement_data.manufacturer_data:
            print("Manufacturer data:")

            for manufacturer_id, data in (
                advertisement_data.manufacturer_data.items()
            ):
                print(
                    f"  {manufacturer_id:#06x}: "
                    f"{hexstr(data)}"
                )

        if advertisement_data.service_data:
            print("Service data:")

            for uuid, data in advertisement_data.service_data.items():
                print(f"  {uuid}: {hexstr(data)}")

        print("=" * 70)

        result["device"] = device
        found.set()

    scanner = BleakScanner(
        detection_callback=detection_callback
    )

    print("Scanning for S226...")

    await scanner.start()

    try:
        try:
            await asyncio.wait_for(
                found.wait(),
                timeout=SCAN_TIMEOUT,
            )
        except asyncio.TimeoutError:
            print("S226 not found.")
            return None

        device = result["device"]

        # IMPORTANT:
        #
        # We use the exact BLEDevice object obtained from the
        # advertisement instead of doing another scan or constructing
        # a new address.
        #
        # This matters because the S226 uses a random BLE address and
        # appears to stop advertising shortly after being discovered.

        print()
        print(f"Connecting immediately to {device.address}...")

        client = BleakClient(
            device,
            timeout=CONNECT_TIMEOUT,
        )

        try:
            await client.connect()
        except Exception:
            try:
                await client.disconnect()
            except Exception:
                pass

            raise

        print(f"Connected: {client.is_connected}")

        return client

    finally:
        await scanner.stop()


async def dump_descriptor(client, descriptor):
    print(f"      Descriptor {descriptor.uuid}")
    print(f"        handle: {descriptor.handle}")

    try:
        value = await client.read_gatt_descriptor(
            descriptor.handle
        )

        print(f"        READ: {hexstr(value)}")

        if value:
            try:
                print(
                    f"        ASCII: "
                    f"{value.decode(errors='replace')!r}"
                )
            except Exception:
                pass

    except Exception as e:
        print(f"        READ FAILED: {e}")


async def dump_characteristic(client, characteristic):
    print()
    print(f"    Characteristic {characteristic.uuid}")
    print(f"      handle:     {characteristic.handle}")
    print(
        f"      properties: "
        f"{', '.join(characteristic.properties)}"
    )

    # Dump descriptors.

    for descriptor in characteristic.descriptors:
        await dump_descriptor(
            client,
            descriptor,
        )

    # Try reading readable characteristics.

    if "read" in characteristic.properties:
        try:
            value = await client.read_gatt_char(
                characteristic
            )

            print(f"      READ: {hexstr(value)}")

            if value:
                try:
                    print(
                        f"            ASCII: "
                        f"{value.decode(errors='replace')!r}"
                    )
                except Exception:
                    pass

        except Exception as e:
            print(f"      READ FAILED: {e}")


async def dump_gatt(client):
    print()
    print("=" * 70)
    print("GATT DATABASE")
    print("=" * 70)

    for service in client.services:
        print()
        print(f"SERVICE {service.uuid}")
        print(f"  handle: {service.handle}")

        for characteristic in service.characteristics:
            await dump_characteristic(
                client,
                characteristic,
            )


async def subscribe_notifications(client):
    print()
    print("=" * 70)
    print("SUBSCRIBING TO NOTIFICATIONS")
    print("=" * 70)

    notify_chars = []

    for service in client.services:
        for characteristic in service.characteristics:
            properties = characteristic.properties

            if (
                "notify" in properties
                or "indicate" in properties
            ):
                notify_chars.append(characteristic)

    def notification_handler(sender, data):
        now = time.strftime("%H:%M:%S")

        print(
            f"[{now}] NOTIFY "
            f"{sender}: "
            f"{hexstr(data)} "
            f"({len(data)} bytes)"
        )

    subscribed = []

    for characteristic in notify_chars:
        try:
            await client.start_notify(
                characteristic,
                notification_handler,
            )

            print(
                f"  subscribed: "
                f"{characteristic.uuid}"
            )

            subscribed.append(characteristic)

        except Exception as e:
            print(
                f"  FAILED: "
                f"{characteristic.uuid}: {e}"
            )

    return subscribed


async def main():
    client = None

    try:
        client = await find_and_connect()

        if client is None:
            return 1

        print()
        print(
            f"Connected: "
            f"{client.is_connected}"
        )

        # Give BlueZ/Bleak a moment to finish service discovery.

        await asyncio.sleep(0.5)

        await dump_gatt(client)

        subscribed = await subscribe_notifications(
            client
        )

        print()
        print("=" * 70)
        print(
            f"LISTENING FOR "
            f"{LISTEN_SECONDS} SECONDS"
        )
        print("=" * 70)
        print()
        print(
            "Interact with the watch while this is running."
        )
        print(
            "Try menus, heart-rate measurement, etc."
        )
        print()

        try:
            await asyncio.sleep(LISTEN_SECONDS)

        except KeyboardInterrupt:
            pass

        # Unsubscribe cleanly.

        for characteristic in subscribed:
            try:
                await client.stop_notify(
                    characteristic
                )
            except Exception:
                pass

    except Exception as e:
        print()
        print("=" * 70)
        print("ERROR")
        print("=" * 70)
        print(f"{type(e).__name__}: {e}")

        return 1

    finally:
        if client is not None:
            try:
                if client.is_connected:
                    print()
                    print("Disconnecting...")
                    await client.disconnect()
            except Exception:
                pass

    print()
    print("Done.")

    return 0


if __name__ == "__main__":
    try:
        sys.exit(
            asyncio.run(main())
        )
    except KeyboardInterrupt:
        print()
        print("Interrupted.")
        sys.exit(130)
