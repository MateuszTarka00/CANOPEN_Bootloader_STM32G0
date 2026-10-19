/**
 * @file stm32_boot_platform.h
 * @brief STM32G0B1 HAL adapters for Flash, FDCAN, identity and NVM.
 */
#ifndef STM32_BOOT_PLATFORM_H
#define STM32_BOOT_PLATFORM_H

#include "bootloader_interface.h"
#include "bootloader_transaction.h"
#include "persistent_storage.h"
#include "stm32g0xx_hal.h"

typedef struct {
    BlDiagnostics diagnostics;
    PsStore store;
    PsData persistent;
    FiData factory;
    BlTransaction *transaction;
    bool storage_valid;
    bool factory_valid;
    bool handoff_requested;
    uint8_t node_id;
    uint32_t bitrate;
} BlStm32Context;

bool BlStm32_ContextInit(BlStm32Context *context);
BlLayout BlStm32_Layout(void);
BlPlatform BlStm32_Platform(BlStm32Context *context);
bool BlStm32_UpdateAuthAvailable(void);
bool BlStm32_FdcanInit(uint8_t node_id, uint32_t bitrate);
bool BlStm32_FdcanReceive(CoFrame *frame);
bool BlStm32_FdcanSend(void *user, const CoFrame *frame);
void BlStm32_SystemReset(void);

#endif
