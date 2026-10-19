/* Production initialization bodies with observable HAL substitutes. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

typedef enum { HAL_OK, HAL_ERROR } HAL_StatusTypeDef;
typedef struct { void *Instance; } FDCAN_HandleTypeDef;
typedef struct { void *Instance; } TIM_HandleTypeDef;
typedef struct {
    uint32_t Pin, Mode, Pull, Speed, Alternate;
} GPIO_InitTypeDef;
typedef struct {
    uint32_t IdType, FilterIndex, FilterType, FilterConfig, FilterID1, FilterID2;
} FDCAN_FilterTypeDef;
typedef struct {
    uint32_t OscillatorType, HSEState, LSIState;
    struct { uint32_t PLLState, PLLSource, PLLM, PLLN, PLLP, PLLQ, PLLR; } PLL;
} RCC_OscInitTypeDef;
typedef struct {
    uint32_t ClockType, SYSCLKSource, AHBCLKDivider, APB1CLKDivider;
} RCC_ClkInitTypeDef;
typedef struct { uint32_t PeriphClockSelection, FdcanClockSelection; } RCC_PeriphCLKInitTypeDef;

static uint32_t can1, can2, gpio, timer1;
#define FDCAN1 (&can1)
#define FDCAN2 (&can2)
#define GPIOC (&gpio)
#define GPIO_PIN_2 (1U << 2)
#define GPIO_PIN_3 (1U << 3)
#define GPIO_PIN_4 (1U << 4)
#define GPIO_PIN_5 (1U << 5)
#define GPIO_MODE_AF_PP 2U
#define GPIO_PULLUP 1U
#define GPIO_SPEED_FREQ_VERY_HIGH 3U
#define GPIO_AF3_FDCAN1 3U
#define GPIO_AF3_FDCAN2 3U
#define BL_CAN_SUPPORTED_BITRATE 250000U
#define FDCAN_STANDARD_ID 0U
#define FDCAN_FILTER_MASK 2U
#define FDCAN_FILTER_TO_RXFIFO0 1U
#define FDCAN_REJECT 2U
#define FDCAN_REJECT_REMOTE 1U
#define FDCAN_IT_RX_FIFO0_NEW_MESSAGE 1U
#define TIM16_FDCAN_IT0_IRQn 21U
#define TIM17_FDCAN_IT1_IRQn 22U
#define PWR_REGULATOR_VOLTAGE_SCALE1 1U
#define RCC_OSCILLATORTYPE_LSI 1U
#define RCC_OSCILLATORTYPE_HSE 2U
#define RCC_HSE_ON 1U
#define RCC_LSI_ON 1U
#define RCC_PLL_ON 1U
#define RCC_PLLSOURCE_HSE 1U
#define RCC_PLLM_DIV1 1U
#define RCC_PLLP_DIV2 2U
#define RCC_PLLQ_DIV2 2U
#define RCC_PLLR_DIV2 2U
#define RCC_CLOCKTYPE_HCLK 1U
#define RCC_CLOCKTYPE_SYSCLK 2U
#define RCC_CLOCKTYPE_PCLK1 4U
#define RCC_SYSCLKSOURCE_PLLCLK 2U
#define RCC_SYSCLK_DIV1 1U
#define RCC_HCLK_DIV1 1U
#define FLASH_LATENCY_2 2U
#define RCC_PERIPHCLK_FDCAN 1U
#define RCC_FDCANCLKSOURCE_PCLK1 1U

static FDCAN_HandleTypeDef hfdcan1, hfdcan2;
static TIM_HandleTypeDef htim1;
static uint32_t HAL_RCC_FDCAN_CLK_ENABLED;
static uint32_t can_clocks, gpio_clocks, gpio_inits, irq_enables, notifications;
static uint32_t filters, starts, timer_irqs, oscillator_calls, clock_calls, peripheral_clock_calls;
static uint32_t fail_step;
static HAL_StatusTypeDef voltage_status;
static jmp_buf fatal;
#define __HAL_RCC_FDCAN_CLK_ENABLE() (++can_clocks)
#define __HAL_RCC_GPIOC_CLK_ENABLE() (++gpio_clocks)
void HAL_NVIC_SetPriority(uint32_t irq, uint32_t priority, uint32_t subpriority)
{ (void)irq; (void)priority; (void)subpriority; }
void HAL_NVIC_EnableIRQ(uint32_t irq) { (void)irq; ++irq_enables; }
void HAL_GPIO_Init(void *port, const GPIO_InitTypeDef *init)
{
    assert(port == GPIOC && init->Mode == GPIO_MODE_AF_PP);
    assert(init->Pin == (GPIO_PIN_4 | GPIO_PIN_5) ||
           init->Pin == (GPIO_PIN_2 | GPIO_PIN_3));
    ++gpio_inits;
}
void HAL_FDCAN_MspInit(FDCAN_HandleTypeDef *handle);
void MX_FDCAN1_Init(void)
{
    hfdcan1.Instance = FDCAN1;
    HAL_FDCAN_MspInit(&hfdcan1);
}
HAL_StatusTypeDef HAL_FDCAN_ConfigFilter(FDCAN_HandleTypeDef *handle,
                                        const FDCAN_FilterTypeDef *filter)
{
    assert(handle == &hfdcan1 && handle->Instance == FDCAN1);
    assert(filter->FilterIndex == filters && filter->FilterID2 == 0x7FFU);
    assert(filter->FilterID1 == ((filters == 0U) ? 0U : 0x67EU));
    ++filters;
    return (fail_step == filters) ? HAL_ERROR : HAL_OK;
}
HAL_StatusTypeDef HAL_FDCAN_ConfigGlobalFilter(FDCAN_HandleTypeDef *handle,
                                              uint32_t standard, uint32_t extended,
                                              uint32_t remote_standard, uint32_t remote_extended)
{
    assert(handle == &hfdcan1 && filters == 2U);
    assert(standard == FDCAN_REJECT && extended == FDCAN_REJECT);
    assert(remote_standard == FDCAN_REJECT_REMOTE && remote_extended == FDCAN_REJECT_REMOTE);
    return (fail_step == 3U) ? HAL_ERROR : HAL_OK;
}
HAL_StatusTypeDef HAL_FDCAN_Start(FDCAN_HandleTypeDef *handle)
{
    assert(handle == &hfdcan1);
    ++starts;
    return (fail_step == 4U) ? HAL_ERROR : HAL_OK;
}
HAL_StatusTypeDef HAL_FDCAN_ActivateNotification(FDCAN_HandleTypeDef *handle,
                                               uint32_t events, uint32_t buffers)
{
    (void)handle; (void)events; (void)buffers;
    ++notifications;
    return HAL_OK;
}
void HAL_TIM_IRQHandler(TIM_HandleTypeDef *handle)
{
    assert(handle == &htim1 && handle->Instance == &timer1);
    ++timer_irqs;
}
HAL_StatusTypeDef HAL_PWREx_ControlVoltageScaling(uint32_t scale)
{ assert(scale == PWR_REGULATOR_VOLTAGE_SCALE1); return voltage_status; }
HAL_StatusTypeDef HAL_RCC_OscConfig(const RCC_OscInitTypeDef *osc)
{
    assert(osc->PLL.PLLM == 1U && osc->PLL.PLLN == 16U && osc->PLL.PLLR == 2U);
    ++oscillator_calls;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_RCC_ClockConfig(const RCC_ClkInitTypeDef *clock, uint32_t latency)
{
    assert(clock->AHBCLKDivider == 1U && clock->APB1CLKDivider == 1U);
    assert(latency == FLASH_LATENCY_2);
    ++clock_calls;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_RCCEx_PeriphCLKConfig(const RCC_PeriphCLKInitTypeDef *clock)
{
    assert(clock->FdcanClockSelection == RCC_FDCANCLKSOURCE_PCLK1);
    ++peripheral_clock_calls;
    return HAL_OK;
}
void Error_Handler(void) { longjmp(fatal, 1); }
#include "peripheral_bodies.inc"

static void reset(void)
{
    memset(&hfdcan1, 0, sizeof(hfdcan1));
    memset(&hfdcan2, 0, sizeof(hfdcan2));
    HAL_RCC_FDCAN_CLK_ENABLED = 0U;
    can_clocks = gpio_clocks = gpio_inits = irq_enables = notifications = 0U;
    filters = starts = fail_step = 0U;
}
int main(void)
{
    reset();
    assert(!BlStm32_FdcanInit(0U, 250000U));
    assert(!BlStm32_FdcanInit(128U, 250000U));
    assert(!BlStm32_FdcanInit(126U, 125000U));
    assert(can_clocks == 0U && hfdcan1.Instance == NULL);
    assert(BlStm32_FdcanInit(126U, 250000U));
    assert(hfdcan2.Instance == NULL);
    assert(can_clocks == 1U && gpio_clocks == 1U && gpio_inits == 1U);
    assert(filters == 2U && starts == 1U);
    assert(irq_enables == 0U && notifications == 0U);
    for (uint32_t step = 1U; step <= 4U; ++step) {
        reset();
        fail_step = step;
        assert(!BlStm32_FdcanInit(126U, 250000U));
        assert(hfdcan2.Instance == NULL && irq_enables == 0U && notifications == 0U);
        assert(starts == ((step == 4U) ? 1U : 0U));
    }
    reset();
    hfdcan2.Instance = FDCAN2;
    HAL_FDCAN_MspInit(&hfdcan2);
    assert(irq_enables == 0U);
    htim1.Instance = &timer1;
    TIM1_BRK_UP_TRG_COM_IRQHandler();
    assert(timer_irqs == 1U);
    voltage_status = HAL_ERROR;
    if (setjmp(fatal) == 0) {
        SystemClock_Config();
        assert(!"Voltage-scaling failure must stop clock initialization");
    }
    assert(oscillator_calls == 0U && clock_calls == 0U && peripheral_clock_calls == 0U);
    voltage_status = HAL_OK;
    SystemClock_Config();
    assert(oscillator_calls == 1U && clock_calls == 1U && peripheral_clock_calls == 1U);
    puts("Boot peripherals: polling-only CAN, TIM1 tick, voltage failure and HAL errors passed.");
    return 0;
}
