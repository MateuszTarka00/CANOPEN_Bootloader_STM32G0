/**
 * @file object_dictionary.c
 * @brief Explicit little-endian OD access and vendor update profile v1.
 */
#include "object_dictionary.h"
#include "bootloader_layout.h"
#include <string.h>

#define BOOT_PROFILE_VERSION 1U
#define BOOT_HANDOFF_VERSION 2U
#define BOOT_EXECUTION_MODE 1U
#define BOOT_SOFTWARE_VERSION UINT32_C(0x00010000)
#define BOOT_HEARTBEAT_DEFAULT 1000U

static uint32_t writeNumber(uint8_t *data, uint32_t *size, uint32_t value,
                            uint32_t width)
{
    if ((data == NULL) || (size == NULL)) {
        return CO_ABORT_HARDWARE;
    }
    if (width == 1U) {
        data[0] = (uint8_t)value;
    } else if (width == 2U) {
        Co_Put16(data, (uint16_t)value);
    } else {
        Co_Put32(data, value);
    }
    *size = width;
    return 0U;
}

static uint8_t publicUpdateState(const BootOd *od)
{
    if (od->persistent->update_state == BL_TX_RECEIVING) {
        return 1U;
    }
    if (od->persistent->update_state == BL_TX_VERIFIED) {
        return 2U;
    }
    if ((od->persistent->update_state >= BL_TX_SWAP_FORWARD_BASE) &&
        (od->persistent->update_state < BL_TX_SWAP_ROLLBACK_BASE)) {
        return 3U;
    }
    if ((od->persistent->update_state >= BL_TX_SWAP_ROLLBACK_BASE) &&
        (od->persistent->update_state < (BL_TX_SWAP_ROLLBACK_BASE + 0x200U))) {
        return 5U;
    }
    if (od->persistent->update_state == BL_TX_TRIAL) {
        return 4U;
    }
    return (od->persistent->update_state == BL_TX_IDLE) ? 0U : 0xFFU;
}

void BootOd_Init(BootOd *od, BootloaderInterface *bootloader,
                 FirmwareUpdate *update, BlTransaction *transaction,
                 PsStore *store, PsData *persistent, const FiData *factory,
                 CoEmcy *emcy, uint8_t node_id, bool update_auth_available)
{
    memset(od, 0, sizeof(*od));
    od->bootloader = bootloader;
    od->update = update;
    od->transaction = transaction;
    od->store = store;
    od->persistent = persistent;
    od->emcy = emcy;
    od->node_id = node_id;
    od->update_auth_available = update_auth_available;
    od->factory_valid = Fi_Validate(factory);
    if (od->factory_valid) {
        od->factory = *factory;
    }
}

CoSdoOps BootOd_SdoOps(BootOd *od)
{
    CoSdoOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.user = od;
    ops.read = BootOd_Read;
    ops.check_download = BootOd_CheckDownload;
    ops.write = BootOd_Write;
    ops.stream_begin = BootOd_StreamBegin;
    ops.stream_write = BootOd_StreamWrite;
    ops.stream_end = BootOd_StreamEnd;
    ops.stream_abort = BootOd_StreamAbort;
    return ops;
}

uint32_t BootOd_Read(void *user, uint16_t index, uint8_t subindex,
                     uint8_t *data, uint32_t *size)
{
    BootOd *od = (BootOd *)user;
    const char *text = NULL;
    uint32_t value = 0U;
    uint32_t width = 4U;
    if ((od == NULL) || (data == NULL) || (size == NULL)) {
        return CO_ABORT_HARDWARE;
    }
    *size = 0U;
    switch (index) {
    case 0x1000U:
        value = 0U;
        break;
    case 0x1001U:
        value = (od->emcy != NULL) ? od->emcy->error_register : 0U;
        width = 1U;
        break;
    case 0x1003U:
        if (subindex == 0U) {
            value = (od->emcy != NULL) ? od->emcy->count : 0U;
            width = 1U;
        } else if ((od->emcy != NULL) && (subindex <= od->emcy->count)) {
            value = od->emcy->history[subindex - 1U];
            width = 4U;
        } else {
            return CO_ABORT_SUBINDEX;
        }
        break;
    case 0x1008U:
        text = "CANopen Bootloader";
        break;
    case 0x1009U:
        text = "1.0";
        break;
    case 0x100AU:
        text = "1.0.0";
        break;
    case 0x1014U:
        value = 0x80U + od->node_id;
        break;
    case 0x1017U:
        value = od->persistent->heartbeat_ms;
        width = 2U;
        break;
    case 0x1018U:
        if (subindex == 0U) {
            value = 4U;
            width = 1U;
        } else if (!od->factory_valid) {
            return CO_ABORT_HARDWARE;
        } else if (subindex == 1U) {
            value = od->factory.vendor_id;
        } else if (subindex == 2U) {
            value = od->factory.product_code;
        } else if (subindex == 3U) {
            value = od->factory.product_revision;
        } else if (subindex == 4U) {
            value = od->factory.serial;
        } else {
            return CO_ABORT_SUBINDEX;
        }
        break;
    case 0x1200U:
        if (subindex == 0U) {
            value = 2U;
            width = 1U;
        } else if (subindex == 1U) {
            value = 0x600U + od->node_id;
        } else if (subindex == 2U) {
            value = 0x580U + od->node_id;
        } else {
            return CO_ABORT_SUBINDEX;
        }
        break;
    case 0x2001U:
        value = publicUpdateState(od);
        width = 1U;
        break;
    case 0x2003U:
        value = od->persistent->firmware_crc;
        break;
    case 0x2004U:
        value = od->persistent->firmware_size;
        break;
    case 0x2005U:
        value = od->persistent->pending_version;
        break;
    case 0x2006U:
        value = od->persistent->active_version;
        break;
    case 0x2007U:
        value = od->persistent->update_result;
        break;
    case 0x2008U:
        if (!od->manifest_valid) {
            return CO_ABORT_NOT_FOUND;
        }
        memcpy(data, od->manifest_record, sizeof(od->manifest_record));
        *size = sizeof(od->manifest_record);
        return 0U;
    case 0x2009U:
        value = BOOT_HANDOFF_VERSION;
        break;
    case 0x200AU:
        value = BOOT_EXECUTION_MODE;
        width = 1U;
        break;
    case 0x200BU:
        value = (od->factory_valid && od->update_auth_available) ? 1U : 0U;
        width = 1U;
        break;
    case 0x200CU:
        value = BOOT_PROFILE_VERSION;
        width = 1U;
        break;
    case 0x200DU:
        value = BOOT_SOFTWARE_VERSION;
        break;
    case 0x200EU:
        value = od->persistent->active_slot;
        width = 1U;
        break;
    case 0x200FU:
        value = BL_SLOT_IMAGE_MAX;
        break;
    default:
        return (index == 0x2000U || index == 0x2002U) ?
            CO_ABORT_WRITE_ONLY : CO_ABORT_NOT_FOUND;
    }
    if ((subindex != 0U) && (index != 0x1003U) &&
        (index != 0x1018U) && (index != 0x1200U)) {
        return CO_ABORT_SUBINDEX;
    }
    if (text != NULL) {
        *size = (uint32_t)strlen(text);
        memcpy(data, text, *size);
        return 0U;
    }
    return writeNumber(data, size, value, width);
}

uint32_t BootOd_CheckDownload(void *user, uint16_t index, uint8_t subindex,
                              uint32_t size, bool size_known)
{
    BootOd *od = (BootOd *)user;
    uint32_t expected;
    if ((od == NULL) || !size_known) {
        return CO_ABORT_LENGTH;
    }
    if (subindex != 0U) {
        return CO_ABORT_SUBINDEX;
    }
    switch (index) {
    case 0x1003U:
        expected = 1U;
        break;
    case 0x1017U:
        expected = 2U;
        break;
    case 0x2000U:
        expected = 1U;
        break;
    case 0x2002U:
        if ((od->update == NULL) || (od->update->state != FWU_RECEIVING) ||
            (size == 0U) || (size != od->manifest.image_size) ||
            (size > BL_SLOT_IMAGE_MAX)) {
            return CO_ABORT_STATE;
        }
        return 0U;
    case 0x2008U:
        expected = BL_MANIFEST_RECORD_SIZE;
        break;
    case 0x1000U: case 0x1001U: case 0x1008U: case 0x1009U: case 0x100AU:
    case 0x1014U: case 0x1018U: case 0x1200U:
    case 0x2001U: case 0x2003U: case 0x2004U: case 0x2005U:
    case 0x2006U: case 0x2007U: case 0x2009U: case 0x200AU:
    case 0x200BU: case 0x200CU: case 0x200DU: case 0x200EU: case 0x200FU:
        return CO_ABORT_READ_ONLY;
    default:
        return CO_ABORT_NOT_FOUND;
    }
    return (size == expected) ? 0U : CO_ABORT_LENGTH;
}

static uint32_t persistError(BootOd *od, uint32_t result)
{
    od->persistent->update_result = result;
    if (Ps_Save(od->store, od->persistent) != PS_OK) {
        return CO_ABORT_HARDWARE;
    }
    return 0U;
}

uint32_t BootOd_Write(void *user, uint16_t index, uint8_t subindex,
                      const uint8_t *data, uint32_t size, uint32_t now)
{
    BootOd *od = (BootOd *)user;
    uint32_t abort_code;
    if ((od == NULL) || (data == NULL)) {
        return CO_ABORT_HARDWARE;
    }
    abort_code = BootOd_CheckDownload(od, index, subindex, size, true);
    if (abort_code != 0U) {
        return abort_code;
    }
    if (index == 0x1003U) {
        CoFrame reset_frame;
        if (data[0] != 0U) {
            return CO_ABORT_VALUE;
        }
        CoEmcy_Reset(od->emcy, &reset_frame);
        return 0U;
    }
    if (index == 0x1017U) {
        PsData candidate = *od->persistent;
        candidate.heartbeat_ms = Co_Get16(data);
        if (Ps_Save(od->store, &candidate) != PS_OK) {
            return CO_ABORT_HARDWARE;
        }
        *od->persistent = candidate;
        return 0U;
    }
    if (index == 0x2002U) {
        return BootOd_StreamWrite(user, index, subindex, data, size, now);
    }
    if (index == 0x2008U) {
        BlImageManifest manifest;
        if ((od->update->state == FWU_RECEIVING) ||
            !BlManifest_Decode(data, size, &manifest)) {
            return CO_ABORT_VALUE;
        }
        od->manifest = manifest;
        memcpy(od->manifest_record, data, size);
        od->manifest_valid = true;
        return 0U;
    }
    if (index == 0x2000U) {
        uint8_t command = data[0];
        if ((command == 1U) && od->manifest_valid &&
            ((od->persistent->update_state == BL_TX_IDLE) ||
             (od->persistent->update_state == BL_TX_RECEIVING)) &&
            od->factory_valid && od->update_auth_available) {
            PsData candidate = *od->persistent;
            candidate.update_state = BL_TX_RECEIVING;
            candidate.pending_slot = 1U;
            candidate.pending_version = od->manifest.version;
            candidate.firmware_size = od->manifest.image_size;
            candidate.firmware_crc = od->manifest.image_crc;
            candidate.update_result = 0U;
            if (Ps_Save(od->store, &candidate) != PS_OK) {
                return CO_ABORT_HARDWARE;
            }
            *od->persistent = candidate;
            switch (FirmwareUpdate_Begin(od->update, &od->manifest, now)) {
            case FWU_RESULT_NONE:
                return 0U;
            default:
                if (persistError(od, (uint32_t)od->update->result) != 0U) {
                    return CO_ABORT_HARDWARE;
                }
                return (od->update->result == FWU_RESULT_INCOMPATIBLE_IMAGE) ?
                    CO_ABORT_VALUE : CO_ABORT_HARDWARE;
            }
        }
        if ((command == 2U) && (od->update->state == FWU_RECEIVING)) {
            FirmwareUpdateResult result = FirmwareUpdate_Finish(od->update, now);
            return (result == FWU_RESULT_NONE) ? 0U : CO_ABORT_HARDWARE;
        }
        if ((command == 3U) &&
            (od->persistent->update_state == BL_TX_VERIFIED) &&
            BlTransaction_ScheduleActivation(od->transaction)) {
            return 0U;
        }
        if (command == 0U) {
            if (od->update->state == FWU_RECEIVING) {
                (void)FirmwareUpdate_Cancel(od->update);
            }
            if ((od->persistent->update_state == BL_TX_RECEIVING) ||
                (od->persistent->update_state == BL_TX_VERIFIED)) {
                PsData candidate = *od->persistent;
                candidate.update_state = BL_TX_IDLE;
                candidate.pending_slot = UINT32_MAX;
                candidate.pending_version = 0U;
                candidate.update_result = FWU_RESULT_CANCELLED;
                candidate.boot_request = 0U;
                candidate.boot_node_id = 0U;
                candidate.boot_bitrate = 0U;
                candidate.boot_protocol = 0U;
                if (Ps_Save(od->store, &candidate) != PS_OK) {
                    return CO_ABORT_HARDWARE;
                }
                *od->persistent = candidate;
                od->manifest_valid = false;
                return 0U;
            }
        }
        return CO_ABORT_STATE;
    }
    return CO_ABORT_NOT_FOUND;
}

uint32_t BootOd_StreamBegin(void *user, uint16_t index, uint8_t subindex,
                            uint32_t size, uint32_t now)
{
    (void)now;
    return BootOd_CheckDownload(user, index, subindex, size, true);
}

uint32_t BootOd_StreamWrite(void *user, uint16_t index, uint8_t subindex,
                            const uint8_t *data, uint32_t size, uint32_t now)
{
    BootOd *od = (BootOd *)user;
    if ((index != 0x2002U) || (subindex != 0U) || (od == NULL)) {
        return CO_ABORT_ACCESS;
    }
    switch (FirmwareUpdate_Write(od->update, data, size, now)) {
    case FWU_RESULT_NONE:
        return 0U;
    case FWU_RESULT_TIMEOUT:
        return CO_ABORT_TIMEOUT;
    case FWU_RESULT_SIZE:
        return CO_ABORT_LENGTH;
    case FWU_RESULT_WRITE_FAILED:
        return CO_ABORT_HARDWARE;
    default:
        return CO_ABORT_STATE;
    }
}

uint32_t BootOd_StreamEnd(void *user, uint16_t index, uint8_t subindex,
                          uint32_t size, uint32_t now)
{
    BootOd *od = (BootOd *)user;
    (void)now;
    if ((od == NULL) || (index != 0x2002U) || (subindex != 0U) ||
        (od->update->received != size)) {
        return CO_ABORT_LENGTH;
    }
    return 0U;
}

void BootOd_StreamAbort(void *user, uint16_t index, uint8_t subindex)
{
    BootOd *od = (BootOd *)user;
    if ((od != NULL) && (index == 0x2002U) && (subindex == 0U) &&
        (od->update->state == FWU_RECEIVING)) {
        (void)FirmwareUpdate_Cancel(od->update);
        if ((od->persistent->update_state == BL_TX_RECEIVING) &&
            (od->store != NULL)) {
            PsData candidate = *od->persistent;
            candidate.update_state = BL_TX_IDLE;
            candidate.pending_slot = UINT32_MAX;
            candidate.pending_version = 0U;
            candidate.update_result = FWU_RESULT_CANCELLED;
            candidate.boot_request = 0U;
            candidate.boot_node_id = 0U;
            candidate.boot_bitrate = 0U;
            candidate.boot_protocol = 0U;
            if (Ps_Save(od->store, &candidate) == PS_OK) {
                *od->persistent = candidate;
            }
        }
    }
}
