/**
 * @file canopen_heartbeat.h
 * @brief CiA 301 heartbeat producer.
 */
#ifndef CANOPEN_HEARTBEAT_H
#define CANOPEN_HEARTBEAT_H

#include "canopen_types.h"
#include "canopen_nmt.h"

typedef struct {
    uint8_t node_id;
    uint16_t period_ms;
    uint32_t last_sent;
} CoHeartbeat;

void CoHeartbeat_Init(CoHeartbeat *heartbeat, uint8_t node_id,
                      uint16_t period_ms, uint32_t now);
bool CoHeartbeat_BootUp(const CoHeartbeat *heartbeat, CoFrame *frame);
bool CoHeartbeat_Poll(CoHeartbeat *heartbeat, CoNmtState state,
                      uint32_t now, CoFrame *frame);

#endif
