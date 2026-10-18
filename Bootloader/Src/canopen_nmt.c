/**
 * @file canopen_nmt.c
 * @brief CiA 301 NMT slave; unsupported NMT commands are ignored.
 */
#include "canopen_nmt.h"
#include <string.h>

void CoNmt_Init(CoNmt *nmt, uint8_t node_id)
{
    memset(nmt, 0, sizeof(*nmt));
    nmt->node_id = node_id;
    nmt->state = CO_NMT_PRE_OPERATIONAL;
}

void CoNmt_ResetCommunication(CoNmt *nmt)
{
    if (nmt != NULL) {
        nmt->state = CO_NMT_PRE_OPERATIONAL;
        nmt->reset_requested = false;
        nmt->reset_communication_requested = false;
    }
}

bool CoNmt_Receive(CoNmt *nmt, const CoFrame *frame)
{
    uint8_t command;
    uint8_t target;
    if ((nmt == NULL) || (frame == NULL) || (frame->id != 0U)) {
        return false;
    }
    if (frame->length != 2U) {
        return true;
    }
    command = frame->data[0];
    target = frame->data[1];
    if ((target != 0U) && (target != nmt->node_id)) {
        return false;
    }
    switch (command) {
    case 0x01U:
        nmt->state = CO_NMT_OPERATIONAL;
        break;
    case 0x02U:
        nmt->state = CO_NMT_STOPPED;
        break;
    case 0x80U:
        nmt->state = CO_NMT_PRE_OPERATIONAL;
        break;
    case 0x81U:
        nmt->reset_requested = true;
        break;
    case 0x82U:
        nmt->reset_communication_requested = true;
        break;
    default:
        break;
    }
    return true;
}
