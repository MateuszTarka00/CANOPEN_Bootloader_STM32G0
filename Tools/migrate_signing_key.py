"""Move the existing signing key using password-encrypted PEM and local DPAPI."""
from __future__ import annotations

import argparse
import ctypes
import getpass
import hashlib
import sys
import warnings
from pathlib import Path
from typing import Sequence

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

if __package__:
    from .package_firmware import (
        DEFAULT_PRIVATE_KEY, DEFAULT_PUBLIC_KEY_HEADER, DataBlob, PackageError,
        _unprotect_windows_dpapi, load_private_key, read_firmware_public_key,
    )
else:
    from package_firmware import (
        DEFAULT_PRIVATE_KEY, DEFAULT_PUBLIC_KEY_HEADER, DataBlob, PackageError,
        _unprotect_windows_dpapi, load_private_key, read_firmware_public_key,
    )


def protect_dpapi(plaintext: bytes) -> bytes:
    """Use CurrentUser protection, not machine-wide protection or optional entropy."""
    if sys.platform != "win32":
        raise PackageError("DPAPI import requires Windows.")
    crypt32 = ctypes.WinDLL("crypt32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    source_buffer = ctypes.create_string_buffer(plaintext)
    source = DataBlob(len(plaintext), ctypes.cast(
        source_buffer, ctypes.POINTER(ctypes.c_ubyte)))
    result = DataBlob()
    crypt32.CryptProtectData.argtypes = [
        ctypes.POINTER(DataBlob), ctypes.c_wchar_p, ctypes.c_void_p,
        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(DataBlob),
    ]
    crypt32.CryptProtectData.restype = ctypes.c_int
    kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    kernel32.LocalFree.restype = ctypes.c_void_p
    try:
        if not crypt32.CryptProtectData(
                ctypes.byref(source), "CANopen firmware signing key", None,
                None, None, 0x01, ctypes.byref(result)):
            raise PackageError(
                f"Windows DPAPI encryption failed (Windows error {ctypes.get_last_error()})."
            )
        try:
            return ctypes.string_at(result.data, result.size)
        finally:
            kernel32.LocalFree(ctypes.cast(result.data, ctypes.c_void_p))
    finally:
        ctypes.memset(source_buffer, 0, ctypes.sizeof(source_buffer))


def checked_public_key(key: Ed25519PrivateKey, header: Path) -> bytes:
    public = key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    if public != read_firmware_public_key(header):
        raise PackageError("Key does not match the bootloader public-key header. No file written.")
    return public


def read_password(confirm: bool) -> bytes:
    # Refuse getpass's echoed-input fallback in redirected/noninteractive terminals.
    with warnings.catch_warnings():
        warnings.simplefilter("error", getpass.GetPassWarning)
        password = getpass.getpass("Encrypted PEM password (input hidden): ")
        if confirm:
            if len(password) < 16:
                raise PackageError("Use a strong, unique password of at least 16 characters.")
            if password != getpass.getpass("Repeat password: "):
                raise PackageError("Passwords do not match.")
    if not password:
        raise PackageError("Empty passwords are not accepted.")
    return password.encode("utf-8")


def write_new(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("xb") as output:
        output.write(data)


def export_key(source: Path, output: Path, header: Path, password: bytes) -> bytes:
    if len(password) < 16:
        raise PackageError("Use an export password of at least 16 UTF-8 bytes.")
    key = load_private_key(source)
    public = checked_public_key(key, header)
    encrypted = key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.BestAvailableEncryption(password),
    )
    restored = serialization.load_pem_private_key(encrypted, password)
    if not isinstance(restored, Ed25519PrivateKey):
        raise PackageError("Encrypted PEM round-trip did not restore an Ed25519 key.")
    checked_public_key(restored, header)
    write_new(output, encrypted)
    return public


def import_key(source: Path, output: Path, header: Path, password: bytes) -> bytes:
    if output.suffix.lower() != ".dpapi":
        raise PackageError("Import output must have a .dpapi extension.")
    encrypted = source.read_bytes()
    if not encrypted.startswith(b"-----BEGIN ENCRYPTED PRIVATE KEY-----"):
        raise PackageError("Import requires a password-encrypted PKCS8 PEM, not plaintext.")
    try:
        key = serialization.load_pem_private_key(encrypted, password)
    except (ValueError, TypeError) as exc:
        raise PackageError("Cannot decrypt PEM: incorrect password or damaged file.") from exc
    if not isinstance(key, Ed25519PrivateKey):
        raise PackageError("Imported key is not Ed25519.")
    public = checked_public_key(key, header)
    plaintext = key.private_bytes(
        serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    )
    ciphertext = protect_dpapi(plaintext)
    if _unprotect_windows_dpapi(ciphertext) != plaintext:
        raise PackageError("Local DPAPI round-trip failed. No file written.")
    write_new(output, ciphertext)
    return public


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--public-key-header", type=Path, default=DEFAULT_PUBLIC_KEY_HEADER)
    commands = parser.add_subparsers(dest="command", required=True)
    export = commands.add_parser("export", help="On the original computer")
    export.add_argument("--source", type=Path, default=DEFAULT_PRIVATE_KEY)
    export.add_argument("--output", type=Path, required=True, help="New encrypted PEM path")
    import_command = commands.add_parser("import", help="On the destination Windows computer")
    import_command.add_argument("--source", type=Path, required=True, help="Encrypted PEM path")
    import_command.add_argument("--output", type=Path, default=DEFAULT_PRIVATE_KEY)
    verify = commands.add_parser("verify", help="Check the local DPAPI key against the bootloader")
    verify.add_argument("--source", type=Path, default=DEFAULT_PRIVATE_KEY)
    args = parser.parse_args(argv)
    try:
        if args.command == "export":
            public = export_key(args.source, args.output, args.public_key_header,
                                read_password(confirm=True))
            print(f"Encrypted export created: {args.output}")
        elif args.command == "import":
            public = import_key(args.source, args.output, args.public_key_header,
                                read_password(confirm=False))
            print(f"Local CurrentUser DPAPI key created: {args.output}")
        else:
            key = load_private_key(args.source)
            public = checked_public_key(key, args.public_key_header)
            challenge = b"CANopen signing key migration verification v1"
            key.public_key().verify(key.sign(challenge), challenge)
            print("Local key decrypts, signs and matches the bootloader public key.")
    except (OSError, ValueError, TypeError, PackageError, EOFError,
            getpass.GetPassWarning) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    print(f"Public-key SHA-256: {hashlib.sha256(public).hexdigest()}")
    print("No bootloader rebuild or key replacement is required.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
