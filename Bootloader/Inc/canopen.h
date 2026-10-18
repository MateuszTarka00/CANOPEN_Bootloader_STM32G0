/**
 * @file canopen.h
 * @brief Single-owner CANopen bootloader node.
 */
#ifndef CANOPEN_H
#define CANOPEN_H

#include "canopen_heartbeat.h"
#include "canopen_nmt.h"
#include "canopen_sdo.h"
#include "object_dictionary.h"

typedef struct {
    CoSend send;
    void *send_user;
    CoNmt nmt;
    CoHeartbeat heartbeat;
    CoEmcy emcy;
    CoSdo sdo;
    BootOd od;
    CoFrame pending_tx;
    uint32_t tx_failures;
    bool tx_pending;
} Canopen;

bool Co_Init(Canopen *co, uint8_t node_id, uint16_t heartbeat_ms,
             CoSend send, void *send_user,
             BootloaderInterface *bootloader, FirmwareUpdate *update,
             BlTransaction *transaction, PsStore *store, PsData *persistent,
             const FiData *factory, bool update_auth_available, uint32_t now);
/** true indicates that the frame belongs to this bootloader node. */
bool Co_Receive(Canopen *co, const CoFrame *frame, uint32_t now);
void Co_Poll(Canopen *co, uint32_t now);

#endif
