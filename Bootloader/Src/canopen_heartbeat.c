/**
 * @file canopen_heartbeat.c
 * @brief CiA 301 boot-up and heartbeat frames.
 */
#include "canopen_heartbeat.h"
#include <string.h>

void CoHeartbeat_Init(CoHeartbeat *heartbeat, uint8_t node_id,
                      uint16_t period_ms, uint32_t now)
{
    memset(heartbeat, 0, sizeof(*heartbeat));
    heartbeat->node_id = node_id;
    heartbeat->period_ms = period_ms;
    heartbeat->last_sent = now;
}

bool CoHeartbeat_BootUp(const CoHeartbeat *heartbeat, CoFrame *frame)
{
    if ((heartbeat == NULL) || (frame == NULL)) {
        return false;
    }
    memset(frame, 0, sizeof(*frame));
    frame->id = (uint16_t)(0x700U + heartbeat->node_id);
    frame->length = 1U;
    frame->data[0] = 0U;
    return true;
}

bool CoHeartbeat_Poll(CoHeartbeat *heartbeat, CoNmtState state,
                      uint32_t now, CoFrame *frame)
{
    if ((heartbeat == NULL) || (frame == NULL) ||
        (heartbeat->period_ms == 0U) ||
        ((now - heartbeat->last_sent) < heartbeat->period_ms)) {
        return false;
    }
    memset(frame, 0, sizeof(*frame));
    frame->id = (uint16_t)(0x700U + heartbeat->node_id);
    frame->length = 1U;
    frame->data[0] = (uint8_t)state;
    heartbeat->last_sent = now;
    return true;
}
