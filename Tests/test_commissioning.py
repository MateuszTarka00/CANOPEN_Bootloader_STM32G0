import contextlib
import io
import struct
import tempfile
import unittest
import uuid
import zipfile
import zlib
from pathlib import Path
from unittest.mock import patch

import can
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from Tools import prepare_factory, update_firmware
from Tools.package_firmware import PackageError, make_signed_manifest, verify_package


def decode_hex(text: str) -> dict[int, int]:
    memory: dict[int, int] = {}
    upper = 0
    for line in text.splitlines():
        record = bytes.fromhex(line[1:])
        if sum(record) & 255 or len(record) != record[0] + 5:
            raise AssertionError("Bad Intel HEX checksum/length")
        address = int.from_bytes(record[1:3], "big")
        if record[3] == 4:
            upper = int.from_bytes(record[4:6], "big") << 16
        elif record[3] == 0:
            for offset, value in enumerate(record[4:-1]):
                memory[upper + address + offset] = value
    return memory


class UpdateServer(can.Listener):
    """Virtual CAN peer checks the on-wire protocol, including every toggle."""

    def __init__(self, bus: can.BusABC):
        self.bus = bus
        self.mode = 0
        self.state = 0
        self.version = 0
        self.index = 0
        self.size = 0
        self.toggle = 0
        self.buffer = bytearray()
        self.downloads: dict[int, bytes] = {}
        self.commands: list[int] = []
        self.errors: list[str] = []

    def on_message_received(self, message: can.Message) -> None:
        data = bytes(message.data)
        index = int.from_bytes(data[1:3], "little")
        address = data[1:4]
        if data[0] == 0x40:
            values = {0x200A: self.mode, 0x2009: 2, 0x200B: 1, 0x200C: 1,
                      0x2001: self.state, 0x2006: self.version}
            reply = b"\x43" + address + struct.pack("<I", values[index])
        elif data[0] == 0x2F and index == 0x2000:
            command = data[4]
            self.commands.append(command)
            if self.mode == 0:
                self.mode = 1
            elif command == 1:
                self.state = 1
            elif command == 2:
                self.state = 2
            elif command == 3:
                self.mode = 0
                self.version = struct.unpack_from("<I", self.downloads[0x2008], 16)[0]
            reply = b"\x60" + address + b"\0" * 4
        elif data[0] == 0x21:
            self.index = index
            self.size = int.from_bytes(data[4:], "little")
            self.toggle = 0
            self.buffer.clear()
            reply = b"\x60" + address + b"\0" * 4
        else:
            if data[0] & 0x10 != self.toggle:
                self.errors.append("Incorrect segment toggle")
            count = 7 - ((data[0] >> 1) & 7)
            self.buffer.extend(data[1:1 + count])
            reply = bytes((0x20 | self.toggle,)) + b"\0" * 7
            self.toggle ^= 0x10
            if data[0] & 1:
                if len(self.buffer) != self.size:
                    self.errors.append("Incorrect download size")
                self.downloads[self.index] = bytes(self.buffer)
        self.bus.send(can.Message(arbitration_id=0x581, data=reply,
                                  is_extended_id=False))


class CommissioningTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.key = Ed25519PrivateKey.generate()
        self.public = self.key.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        self.image = bytearray(512)
        struct.pack_into("<II", self.image, 0, 0x20010000, 0x08009009)
        self.manifest = make_signed_manifest(
            bytes(self.image), 0x100, 1, 2, 3, 0x10000, 0x10005, self.key)
        self.package = self.root / "update.zip"
        self.write_package(bytes(self.image))

    def write_package(self, image: bytes) -> None:
        with zipfile.ZipFile(self.package, "w") as archive:
            archive.writestr("firmware.bin", image)
            archive.writestr("manifest.bin", self.manifest)
            archive.writestr("package.json", "{}")

    def test_package_rejects_image_modification_and_wrong_key(self) -> None:
        self.assertEqual(verify_package(self.package, self.public)[1], self.manifest)
        wrong = Ed25519PrivateKey.generate().public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)
        with self.assertRaises(InvalidSignature):
            verify_package(self.package, wrong)
        self.image[-1] ^= 1
        self.write_package(bytes(self.image))
        with self.assertRaises(PackageError):
            verify_package(self.package, self.public)

    def test_factory_record_and_sparse_hex(self) -> None:
        boot = bytearray(b"\xff" * 0x9000)
        boot[512:544] = self.public
        struct.pack_into("<8I", boot, 0x8FE0, 0x424C4331, 2, 0x08009000,
                         0x3A800, 0x0807E800, 0x0807F000, 0x9000, 1)
        boot_path = self.root / "boot.bin"
        boot_path.write_bytes(boot)
        output = self.root / "factory"
        args = ["prepare_factory", "--package", str(self.package), "--bootloader",
                str(boot_path), "--serial", "123", "--product-revision", "1",
                "--hardware-revision", "0x10003", "--output", str(output)]
        with patch("sys.argv", args), patch.object(
                prepare_factory, "read_firmware_public_key", return_value=self.public):
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(prepare_factory.main(), 0)
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(prepare_factory.main(), 2)  # Never overwrite commissioning output.
        record = (output / "factory-identity.bin").read_bytes()
        self.assertEqual(len(record), 128)
        self.assertEqual(struct.unpack_from("<I", record, 24)[0], 123)
        self.assertEqual(struct.unpack_from("<I", record, 120)[0], zlib.crc32(record[:120]))
        memory = decode_hex((output / "initial.hex").read_text())
        self.assertEqual(bytes(memory[0x08043000 + i] for i in range(152)), self.manifest)
        self.assertEqual(bytes(memory[0x08009000 + i] for i in range(512)), self.image)
        self.assertTrue(all(address < 0x08043800 or 0x0807E800 <= address < 0x0807E880
                            for address in memory))
        with self.assertRaises(PackageError):
            prepare_factory.factory_record(self.manifest, 0, 1, 0x10003)
        with self.assertRaises(PackageError):
            prepare_factory.factory_record(self.manifest, 123, 1, 0x20000)

    def test_hex_crosses_extended_address_without_wrapping(self) -> None:
        payload = bytes(range(32))
        memory = decode_hex(prepare_factory.intel_hex([(0x0800FFF9, payload)]))
        self.assertEqual(bytes(memory[0x0800FFF9 + i] for i in range(32)), payload)
        with self.assertRaises(PackageError):
            prepare_factory.intel_hex([(100, bytes(10)), (105, bytes(10))])

    def test_complete_update_on_virtual_can(self) -> None:
        channel = uuid.uuid4().hex
        with can.Bus(interface="virtual", channel=channel) as peer:
            client_bus = can.Bus(interface="virtual", channel=channel)
            server = UpdateServer(peer)
            notifier = can.Notifier(peer, [server])
            try:
                args = ["update_firmware", "--package", str(self.package),
                        "--interface", "virtual", "--channel", channel, "--confirm-update"]
                with patch("sys.argv", args), patch.object(
                        update_firmware, "read_firmware_public_key", return_value=self.public
                        ), patch.object(update_firmware.can, "Bus", return_value=client_bus):
                    with contextlib.redirect_stdout(io.StringIO()):
                        self.assertEqual(update_firmware.main(), 0)
                self.assertEqual(server.errors, [])
                self.assertEqual(server.commands, [1, 1, 2, 3])
                self.assertEqual(server.downloads[0x2008], self.manifest)
                self.assertEqual(server.downloads[0x2002], self.image)
                self.assertEqual(server.mode, 0)
            finally:
                notifier.stop()
                client_bus.shutdown()

    def test_timeout_and_abort_are_not_retried(self) -> None:
        channel = uuid.uuid4().hex
        with can.Bus(interface="virtual", channel=channel) as bus:
            client = update_firmware.SdoClient(bus, 1, timeout=0.01)
            with self.assertRaises(TimeoutError):
                client.write(0x2000, b"\x01")
            abort = can.Message(arbitration_id=0x581, is_extended_id=False,
                                data=b"\x80\x00\x20\0\x00\x00\x02\x06")
            with patch.object(bus, "recv", side_effect=[None, abort]):
                with self.assertRaises(update_firmware.SdoError):
                    client.read(0x2000)
            with self.assertRaises(ValueError):
                client.write(0x2002, b"")


if __name__ == "__main__":
    unittest.main()
