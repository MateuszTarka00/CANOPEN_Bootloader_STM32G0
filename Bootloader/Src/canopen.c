/**
 * @file canopen.c
 * @brief Bounded CANopen bootloader node; caller owns all processing.
 */
#include "canopen.h"
#include <string.h>

static uint32_t pollTime(const Canopen *co, uint32_t fallback)
{
    return (co->sdo.ops.get_time_ms != NULL) ?
        co->sdo.ops.get_time_ms(co->sdo.ops.user) : fallback;
}

static void queueFrame(Canopen *co, const CoFrame *frame)
{
    if (!co->tx_pending) {
        co->pending_tx = *frame;
        co->tx_pending = true;
    }
}

static void sendBootUp(Canopen *co)
{
    CoFrame frame;
    if (CoHeartbeat_BootUp(&co->heartbeat, &frame)) {
        queueFrame(co, &frame);
    }

}

static void pollUpdateTimeout(Canopen *co, uint32_t now)
{
    if ((co->od.update->state == FWU_RECEIVING) &&
        (FirmwareUpdate_Poll(co->od.update, now) == FWU_RESULT_TIMEOUT)) {
        PsData candidate = *co->od.persistent;
        CoFrame frame;
        uint32_t detail = (uint32_t)FWU_RESULT_TIMEOUT;
        candidate.update_result = detail;
        if (Ps_Save(co->od.store, &candidate) == PS_OK) {
            *co->od.persistent = candidate;
        } else {
            co->od.update_auth_available = false;
            detail = (uint32_t)PS_IO_ERROR;
        }
        CoEmcy_Raise(&co->emcy, 0xFF01U, 1U, detail, &frame);
        queueFrame(co, &frame);
    }
}

bool Co_Init(Canopen *co, uint8_t node_id, uint16_t heartbeat_ms,
             CoSend send, void *send_user,
             BootloaderInterface *bootloader, FirmwareUpdate *update,
             BlTransaction *transaction, PsStore *store, PsData *persistent,
             const FiData *factory, bool update_auth_available, uint32_t now)
{
    CoSdoOps sdo_ops;
    if ((co == NULL) || (node_id == 0U) || (node_id > 127U) ||
        (send == NULL) || (bootloader == NULL) || (update == NULL) ||
        (transaction == NULL) || (store == NULL) || (persistent == NULL)) {
        return false;
    }
    memset(co, 0, sizeof(*co));
    co->send = send;
    co->send_user = send_user;
    CoNmt_Init(&co->nmt, node_id);
    CoHeartbeat_Init(&co->heartbeat, node_id, heartbeat_ms, now);
    CoEmcy_Init(&co->emcy, node_id);
    BootOd_Init(&co->od, bootloader, update, transaction, store, persistent,
                factory, &co->emcy, node_id, update_auth_available);
    sdo_ops = BootOd_SdoOps(&co->od);
    CoSdo_Init(&co->sdo, node_id, &sdo_ops);
    sendBootUp(co);
    return true;
}

bool Co_Receive(Canopen *co, const CoFrame *frame, uint32_t now)
{
    CoFrame reply;
    bool handled;
    if ((co == NULL) || (frame == NULL)) {
        return false;
    }
    if (frame->id == 0U) {
        return CoNmt_Receive(&co->nmt, frame);
    }
    if (frame->id != (uint16_t)(0x600U + co->nmt.node_id)) {
        return false;
    }
    if (co->nmt.state == CO_NMT_STOPPED) {
        return true;
    }
    handled = CoSdo_Receive(&co->sdo, frame, now, &reply);
    if (handled) {
        queueFrame(co, &reply);
    }
    return handled;
}

void Co_Poll(Canopen *co, uint32_t now)
{
    CoFrame frame;
    if (co == NULL) {
        return;
    }
    /* Receive callbacks may have advanced the clock during synchronous Flash IO. */
    now = pollTime(co, now);
    pollUpdateTimeout(co, now);
    /* Timeout recovery can itself persist a journal record. */
    now = pollTime(co, now);
    if (co->nmt.reset_communication_requested) {
        CoSdo_Reset(&co->sdo);
        co->nmt.reset_communication_requested = false;
        co->nmt.state = CO_NMT_PRE_OPERATIONAL;
        co->heartbeat.last_sent = now;
    }
    if (!co->tx_pending && CoSdo_Poll(&co->sdo, now, &frame)) {
        queueFrame(co, &frame);
    }
    if (!co->tx_pending &&
        CoHeartbeat_Poll(&co->heartbeat, co->nmt.state, now, &frame)) {
        queueFrame(co, &frame);
    }
    if (co->tx_pending && co->send(co->send_user, &co->pending_tx)) {
        co->tx_pending = false;
    } else if (co->tx_pending) {
        co->tx_failures++;
    }
}
