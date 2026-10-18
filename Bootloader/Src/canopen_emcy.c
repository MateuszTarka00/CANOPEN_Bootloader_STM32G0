/**
 * @file canopen_emcy.c
 * @brief CiA 301 emergency message generation.
 */
#include "canopen_emcy.h"
#include <string.h>

void CoEmcy_Init(CoEmcy *emcy, uint8_t node_id)
{
    memset(emcy, 0, sizeof(*emcy));
    emcy->node_id = node_id;
}

void CoEmcy_Raise(CoEmcy *emcy, uint16_t code, uint8_t error_register,
                  uint32_t manufacturer_data, CoFrame *frame)
{
    uint8_t i;
    if ((emcy == NULL) || (frame == NULL)) {
        return;
    }
    if (emcy->count < CO_EMCY_HISTORY_SIZE) {
        emcy->history[emcy->count++] = code;
    } else {
        for (i = 1U; i < CO_EMCY_HISTORY_SIZE; ++i) {
            emcy->history[i - 1U] = emcy->history[i];
        }
        emcy->history[CO_EMCY_HISTORY_SIZE - 1U] = code;
    }
    emcy->error_register = error_register;
    memset(frame, 0, sizeof(*frame));
    frame->id = (uint16_t)(0x080U + emcy->node_id);
    frame->length = 8U;
    Co_Put16(&frame->data[0], code);
    frame->data[2] = error_register;
    Co_Put32(&frame->data[3], manufacturer_data);
}

void CoEmcy_Reset(CoEmcy *emcy, CoFrame *frame)
{
    if ((emcy == NULL) || (frame == NULL)) {
        return;
    }
    memset(emcy->history, 0, sizeof(emcy->history));
    emcy->count = 0U;
    emcy->error_register = 0U;
    memset(frame, 0, sizeof(*frame));
    frame->id = (uint16_t)(0x080U + emcy->node_id);
    frame->length = 8U;
}
