/**
 * @file bootloader_layout.h
 * @brief Fixed STM32G0B1RCT6 Flash partition map.
 *
 * The two application slots contain identical-address-linked images. The
 * bootloader swaps complete pages through the scratch page after verification;
 * only slot A is ever executed. The last page of each slot stores its manifest.
 */
#ifndef BOOTLOADER_LAYOUT_H
#define BOOTLOADER_LAYOUT_H

#include <stdint.h>
#include "device_memory.h"

#define BL_FLASH_START             DEVICE_FLASH_START
#define BL_FLASH_SIZE              DEVICE_FLASH_SIZE
#define BL_ERASE_PAGE_SIZE         DEVICE_FLASH_PAGE_SIZE
#define BL_PROGRAM_UNIT_SIZE       UINT8_C(8)

#define BL_BOOT_START              UINT32_C(0x08000000)
#define BL_BOOT_SIZE               DEVICE_BOOT_SIZE
#define BL_SLOT_A_START            DEVICE_SLOT_A_START
#define BL_SLOT_B_START            DEVICE_SLOT_B_START
#define BL_SLOT_SIZE               DEVICE_SLOT_SIZE
#define BL_SLOT_IMAGE_MAX          (BL_SLOT_SIZE - BL_ERASE_PAGE_SIZE)
#define BL_SWAP_SCRATCH_START      DEVICE_SCRATCH_START
#define BL_FACTORY_START           DEVICE_FACTORY_START
#define BL_NVM_START               DEVICE_NVM_START
#define BL_NVM_SIZE                DEVICE_NVM_SIZE

#define BL_SLOT_COUNT              2U
#define BL_SWAP_PAGE_COUNT         (BL_SLOT_SIZE / BL_ERASE_PAGE_SIZE)

#endif
