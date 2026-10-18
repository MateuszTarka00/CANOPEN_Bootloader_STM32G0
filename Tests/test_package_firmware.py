import struct
import unittest
import zlib

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from Tools.package_firmware import (
    MANIFEST_COMMIT,
    MANIFEST_MAGIC,
    PackageError,
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
        struct.pack_into("<I", image, 4, 0x08043001)
        with self.assertRaises(PackageError):
            validate_image(bytes(image))

    def test_rejects_image_above_slot_payload_limit(self) -> None:
        image = bytearray(0x3A001)
        struct.pack_into("<II", image, 0, 0x20010000, 0x08009009)
        with self.assertRaises(PackageError):
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


if __name__ == "__main__":
    unittest.main()
