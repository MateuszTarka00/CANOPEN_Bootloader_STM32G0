/**
 * @file object_dictionary.h
 * @brief CiA 301 identity and bootloader-specific update OD.
 */
#ifndef OBJECT_DICTIONARY_H
#define OBJECT_DICTIONARY_H

#include "bootloader_transaction.h"
#include "canopen_emcy.h"
#include "canopen_sdo.h"
#include "firmware_manifest.h"
#include "firmware_update.h"

typedef struct {
    BootloaderInterface *bootloader;
    FirmwareUpdate *update;
    BlTransaction *transaction;
    PsStore *store;
    PsData *persistent;
    FiData factory;
    CoEmcy *emcy;
    BlImageManifest manifest;
    uint8_t manifest_record[BL_MANIFEST_RECORD_SIZE];
    uint8_t node_id;
    bool factory_valid;
    bool manifest_valid;
    bool update_auth_available;
} BootOd;

void BootOd_Init(BootOd *od, BootloaderInterface *bootloader,
                 FirmwareUpdate *update, BlTransaction *transaction,
                 PsStore *store, PsData *persistent, const FiData *factory,
                 CoEmcy *emcy, uint8_t node_id, bool update_auth_available);
CoSdoOps BootOd_SdoOps(BootOd *od);
uint32_t BootOd_Read(void *user, uint16_t index, uint8_t subindex,
                     uint8_t *data, uint32_t *size);
uint32_t BootOd_CheckDownload(void *user, uint16_t index, uint8_t subindex,
                              uint32_t size, bool size_known);
uint32_t BootOd_Write(void *user, uint16_t index, uint8_t subindex,
                      const uint8_t *data, uint32_t size, uint32_t now);
uint32_t BootOd_StreamBegin(void *user, uint16_t index, uint8_t subindex,
                            uint32_t size, uint32_t now);
uint32_t BootOd_StreamWrite(void *user, uint16_t index, uint8_t subindex,
                            const uint8_t *data, uint32_t size, uint32_t now);
uint32_t BootOd_StreamEnd(void *user, uint16_t index, uint8_t subindex,
                          uint32_t size, uint32_t now);
void BootOd_StreamAbort(void *user, uint16_t index, uint8_t subindex);

#endif
