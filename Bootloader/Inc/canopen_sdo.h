/**
 * @file canopen_sdo.h
 * @brief Bounded CiA 301 expedited, segmented and block-download SDO server.
 */
#ifndef CANOPEN_SDO_H
#define CANOPEN_SDO_H

#include "canopen_types.h"

#define CO_SDO_BUFFER_SIZE 160U

typedef enum {
    CO_SDO_IDLE = 0U,
    CO_SDO_UPLOAD,
    CO_SDO_DOWNLOAD,
    CO_SDO_STREAM_DOWNLOAD,
    CO_SDO_BLOCK_DOWNLOAD,
    CO_SDO_BLOCK_END
} CoSdoState;

typedef struct {
    void *user;
    uint32_t (*read)(void *user, uint16_t index, uint8_t subindex,
                     uint8_t *data, uint32_t *size);
    uint32_t (*check_download)(void *user, uint16_t index, uint8_t subindex,
                               uint32_t size, bool size_known);
    uint32_t (*write)(void *user, uint16_t index, uint8_t subindex,
                      const uint8_t *data, uint32_t size, uint32_t now);
    uint32_t (*stream_begin)(void *user, uint16_t index, uint8_t subindex,
                             uint32_t size, uint32_t now);
    uint32_t (*stream_write)(void *user, uint16_t index, uint8_t subindex,
                             const uint8_t *data, uint32_t size, uint32_t now);
    uint32_t (*stream_end)(void *user, uint16_t index, uint8_t subindex,
                           uint32_t size, uint32_t now);
    void (*stream_abort)(void *user, uint16_t index, uint8_t subindex);
} CoSdoOps;

typedef struct {
    CoSdoOps ops;
    CoSdoState state;
    uint8_t node_id;
    uint8_t toggle;
    uint16_t index;
    uint8_t subindex;
    uint32_t size;
    uint32_t offset;
    uint32_t last_activity;
    uint32_t timeout_ms;
    uint16_t block_crc;
    uint8_t block_tail_size;
    bool block_crc_enabled;
    bool block_stream;
    uint8_t buffer[CO_SDO_BUFFER_SIZE];
} CoSdo;

void CoSdo_Init(CoSdo *sdo, uint8_t node_id, const CoSdoOps *ops);
void CoSdo_Reset(CoSdo *sdo);
bool CoSdo_Receive(CoSdo *sdo, const CoFrame *request, uint32_t now,
                   CoFrame *reply);
bool CoSdo_Poll(CoSdo *sdo, uint32_t now, CoFrame *reply);

#endif
