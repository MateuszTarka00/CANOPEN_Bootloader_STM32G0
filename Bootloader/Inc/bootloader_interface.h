/**
 * @file bootloader_interface.h
 * @brief Portable, bounded interface between the bootloader core and Flash/platform code.
 * @details Dependency diagram: bootloader_interface -> factory_identity / persistent_storage.
 *          The STM32 application does not include this module. A separate bootloader target
 *          must provide the callbacks and a measured, validated flash layout.
 * @example BootloaderInterface_Init(&bl, &layout, &ops, &identity, 0U, BL_NO_SLOT, BL_NO_SLOT);
 */
#ifndef BOOTLOADER_INTERFACE_H
#define BOOTLOADER_INTERFACE_H

#include "factory_identity.h"

#define BL_NO_SLOT UINT8_C(0xFF)
#define BL_MAX_SIGNATURE_SIZE 64U
#define BL_MAX_PROGRAM_UNIT 16U
#define BL_CRC_READ_SIZE 32U
#define BL_IMAGE_DIGEST_SIZE 32U

typedef struct {
    uint32_t address;
    uint32_t size;
} BlRange;

typedef struct {
    uint32_t flash_start;
    uint32_t flash_size;
    BlRange bootloader;
    BlRange slot[2];
    BlRange scratch;
    BlRange factory;
    BlRange journal;
    uint32_t erase_unit;
    uint8_t program_unit;
} BlLayout;

/**
 * @brief Signed image metadata supplied by the update client.
 * @details Signature interpretation and cryptographic verification are platform policy.
 *          CRC-32 is for accidental-corruption detection, not authenticity.
 */
typedef struct {
    uint32_t image_size;
    uint32_t image_crc;
    uint32_t version;
    FiCompatibility compatibility;
    uint8_t image_digest[BL_IMAGE_DIGEST_SIZE];
    uint8_t signature_size;
    uint8_t signature[BL_MAX_SIGNATURE_SIZE];
} BlImageManifest;

/**
 * @brief Hardware/persistent operations supplied by a separate bootloader target.
 * @details Addresses passed to read/erase/program have already been range-checked.
 *          Callbacks must be bounded, synchronous, and report every hardware failure.
 */
typedef struct {
    void *user;
    bool (*read)(void *user, uint32_t address, uint8_t *data, uint32_t size);
    bool (*erase)(void *user, uint32_t address, uint32_t size);
    bool (*program)(void *user, uint32_t address, const uint8_t *data, uint32_t size);
    bool (*verify_manifest)(void *user, const BlImageManifest *manifest);
    bool (*validate_programmed_image)(void *user, uint8_t slot,
                                      const BlImageManifest *manifest);
    /** Validate a stored active/pending image using its durable authenticated metadata. */
    bool (*validate_application)(void *user, uint8_t slot);
    /** Write explicit, versioned metadata transactionally; never persist a raw C struct. */
    bool (*mark_pending)(void *user, uint8_t slot, const BlImageManifest *manifest);
    bool (*activate)(void *user, uint8_t slot);
    bool (*rollback)(void *user, uint8_t slot);
    bool (*jump)(void *user, uint32_t vector_address);
} BlPlatform;

typedef struct {
    BlLayout layout;
    BlPlatform platform;
    FiData factory;
    uint8_t active_slot;
    uint8_t pending_slot;
    uint8_t rollback_slot;
    uint8_t write_slot;
    uint32_t write_limit;
    uint32_t written;
    BlImageManifest write_manifest;
    bool initialized;
    bool recovery_required;
} BootloaderInterface;

/** Initialize only if every memory range and mandatory platform callback is valid. */
bool BootloaderInterface_Init(BootloaderInterface *bl, const BlLayout *layout,
                              const BlPlatform *platform, const FiData *factory,
                              uint8_t active_slot, uint8_t pending_slot,
                              uint8_t rollback_slot);

/** Check bounds, hardware compatibility and the mandatory manifest-authentication callback. */
bool BootloaderInterface_ValidateFirmware(BootloaderInterface *bl, uint8_t slot,
                                           const BlImageManifest *manifest);

/** Authenticate, authorize and erase an inactive slot before any image writes. */
bool BootloaderInterface_PrepareFirmware(BootloaderInterface *bl, uint8_t slot,
                                          const BlImageManifest *manifest);

/** Erase only the manifest-authorized inactive slot in its configured page range. */
bool BootloaderInterface_EraseArea(BootloaderInterface *bl, uint8_t slot);

/** Revoke authorization after cancellation, timeout or any transfer failure. */
void BootloaderInterface_AbortFirmware(BootloaderInterface *bl, uint8_t slot);

/** Program aligned complete units in the inactive slot; size must be a unit multiple. */
bool BootloaderInterface_WriteChunk(BootloaderInterface *bl, uint8_t slot,
                                     uint32_t offset, const uint8_t *data,
                                     uint32_t size);

/** Calculate CRC-32/ISO-HDLC over a bounded slot-relative image range. */
bool BootloaderInterface_CalculateCRC(BootloaderInterface *bl, uint8_t slot,
                                       uint32_t size, uint32_t *crc);

/** Calculate SHA-256 over a bounded slot-relative image range. */
bool BootloaderInterface_CalculateSHA256(BootloaderInterface *bl, uint8_t slot,
                                          uint32_t size, uint8_t digest[32]);

/** Persist a pending selection only after the caller has fully verified the image. */
bool BootloaderInterface_MarkPendingUpdate(BootloaderInterface *bl, uint8_t slot,
                                            const BlImageManifest *manifest);

/** Activate the previously persisted pending image after validating it again. */
bool BootloaderInterface_ActivateFirmware(BootloaderInterface *bl, uint8_t slot);

/** Restore the previously confirmed slot through the platform's durable rollback hook. */
bool BootloaderInterface_RollbackFirmware(BootloaderInterface *bl);

/** Jump only to the active slot after platform validation; never performs a raw vector jump. */
bool BootloaderInterface_JumpToApplication(BootloaderInterface *bl);

#endif
