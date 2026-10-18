import struct
import shutil
import unittest
import uuid
import zlib
import zipfile
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from Tools.package_firmware import (
    MANIFEST_COMMIT,
    MANIFEST_MAGIC,
    PackageError,
    _write_outputs,
    make_signed_manifest,
    validate_image,
)


def valid_test_image() -> bytes:
    image = bytearray(64)
    struct.pack_into("<II", image, 0, 0x20010000, 0x08009009)
    return bytes(image)


class PackageFirmwareTests(unittest.TestCase):
    def test_manifest_matches_bootloader_wire_format(self) -> None:
        image = valid_test_image()
        private_key = Ed25519PrivateKey.generate()
        manifest = make_signed_manifest(
            image=image,
            version=7,
            vendor_id=1,
            product_code=2,
            hardware_variant=3,
            hardware_min=0x00010000,
            hardware_max=0x00020005,
            private_key=private_key,
        )

        self.assertEqual(len(manifest), 152)
        fields = struct.unpack_from("<IHH8I32s4B", manifest)
        self.assertEqual(fields[0:3], (MANIFEST_MAGIC, 1, 152))
        self.assertEqual(fields[3:11], (
            len(image),
            zlib.crc32(image) & 0xFFFFFFFF,
            7,
            1,
            2,
            3,
            0x00010000,
            0x00020005,
        ))
        self.assertEqual(manifest[72:76], bytes((1, 64, 0, 0)))
        private_key.public_key().verify(manifest[76:140], manifest[:76])
        self.assertEqual(
            struct.unpack_from("<I", manifest, 140)[0],
            zlib.crc32(manifest[:140]) & 0xFFFFFFFF,
        )
        self.assertEqual(
            struct.unpack_from("<II", manifest, 144),
            (MANIFEST_COMMIT, (~MANIFEST_COMMIT) & 0xFFFFFFFF),
        )

    def test_rejects_reset_vector_outside_slot_a(self) -> None:
        image = bytearray(valid_test_image())
        struct.pack_into("<I", image, 4, 0x08023001)
        with self.assertRaises(PackageError):
            validate_image(bytes(image))

    def test_rejects_image_above_slot_payload_limit(self) -> None:
        image = bytearray(0x1A001)
        struct.pack_into("<II", image, 0, 0x20010000, 0x08009009)
        with self.assertRaises(PackageError):
            validate_image(bytes(image))

    def test_accepts_exactly_104_kib_payload(self) -> None:
        image = bytearray(104 * 1024)
        struct.pack_into("<II", image, 0, 0x20024000, 0x08022FFF)
        validate_image(bytes(image))

    def test_rejects_inverted_hardware_revision_range(self) -> None:
        with self.assertRaises(PackageError):
            make_signed_manifest(
                image=valid_test_image(),
                version=1,
                vendor_id=1,
                product_code=2,
                hardware_variant=3,
                hardware_min=10,
                hardware_max=9,
                private_key=Ed25519PrivateKey.generate(),
            )

    def test_writes_signed_update_container_and_preserves_existing_outputs(self) -> None:
        image = valid_test_image()
        manifest = make_signed_manifest(
            image=image,
            version=7,
            vendor_id=1,
            product_code=2,
            hardware_variant=3,
            hardware_min=0x00010000,
            hardware_max=0x00020005,
            private_key=Ed25519PrivateKey.generate(),
        )
        directory = Path(__file__).parent / (".package-" + uuid.uuid4().hex)
        directory.mkdir()
        try:
            prefix = directory / "firmware"
            manifest_path, package_path, update_path = _write_outputs(
                Path("firmware.bin"),
                prefix,
                image,
                manifest,
                {"image_size": len(image)},
                False,
            )
            self.assertEqual(manifest_path.read_bytes(), manifest)
            self.assertEqual(update_path.read_bytes(), manifest + image)
            with zipfile.ZipFile(package_path) as archive:
                self.assertEqual(
                    sorted(archive.namelist()),
                    ["firmware.bin", "manifest.bin", "package.json"],
                )
            with self.assertRaises(PackageError):
                _write_outputs(
                    Path("firmware.bin"), prefix, image, manifest, {}, False
                )
            _write_outputs(
                Path("firmware.bin"), prefix, image, manifest, {}, True
            )
        finally:
            shutil.rmtree(directory, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
