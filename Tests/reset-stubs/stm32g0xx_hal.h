#ifndef TEST_RESET_HAL_H
#define TEST_RESET_HAL_H
#include <stdint.h>
typedef struct { volatile uint32_t APBENR1, CSR; } ResetRcc;
typedef struct { volatile uint32_t CR1; } ResetPwr;
typedef struct { volatile uint32_t BKP0R, BKP1R; } ResetTamp;
extern ResetRcc reset_rcc;
extern ResetPwr reset_pwr;
extern ResetTamp reset_tamp;
void resetBarrier(void);
#define RCC (&reset_rcc)
#define PWR (&reset_pwr)
#define TAMP (&reset_tamp)
#define __DSB() resetBarrier()
#define RCC_APBENR1_PWREN (1UL << 28U)
#define RCC_APBENR1_RTCAPBEN (1UL << 10U)
#define PWR_CR1_DBP (1UL << 8U)
#define RCC_CSR_RMVF (1UL << 23U)
#define RCC_CSR_OBLRSTF (1UL << 25U)
#define RCC_CSR_PINRSTF (1UL << 26U)
#define RCC_CSR_PWRRSTF (1UL << 27U)
#define RCC_CSR_SFTRSTF (1UL << 28U)
#define RCC_CSR_IWDGRSTF (1UL << 29U)
#define RCC_CSR_WWDGRSTF (1UL << 30U)
#define RCC_CSR_LPWRRSTF (1UL << 31U)
#endif
