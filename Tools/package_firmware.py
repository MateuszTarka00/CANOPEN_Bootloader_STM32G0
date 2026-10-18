#!/usr/bin/env python3
"""Create an Ed25519-signed firmware manifest and CANopen update package."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import re
import struct
import sys
import tempfile
import zipfile
import zlib
from pathlib import Path
from typing import Sequence

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey,
    Ed25519PublicKey,
)

REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PRIVATE_KEY = (
    Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local"))
    / "CANOPEN_Bootloader"
    / "PrototypeSigningKey"
    / "ed25519-signing-key.dpapi"
)
DEFAULT_PUBLIC_KEY_HEADER = (
    REPOSITORY_ROOT / "Bootloader" / "Inc" / "firmware_auth_key.h"
)

FLASH_SLOT_A_START = 0x08009000
FLASH_SIZE = 0x40000
FLASH_BOOT_SIZE = 0x9000
FLASH_SLOT_SIZE = 0x1A800
FLASH_SLOT_IMAGE_MAX = 0x1A000
FLASH_SLOT_B_START = 0x08023800
FLASH_FACTORY_START = 0x0803E800
FLASH_NVM_START = 0x0803F000
RAM_START = 0x20000000
RAM_END = 0x20024000
MANIFEST_SIZE = 152
SIGNED_SIZE = 76
SIGNATURE_SIZE = 64
MANIFEST_MAGIC = 0x31424756
MANIFEST_COMMIT = 0x544D4342
MANIFEST_VERSION = 1
SIGNATURE_ED25519 = 1


class PackageError(Exception):
    """Raised when an input cannot produce a bootloader-valid package."""


class DataBlob(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("data", ctypes.POINTER(ctypes.c_ubyte))]


def verify_package(path: Path, public_key: bytes) -> tuple[bytes, bytes]:
    """Verify the signed wire record, not the descriptive JSON in the ZIP."""
    with zipfile.ZipFile(path) as archive:
        if sorted(archive.namelist()) != ["firmware.bin", "manifest.bin", "package.json"]:
            raise PackageError("Unexpected or duplicate ZIP members.")
        if archive.getinfo("firmware.bin").file_size > FLASH_SLOT_IMAGE_MAX:
            raise PackageError("ZIP firmware exceeds the payload limit.")
        if archive.getinfo("manifest.bin").file_size != MANIFEST_SIZE:
            raise PackageError("ZIP manifest must contain exactly 152 bytes.")
        image = archive.read("firmware.bin")
        manifest = archive.read("manifest.bin")
    validate_image(image)
    if struct.unpack_from("<IHH", manifest) != (MANIFEST_MAGIC, 1, MANIFEST_SIZE):
        raise PackageError("Unsupported manifest header.")
    if manifest[72:76] != bytes((1, 64, 0, 0)):
        raise PackageError("Unsupported signature format.")
    if struct.unpack_from("<III", manifest, 140) != (
        zlib.crc32(manifest[:140]) & 0xFFFFFFFF,
        MANIFEST_COMMIT, ~MANIFEST_COMMIT & 0xFFFFFFFF,
    ):
        raise PackageError("Invalid manifest CRC/commit markers.")
    Ed25519PublicKey.from_public_bytes(public_key).verify(
        manifest[76:140], manifest[:76]
    )
    size, crc = struct.unpack_from("<II", manifest, 8)
    if (size != len(image) or crc != zlib.crc32(image) & 0xFFFFFFFF
            or manifest[40:72] != hashlib.sha256(image).digest()):
        raise PackageError("Firmware does not match the signed manifest.")
    if struct.unpack_from("<I", manifest, 32)[0] > struct.unpack_from("<I", manifest, 36)[0]:
        raise PackageError("Inverted hardware revision range.")
    return image, manifest


def _unprotect_windows_dpapi(ciphertext: bytes) -> bytes:
    if sys.platform != "win32":
        raise PackageError("DPAPI-protected private keys can only be read on Windows.")

    crypt32 = ctypes.WinDLL("crypt32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    source_buffer = ctypes.create_string_buffer(ciphertext)
    source = DataBlob(
        len(ciphertext),
        ctypes.cast(source_buffer, ctypes.POINTER(ctypes.c_ubyte)),
    )
    result = DataBlob()
    crypt32.CryptUnprotectData.argtypes = [
        ctypes.POINTER(DataBlob),
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_uint32,
        ctypes.POINTER(DataBlob),
    ]
    crypt32.CryptUnprotectData.restype = ctypes.c_int
    kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    kernel32.LocalFree.restype = ctypes.c_void_p

    if not crypt32.CryptUnprotectData(
        ctypes.byref(source), None, None, None, None, 0x01, ctypes.byref(result)
    ):
        error = ctypes.get_last_error()
        raise PackageError(
            "Windows DPAPI could not decrypt the private key for this account "
            f"(Windows error {error})."
        )
    try:
        return ctypes.string_at(result.data, result.size)
    finally:
        kernel32.LocalFree(ctypes.cast(result.data, ctypes.c_void_p))


def load_private_key(path: Path) -> Ed25519PrivateKey:
    key_data = path.read_bytes()
    if path.suffix.lower() == ".dpapi":
        key_data = _unprotect_windows_dpapi(key_data)

    try:
        if key_data.startswith(b"-----BEGIN OPENSSH PRIVATE KEY-----"):
            key = serialization.load_ssh_private_key(key_data, password=None)
        elif key_data.startswith(b"-----BEGIN"):
            key = serialization.load_pem_private_key(key_data, password=None)
        else:
            key = serialization.load_ssh_private_key(key_data, password=None)
    except (TypeError, ValueError) as exc:
        raise PackageError(
            f"Cannot read an unencrypted Ed25519 PEM/OpenSSH private key from {path}."
        ) from exc
    if not isinstance(key, Ed25519PrivateKey):
        raise PackageError(f"The private key in {path} is not an Ed25519 key.")
    return key


def read_firmware_public_key(path: Path) -> bytes:
    text = path.read_text(encoding="ascii")
    match = re.search(
        r"bl_signing_public_key\s*\[\s*32\s*\]\s*=\s*\{([^}]*)\}",
        text,
        re.DOTALL,
    )
    if match is None:
        raise PackageError(f"Could not find the 32-byte public-key array in {path}.")
    values = re.findall(r"0[xX]([0-9a-fA-F]{1,2})\s*U", match.group(1))
    if len(values) != 32:
        raise PackageError(
            f"Expected 32 byte values in the public-key array in {path}; found {len(values)}."
        )
    return bytes(int(value, 16) for value in values)


def validate_image(image: bytes) -> None:
    if not image:
        raise PackageError("The firmware image is empty.")
    if len(image) > FLASH_SLOT_IMAGE_MAX:
        raise PackageError(
            f"Image is {len(image)} bytes; Slot A allows at most "
            f"{FLASH_SLOT_IMAGE_MAX} bytes (104 KiB)."
        )
    if len(image) < 8:
        raise PackageError("Image is too short to contain an STM32 vector table.")

    stack_pointer, reset_handler = struct.unpack_from("<II", image)
    if not (RAM_START <= stack_pointer <= RAM_END) or stack_pointer % 8 != 0:
        raise PackageError(
            f"Invalid initial stack pointer 0x{stack_pointer:08X}; expected "
            "an 8-byte-aligned address in SRAM."
        )
    reset_address = reset_handler & ~1
    if (
        reset_handler & 1 == 0
        or reset_address < FLASH_SLOT_A_START
        or reset_address >= FLASH_SLOT_A_START + len(image)
    ):
        raise PackageError(
            f"Invalid reset vector 0x{reset_handler:08X}; expected a Thumb "
            "address inside the Slot-A-linked image."
        )


def make_signed_manifest(
    image: bytes,
    version: int,
    vendor_id: int,
    product_code: int,
    hardware_variant: int,
    hardware_min: int,
    hardware_max: int,
    private_key: Ed25519PrivateKey,
) -> bytes:
    values = (
        version,
        vendor_id,
        product_code,
        hardware_variant,
        hardware_min,
        hardware_max,
    )
    if any(value < 0 or value > 0xFFFFFFFF for value in values):
        raise PackageError("Version and compatibility values must be uint32 values.")
    if hardware_min > hardware_max:
        raise PackageError("--hw-min must be less than or equal to --hw-max.")

    image_crc = zlib.crc32(image) & 0xFFFFFFFF
    image_digest = hashlib.sha256(image).digest()
    signed_fields = struct.pack(
        "<IHH8I32s4B",
        MANIFEST_MAGIC,
        MANIFEST_VERSION,
        MANIFEST_SIZE,
        len(image),
        image_crc,
        version,
        vendor_id,
        product_code,
        hardware_variant,
        hardware_min,
        hardware_max,
        image_digest,
        SIGNATURE_ED25519,
        SIGNATURE_SIZE,
        0,
        0,
    )
    if len(signed_fields) != SIGNED_SIZE:
        raise PackageError("Internal error: canonical signed manifest is not 76 bytes.")

    signature = private_key.sign(signed_fields)
    Ed25519PublicKey.from_public_bytes(
        private_key.public_key().public_bytes(
            serialization.Encoding.Raw,
            serialization.PublicFormat.Raw,
        )
    ).verify(signature, signed_fields)

    record = bytearray(signed_fields + signature)
    record.extend(struct.pack("<I", zlib.crc32(record) & 0xFFFFFFFF))
    record.extend(
        struct.pack("<II", MANIFEST_COMMIT, (~MANIFEST_COMMIT) & 0xFFFFFFFF)
    )
    if len(record) != MANIFEST_SIZE:
        raise PackageError("Internal error: manifest is not 152 bytes.")
    return bytes(record)


def parse_uint32(text: str) -> int:
    try:
        value = int(text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "expected an integer (decimal or 0x-prefixed hexadecimal)"
        ) from exc
    if value < 0 or value > 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("value must fit in uint32")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Validate a Slot-A-linked STM32 BIN and create its signed 152-byte "
            "manifest, ZIP package, and manifest-prefixed CANopen update container."
        )
    )
    parser.add_argument("--image", type=Path, required=True, help="Input application BIN")
    parser.add_argument(
        "--output-prefix",
        type=Path,
        required=True,
        help="Output path prefix, without an extension",
    )
    parser.add_argument("--version", type=parse_uint32, required=True)
    parser.add_argument("--vendor-id", type=parse_uint32, required=True)
    parser.add_argument("--product-code", type=parse_uint32, required=True)
    parser.add_argument("--hardware-variant", type=parse_uint32, required=True)
    parser.add_argument("--hw-min", type=parse_uint32, required=True)
    parser.add_argument("--hw-max", type=parse_uint32, required=True)
    parser.add_argument(
        "--private-key",
        type=Path,
        default=DEFAULT_PRIVATE_KEY,
        help="Private key (.dpapi, OpenSSH, or PEM); defaults to this prototype key",
    )
    parser.add_argument(
        "--public-key-header",
        type=Path,
        default=DEFAULT_PUBLIC_KEY_HEADER,
        help="Bootloader public-key header used to reject a mismatched signer",
    )
    parser.add_argument(
        "--allow-key-mismatch",
        action="store_true",
        help="Allow signing with a key different from the configured bootloader key",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Replace existing manifest/package outputs",
    )
    return parser


def _write_outputs(
    image_path: Path,
    prefix: Path,
    image: bytes,
    manifest: bytes,
    metadata: dict[str, int | str],
    force: bool,
) -> tuple[Path, Path, Path]:
    manifest_path = prefix.with_name(prefix.name + ".manifest.bin")
    package_path = prefix.with_name(prefix.name + ".package.zip")
    update_path = prefix.with_name(prefix.name + ".update.bin")
    if not force and any(
        path.exists() for path in (manifest_path, package_path, update_path)
    ):
        raise PackageError(
            "Output exists. Choose another --output-prefix or pass --force."
        )
    prefix.parent.mkdir(parents=True, exist_ok=True)

    package_metadata = json.dumps(metadata, indent=2, sort_keys=True).encode("utf-8")
    temporary_paths: list[Path] = []
    try:
        for destination, content in (
            (manifest_path, manifest),
            (package_path, None),
            (update_path, manifest + image),
        ):
            with tempfile.NamedTemporaryFile(
                prefix=destination.name + ".",
                suffix=".tmp",
                dir=destination.parent,
                delete=False,
            ) as temp_file:
                temporary = Path(temp_file.name)
                temporary_paths.append(temporary)
                if content is not None:
                    temp_file.write(content)

        with zipfile.ZipFile(temporary_paths[1], "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("firmware.bin", image)
            archive.writestr("manifest.bin", manifest)
            archive.writestr("package.json", package_metadata)

        os.replace(temporary_paths[0], manifest_path)
        temporary_paths.pop(0)
        os.replace(temporary_paths[0], package_path)
        temporary_paths.pop(0)
        os.replace(temporary_paths[0], update_path)
        temporary_paths.clear()
    finally:
        for temporary in temporary_paths:
            temporary.unlink(missing_ok=True)
    return manifest_path, package_path, update_path


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        image_path = args.image.resolve(strict=True)
        if not image_path.is_file():
            raise PackageError(f"Input is not a file: {image_path}")
        image = image_path.read_bytes()
        validate_image(image)

        private_key = load_private_key(args.private_key)
        public_key = private_key.public_key().public_bytes(
            serialization.Encoding.Raw,
            serialization.PublicFormat.Raw,
        )
        configured_key = read_firmware_public_key(args.public_key_header)
        if public_key != configured_key and not args.allow_key_mismatch:
            raise PackageError(
                "Signing key does not match the public key in "
                f"{args.public_key_header}. Use the matching private key, or "
                "explicitly pass --allow-key-mismatch for a separate test key."
            )

        manifest = make_signed_manifest(
            image=image,
            version=args.version,
            vendor_id=args.vendor_id,
            product_code=args.product_code,
            hardware_variant=args.hardware_variant,
            hardware_min=args.hw_min,
            hardware_max=args.hw_max,
            private_key=private_key,
        )
        metadata: dict[str, int | str] = {
            "format_version": MANIFEST_VERSION,
            "image": image_path.name,
            "image_size": len(image),
            "image_crc32": zlib.crc32(image) & 0xFFFFFFFF,
            "image_sha256": hashlib.sha256(image).hexdigest(),
            "firmware_version": args.version,
            "vendor_id": args.vendor_id,
            "product_code": args.product_code,
            "hardware_variant": args.hardware_variant,
            "hardware_min": args.hw_min,
            "hardware_max": args.hw_max,
            "signing_key_matches_bootloader": public_key == configured_key,
        }
        manifest_path, package_path, update_path = _write_outputs(
            image_path,
            args.output_prefix.resolve(),
            image,
            manifest,
            metadata,
            args.force,
        )
    except (OSError, PackageError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    provisioned = re.search(
        r"#define\s+BL_SIGNING_PUBLIC_KEY_PROVISIONED\s+([01])",
        args.public_key_header.read_text(encoding="ascii"),
    )
    print(f"Firmware: {image_path} ({len(image)} bytes)")
    print(f"Manifest: {manifest_path} ({len(manifest)} bytes)")
    print(f"Update package: {package_path}")
    print(f"CANopen update container: {update_path} ({len(manifest) + len(image)} bytes)")
    if provisioned is not None and provisioned.group(1) == "0":
        print(
            "WARNING: this bootloader has BL_SIGNING_PUBLIC_KEY_PROVISIONED=0; "
            "it will reject this signature until the gate and factory image "
            "are deliberately provisioned."
        )
    if public_key != configured_key:
        print(
            "WARNING: signature key does not match the bootloader public key; "
            "this package is for offline testing only."
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
