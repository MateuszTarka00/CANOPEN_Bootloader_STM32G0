"""Explicit CANopen firmware update client using python-can (Classic CAN)."""
from __future__ import annotations

import argparse
import struct
import sys
import time
import zipfile
from pathlib import Path

import can
from cryptography.exceptions import InvalidSignature

if __package__:
    from .package_firmware import (
        DEFAULT_PUBLIC_KEY_HEADER, PackageError, read_firmware_public_key, verify_package,
        FLASH_SLOT_IMAGE_MAX,
    )
else:
    from package_firmware import (
        DEFAULT_PUBLIC_KEY_HEADER, PackageError, read_firmware_public_key, verify_package,
        FLASH_SLOT_IMAGE_MAX,
    )


class SdoError(Exception):
    """Protocol failure; never retried as if a write were idempotent."""


class SdoClient:
    def __init__(self, bus: can.BusABC, node: int, timeout: float = 15.0):
        self.bus = bus
        self.node = node
        self.timeout = timeout

    def exchange(self, data: bytes) -> bytes:
        # Discard stale SDO replies before starting a new request.
        for _ in range(256):
            if self.bus.recv(0) is None:
                break
        else:
            raise SdoError("Receive queue did not drain; check bus load and receive filters.")
        self.bus.send(can.Message(arbitration_id=0x600 + self.node,
                                  data=data, is_extended_id=False),
                      timeout=self.timeout)
        deadline = time.monotonic() + self.timeout
        while (remaining := deadline - time.monotonic()) > 0:
            reply = self.bus.recv(remaining)
            if reply is None:
                break
            if (reply.arbitration_id != 0x580 + self.node or reply.is_extended_id
                    or reply.is_remote_frame or reply.is_error_frame or reply.is_fd):
                continue
            if len(reply.data) != 8:
                raise SdoError("SDO reply must have DLC 8.")
            result = bytes(reply.data)
            if result[0] == 0x80:
                raise SdoError(
                    f"SDO abort 0x{int.from_bytes(result[4:], 'little'):08X} "
                    f"at {int.from_bytes(result[1:3], 'little'):04X}:{result[3]:02X}"
                )
            return result
        raise TimeoutError("SDO timeout: outcome is uncertain; inspect node before retrying.")

    def read(self, index: int) -> int:
        address = struct.pack("<HB", index, 0)
        reply = self.exchange(b"\x40" + address + b"\0" * 4)
        if reply[1:4] != address or reply[0] not in (0x43, 0x47, 0x4B, 0x4F):
            raise SdoError(f"Expected expedited upload at {index:04X}:00.")
        size = 4 - ((reply[0] >> 2) & 3)
        return int.from_bytes(reply[4:4 + size], "little")

    def write(self, index: int, payload: bytes) -> None:
        if not payload:
            raise ValueError("Zero-length downloads are not supported by this update profile.")
        address = struct.pack("<HB", index, 0)
        if 1 <= len(payload) <= 4:
            command = 0x23 | ((4 - len(payload)) << 2)
            reply = self.exchange(bytes((command,)) + address + payload.ljust(4, b"\0"))
        else:
            reply = self.exchange(b"\x21" + address + struct.pack("<I", len(payload)))
        if reply != b"\x60" + address + b"\0" * 4:
            raise SdoError(f"Unexpected download-init response at {index:04X}:00.")
        if len(payload) <= 4:
            return
        toggle = 0
        for offset in range(0, len(payload), 7):
            chunk = payload[offset:offset + 7]
            last = offset + len(chunk) == len(payload)
            command = toggle | ((7 - len(chunk)) << 1) | int(last)
            reply = self.exchange(bytes((command,)) + chunk.ljust(7, b"\0"))
            if reply != bytes((0x20 | toggle,)) + b"\0" * 7:
                raise SdoError("Invalid segmented download acknowledgement/toggle.")
            toggle ^= 0x10

    def wait_mode(self, mode: int, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        last_error = ""
        while time.monotonic() < deadline:
            try:
                if self.read(0x200A) == mode:
                    return
            except (TimeoutError, SdoError) as exc:
                last_error = str(exc)
            time.sleep(0.2)
        raise TimeoutError(f"Node did not enter mode {mode}. Last response: {last_error}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--interface", required=True, help="python-can backend, e.g. pcan")
    parser.add_argument("--channel", required=True, help="e.g. PCAN_USBBUS1")
    parser.add_argument("--node", type=int, default=1, choices=range(1, 128), metavar="1..127")
    parser.add_argument("--boot-node", type=int, default=126, choices=range(1, 128),
                        metavar="1..127", help="Bootloader Node-ID (default: 126)")
    parser.add_argument("--confirm-update", action="store_true",
                        help="Explicitly authorize application outage and firmware installation")
    args = parser.parse_args()
    if not args.confirm_update:
        parser.error("--confirm-update is required: update restarts the controller.")
    try:
        image, manifest = verify_package(
            args.package, read_firmware_public_key(DEFAULT_PUBLIC_KEY_HEADER)
        )
        with can.Bus(interface=args.interface, channel=args.channel, bitrate=250000,
                     can_filters=[{"can_id": 0x580 + node,
                                   "can_mask": 0x7FF, "extended": False}
                                  for node in {args.node, args.boot_node}]) as bus:
            client = SdoClient(bus, args.node)
            try:
                mode = client.read(0x200A)
            except TimeoutError:
                if args.node == args.boot_node:
                    raise
                print(f"No application response at Node {args.node}; probing "
                      f"bootloader Node {args.boot_node}. No writes retried.")
                client.node = args.boot_node
                mode = client.read(0x200A)
                if mode != 1:
                    raise SdoError("The bootloader address does not report bootloader mode.")
            if mode == 0:
                if client.read(0x2009) != 2 or client.read(0x200B) != 1:
                    raise SdoError("Application does not advertise handoff protocol 2/capability.")
                print("Requesting application -> bootloader handoff.")
                client.write(0x2000, b"\x01")
                client.node = args.boot_node
                client.wait_mode(1, 15)
            elif mode != 1:
                raise SdoError(f"Unknown execution mode {mode}.")
            if client.read(0x200C) != 1 or client.read(0x200B) != 1:
                raise SdoError("Bootloader profile/signing/identity is not ready.")
            if client.read(0x200F) != FLASH_SLOT_IMAGE_MAX:
                raise SdoError(
                    "Bootloader layout does not match STM32G0B1RC / 104 KiB payload. "
                    "Rebuild and reprogram the correct bootloader before updating."
                )
            if client.read(0x2001) != 0:
                raise SdoError("Bootloader is not idle; inspect state before cancelling/retrying.")
            client.write(0x2008, manifest)
            client.write(0x2000, b"\x01")
            print(f"Sending {len(image)} bytes (do not disconnect CAN).")
            client.write(0x2002, image)
            client.write(0x2000, b"\x02")
            print("Image verified; scheduling activation.")
            client.write(0x2000, b"\x03")
            client.node = args.node
            client.wait_mode(0, 120)
            actual = client.read(0x2006)
            expected = struct.unpack_from("<I", manifest, 16)[0]
            if actual != expected:
                raise SdoError(
                    f"Application reports version 0x{actual:08X}, expected 0x{expected:08X}. "
                    "Possible rollback or mismatched --version/package."
                )
            print("Application is responding with the expected version.")
            print("This is not proof of trial confirmation or power-fail qualification.")
    except (OSError, ValueError, PackageError, SdoError, TimeoutError,
            InvalidSignature, zipfile.BadZipFile, can.CanError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        print("No automatic write retry/cancel performed. Inspect mode/state before retrying.",
              file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
