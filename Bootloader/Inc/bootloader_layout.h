/**
 * @file bootloader_layout.h
 * @brief Fixed STM32G0B1VETx Flash partition map.
 *
 * The two application slots contain identical-address-linked images. The
 * bootloader swaps complete pages through the scratch page after verification;
 * only slot A is ever executed. The last page of each slot stores its manifest.
 */
#ifndef BOOTLOADER_LAYOUT_H
#define BOOTLOADER_LAYOUT_H

#include <stdint.h>

#define BL_FLASH_START             UINT32_C(0x08000000)
#define BL_FLASH_SIZE              UINT32_C(0x00080000)
#define BL_ERASE_PAGE_SIZE         UINT32_C(0x00000800)
#define BL_PROGRAM_UNIT_SIZE       UINT8_C(8)

#define BL_BOOT_START              UINT32_C(0x08000000)
#define BL_BOOT_SIZE               UINT32_C(0x00009000)
#define BL_SLOT_A_START            UINT32_C(0x08009000)
#define BL_SLOT_B_START            UINT32_C(0x08043800)
#define BL_SLOT_SIZE               UINT32_C(0x0003A800)
#define BL_SLOT_IMAGE_MAX          (BL_SLOT_SIZE - BL_ERASE_PAGE_SIZE)
#define BL_SWAP_SCRATCH_START      UINT32_C(0x0807E000)
#define BL_FACTORY_START           UINT32_C(0x0807E800)
#define BL_NVM_START               UINT32_C(0x0807F000)
#define BL_NVM_SIZE                UINT32_C(0x00001000)

#define BL_SLOT_COUNT              2U
#define BL_SWAP_PAGE_COUNT         (BL_SLOT_SIZE / BL_ERASE_PAGE_SIZE)

#endif
