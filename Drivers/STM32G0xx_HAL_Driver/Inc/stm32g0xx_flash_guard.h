/* Local synchronous Flash containment. A failure is cleared only by reset. */
#ifndef STM32G0XX_FLASH_GUARD_H
#define STM32G0XX_FLASH_GUARD_H

typedef struct
{
  HAL_StatusTypeDef status;
  uint32_t sr;
  uint32_t stage;
} FLASH_GuardState;

extern FLASH_GuardState FLASH_Guard;
uint32_t HAL_GetTick(void);

static inline HAL_StatusTypeDef FLASH_GuardFailure(HAL_StatusTypeDef status)
{
  if ((status != HAL_OK) && (FLASH_Guard.status == HAL_OK))
  {
    FLASH_Guard.status = status;
    FLASH_Guard.sr = FLASH->SR;
    FLASH_Guard.stage = 0U;
  }
  return status;
}

static inline HAL_StatusTypeDef FLASH_GuardReady(uint32_t stage, uint32_t timeout)
{
  uint32_t start = HAL_GetTick();
  uint32_t spins = 1000000U;
  uint32_t sr;
  if (FLASH_Guard.status != HAL_OK)
  {
    return FLASH_Guard.status;
  }
  for (;;)
  {
    sr = FLASH->SR;
    if ((sr & FLASH_SR_ERRORS) != 0U)
    {
      FLASH_Guard.status = HAL_ERROR;
      break;
    }
    if ((sr & (FLASH_SR_BSY1 | FLASH_SR_CFGBSY
#if defined(FLASH_DBANK_SUPPORT)
               | FLASH_SR_BSY2
#endif
              )) == 0U)
    {
      return HAL_OK;
    }
    /* The iteration bound also contains a stopped HAL timebase. */
    if (((uint32_t)(HAL_GetTick() - start) >= timeout) || (--spins == 0U))
    {
      FLASH_Guard.status = HAL_TIMEOUT;
      break;
    }
  }
  FLASH_Guard.sr = sr;
  FLASH_Guard.stage = stage;
  pFlash.ErrorCode = sr & FLASH_SR_ERRORS;
  return FLASH_Guard.status;
}

static inline HAL_StatusTypeDef FLASH_GuardCleanup(HAL_StatusTypeDef status,
                                                  uint32_t bits)
{
  /* In particular, never clear PER/PG after a failed wait. */
  if (status == HAL_OK)
  {
    status = FLASH_GuardReady(4U, FLASH_TIMEOUT_VALUE);
    if (status == HAL_OK)
    {
      CLEAR_BIT(FLASH->CR, bits);
    }
  }
  return FLASH_GuardFailure(status);
}

#endif
