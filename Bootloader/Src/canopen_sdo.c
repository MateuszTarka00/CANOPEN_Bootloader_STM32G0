/**
 * @file canopen_sdo.c
 * @brief CiA 301 SDO server with streaming domain downloads.
 */
#include "canopen_sdo.h"
#include <string.h>

static void makeReply(CoSdo *sdo, CoFrame *reply, uint8_t command)
{
    memset(reply, 0, sizeof(*reply));
    reply->id = (uint16_t)(0x580U + sdo->node_id);
    reply->length = 8U;
    reply->data[0] = command;
}

void CoSdo_Reset(CoSdo *sdo)
{
    if (sdo != NULL) {
        sdo->state = CO_SDO_IDLE;
        sdo->toggle = 0U;
        sdo->size = 0U;
        sdo->offset = 0U;
    }
}

static void abortTransfer(CoSdo *sdo, CoFrame *reply, uint32_t code)
{
    makeReply(sdo, reply, 0x80U);
    Co_Put16(&reply->data[1], sdo->index);
    reply->data[3] = sdo->subindex;
    Co_Put32(&reply->data[4], code);
    if ((sdo->state == CO_SDO_STREAM_DOWNLOAD) &&
        (sdo->ops.stream_abort != NULL)) {
        sdo->ops.stream_abort(sdo->ops.user, sdo->index, sdo->subindex);
    }
    CoSdo_Reset(sdo);
}

void CoSdo_Init(CoSdo *sdo, uint8_t node_id, const CoSdoOps *ops)
{
    memset(sdo, 0, sizeof(*sdo));
    sdo->node_id = node_id;
    if (ops != NULL) {
        sdo->ops = *ops;
    }
    sdo->timeout_ms = 1000U;
}

static uint32_t startUpload(CoSdo *sdo, const CoFrame *request,
                            uint32_t now, CoFrame *reply)
{
    uint32_t abort_code;
    uint32_t size = 0U;
    uint16_t index = Co_Get16(&request->data[1]);
    uint8_t subindex = request->data[3];
    CoSdo_Reset(sdo);
    sdo->index = index;
    sdo->subindex = subindex;
    if (sdo->ops.read == NULL) {
        abortTransfer(sdo, reply, CO_ABORT_HARDWARE);
        return 0U;
    }
    abort_code = sdo->ops.read(sdo->ops.user, index, subindex,
                               sdo->buffer, &size);
    if (abort_code != 0U) {
        abortTransfer(sdo, reply, abort_code);
        return 0U;
    }
    if (size <= 4U) {
        makeReply(sdo, reply, (uint8_t)(0x43U | ((4U - size) << 2U)));
        memcpy(&reply->data[4], sdo->buffer, size);
        Co_Put16(&reply->data[1], index);
        reply->data[3] = subindex;
    } else if (size <= sizeof(sdo->buffer)) {
        makeReply(sdo, reply, 0x41U);
        Co_Put16(&reply->data[1], index);
        reply->data[3] = subindex;
        Co_Put32(&reply->data[4], size);
        sdo->state = CO_SDO_UPLOAD;
        sdo->size = size;
        sdo->offset = 0U;
        sdo->toggle = 0U;
        sdo->last_activity = now;
    } else {
        abortTransfer(sdo, reply, CO_ABORT_NO_RESOURCE);
    }
    return 0U;
}

static uint32_t startDownload(CoSdo *sdo, const CoFrame *request,
                              uint32_t now, CoFrame *reply)
{
    uint8_t command = request->data[0];
    uint8_t count = (uint8_t)((command >> 2U) & 3U);
    bool expedited = (command & 2U) != 0U;
    bool size_known = (command & 1U) != 0U;
    uint32_t size;
    uint32_t abort_code;
    sdo->index = Co_Get16(&request->data[1]);
    sdo->subindex = request->data[3];
    sdo->offset = 0U;
    sdo->toggle = 0U;
    if ((expedited && !size_known && (count != 0U)) ||
        (!expedited && (count != 0U))) {
        abortTransfer(sdo, reply, CO_ABORT_COMMAND);
        return 0U;
    }
    size = expedited ? (size_known ? 4U - count : 4U) :
        (size_known ? Co_Get32(&request->data[4]) : 0U);
    if ((sdo->ops.check_download == NULL) ||
        (sdo->ops.write == NULL)) {
        abortTransfer(sdo, reply, CO_ABORT_HARDWARE);
        return 0U;
    }
    abort_code = sdo->ops.check_download(sdo->ops.user, sdo->index,
                                          sdo->subindex, size, size_known);
    if (abort_code != 0U) {
        abortTransfer(sdo, reply, abort_code);
        return 0U;
    }
    if (expedited) {
        abort_code = sdo->ops.write(sdo->ops.user, sdo->index, sdo->subindex,
                                    &request->data[4], size, now);
        if (abort_code != 0U) {
            abortTransfer(sdo, reply, abort_code);
            return 0U;
        }
        makeReply(sdo, reply, 0x60U);
        Co_Put16(&reply->data[1], sdo->index);
        reply->data[3] = sdo->subindex;
        return 0U;
    }
    if ((size_known == false) || (size > CO_SDO_BUFFER_SIZE)) {
        if ((sdo->ops.stream_begin == NULL) ||
            (sdo->ops.stream_write == NULL) ||
            (sdo->ops.stream_end == NULL)) {
            abortTransfer(sdo, reply, CO_ABORT_NO_RESOURCE);
            return 0U;
        }
        abort_code = sdo->ops.stream_begin(sdo->ops.user, sdo->index,
                                            sdo->subindex, size, now);
        if (abort_code != 0U) {
            abortTransfer(sdo, reply, abort_code);
            return 0U;
        }
        sdo->state = CO_SDO_STREAM_DOWNLOAD;
    } else {
        sdo->state = CO_SDO_DOWNLOAD;
    }
    sdo->size = size;
    sdo->last_activity = now;
    makeReply(sdo, reply, 0x60U);
    Co_Put16(&reply->data[1], sdo->index);
    reply->data[3] = sdo->subindex;
    return 0U;
}

static bool uploadSegment(CoSdo *sdo, const CoFrame *request,
                          uint32_t now, CoFrame *reply)
{
    uint32_t remaining;
    uint8_t count;
    bool last;
    if ((request->data[0] & 0xEFU) != 0x60U) {
        abortTransfer(sdo, reply, CO_ABORT_COMMAND);
        return true;
    }
    if ((request->data[0] & 0x10U) != sdo->toggle) {
        abortTransfer(sdo, reply, CO_ABORT_TOGGLE);
        return true;
    }
    remaining = sdo->size - sdo->offset;
    count = (uint8_t)((remaining > 7U) ? 7U : remaining);
    last = remaining <= 7U;
    makeReply(sdo, reply,
              (uint8_t)(sdo->toggle | ((7U - count) << 1U) | (last ? 1U : 0U)));
    memcpy(&reply->data[1], &sdo->buffer[sdo->offset], count);
    sdo->offset += count;
    sdo->toggle ^= 0x10U;
    sdo->last_activity = now;
    if (last) {
        CoSdo_Reset(sdo);
    }
    return true;
}

static bool downloadSegment(CoSdo *sdo, const CoFrame *request,
                            uint32_t now, CoFrame *reply)
{
    uint8_t command = request->data[0];
    uint8_t count = (uint8_t)(7U - ((command >> 1U) & 7U));
    bool last = (command & 1U) != 0U;
    uint32_t abort_code = 0U;
    if ((command & 0xE0U) != 0U) {
        abortTransfer(sdo, reply, CO_ABORT_COMMAND);
        return true;
    }
    if ((command & 0x10U) != sdo->toggle) {
        abortTransfer(sdo, reply, CO_ABORT_TOGGLE);
        return true;
    }
    if ((!last && (count != 7U)) || (sdo->offset > sdo->size) ||
        (count > (sdo->size - sdo->offset)) ||
        (last && ((sdo->offset + count) != sdo->size))) {
        abortTransfer(sdo, reply, CO_ABORT_LENGTH);
        return true;
    }
    if (sdo->state == CO_SDO_STREAM_DOWNLOAD) {
        abort_code = sdo->ops.stream_write(sdo->ops.user, sdo->index,
                                            sdo->subindex, &request->data[1],
                                            count, now);
    } else if (count > (sizeof(sdo->buffer) - sdo->offset)) {
        abort_code = CO_ABORT_NO_RESOURCE;
    } else {
        memcpy(&sdo->buffer[sdo->offset], &request->data[1], count);
    }
    if ((abort_code == 0U) && last &&
        (sdo->state == CO_SDO_STREAM_DOWNLOAD)) {
        abort_code = sdo->ops.stream_end(sdo->ops.user, sdo->index,
                                         sdo->subindex, sdo->size, now);
    } else if ((abort_code == 0U) && last) {
        abort_code = sdo->ops.write(sdo->ops.user, sdo->index, sdo->subindex,
                                    sdo->buffer, sdo->size, now);
    }
    if (abort_code != 0U) {
        abortTransfer(sdo, reply, abort_code);
        return true;
    }
    sdo->offset += count;
    sdo->toggle ^= 0x10U;
    sdo->last_activity = now;
    makeReply(sdo, reply, (uint8_t)(0x20U | (sdo->toggle ^ 0x10U)));
    if (last) {
        CoSdo_Reset(sdo);
    }
    return true;
}

bool CoSdo_Receive(CoSdo *sdo, const CoFrame *request, uint32_t now,
                   CoFrame *reply)
{
    uint8_t command;
    uint8_t service;
    if ((sdo == NULL) || (request == NULL) || (reply == NULL) ||
        (request->id != (uint16_t)(0x600U + sdo->node_id))) {
        return false;
    }
    if (request->length != 8U) {
        sdo->index = 0U;
        sdo->subindex = 0U;
        abortTransfer(sdo, reply, CO_ABORT_COMMAND);
        return true;
    }
    command = request->data[0];
    service = (uint8_t)(command & 0xE0U);
    if (command == 0x80U) {
        if ((sdo->state != CO_SDO_IDLE) &&
            (Co_Get16(&request->data[1]) == sdo->index) &&
            (request->data[3] == sdo->subindex)) {
            if ((sdo->state == CO_SDO_STREAM_DOWNLOAD) &&
                (sdo->ops.stream_abort != NULL)) {
                sdo->ops.stream_abort(sdo->ops.user, sdo->index,
                                      sdo->subindex);
            }
            CoSdo_Reset(sdo);
        }
        return false;
    }
    if (sdo->state != CO_SDO_IDLE) {
        sdo->last_activity = now;
    }
    if ((command == 0x40U) && (sdo->state == CO_SDO_IDLE)) {
        (void)startUpload(sdo, request, now, reply);
        return true;
    }
    if ((service == 0x20U) && (sdo->state == CO_SDO_IDLE)) {
        (void)startDownload(sdo, request, now, reply);
        return true;
    }
    if (sdo->state == CO_SDO_UPLOAD) {
        return uploadSegment(sdo, request, now, reply);
    }
    if ((sdo->state == CO_SDO_DOWNLOAD) ||
        (sdo->state == CO_SDO_STREAM_DOWNLOAD)) {
        return downloadSegment(sdo, request, now, reply);
    }
    sdo->index = Co_Get16(&request->data[1]);
    sdo->subindex = request->data[3];
    abortTransfer(sdo, reply, CO_ABORT_COMMAND);
    return true;
}

bool CoSdo_Poll(CoSdo *sdo, uint32_t now, CoFrame *reply)
{
    if ((sdo == NULL) || (reply == NULL) || (sdo->state == CO_SDO_IDLE) ||
        ((now - sdo->last_activity) < sdo->timeout_ms)) {
        return false;
    }
    abortTransfer(sdo, reply, CO_ABORT_TIMEOUT);
    return true;
}
