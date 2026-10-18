/**
 * @file factory_identity.h
 * @brief Shared application/bootloader factory ABI; no HAL, heap or Flash writer.
 * Dependencies: factory_identity -> persistent_storage (CRC), canopen_types.
 * CRC detects corruption, not authenticity. Provision only through factory tools.
 * @example FiData data; FiResult result = Fi_Decode(record, sizeof(record), &data);
 */
#ifndef FACTORY_IDENTITY_H
#define FACTORY_IDENTITY_H
#include "canopen_types.h"
#include "device_memory.h"
#define FI_FLASH_ADDRESS DEVICE_FACTORY_START
#define FI_APPLICATION_START UINT32_C(0x08000000)
#define FI_NVM_ADDRESS DEVICE_NVM_START
#define FI_PAGE_SIZE 2048U
#define FI_RECORD_SIZE 128U
#define FI_SCHEMA 1U
#define FI_TEXT_SIZE 12U
typedef enum { FI_OK, FI_EMPTY, FI_CORRUPT, FI_INVALID, FI_IO_ERROR } FiResult;
typedef struct {
    uint32_t vendor_id, product_code, product_revision, serial, hardware_variant;
    uint16_t hardware_major, hardware_minor;
} FiData;
/** Image requirements must come from an authenticated/validated image header. */
typedef struct {
    uint32_t vendor_id, product_code, hardware_variant;
    uint32_t hardware_min, hardware_max;
} FiCompatibility;
bool Fi_Validate(const FiData *data);
/** Always clears output on failure; exact serialized size, not native struct. */
FiResult Fi_Decode(const uint8_t *record, uint32_t size, FiData *data);
/** Factory host tooling only: creates a record, never writes device Flash. */
FiResult Fi_Encode(const FiData *data, uint8_t *record, uint32_t size);
/** Numeric revision: major[31:16], minor[15:0], compared inclusively. */
bool Fi_Compatible(const FiData *data, const FiCompatibility *image);
/**
 * Upper bound only: future bootloader must also exclude its own protected area.
 * Rejects empty/overflowing ranges and any access to factory or journal pages.
 * Check the full erase-page range, not only the firmware payload range.
 */
bool Fi_ApplicationRange(uint32_t address, uint32_t size);
/** Requires FI_TEXT_SIZE bytes; formats validated numeric HW as "major.minor". */
bool Fi_HardwareText(const FiData *data, char *text, uint32_t size);
#endif
