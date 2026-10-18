/**
 * @file canopen_types.h
 * @brief Portable CANopen wire types. Dependencies: stdint, stdbool, stddef.
 * @example CoFrame frame = {0}; frame.id = 0x601U; frame.length = 8U;
 */
#ifndef CANOPEN_TYPES_H
#define CANOPEN_TYPES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CO_ABORT_TOGGLE       UINT32_C(0x05030000)
#define CO_ABORT_TIMEOUT      UINT32_C(0x05040000)
#define CO_ABORT_COMMAND      UINT32_C(0x05040001)
#define CO_ABORT_SEQUENCE     UINT32_C(0x05040003)
#define CO_ABORT_CRC          UINT32_C(0x05040004)
#define CO_ABORT_ACCESS       UINT32_C(0x06010000)
#define CO_ABORT_WRITE_ONLY   UINT32_C(0x06010001)
#define CO_ABORT_READ_ONLY    UINT32_C(0x06010002)
#define CO_ABORT_NOT_FOUND    UINT32_C(0x06020000)
#define CO_ABORT_HARDWARE     UINT32_C(0x06060000)
#define CO_ABORT_LENGTH       UINT32_C(0x06070010)
#define CO_ABORT_SUBINDEX     UINT32_C(0x06090011)
#define CO_ABORT_VALUE        UINT32_C(0x06090030)
#define CO_ABORT_STATE        UINT32_C(0x08000022)
#define CO_ABORT_NO_RESOURCE  UINT32_C(0x05040005)
#define CO_ABORT_GENERAL      UINT32_C(0x08000000)

typedef struct {
    uint16_t id;
    uint8_t length;
    uint8_t data[8];
} CoFrame;

/** Called from the owner task only. false means NOT accepted by the driver. */
typedef bool (*CoSend)(void *user, const CoFrame *frame);

static inline uint16_t Co_Get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));
}
static inline uint32_t Co_Get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}
static inline void Co_Put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8U);
}
static inline void Co_Put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8U);
    p[2] = (uint8_t)(v >> 16U); p[3] = (uint8_t)(v >> 24U);
}
#endif
