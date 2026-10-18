/**
 * @file persistent_storage.h
 * @brief Power-fail-safe configuration/update journal. Dependencies: canopen_types.
 * @details Two independently erasable pages, one committed record per page.
 * Caller owns/serializes the context. No heap or HAL dependencies.
 * @example PsData data; PsResult result = Ps_Load(&store, &data);
 */
#ifndef PERSISTENT_STORAGE_H
#define PERSISTENT_STORAGE_H
#include "canopen_types.h"
#define PS_RECORD_SIZE 256U
#define PS_STRING_SIZE 32U

typedef enum {
    PS_OK, PS_EMPTY, PS_CORRUPT, PS_IO_ERROR, PS_INVALID
} PsResult;

typedef struct {
    /** Legacy schema 1/2 fields retained for wire compatibility, NOT OD identity. */
    char device_name[PS_STRING_SIZE];
    char hardware_version[PS_STRING_SIZE];
    char software_version[PS_STRING_SIZE];
    uint32_t vendor_id, product_code, revision, serial;
    /** Journal image metadata, not the running application's version source. */
    uint32_t firmware_crc, firmware_size, pending_version, active_version;
    uint32_t update_state, update_result, active_slot, pending_slot;
    uint16_t heartbeat_ms;
    /** Schema 2 handoff contract. Zero request means normal boot. */
    uint32_t boot_request, boot_node_id, boot_bitrate, boot_protocol;
} PsData;

typedef struct {
    void *user;
    bool (*read)(void *user, uint8_t page, uint32_t offset, uint8_t *data, uint32_t size);
    bool (*erase)(void *user, uint8_t page);
    bool (*write)(void *user, uint8_t page, uint32_t offset,
                  const uint8_t *data, uint32_t size);
} PsIo;

typedef struct {
    PsIo io;
    uint32_t sequence;
    uint8_t active_page;
    bool loaded;
} PsStore;

/** CRC-32/ISO-HDLC: reflected 0xEDB88320, init/final xor FFFFFFFF. */
uint32_t Ps_CrcUpdate(uint32_t crc, const uint8_t *data, uint32_t size);
uint32_t Ps_Crc(const uint8_t *data, uint32_t size);
bool Ps_Validate(const PsData *data);
void Ps_Init(PsStore *store, const PsIo *io);
PsResult Ps_Load(PsStore *store, PsData *data);
/** Requires a prior load; never erases the currently committed page. */
PsResult Ps_Save(PsStore *store, const PsData *data);
#endif
