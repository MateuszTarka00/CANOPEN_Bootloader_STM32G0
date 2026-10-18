import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from Tools import migrate_signing_key as migration
from Tools.package_firmware import PackageError, load_private_key


class SigningKeyMigrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.key = Ed25519PrivateKey.generate()
        self.public = self.key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self.header = self.root / "key.h"
        self.header.write_text("static const unsigned char bl_signing_public_key[32] = {"
                               + ",".join(f"0x{value:02x}U" for value in self.public) + "};",
                               encoding="ascii")
        self.password = b"test-only-strong-password"

    def export(self) -> Path:
        output = self.root / "transfer.pem"
        with patch.object(migration, "load_private_key", return_value=self.key):
            self.assertEqual(migration.export_key(
                self.root / "source.dpapi", output, self.header, self.password), self.public)
        return output

    def test_export_is_encrypted_and_never_overwrites(self) -> None:
        output = self.export()
        encrypted = output.read_bytes()
        self.assertTrue(encrypted.startswith(b"-----BEGIN ENCRYPTED PRIVATE KEY-----"))
        with self.assertRaises(TypeError):
            serialization.load_pem_private_key(encrypted, None)
        with self.assertRaises(ValueError):
            serialization.load_pem_private_key(encrypted, b"wrong")
        restored = serialization.load_pem_private_key(encrypted, self.password)
        self.assertIsInstance(restored, Ed25519PrivateKey)
        with patch.object(migration, "load_private_key", return_value=self.key):
            with self.assertRaises(FileExistsError):
                migration.export_key(self.root / "source.dpapi", output,
                                     self.header, self.password)
        self.assertEqual(output.read_bytes(), encrypted)

    def test_wrong_password_and_plaintext_import_write_nothing(self) -> None:
        source = self.export()
        output = self.root / "import.dpapi"
        with self.assertRaises(PackageError):
            migration.import_key(source, output, self.header, b"wrong")
        self.assertFalse(output.exists())
        source.write_bytes(self.key.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
        with self.assertRaises(PackageError):
            migration.import_key(source, output, self.header, self.password)
        self.assertFalse(output.exists())

    def test_wrong_key_export_and_import_write_nothing(self) -> None:
        source = self.export()
        different = Ed25519PrivateKey.generate()
        with patch.object(migration, "load_private_key", return_value=different):
            with self.assertRaises(PackageError):
                migration.export_key(source, self.root / "wrong.pem",
                                     self.header, self.password)
        source.write_bytes(different.private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
            serialization.BestAvailableEncryption(self.password)))
        with self.assertRaises(PackageError):
            migration.import_key(source, self.root / "wrong.dpapi", self.header, self.password)
        self.assertFalse((self.root / "wrong.pem").exists())
        self.assertFalse((self.root / "wrong.dpapi").exists())

    def test_mismatched_password_confirmation(self) -> None:
        with patch.object(migration.getpass, "getpass",
                          side_effect=["long-unique-password", "different-password"]):
            with self.assertRaises(PackageError):
                migration.read_password(confirm=True)

    @unittest.skipUnless(sys.platform == "win32", "Windows DPAPI integration")
    def test_real_current_user_dpapi_roundtrip_and_cli_verification(self) -> None:
        source = self.export()
        output = self.root / "import.dpapi"
        self.assertEqual(migration.import_key(source, output, self.header, self.password),
                         self.public)
        loaded = load_private_key(output)
        self.assertEqual(loaded.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw), self.public)
        before = output.read_bytes()
        with self.assertRaises(FileExistsError):
            migration.import_key(source, output, self.header, self.password)
        self.assertEqual(output.read_bytes(), before)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(migration.main(["--public-key-header", str(self.header),
                                            "verify", "--source", str(output)]), 0)
        self.assertEqual(sorted(path.name for path in self.root.iterdir()),
                         ["import.dpapi", "key.h", "transfer.pem"])


if __name__ == "__main__":
    unittest.main()
