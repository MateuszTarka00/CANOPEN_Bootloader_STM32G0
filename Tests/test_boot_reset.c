/* Execute the shared production register protocol against fault-injected registers. */
#include "boot_reset_request.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
ResetRcc reset_rcc;
ResetPwr reset_pwr;
ResetTamp reset_tamp;
static bool fail_write, fail_clear;
void resetBarrier(void)
{
    if (fail_write && (TAMP->BKP0R != 0U)) { TAMP->BKP1R ^= 1U; }
    if (fail_clear && (TAMP->BKP0R == 0U)) { TAMP->BKP0R = UINT32_MAX; }
}
static void setup(void)
{
    memset(&reset_rcc, 0, sizeof(reset_rcc));
    memset(&reset_pwr, 0, sizeof(reset_pwr));
    memset(&reset_tamp, 0, sizeof(reset_tamp));
    fail_write = false; fail_clear = false;
}
int main(void)
{
    uint32_t mode, bit, request, previous;
    static const uint32_t rejected[] = {
        0U, RCC_CSR_PWRRSTF, RCC_CSR_IWDGRSTF, RCC_CSR_WWDGRSTF,
        RCC_CSR_SFTRSTF | RCC_CSR_PWRRSTF,
        RCC_CSR_SFTRSTF | RCC_CSR_IWDGRSTF,
        RCC_CSR_SFTRSTF | RCC_CSR_WWDGRSTF,
        RCC_CSR_SFTRSTF | RCC_CSR_OBLRSTF,
        RCC_CSR_SFTRSTF | RCC_CSR_LPWRRSTF
    };
    for (previous = 0U; previous < 2U; ++previous) {
        for (request = 1U; request <= 2U; ++request) {
            setup(); PWR->CR1 = previous ? PWR_CR1_DBP : 0U;
            assert(BootReset_Set(request));
            assert((PWR->CR1 & PWR_CR1_DBP) == (previous ? PWR_CR1_DBP : 0U));
            assert(TAMP->BKP0R == (BOOT_RESET_MAGIC | request));
            assert(TAMP->BKP1R == ~(BOOT_RESET_MAGIC | request));
            /* Pin reset flag can accompany other reset sources on STM32. */
            RCC->CSR = RCC_CSR_SFTRSTF | RCC_CSR_PINRSTF;
            assert(BootReset_Take(&mode) && mode == request);
            assert(TAMP->BKP0R == 0U && TAMP->BKP1R == 0U);
            assert((PWR->CR1 & PWR_CR1_DBP) == (previous ? PWR_CR1_DBP : 0U));
            assert((RCC->CSR & RCC_CSR_RMVF) != 0U);
            assert(BootReset_Take(&mode) && mode == 0U);
        }
    }
    for (bit = 0U; bit < sizeof(rejected) / sizeof(rejected[0]); ++bit) {
        setup(); assert(BootReset_Set(BOOT_RESET_DISCOVERY));
        RCC->CSR = rejected[bit];
        assert(BootReset_Take(&mode) && mode == 0U);
        assert(TAMP->BKP0R == 0U && TAMP->BKP1R == 0U);
    }
    for (bit = 0U; bit < 64U; ++bit) {
        setup(); assert(BootReset_Set(BOOT_RESET_DISCOVERY));
        if (bit < 32U) { TAMP->BKP0R ^= UINT32_C(1) << bit; }
        else { TAMP->BKP1R ^= UINT32_C(1) << (bit - 32U); }
        RCC->CSR = RCC_CSR_SFTRSTF;
        assert(BootReset_Take(&mode) && mode == 0U);
    }
    setup(); RCC->CSR = RCC_CSR_SFTRSTF;
    TAMP->BKP1R = ~(BOOT_RESET_MAGIC | BOOT_RESET_DISCOVERY);
    assert(BootReset_Take(&mode) && mode == 0U); /* Interrupted before commit. */
    assert(!BootReset_Set(0U) && !BootReset_Set(3U) && !BootReset_Set(UINT32_MAX));
    fail_write = true;
    assert(!BootReset_Set(BOOT_RESET_DISCOVERY));
    fail_write = false; assert(BootReset_Clear());
    assert(BootReset_Set(BOOT_RESET_UPDATE));
    fail_clear = true;
    assert(!BootReset_Clear());
    assert(!BootReset_Take(&mode) && mode == 0U);
    fail_clear = false;
    assert(BootReset_Clear());
    puts("reset marker: one shot, software reset only, 64 bit corruptions, partial/write/clear faults OK");
    return 0;
}
