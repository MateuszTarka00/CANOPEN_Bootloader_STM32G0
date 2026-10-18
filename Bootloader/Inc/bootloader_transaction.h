/**
 * @file bootloader_transaction.h
 * @brief Power-fail-resumable A/B page swap and trial/rollback state machine.
 */
#ifndef BOOTLOADER_TRANSACTION_H
#define BOOTLOADER_TRANSACTION_H

#include "bootloader_interface.h"
#include "persistent_storage.h"

#define BL_TX_IDLE UINT32_C(0)
#define BL_TX_RECEIVING UINT32_C(1)
#define BL_TX_VERIFIED UINT32_C(2)
#define BL_TX_SWAP_FORWARD_BASE UINT32_C(0x100)
#define BL_TX_SWAP_ROLLBACK_BASE UINT32_C(0x300)
#define BL_TX_TRIAL UINT32_C(0x500)
#define BL_TX_CONFIRMED UINT32_C(0x501)
#define BL_TX_PHASES_PER_PAGE 3U

typedef struct {
    BootloaderInterface *bootloader;
    PsStore *store;
    PsData *data;
    uint8_t page_buffer[2048];
    bool failed;
} BlTransaction;

void BlTransaction_Init(BlTransaction *transaction,
                        BootloaderInterface *bootloader,
                        PsStore *store, PsData *data);
bool BlTransaction_ScheduleActivation(BlTransaction *transaction);
bool BlTransaction_ConfirmTrial(PsStore *store, PsData *data);
/** Reconcile durable trial/confirmation state once, immediately after reset. */
bool BlTransaction_OnStartup(BlTransaction *transaction);
/** Performs one durable page-swap phase and returns true while work remains. */
bool BlTransaction_Poll(BlTransaction *transaction);
bool BlTransaction_IsBusy(const BlTransaction *transaction);

#endif
