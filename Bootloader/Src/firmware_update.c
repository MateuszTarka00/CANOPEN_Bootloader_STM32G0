/**
 * @file firmware_update.c
 * @brief Bounded sequential image transfer and pending-image commit.
 * @details Dependency diagram: firmware_update -> bootloader_interface -> factory_identity.
 */
#include "firmware_update.h"
#include "persistent_storage.h"
#include <string.h>

static FirmwareUpdateResult failUpdate(FirmwareUpdate *update,
                                      FirmwareUpdateResult result)
{
    if ((update != NULL) && (update->bootloader != NULL)) {
        BootloaderInterface_AbortFirmware(update->bootloader, update->target_slot);
        update->state = FWU_FAILED;
        update->result = result;
        update->buffered = 0U;
    }
    return result;
}

bool FirmwareUpdate_Init(FirmwareUpdate *update, BootloaderInterface *bootloader,
                         uint32_t timeout_ms)
{
    if ((update == NULL) || (bootloader == NULL) || !bootloader->initialized ||
        (timeout_ms == 0U) || (timeout_ms > (UINT32_MAX >> 1U))) {
        return false;
    }
    memset(update, 0, sizeof(*update));
    update->bootloader = bootloader;
    update->timeout_ms = timeout_ms;
    update->state = FWU_IDLE;
    return true;
}

FirmwareUpdateResult FirmwareUpdate_Begin(FirmwareUpdate *update,
                                           const BlImageManifest *manifest,
                                           uint32_t now)
{
    if ((update == NULL) || (manifest == NULL) || (update->bootloader == NULL)) {
        return FWU_RESULT_INVALID_ARGUMENT;
    }
    if ((update->state != FWU_IDLE) && (update->state != FWU_FAILED)) {
        return FWU_RESULT_INVALID_STATE;
    }
    if (update->bootloader->pending_slot != BL_NO_SLOT) {
        return FWU_RESULT_INVALID_STATE;
    }
    if (update->bootloader->recovery_required) {
        return FWU_RESULT_INVALID_STATE;
    }
    update->result = FWU_RESULT_NONE;
    update->received = 0U;
    update->buffered = 0U;
    update->stream_crc = UINT32_MAX;
    update->target_slot = (uint8_t)(update->bootloader->active_slot ^ 1U);
    BlDiagnosticStage(update->bootloader, 4U);
    if (!BootloaderInterface_ValidateFirmware(update->bootloader,
                                               update->target_slot, manifest)) {
        update->state = FWU_FAILED;
        update->result = FWU_RESULT_INCOMPATIBLE_IMAGE;
        return update->result;
    }
    if (!BootloaderInterface_PrepareFirmware(update->bootloader,
                                              update->target_slot, manifest)) {
        update->state = FWU_FAILED;
        update->result = FWU_RESULT_ERASE_FAILED;
        return update->result;
    }
    update->manifest = *manifest;
    update->last_activity = (update->bootloader->platform.get_time_ms != NULL) ?
        update->bootloader->platform.get_time_ms(
            update->bootloader->platform.user) : now;
    update->state = FWU_RECEIVING;
    BlDiagnosticStage(update->bootloader, 7U);
    return FWU_RESULT_NONE;
}

FirmwareUpdateResult FirmwareUpdate_Write(FirmwareUpdate *update,
                                           const uint8_t *data, uint32_t size,
                                           uint32_t now)
{
    uint32_t i;
    uint8_t unit;
    if ((update == NULL) || (data == NULL) || (size == 0U)) {
        return FWU_RESULT_INVALID_ARGUMENT;
    }
    if ((update->state != FWU_RECEIVING) || (update->bootloader == NULL)) {
        return FWU_RESULT_INVALID_STATE;
    }
    BlDiagnosticStage(update->bootloader, 8U);
    if ((now - update->last_activity) >= update->timeout_ms) {
        return failUpdate(update, FWU_RESULT_TIMEOUT);
    }
    if ((update->received > update->manifest.image_size) ||
        (size > (update->manifest.image_size - update->received))) {
        return failUpdate(update, FWU_RESULT_SIZE);
    }
    update->stream_crc = Ps_CrcUpdate(update->stream_crc, data, size);
    unit = update->bootloader->layout.program_unit;
    for (i = 0U; i < size; ++i) {
        update->program_buffer[update->buffered] = data[i];
        update->buffered++;
        update->received++;
        if (update->buffered == unit) {
            uint32_t offset = update->received - (uint32_t)update->buffered;
            if (!BootloaderInterface_WriteChunk(update->bootloader,
                                                update->target_slot, offset,
                                                update->program_buffer, unit)) {
                return failUpdate(update, FWU_RESULT_WRITE_FAILED);
            }
            update->buffered = 0U;
        }
    }
    update->last_activity = now;
    return FWU_RESULT_NONE;
}

FirmwareUpdateResult FirmwareUpdate_Finish(FirmwareUpdate *update, uint32_t now)
{
    uint8_t unit;
    uint32_t offset;
    uint32_t flash_crc;
    uint32_t stream_crc;
    uint8_t flash_digest[BL_IMAGE_DIGEST_SIZE];
    if ((update == NULL) || (update->bootloader == NULL)) {
        return FWU_RESULT_INVALID_ARGUMENT;
    }
    if (update->state != FWU_RECEIVING) {
        return FWU_RESULT_INVALID_STATE;
    }
    BlDiagnosticStage(update->bootloader, 9U);
    if ((now - update->last_activity) >= update->timeout_ms) {
        return failUpdate(update, FWU_RESULT_TIMEOUT);
    }
    if (update->received != update->manifest.image_size) {
        return failUpdate(update, FWU_RESULT_SIZE);
    }
    stream_crc = update->stream_crc ^ UINT32_MAX;
    if (stream_crc != update->manifest.image_crc) {
        return failUpdate(update, FWU_RESULT_CRC_MISMATCH);
    }
    if (update->buffered != 0U) {
        unit = update->bootloader->layout.program_unit;
        offset = update->received - (uint32_t)update->buffered;
        memset(&update->program_buffer[update->buffered], 0xFF,
               (size_t)(unit - update->buffered));
        if (!BootloaderInterface_WriteChunk(update->bootloader,
                                            update->target_slot, offset,
                                            update->program_buffer, unit)) {
            return failUpdate(update, FWU_RESULT_WRITE_FAILED);
        }
        update->buffered = 0U;
    }
    update->state = FWU_VERIFIED;
    if (!BootloaderInterface_CalculateCRC(update->bootloader,
                                          update->target_slot,
                                          update->manifest.image_size,
                                          &flash_crc)) {
        return failUpdate(update, FWU_RESULT_READBACK_FAILED);
    }
    if (flash_crc != update->manifest.image_crc) {
        return failUpdate(update, FWU_RESULT_CRC_MISMATCH);
    }
    if (!BootloaderInterface_CalculateSHA256(update->bootloader,
                                              update->target_slot,
                                              update->manifest.image_size,
                                              flash_digest)) {
        return failUpdate(update, FWU_RESULT_READBACK_FAILED);
    }
    if (memcmp(flash_digest, update->manifest.image_digest,
               sizeof(flash_digest)) != 0) {
        memset(flash_digest, 0, sizeof(flash_digest));
        return failUpdate(update, FWU_RESULT_IMAGE_REJECTED);
    }
    memset(flash_digest, 0, sizeof(flash_digest));
    if (!update->bootloader->platform.validate_programmed_image(
            update->bootloader->platform.user, update->target_slot,
            &update->manifest)) {
        return failUpdate(update, FWU_RESULT_IMAGE_REJECTED);
    }
    if (!BootloaderInterface_MarkPendingUpdate(update->bootloader,
                                                update->target_slot,
                                                &update->manifest)) {
        return failUpdate(update, FWU_RESULT_PENDING_FAILED);
    }
    update->last_activity = now;
    update->state = FWU_PENDING;
    update->result = FWU_RESULT_NONE;
    BlDiagnosticStage(update->bootloader, 10U);
    return FWU_RESULT_NONE;
}

FirmwareUpdateResult FirmwareUpdate_Cancel(FirmwareUpdate *update)
{
    if (update == NULL) {
        return FWU_RESULT_INVALID_ARGUMENT;
    }
    if (update->state != FWU_RECEIVING) {
        return FWU_RESULT_INVALID_STATE;
    }
    update->state = FWU_IDLE;
    update->result = FWU_RESULT_CANCELLED;
    update->received = 0U;
    update->buffered = 0U;
    BootloaderInterface_AbortFirmware(update->bootloader, update->target_slot);
    return update->result;
}

FirmwareUpdateResult FirmwareUpdate_Poll(FirmwareUpdate *update, uint32_t now)
{
    if (update == NULL) {
        return FWU_RESULT_INVALID_ARGUMENT;
    }
    if ((update->state == FWU_RECEIVING) &&
        ((now - update->last_activity) >= update->timeout_ms)) {
        return failUpdate(update, FWU_RESULT_TIMEOUT);
    }
    return update->result;
}
