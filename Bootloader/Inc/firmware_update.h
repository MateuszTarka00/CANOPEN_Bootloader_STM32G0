/**
 * @file firmware_update.h
 * @brief Fixed-memory, sequential image receiver for a separate CANopen bootloader.
 * @details Dependency diagram: firmware_update -> bootloader_interface -> factory_identity.
 *          SDO integration is intentionally a caller responsibility; never used by the app.
 * @example FirmwareUpdate_Begin(&update, &manifest, now); // after service authorization
 */
#ifndef FIRMWARE_UPDATE_H
#define FIRMWARE_UPDATE_H

#include "bootloader_interface.h"

typedef enum {
    FWU_IDLE = 0,
    FWU_RECEIVING,
    FWU_VERIFIED,
    FWU_PENDING,
    FWU_FAILED
} FirmwareUpdateState;

typedef enum {
    FWU_RESULT_NONE = 0,
    FWU_RESULT_INVALID_ARGUMENT,
    FWU_RESULT_INVALID_STATE,
    FWU_RESULT_INCOMPATIBLE_IMAGE,
    FWU_RESULT_ERASE_FAILED,
    FWU_RESULT_SIZE,
    FWU_RESULT_WRITE_FAILED,
    FWU_RESULT_TIMEOUT,
    FWU_RESULT_CRC_MISMATCH,
    FWU_RESULT_READBACK_FAILED,
    FWU_RESULT_IMAGE_REJECTED,
    FWU_RESULT_PENDING_FAILED,
    FWU_RESULT_CANCELLED
} FirmwareUpdateResult;

typedef struct {
    BootloaderInterface *bootloader;
    BlImageManifest manifest;
    FirmwareUpdateState state;
    FirmwareUpdateResult result;
    uint32_t received;
    uint32_t stream_crc;
    uint32_t last_activity;
    uint32_t timeout_ms;
    uint8_t target_slot;
    uint8_t buffered;
    uint8_t program_buffer[BL_MAX_PROGRAM_UNIT];
} FirmwareUpdate;

/** Initialize a transfer context with a non-zero, wrap-safe timeout. */
bool FirmwareUpdate_Init(FirmwareUpdate *update, BootloaderInterface *bootloader,
                         uint32_t timeout_ms);

/** Authenticate/validate manifest before erasing the inactive slot. */
FirmwareUpdateResult FirmwareUpdate_Begin(FirmwareUpdate *update,
                                           const BlImageManifest *manifest,
                                           uint32_t now);

/** Accept the next sequential bytes; caller maps non-NONE results to an SDO abort. */
FirmwareUpdateResult FirmwareUpdate_Write(FirmwareUpdate *update,
                                           const uint8_t *data, uint32_t size,
                                           uint32_t now);

/** Verify transfer CRC and Flash readback, validate image, then commit pending state. */
FirmwareUpdateResult FirmwareUpdate_Finish(FirmwareUpdate *update, uint32_t now);

/** Explicitly abandon an in-progress transfer; active image is never erased. */
FirmwareUpdateResult FirmwareUpdate_Cancel(FirmwareUpdate *update);

/** Abort an inactive transfer on communication timeout. */
FirmwareUpdateResult FirmwareUpdate_Poll(FirmwareUpdate *update, uint32_t now);

#endif
