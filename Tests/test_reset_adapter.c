/* Production application callbacks with only the physical register addresses substituted. */
#include "boot_reset_request.h"
#include "device_memory.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
ResetRcc reset_rcc;
ResetPwr reset_pwr;
ResetTamp reset_tamp;
static uint32_t test_contract[8], test_vectors[2], resets;
#undef DEVICE_BOOT_CONTRACT
#define DEVICE_BOOT_CONTRACT ((uintptr_t)test_contract)
void resetBarrier(void) {}
static void NVIC_SystemReset(void)
{
    assert((RCC->CSR & RCC_CSR_RMVF) != 0U);
    assert(TAMP->BKP0R == (BOOT_RESET_MAGIC | BOOT_RESET_DISCOVERY));
    ++resets;
}
#include "reset_adapter_bodies.inc"
int main(void)
{
    uint32_t i, flags, mode;
    static const uint32_t valid[8] = {
        UINT32_C(0x424C4331), 2U, DEVICE_SLOT_A_START, DEVICE_SLOT_SIZE,
        DEVICE_FACTORY_START, DEVICE_NVM_START, DEVICE_BOOT_SIZE, 7U
    };
    for (i = 0U; i < 8U; ++i) { test_contract[i] = valid[i]; }
    test_vectors[0] = UINT32_C(0x20024000);
    test_vectors[1] = UINT32_C(0x08000101);
    for (flags = 0U; flags < 16U; ++flags) {
        test_contract[7] = flags;
        assert(bootInstalled(NULL) == ((flags & 5U) == 5U));
        assert(discoveryInstalled(NULL) == ((flags & 7U) == 7U));
    }
    test_contract[7] = 7U;
    for (i = 0U; i < 7U; ++i) {
        test_contract[i] ^= 1U;
        assert(!bootInstalled(NULL));
        test_contract[i] = valid[i];
    }
    test_vectors[0] += 8U; assert(!bootInstalled(NULL));
    test_vectors[0] = UINT32_C(0x20024000);
    test_vectors[1] &= ~1U; assert(!bootInstalled(NULL));
    test_vectors[1] = UINT32_C(0x08008FE1); assert(!bootInstalled(NULL));
    assert(prepareReset(NULL, BOOT_RESET_DISCOVERY));
    resetForUpdate(NULL); assert(resets == 1U);
    RCC->CSR = RCC_CSR_SFTRSTF;
    assert(BootReset_Take(&mode) && mode == BOOT_RESET_DISCOVERY);
    assert(clearReset(NULL) && TAMP->BKP0R == 0U && TAMP->BKP1R == 0U);
    puts("application reset adapter: old bootloader rejected, capability/layout/vector gates, reset flags OK");
    return 0;
}
