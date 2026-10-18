/**
 * @file canopen_emcy.h
 * @brief CiA 301 EMCY producer and bounded error register.
 */
#ifndef CANOPEN_EMCY_H
#define CANOPEN_EMCY_H

#include "canopen_types.h"

#define CO_EMCY_HISTORY_SIZE 8U

typedef struct {
    uint16_t history[CO_EMCY_HISTORY_SIZE];
    uint8_t count;
    uint8_t error_register;
    uint8_t node_id;
} CoEmcy;

void CoEmcy_Init(CoEmcy *emcy, uint8_t node_id);
void CoEmcy_Raise(CoEmcy *emcy, uint16_t code, uint8_t error_register,
                  uint32_t manufacturer_data, CoFrame *frame);
void CoEmcy_Reset(CoEmcy *emcy, CoFrame *frame);

#endif
