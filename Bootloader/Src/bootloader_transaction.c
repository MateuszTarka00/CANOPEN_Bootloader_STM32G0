/**
 * @file bootloader_transaction.c
 * @brief Restartable fixed-address A/B swap through a dedicated scratch page.
 */
#include "bootloader_transaction.h"
#include "bootloader_layout.h"
#include "firmware_update.h"
#include "persistent_storage.h"
#include <string.h>

static bool saveState(BlTransaction *transaction, uint32_t state)
{
    transaction->data->update_state = state;
    if (Ps_Save(transaction->store, transaction->data) != PS_OK) {
        transaction->failed = true;
        return false;
    }
    return true;
}

static uint32_t swapPageCount(const BlTransaction *transaction)
{
    if ((transaction == NULL) || (transaction->bootloader == NULL) ||
        (transaction->bootloader->layout.erase_unit == 0U)) {
        return 0U;
    }
    return transaction->bootloader->layout.slot[0].size /
        transaction->bootloader->layout.erase_unit;
}

static bool isSwapState(const BlTransaction *transaction, uint32_t state,
                        uint32_t base)
{
    uint32_t page_count = swapPageCount(transaction);
    return (page_count != 0U) &&
        (page_count <= ((UINT32_MAX - base) / BL_TX_PHASES_PER_PAGE)) &&
        (state >= base) &&
        (state < (base + (page_count * BL_TX_PHASES_PER_PAGE)));
}

static bool copyPage(BlTransaction *transaction, uint32_t source,
                     uint32_t destination)
{
    uint32_t offset;
    uint32_t unit = transaction->bootloader->layout.program_unit;
    uint32_t page_size = transaction->bootloader->layout.erase_unit;
    uint32_t source_crc;
    if (!transaction->bootloader->platform.read(
            transaction->bootloader->platform.user, source,
            transaction->page_buffer, page_size) ||
        !transaction->bootloader->platform.erase(
            transaction->bootloader->platform.user, destination,
            page_size)) {
        return false;
    }
    source_crc = Ps_Crc(transaction->page_buffer, page_size);
    for (offset = 0U; offset < page_size; offset += unit) {
        if (!transaction->bootloader->platform.program(
                transaction->bootloader->platform.user, destination + offset,
                &transaction->page_buffer[offset], unit)) {
            return false;
        }
    }
    if (!transaction->bootloader->platform.read(
            transaction->bootloader->platform.user, destination,
            transaction->page_buffer, page_size)) {
        return false;
    }
    return Ps_Crc(transaction->page_buffer, page_size) == source_crc;
}

static bool finalizeForward(BlTransaction *transaction)
{
    transaction->data->active_slot = 0U;
    transaction->data->pending_slot = 1U;
    transaction->data->update_result = 0U;
    transaction->data->boot_request = 0U;
    transaction->data->boot_node_id = 0U;
    transaction->data->boot_bitrate = 0U;
    transaction->data->boot_protocol = 0U;
    return saveState(transaction, BL_TX_TRIAL);
}

static bool finalizeRollback(BlTransaction *transaction)
{
    transaction->data->active_slot = 0U;
    transaction->data->pending_slot = UINT32_MAX;
    transaction->data->pending_version = 0U;
    transaction->data->update_result = (uint32_t)FWU_RESULT_CANCELLED;
    transaction->data->boot_request = 0U;
    transaction->data->boot_node_id = 0U;
    transaction->data->boot_bitrate = 0U;
    transaction->data->boot_protocol = 0U;
    return saveState(transaction, BL_TX_IDLE);
}

void BlTransaction_Init(BlTransaction *transaction,
                        BootloaderInterface *bootloader,
                        PsStore *store, PsData *data)
{
    memset(transaction, 0, sizeof(*transaction));
    transaction->bootloader = bootloader;
    transaction->store = store;
    transaction->data = data;
}

bool BlTransaction_ScheduleActivation(BlTransaction *transaction)
{
    if ((transaction == NULL) || transaction->failed || (transaction->data == NULL) ||
        (transaction->data->update_state != BL_TX_VERIFIED) ||
        (transaction->data->pending_slot != 1U)) {
        return false;
    }
    return saveState(transaction, BL_TX_SWAP_FORWARD_BASE);
}

bool BlTransaction_OnStartup(BlTransaction *transaction)
{
    if ((transaction == NULL) || transaction->failed ||
        (transaction->data == NULL)) {
        return false;
    }
    if (transaction->data->update_state == BL_TX_CONFIRMED) {
        transaction->data->pending_slot = UINT32_MAX;
        transaction->data->pending_version = 0U;
        transaction->data->update_result = 0U;
        return saveState(transaction, BL_TX_IDLE);
    }
    if (transaction->data->update_state != BL_TX_TRIAL) {
        return !BlTransaction_IsBusy(transaction);
    }
    if (transaction->data->update_result == 0U) {
        transaction->data->update_result = 1U;
        return Ps_Save(transaction->store, transaction->data) == PS_OK;
    }
    transaction->data->update_result = 0U;
    (void)saveState(transaction, BL_TX_SWAP_ROLLBACK_BASE);
    return false; /* Do not execute the failed trial while rollback is pending. */
}

bool BlTransaction_ConfirmTrial(PsStore *store, PsData *data)
{
    if ((store == NULL) || (data == NULL) ||
        (data->update_state != BL_TX_TRIAL) ||
        (data->pending_slot != 1U) ||
        (data->pending_version == 0U)) {
        return false;
    }
    data->update_state = BL_TX_CONFIRMED;
    data->active_version = data->pending_version;
    return Ps_Save(store, data) == PS_OK;
}

bool BlTransaction_IsBusy(const BlTransaction *transaction)
{
    if ((transaction == NULL) || (transaction->data == NULL)) {
        return true;
    }
    return isSwapState(transaction, transaction->data->update_state,
                       BL_TX_SWAP_FORWARD_BASE) ||
        isSwapState(transaction, transaction->data->update_state,
                    BL_TX_SWAP_ROLLBACK_BASE);
}

bool BlTransaction_Poll(BlTransaction *transaction)
{
    uint32_t state;
    uint32_t base;
    uint32_t encoded;
    uint32_t page;
    uint32_t phase;
    uint32_t offset;
    uint32_t slot_a;
    uint32_t slot_b;
    uint32_t scratch;
    uint32_t source;
    uint32_t destination;
    if ((transaction == NULL) || transaction->failed ||
        (transaction->data == NULL) ||
        (transaction->bootloader == NULL)) {
        return false;
    }
    state = transaction->data->update_state;
    if (isSwapState(transaction, state, BL_TX_SWAP_FORWARD_BASE)) {
        base = BL_TX_SWAP_FORWARD_BASE;
    } else if (isSwapState(transaction, state, BL_TX_SWAP_ROLLBACK_BASE)) {
        base = BL_TX_SWAP_ROLLBACK_BASE;
    } else {
        return false;
    }
    encoded = state - base;
    page = encoded / BL_TX_PHASES_PER_PAGE;
    phase = encoded % BL_TX_PHASES_PER_PAGE;
    if ((page >= swapPageCount(transaction)) ||
        (transaction->bootloader->layout.erase_unit !=
         sizeof(transaction->page_buffer))) {
        transaction->failed = true;
        return false;
    }
    offset = page * transaction->bootloader->layout.erase_unit;
    slot_a = transaction->bootloader->layout.slot[0].address + offset;
    slot_b = transaction->bootloader->layout.slot[1].address + offset;
    scratch = transaction->bootloader->layout.scratch.address;
    if (phase == 0U) {
        source = slot_a;
        destination = scratch;
    } else if (phase == 1U) {
        source = slot_b;
        destination = slot_a;
    } else {
        source = scratch;
        destination = slot_b;
    }
    if (!copyPage(transaction, source, destination)) {
        transaction->failed = true;
        return false;
    }
    if (phase < 2U) {
        if (!saveState(transaction, base + encoded + 1U)) {
            return false;
        }
        return true;
    }
    if ((page + 1U) < swapPageCount(transaction)) {
        if (!saveState(transaction, base +
                       ((page + 1U) * BL_TX_PHASES_PER_PAGE))) {
            return false;
        }
        return true;
    }
    return (base == BL_TX_SWAP_FORWARD_BASE) ?
        finalizeForward(transaction) : finalizeRollback(transaction);
}
