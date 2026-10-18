/**
 * @file canopen_nmt.h
 * @brief CiA 301 NMT slave state and command handling.
 */
#ifndef CANOPEN_NMT_H
#define CANOPEN_NMT_H

#include "canopen_types.h"

typedef enum {
    CO_NMT_INITIALIZING = 0U,
    CO_NMT_STOPPED = 4U,
    CO_NMT_OPERATIONAL = 5U,
    CO_NMT_PRE_OPERATIONAL = 127U
} CoNmtState;

typedef struct {
    uint8_t node_id;
    CoNmtState state;
    bool reset_requested;
    bool reset_communication_requested;
} CoNmt;

void CoNmt_Init(CoNmt *nmt, uint8_t node_id);
bool CoNmt_Receive(CoNmt *nmt, const CoFrame *frame);
void CoNmt_ResetCommunication(CoNmt *nmt);

#endif
