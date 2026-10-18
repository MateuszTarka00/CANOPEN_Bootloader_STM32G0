"""Keep packaging, linker partitions and the C target contract synchronized."""
import re
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

from Tools import package_firmware as package

ROOT = Path(__file__).resolve().parents[1]


class MemoryContractTests(unittest.TestCase):
    def test_python_constants_match_c_device_contract(self) -> None:
        header = (ROOT / "Bootloader" / "Inc" / "device_memory.h").read_text()
        constants = {
            name: int(value, 16) for name, value in re.findall(
                r"#define\s+(DEVICE_\w+)\s+UINT32_C\((0x[0-9A-Fa-f]+)\)", header)
        }
        expected = {
            "DEVICE_FLASH_SIZE": package.FLASH_SIZE,
            "DEVICE_BOOT_SIZE": package.FLASH_BOOT_SIZE,
            "DEVICE_SLOT_A_START": package.FLASH_SLOT_A_START,
            "DEVICE_SLOT_B_START": package.FLASH_SLOT_B_START,
            "DEVICE_SLOT_SIZE": package.FLASH_SLOT_SIZE,
            "DEVICE_FACTORY_START": package.FLASH_FACTORY_START,
            "DEVICE_NVM_START": package.FLASH_NVM_START,
            "DEVICE_RAM_END": package.RAM_END,
        }
        for name, value in expected.items():
            with self.subTest(constant=name):
                self.assertEqual(constants[name], value)
        self.assertEqual(package.FLASH_SIZE, 256 * 1024)
        self.assertEqual(package.FLASH_SLOT_IMAGE_MAX, 104 * 1024)
        self.assertEqual(package.FLASH_SLOT_A_START + package.FLASH_SLOT_SIZE,
                         package.FLASH_SLOT_B_START)
        self.assertEqual(package.FLASH_SLOT_B_START + package.FLASH_SLOT_SIZE,
                         constants["DEVICE_SCRATCH_START"])
        self.assertEqual(package.FLASH_NVM_START + 4096, 0x08040000)

    def test_bootloader_linker_contract_and_cubeide_device(self) -> None:
        text = (ROOT / "STM32G0B1RCTX_FLASH.ld").read_text()
        symbols = {name: int(value, 16) for name, value in re.findall(
            r"(__\w+__)\s*=\s*(0x[0-9A-Fa-f]+);", text)}
        self.assertEqual(symbols["__slot_a_start__"], package.FLASH_SLOT_A_START)
        self.assertEqual(symbols["__slot_b_start__"], package.FLASH_SLOT_B_START)
        self.assertEqual(symbols["__slot_size__"], package.FLASH_SLOT_SIZE)
        self.assertEqual(symbols["__factory_identity_start__"], package.FLASH_FACTORY_START)
        self.assertEqual(symbols["__canopen_nvm_start__"], package.FLASH_NVM_START)
        self.assertIn("== 0x08040000", text)
        project = ET.parse(ROOT / ".cproject")
        devices = [option.get("value") for option in project.iter("option")
                   if option.get("superClass", "").endswith(".option.target_mcu")]
        linkers = [option.get("value") for option in project.iter("option")
                   if option.get("superClass", "").endswith(".linker.option.script")]
        self.assertEqual(devices, ["STM32G0B1RCTx"] * 2)
        self.assertEqual(len(linkers), 2)
        self.assertTrue(all("STM32G0B1RCTX_FLASH.ld" in path for path in linkers))


if __name__ == "__main__":
    unittest.main()
