/**
 * @file boot_reset_request.h
 * @brief One-shot software-reset handoff in otherwise unused TAMP BKP0R/BKP1R.
 * Dependencies: STM32 HAL register definitions only. No RTC clock source or Flash IO.
 * Keep identical in application and bootloader. Single owner; caller resets after Set.
 */
#ifndef BOOT_RESET_REQUEST_H
#define BOOT_RESET_REQUEST_H
#include "stm32g0xx_hal.h"
#include <stdbool.h>
#include <stdint.h>

#define BOOT_RESET_UPDATE 1U
#define BOOT_RESET_DISCOVERY 2U
#define BOOT_RESET_MAGIC UINT32_C(0x42525100)
#define BOOT_RESET_CAPABILITY 4U

static inline bool BootReset_Open(void)
{
    uint32_t attempts = 1024U;
    RCC->APBENR1 |= RCC_APBENR1_PWREN | RCC_APBENR1_RTCAPBEN;
    (void)RCC->APBENR1;
    PWR->CR1 |= PWR_CR1_DBP;
    while ((PWR->CR1 & PWR_CR1_DBP) == 0U) {
        if (--attempts == 0U) { return false; }
    }
    return true;
}

static inline void BootReset_Close(uint32_t previous_dbp)
{
    if (previous_dbp == 0U) { PWR->CR1 &= ~PWR_CR1_DBP; }
}

static inline bool BootReset_Clear(void)
{
    bool cleared;
    uint32_t previous_dbp;
    RCC->APBENR1 |= RCC_APBENR1_PWREN;
    (void)RCC->APBENR1;
    previous_dbp = PWR->CR1 & PWR_CR1_DBP;
    if (!BootReset_Open()) { return false; }
    TAMP->BKP0R = 0U;
    TAMP->BKP1R = 0U;
    __DSB();
    cleared = (TAMP->BKP0R == 0U) && (TAMP->BKP1R == 0U);
    BootReset_Close(previous_dbp);
    return cleared;
}

static inline bool BootReset_Set(uint32_t mode)
{
    uint32_t token, previous_dbp;
    bool written;
    if ((mode != BOOT_RESET_UPDATE) && (mode != BOOT_RESET_DISCOVERY)) {
        return false;
    }
    RCC->APBENR1 |= RCC_APBENR1_PWREN;
    (void)RCC->APBENR1;
    previous_dbp = PWR->CR1 & PWR_CR1_DBP;
    if (!BootReset_Open()) { return false; }
    token = BOOT_RESET_MAGIC | mode;
    TAMP->BKP0R = 0U;
    TAMP->BKP1R = ~token;
    __DSB();
    TAMP->BKP0R = token;
    __DSB();
    written = (TAMP->BKP0R == token) && (TAMP->BKP1R == ~token);
    BootReset_Close(previous_dbp);
    return written;
}

/** Consume before boot decisions on every startup, including cold/watchdog resets. */
static inline bool BootReset_Take(uint32_t *mode)
{
    uint32_t token, inverse, flags, previous_dbp;
    bool cleared;
    const uint32_t other_resets = RCC_CSR_PWRRSTF | RCC_CSR_IWDGRSTF |
        RCC_CSR_WWDGRSTF | RCC_CSR_LPWRRSTF | RCC_CSR_OBLRSTF;
    *mode = 0U;
    RCC->APBENR1 |= RCC_APBENR1_PWREN;
    (void)RCC->APBENR1;
    previous_dbp = PWR->CR1 & PWR_CR1_DBP;
    if (!BootReset_Open()) { return false; }
    token = TAMP->BKP0R;
    inverse = TAMP->BKP1R;
    flags = RCC->CSR;
    cleared = BootReset_Clear();
    BootReset_Close(previous_dbp);
    if (!cleared) { return false; }
    if (((flags & RCC_CSR_SFTRSTF) != 0U) &&
        ((flags & other_resets) == 0U) && (inverse == ~token) &&
        ((token == (BOOT_RESET_MAGIC | BOOT_RESET_UPDATE)) ||
         (token == (BOOT_RESET_MAGIC | BOOT_RESET_DISCOVERY)))) {
        *mode = token & 0xFFU;
    }
    RCC->CSR |= RCC_CSR_RMVF;
    return true;
}
#endif
