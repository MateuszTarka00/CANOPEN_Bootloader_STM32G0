/* Actual synchronous HAL bodies, with observable register/primitive substitutes. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef enum { HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT } HAL_StatusTypeDef;
#define FLASH_DBANK_SUPPORT
#define FLASH_SR_BSY1 (1U << 16)
#define FLASH_SR_BSY2 (1U << 17)
#define FLASH_SR_CFGBSY (1U << 18)
#define FLASH_SR_ERRORS 0x3FAU
#define FLASH_SR_CLEAR 0U /* W1C success path: no pending flags in this model. */
#define FLASH_TIMEOUT_VALUE 10U
#define FLASH_CR_PER 2U
#define FLASH_CR_PG 1U
#define FLASH_CR_LOCK (1U << 31)
#define FLASH_TYPEPROGRAM_DOUBLEWORD 1U
#define FLASH_TYPEERASE_PAGES 0U
#define FLASH_TYPEERASE_MASS 4U
#define FLASH_BANK_1 1U
#define FLASH_KEY1 0x45670123U
#define FLASH_KEY2 0xCDEF89ABU
#define HAL_FLASH_ERROR_NONE 0U
#define HAL_UNLOCKED 0U
#define HAL_LOCKED 1U
#define assert_param(x) ((void)0)
static struct { uint32_t SR, CR, KEYR; } registers;
#define FLASH (&registers)
static uint32_t tick, tick_step, injection, controls, keys, programs, erases;
uint32_t HAL_GetTick(void) { uint32_t now = tick; tick += tick_step; return now; }
static void control(uint32_t *reg, uint32_t value)
{
    assert((FLASH->SR & (FLASH_SR_BSY1 | FLASH_SR_BSY2 | FLASH_SR_CFGBSY)) == 0U);
    ++controls;
    *reg = value;
}
#define SET_BIT(reg, bits) control(&(reg), (reg) | (bits))
#define CLEAR_BIT(reg, bits) control(&(reg), (reg) & ~(bits))
#define READ_BIT(reg, bits) ((reg) & (bits))
#define WRITE_REG(reg, value) do { (reg) = (value); ++keys; \
    if ((value) == FLASH_KEY2) { FLASH->CR &= ~FLASH_CR_LOCK; } } while (0)
#define __HAL_LOCK(p) do { if ((p)->Lock == HAL_LOCKED) { return HAL_BUSY; } \
    (p)->Lock = HAL_LOCKED; } while (0)
#define __HAL_UNLOCK(p) ((p)->Lock = HAL_UNLOCKED)
static struct { uint32_t Lock, ErrorCode; } pFlash;
typedef struct { uint32_t TypeErase, Banks, Page, NbPages; } FLASH_EraseInitTypeDef;
#include "stm32g0xx_flash_guard.h"
FLASH_GuardState FLASH_Guard;
static void FLASH_Program_DoubleWord(uint32_t address, uint64_t data)
{
    (void)address; (void)data;
    SET_BIT(FLASH->CR, FLASH_CR_PG);
    ++programs;
    FLASH->SR = injection;
}
static void FLASH_Program_Fast(uint32_t address, uint32_t data)
{ FLASH_Program_DoubleWord(address, data); }
static void FLASH_PageErase(uint32_t bank, uint32_t page)
{
    (void)bank; (void)page;
    SET_BIT(FLASH->CR, FLASH_CR_PER);
    ++erases;
    FLASH->SR = injection;
}
static void FLASH_MassErase(uint32_t bank) { FLASH_PageErase(bank, 0U); }
HAL_StatusTypeDef FLASH_WaitForLastOperation(uint32_t timeout);
#include "flash_hal_bodies.inc"

static void reset(void)
{
    memset(&registers, 0, sizeof(registers));
    memset(&pFlash, 0, sizeof(pFlash));
    memset(&FLASH_Guard, 0, sizeof(FLASH_Guard));
    tick = 0U; tick_step = 1U; injection = 0U;
    controls = keys = programs = erases = 0U;
}
static void sticky(void)
{
    FLASH_EraseInitTypeDef erase = {FLASH_TYPEERASE_PAGES, 1U, 0U, 2U};
    uint32_t error, before = controls, key_before = keys;
    uint32_t program_before = programs, erase_before = erases;
    FLASH_GuardState first = FLASH_Guard;
    FLASH->SR = 0U; /* Even recovery of the hardware must not permit retry. */
    assert(HAL_FLASH_Unlock() != HAL_OK);
    assert(HAL_FLASH_Lock() != HAL_OK);
    assert(HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, 0U, 0U) != HAL_OK);
    assert(HAL_FLASHEx_Erase(&erase, &error) != HAL_OK);
    assert(FLASH_GuardCleanup(HAL_OK, FLASH_CR_PER | FLASH_CR_PG) != HAL_OK);
    assert(controls == before && keys == key_before);
    assert(programs == program_before && erases == erase_before);
    assert(memcmp(&first, &FLASH_Guard, sizeof(first)) == 0);
}
int main(void)
{
    const uint32_t faults[] = {FLASH_SR_CFGBSY, FLASH_SR_BSY1, FLASH_SR_BSY2,
        2U, 2U | FLASH_SR_CFGBSY, FLASH_SR_BSY1 | FLASH_SR_BSY2};
    FLASH_EraseInitTypeDef erase = {FLASH_TYPEERASE_PAGES, 1U, 4U, 2U};
    uint32_t error;
    reset();
    assert(HAL_FLASH_Unlock() == HAL_OK);
    assert(HAL_FLASHEx_Erase(&erase, &error) == HAL_OK && erases == 2U);
    assert((FLASH->CR & FLASH_CR_PER) == 0U);
    assert(HAL_FLASH_Program(1U, 0U, 0U) == HAL_OK && programs == 1U);
    assert((FLASH->CR & FLASH_CR_PG) == 0U);
    assert(HAL_FLASH_Lock() == HAL_OK);
    for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        for (unsigned op = 0; op < 4U; ++op) {
            reset();
            HAL_StatusTypeDef expected = (faults[i] & FLASH_SR_ERRORS) ? HAL_ERROR : HAL_TIMEOUT;
            if (op < 2U) {
                injection = faults[i];
                if (op == 0U) {
                    assert(HAL_FLASHEx_Erase(&erase, &error) == expected);
                    assert(erases == 1U && error == erase.Page && controls == 1U);
                } else {
                    assert(HAL_FLASH_Program(1U, 0U, 0U) == expected);
                    assert(programs == 1U && controls == 1U);
                }
                assert(FLASH_Guard.stage == 3U);
            } else {
                FLASH->SR = faults[i];
                assert((op == 2U ? HAL_FLASH_Unlock() : HAL_FLASH_Lock()) == expected);
                assert(controls == 0U && keys == 0U);
            }
            assert(FLASH_Guard.sr == faults[i]);
            sticky();
        }
        reset();
        FLASH->SR = faults[i];
        assert(FLASH_GuardCleanup(HAL_OK, FLASH_CR_PER) != HAL_OK);
        assert(controls == 0U && FLASH_Guard.stage == 4U);
        for (unsigned op = 0U; op < 2U; ++op) {
            reset();
            FLASH->SR = faults[i];
            if (op == 0U) {
                assert(HAL_FLASHEx_Erase(&erase, &error) != HAL_OK);
            } else {
                assert(HAL_FLASH_Program(1U, 0U, 0U) != HAL_OK);
            }
            assert(controls == 0U && programs == 0U && erases == 0U);
            assert(FLASH_Guard.stage == 2U && FLASH_Guard.sr == faults[i]);
            sticky();
        }
    }
    reset();
    FLASH->CR = FLASH_CR_LOCK;
    assert(HAL_FLASH_Unlock() == HAL_OK && keys == 2U);
    reset();
    assert(FLASH_GuardCleanup(HAL_ERROR, FLASH_CR_PER) == HAL_ERROR);
    assert(controls == 0U);
    sticky();
    reset();
    assert(FLASH_GuardCleanup(HAL_TIMEOUT, FLASH_CR_PG) == HAL_TIMEOUT);
    assert(controls == 0U);
    sticky();
    reset(); tick = UINT32_MAX - 3U;
    FLASH->SR = FLASH_SR_CFGBSY;
    assert(HAL_FLASH_Unlock() == HAL_TIMEOUT);
    reset(); tick_step = 0U;
    FLASH->SR = FLASH_SR_CFGBSY;
    assert(HAL_FLASH_Unlock() == HAL_TIMEOUT); /* stopped timebase */
    puts("Actual HAL Flash containment: normal, error, busy, timeout, sticky passed.");
    return 0;
}
