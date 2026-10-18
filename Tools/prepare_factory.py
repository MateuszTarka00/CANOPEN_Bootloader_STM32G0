"""Prepare sparse SWD programming files; never connect to or erase a device."""
from __future__ import annotations

import argparse
import struct
import sys
import zlib
import zipfile
from pathlib import Path

from cryptography.exceptions import InvalidSignature

if __package__:
    from .package_firmware import (
        DEFAULT_PUBLIC_KEY_HEADER, FLASH_SLOT_A_START, FLASH_SLOT_IMAGE_MAX,
        FLASH_BOOT_SIZE, FLASH_SLOT_SIZE, FLASH_FACTORY_START, FLASH_NVM_START,
        PackageError, parse_uint32, read_firmware_public_key, verify_package,
    )
else:
    from package_firmware import (
        DEFAULT_PUBLIC_KEY_HEADER, FLASH_SLOT_A_START, FLASH_SLOT_IMAGE_MAX,
        FLASH_BOOT_SIZE, FLASH_SLOT_SIZE, FLASH_FACTORY_START, FLASH_NVM_START,
        PackageError, parse_uint32, read_firmware_public_key, verify_package,
    )


def factory_record(manifest: bytes, serial: int, revision: int, hardware: int) -> bytes:
    vendor, product, variant, low, high = struct.unpack_from("<5I", manifest, 20)
    if any(value in (0, 0xFFFFFFFF) for value in (vendor, product, variant, serial)):
        raise PackageError("Factory vendor/product/variant/serial cannot be 0 or 0xFFFFFFFF.")
    if revision == 0xFFFFFFFF or hardware >> 16 == 0xFFFF or hardware & 0xFFFF == 0xFFFF:
        raise PackageError("Reserved factory revision value.")
    if not low <= hardware <= high:
        raise PackageError("Factory hardware revision is outside the signed image range.")
    record = bytearray(b"\xff" * 128)
    struct.pack_into("<8I2H", record, 0, 0x46494431, 1, 128, vendor, product,
                     revision, serial, variant, hardware >> 16, hardware & 0xFFFF)
    struct.pack_into("<II", record, 120, zlib.crc32(record[:120]), 0x46434D54)
    return bytes(record)


def intel_hex(regions: list[tuple[int, bytes]]) -> str:
    lines: list[str] = []
    upper = -1

    def emit(address: int, kind: int, data: bytes) -> None:
        body = bytes((len(data), address >> 8, address & 255, kind)) + data
        lines.append(":" + (body + bytes((-sum(body) & 255,))).hex().upper())

    end = 0
    for start, payload in sorted(regions):
        if start < end:
            raise PackageError("Overlapping programming regions.")
        end = start + len(payload)
        offset = 0
        while offset < len(payload):
            address = start + offset
            count = min(16, len(payload) - offset, 0x10000 - (address & 0xFFFF))
            if address >> 16 != upper:
                upper = address >> 16
                emit(0, 4, upper.to_bytes(2, "big"))
            emit(address & 0xFFFF, 0, payload[offset:offset + count])
            offset += count
    emit(0, 1, b"")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--bootloader", type=Path, required=True, help="Bootloader BIN")
    parser.add_argument("--serial", type=parse_uint32, required=True)
    parser.add_argument("--product-revision", type=parse_uint32, required=True)
    parser.add_argument("--hardware-revision", type=parse_uint32, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New output directory")
    args = parser.parse_args()
    try:
        public = read_firmware_public_key(DEFAULT_PUBLIC_KEY_HEADER)
        image, manifest = verify_package(args.package, public)
        boot = args.bootloader.read_bytes()
        if len(boot) != FLASH_BOOT_SIZE:
            raise PackageError("Expected a 36 KiB bootloader BIN including its discovery record.")
        contract = struct.unpack_from("<8I", boot, FLASH_BOOT_SIZE - 32)
        if contract != (0x424C4331, 2, FLASH_SLOT_A_START, FLASH_SLOT_SIZE,
                        FLASH_FACTORY_START, FLASH_NVM_START, FLASH_BOOT_SIZE, 1):
            raise PackageError(
                "Bootloader discovery contract does not match the STM32G0B1RC "
                "256 KiB layout, or signing is disabled. Rebuild both projects."
            )
        if public not in boot:
            raise PackageError("Bootloader BIN does not contain the configured signing public key.")
        identity = factory_record(manifest, args.serial, args.product_revision,
                                  args.hardware_revision)
        slot = image.ljust(FLASH_SLOT_IMAGE_MAX, b"\xff") + manifest
        regions = [(0x08000000, boot), (FLASH_SLOT_A_START, image),
                   (FLASH_SLOT_A_START + FLASH_SLOT_IMAGE_MAX, manifest),
                   (FLASH_FACTORY_START, identity)]
        hex_text = intel_hex(regions)
        args.output.mkdir(parents=True, exist_ok=False)
        (args.output / "initial.hex").write_text(hex_text, encoding="ascii")
        (args.output / "slot-a.bin").write_bytes(slot)
        (args.output / "factory-identity.bin").write_bytes(identity)
    except (OSError, ValueError, PackageError, InvalidSignature,
            struct.error, zipfile.BadZipFile) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    print(f"Prepared {args.output / 'initial.hex'}")
    print("Sparse HEX includes bootloader, application, manifest and device identity.")
    print("NVM, Slot B and scratch are NOT included. See README before first programming.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
