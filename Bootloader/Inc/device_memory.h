/**
 * @file device_memory.h
 * @brief Shared STM32G0B1RCT6 memory contract and bank-local erase planning.
 * Keep identical to the application's copy; no HAL dependency.
 */
#ifndef DEVICE_MEMORY_H
#define DEVICE_MEMORY_H

#include <stdbool.h>
#include <stdint.h>

#define DEVICE_FLASH_START       UINT32_C(0x08000000)
#define DEVICE_FLASH_SIZE        UINT32_C(0x00040000)
#define DEVICE_FLASH_PAGE_SIZE   UINT32_C(0x00000800)
#define DEVICE_RAM_START         UINT32_C(0x20000000)
#define DEVICE_RAM_END           UINT32_C(0x20024000)
#define DEVICE_BOOT_SIZE         UINT32_C(0x00009000)
#define DEVICE_BOOT_NODE_ID      126U
#define DEVICE_BOOT_CONTRACT     UINT32_C(0x08008FE0)
#define DEVICE_SLOT_A_START      UINT32_C(0x08009000)
#define DEVICE_SLOT_B_START      UINT32_C(0x08023800)
#define DEVICE_SLOT_SIZE         UINT32_C(0x0001A800)
#define DEVICE_IMAGE_MAX         (DEVICE_SLOT_SIZE - DEVICE_FLASH_PAGE_SIZE)
#define DEVICE_SCRATCH_START     UINT32_C(0x0803E000)
#define DEVICE_FACTORY_START     UINT32_C(0x0803E800)
#define DEVICE_NVM_START         UINT32_C(0x0803F000)
#define DEVICE_NVM_SIZE          UINT32_C(0x00001000)
#define DEVICE_SWAP_STATE_COUNT  ((DEVICE_SLOT_SIZE / DEVICE_FLASH_PAGE_SIZE) * 3U)

typedef struct {
    uint32_t bank;       /**< Logical bank number, 1 or 2; no bank swap supported. */
    uint32_t page;
    uint32_t pages;
} DeviceEraseSpan;

/** Plan the next erase operation without crossing a physical bank boundary. */
static inline bool DeviceMemory_EraseSpan(uint32_t offset, uint32_t remaining,
                                          uint32_t bank_count, DeviceEraseSpan *span)
{
    uint32_t bank_size;
    uint32_t bank_offset;
    uint32_t count;
    if ((span == (DeviceEraseSpan *)0) ||
        ((bank_count != 1U) && (bank_count != 2U)) ||
        (remaining == 0U) || (offset >= DEVICE_FLASH_SIZE) ||
        (remaining > (DEVICE_FLASH_SIZE - offset)) ||
        ((offset % DEVICE_FLASH_PAGE_SIZE) != 0U) ||
        ((remaining % DEVICE_FLASH_PAGE_SIZE) != 0U)) {
        return false;
    }
    bank_size = DEVICE_FLASH_SIZE / bank_count;
    bank_offset = offset % bank_size;
    count = bank_size - bank_offset;
    if (count > remaining) { count = remaining; }
    span->bank = offset / bank_size + 1U;
    span->page = bank_offset / DEVICE_FLASH_PAGE_SIZE;
    span->pages = count / DEVICE_FLASH_PAGE_SIZE;
    return true;
}

#endif
