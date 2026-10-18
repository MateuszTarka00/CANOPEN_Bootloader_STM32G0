/**
 * @file bootloader_main.c
 * @brief Cooperative bootloader application and reset-time A/B reconciliation.
 */
#include "bootloader_main.h"
#include "bootloader_layout.h"
#include "canopen.h"
#include "firmware_update.h"
#include "main.h"
#include "stm32_boot_platform.h"
#include <string.h>

#define BL_UPDATE_TIMEOUT_MS UINT32_C(15000)
#define BL_LOOP_DELAY_MS 1U

typedef struct {
    BlStm32Context platform;
    BootloaderInterface interface;
    FirmwareUpdate update;
    BlTransaction transaction;
    Canopen canopen;
    bool interface_valid;
    bool update_valid;
    bool canopen_valid;
    bool trial_decision_done;
} BlRuntime;

static BlRuntime runtime;

static bool initializeCore(void)
{
    BlLayout layout;
    BlPlatform platform;
    uint8_t active_slot = 0U;
    uint8_t pending_slot = BL_NO_SLOT;
    uint8_t rollback_slot = BL_NO_SLOT;
    if (!BlStm32_ContextInit(&runtime.platform)) {
        return false;
    }
    if (runtime.platform.storage_valid &&
        (runtime.platform.persistent.active_slot <= 1U)) {
        active_slot = (uint8_t)runtime.platform.persistent.active_slot;
    }
    if (runtime.platform.storage_valid &&
        (runtime.platform.persistent.pending_slot <= 1U) &&
        (runtime.platform.persistent.update_state != BL_TX_RECEIVING)) {
        pending_slot = (uint8_t)runtime.platform.persistent.pending_slot;
    }
    layout = BlStm32_Layout();
    platform = BlStm32_Platform(&runtime.platform);
    if (runtime.platform.factory_valid && runtime.platform.storage_valid &&
        BootloaderInterface_Init(&runtime.interface, &layout, &platform,
                                 &runtime.platform.factory, active_slot,
                                 pending_slot, rollback_slot)) {
        runtime.interface_valid = true;
        if (FirmwareUpdate_Init(&runtime.update, &runtime.interface,
                                BL_UPDATE_TIMEOUT_MS)) {
            runtime.update_valid = true;
        }
    }
    BlTransaction_Init(&runtime.transaction, &runtime.interface,
                       &runtime.platform.store, &runtime.platform.persistent);
    runtime.platform.transaction = &runtime.transaction;
    return true;
}

static bool applicationValid(void)
{
    if (!runtime.interface_valid ||
        !runtime.platform.storage_valid ||
        !runtime.platform.factory_valid) {
        return false;
    }
    return runtime.interface.platform.validate_application(
        runtime.interface.platform.user, 0U);
}

static bool bootApplication(void)
{
    if (!applicationValid()) {
        return false;
    }
    return BootloaderInterface_JumpToApplication(&runtime.interface);
}

static bool runTrialDecision(void)
{
    bool requested_after_confirmation = runtime.platform.handoff_requested &&
        (runtime.platform.persistent.update_state == BL_TX_CONFIRMED);
    bool may_boot = BlTransaction_OnStartup(&runtime.transaction);
    if (!may_boot || requested_after_confirmation) {
        return false;
    }
    if (applicationValid() && bootApplication()) {
        return true;
    }
    if (runtime.platform.persistent.update_state == BL_TX_TRIAL) {
        (void)BlTransaction_OnStartup(&runtime.transaction);
    }
    return false;
}

static void prepareOdManifest(void)
{
    BlImageManifest manifest;
    uint32_t address;
    if (!runtime.platform.storage_valid ||
        (runtime.platform.persistent.update_state != BL_TX_VERIFIED)) {
        return;
    }
    address = BL_SLOT_B_START + BL_SLOT_SIZE - BL_ERASE_PAGE_SIZE;
    if (BlManifest_Decode((const uint8_t *)(uintptr_t)address,
                          BL_MANIFEST_RECORD_SIZE, &manifest)) {
        runtime.canopen.od.manifest = manifest;
        memcpy(runtime.canopen.od.manifest_record,
               (const void *)(uintptr_t)address,
               BL_MANIFEST_RECORD_SIZE);
        runtime.canopen.od.manifest_valid = true;
    }
}

static void emitStartupError(uint16_t code, uint32_t detail)
{
    CoFrame frame;
    CoEmcy_Raise(&runtime.canopen.emcy, code, 1U, detail, &frame);
    runtime.canopen.pending_tx = frame;
    runtime.canopen.tx_pending = true;
}

void Bootloader_Main(void)
{
    bool app_ok;
    uint8_t node_id;
    uint32_t bitrate;
    uint32_t now;
    CoFrame received;

    if (!initializeCore()) {
        Error_Handler();
    }
    node_id = runtime.platform.node_id;
    bitrate = runtime.platform.bitrate;
    app_ok = applicationValid();

    if (runtime.platform.storage_valid &&
        (runtime.platform.persistent.update_state == BL_TX_TRIAL ||
         runtime.platform.persistent.update_state == BL_TX_CONFIRMED)) {
        runtime.trial_decision_done = true;
        if (runTrialDecision()) {
            Error_Handler();
        }
    } else if (!runtime.platform.handoff_requested && app_ok &&
               (runtime.platform.persistent.update_state == BL_TX_IDLE) &&
               bootApplication()) {
        Error_Handler();
    }

    if (!BlStm32_FdcanInit(node_id, bitrate)) {
        Error_Handler();
    }
    runtime.canopen_valid = Co_Init(
        &runtime.canopen, node_id,
        runtime.platform.storage_valid ?
            runtime.platform.persistent.heartbeat_ms : 1000U,
        BlStm32_FdcanSend, NULL, &runtime.interface, &runtime.update,
        &runtime.transaction, &runtime.platform.store,
        &runtime.platform.persistent,
        runtime.platform.factory_valid ? &runtime.platform.factory : NULL,
        runtime.platform.factory_valid && runtime.update_valid &&
            BlStm32_UpdateAuthAvailable(),
        HAL_GetTick());
    if (!runtime.canopen_valid) {
        Error_Handler();
    }
    prepareOdManifest();
    if (!runtime.platform.storage_valid) {
        emitStartupError(0x5000U, (uint32_t)PS_CORRUPT);
    } else if (!runtime.platform.factory_valid) {
        emitStartupError(0x5000U, (uint32_t)FI_CORRUPT);
    } else if (!app_ok) {
        emitStartupError(0x5001U, UINT32_C(0x494D4147));
    }

    for (;;) {
        now = HAL_GetTick();
        if (BlStm32_FdcanReceive(&received)) {
            (void)Co_Receive(&runtime.canopen, &received, now);
        }
        Co_Poll(&runtime.canopen, now);
        if (runtime.canopen.nmt.reset_requested) {
            BlStm32_SystemReset();
        }
        if (runtime.platform.storage_valid &&
            BlTransaction_IsBusy(&runtime.transaction)) {
            (void)BlTransaction_Poll(&runtime.transaction);
            if (!BlTransaction_IsBusy(&runtime.transaction) &&
                (runtime.platform.persistent.update_state == BL_TX_IDLE) &&
                applicationValid()) {
                BlStm32_SystemReset();
            }
        }
        if (runtime.platform.storage_valid &&
            (runtime.platform.persistent.update_state == BL_TX_TRIAL) &&
            !runtime.trial_decision_done) {
            runtime.trial_decision_done = true;
            if (runTrialDecision()) {
                BlStm32_SystemReset();
            }
        }
        if (runtime.platform.storage_valid &&
            (runtime.platform.persistent.update_state == BL_TX_IDLE) &&
            (runtime.update.state == FWU_PENDING)) {
            runtime.update.state = FWU_IDLE;
        }
        HAL_Delay(BL_LOOP_DELAY_MS);
    }
}
