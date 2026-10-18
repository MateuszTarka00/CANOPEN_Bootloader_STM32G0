"""Compile/run portable C tests and verify Python-generated signatures in C."""
from __future__ import annotations

import importlib.util
import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from Tools.package_firmware import DEFAULT_PRIVATE_KEY, load_private_key, make_signed_manifest
from Tools.prepare_factory import factory_record


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--application-root", type=Path,
                        help="Also run the existing VEGA CANopen/handoff C suite")
    args = parser.parse_args()
    spec = importlib.util.find_spec("ziglang")
    if spec is None or spec.origin is None:
        raise SystemExit("Missing native compiler: install requirements-dev.txt.")
    compiler = Path(spec.origin).parent / ("zig.exe" if sys.platform == "win32" else "zig")
    sources = [
        ROOT / "Bootloader" / "Src" / (name + ".c") for name in (
            "bootloader_interface", "firmware_auth", "firmware_update",
            "bootloader_transaction", "firmware_manifest", "sha256", "factory_identity",
            "persistent_storage", "canopen_sdo", "object_dictionary", "canopen_emcy",
            "canopen", "canopen_nmt", "canopen_heartbeat",
        )
    ]
    flags = [
        str(compiler), "cc", "-std=c99", "-Wall", "-Wextra", "-Werror",
        "-I" + str(ROOT / "Bootloader" / "Inc"),
        "-I" + str(ROOT / "ThirdParty" / "Monocypher" / "src"),
        "-I" + str(ROOT / "ThirdParty" / "Monocypher" / "src" / "optional"),
    ]
    sources += [
        ROOT / "ThirdParty" / "Monocypher" / "src" / "monocypher.c",
        ROOT / "ThirdParty" / "Monocypher" / "src" / "optional" / "monocypher-ed25519.c",
    ]
    with tempfile.TemporaryDirectory(prefix="boot-native-") as temporary:
        output = Path(temporary)
        for name in ("test_bootloader", "verify_manifest"):
            executable = output / (name + ".exe")
            subprocess.run(flags + [str(ROOT / "Tests" / (name + ".c"))]
                           + [str(source) for source in sources]
                           + ["-o", str(executable)], check=True, cwd=ROOT)
            if name == "test_bootloader":
                subprocess.run([str(executable)], check=True)
        image = bytearray(256)
        struct.pack_into("<II", image, 0, 0x20024000, 0x08009009)
        manifest = make_signed_manifest(
            bytes(image), 0x100, 1, 2, 3, 0x10000, 0x10000,
            load_private_key(DEFAULT_PRIVATE_KEY),
        )
        record_path, image_path = output / "manifest.bin", output / "image.bin"
        record_path.write_bytes(manifest)
        image_path.write_bytes(image)
        factory_path = output / "factory.bin"
        factory_path.write_bytes(factory_record(manifest, 1234, 1, 0x10000))
        verify = [str(output / "verify_manifest.exe"), str(record_path), str(image_path),
                  str(factory_path)]
        subprocess.run(verify, check=True)
        modified = bytearray(manifest)
        modified[76] ^= 1
        record_path.write_bytes(modified)
        if subprocess.run(verify, check=False).returncode != 1:
            raise RuntimeError("C verifier did not reject the corrupted manifest.")
        record_path.write_bytes(manifest)
        image[20] ^= 1
        image_path.write_bytes(image)
        if subprocess.run(verify, check=False).returncode != 1:
            raise RuntimeError("C verifier did not reject the altered image.")
        if args.application_root is not None:
            app = args.application_root.resolve(strict=True)
            app_sources = [app / "Tests" / "test_canopen.c"] + [
                app / "Core" / "Src" / (name + ".c") for name in (
                    "canopen", "canopen_sdo", "canopen_nmt", "canopen_heartbeat",
                    "canopen_emcy", "object_dictionary", "persistent_storage",
                    "factory_identity", "bootloader_handoff",
                )
            ]
            executable = output / "test_application.exe"
            subprocess.run(
                [str(compiler), "cc", "-std=c99", "-Wall", "-Werror",
                 "-I" + str(app / "Core" / "Inc")]
                + [str(source) for source in app_sources]
                + ["-o", str(executable)], check=True,
            )
            subprocess.run([str(executable)], check=True)
    print("Python signer -> C Monocypher verifier: valid accepted, modified inputs rejected.")


if __name__ == "__main__":
    main()
