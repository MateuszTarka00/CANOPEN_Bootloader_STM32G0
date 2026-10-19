/** @file persistent_storage.c
 * @brief Serialized, CRC-protected two-page journal; see persistent_storage.h.
 * Dependencies: persistent_storage -> canopen_types, string.
 */
#include "persistent_storage.h"
#include "device_memory.h"
#include <string.h>

#define PS_MAGIC UINT32_C(0x434F5053)
#define PS_COMMIT UINT32_C(0x434F4D54)

uint32_t Ps_CrcUpdate(uint32_t crc, const uint8_t *data, uint32_t size)
{
    uint32_t i;
    for (i = 0U; i < size; ++i) {
        uint8_t bit;
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = ((crc & 1U) != 0U) ? (crc >> 1U) ^ UINT32_C(0xEDB88320) : crc >> 1U;
        }
    }
    return crc;
}

uint32_t Ps_Crc(const uint8_t *data, uint32_t size)
{
    return Ps_CrcUpdate(UINT32_MAX, data, size) ^ UINT32_MAX;
}

bool Ps_Validate(const PsData *d)
{
    bool update_state_valid;
    update_state_valid = (d != NULL) &&
        ((d->update_state <= 8U) ||
         ((d->update_state >= UINT32_C(0x100)) &&
          (d->update_state < (UINT32_C(0x100) + DEVICE_SWAP_STATE_COUNT))) ||
         ((d->update_state >= UINT32_C(0x300)) &&
          (d->update_state < (UINT32_C(0x300) + DEVICE_SWAP_STATE_COUNT))) ||
         (d->update_state == UINT32_C(0x500)) ||
         (d->update_state == UINT32_C(0x501)));
    return (d != NULL) &&
        (memchr(d->device_name, '\0', PS_STRING_SIZE) != NULL) &&
        (memchr(d->hardware_version, '\0', PS_STRING_SIZE) != NULL) &&
        (memchr(d->software_version, '\0', PS_STRING_SIZE) != NULL) &&
        (d->active_slot <= 1U) &&
        ((d->pending_slot <= 1U) || (d->pending_slot == UINT32_MAX)) &&
        update_state_valid && (d->boot_request <= 2U) &&
        ((d->boot_request == 0U) ||
         ((d->boot_node_id >= 1U) && (d->boot_node_id <= 127U) &&
          (d->boot_bitrate != 0U) && (d->boot_protocol == 2U)));
}

void Ps_Init(PsStore *s, const PsIo *io)
{
    memset(s, 0, sizeof(*s));
    s->io = *io;
}

static void encode(uint8_t *r, const PsData *d, uint32_t sequence)
{
    memset(r, 0xFF, PS_RECORD_SIZE);
    Co_Put32(&r[0], PS_MAGIC);
    Co_Put32(&r[4], 2U);
    Co_Put32(&r[8], sequence);
    memcpy(&r[16], d->device_name, PS_STRING_SIZE);
    memcpy(&r[48], d->hardware_version, PS_STRING_SIZE);
    memcpy(&r[80], d->software_version, PS_STRING_SIZE);
    Co_Put32(&r[112], d->vendor_id); Co_Put32(&r[116], d->product_code);
    Co_Put32(&r[120], d->revision); Co_Put32(&r[124], d->serial);
    Co_Put32(&r[128], d->firmware_crc); Co_Put32(&r[132], d->firmware_size);
    Co_Put32(&r[136], d->pending_version); Co_Put32(&r[140], d->active_version);
    Co_Put32(&r[144], d->update_state); Co_Put32(&r[148], d->update_result);
    Co_Put32(&r[152], d->active_slot); Co_Put32(&r[156], d->pending_slot);
    Co_Put16(&r[160], d->heartbeat_ms);
    Co_Put32(&r[164], d->boot_request);
    Co_Put32(&r[168], d->boot_node_id);
    Co_Put32(&r[172], d->boot_bitrate);
    Co_Put32(&r[176], d->boot_protocol);
    Co_Put32(&r[240], Ps_Crc(r, 240U));
    Co_Put32(&r[248], PS_COMMIT);
    Co_Put32(&r[252], ~PS_COMMIT);
}

static bool decode(const uint8_t *r, PsData *d)
{
    uint32_t schema = Co_Get32(&r[4]);
    if ((Co_Get32(r) != PS_MAGIC) || ((schema != 1U) && (schema != 2U)) ||
        (Co_Get32(&r[248]) != PS_COMMIT) || (Co_Get32(&r[252]) != ~PS_COMMIT) ||
        (Co_Get32(&r[240]) != Ps_Crc(r, 240U))) {
        return false;
    }
    memset(d, 0, sizeof(*d));
    memcpy(d->device_name, &r[16], PS_STRING_SIZE);
    memcpy(d->hardware_version, &r[48], PS_STRING_SIZE);
    memcpy(d->software_version, &r[80], PS_STRING_SIZE);
    d->vendor_id = Co_Get32(&r[112]); d->product_code = Co_Get32(&r[116]);
    d->revision = Co_Get32(&r[120]); d->serial = Co_Get32(&r[124]);
    d->firmware_crc = Co_Get32(&r[128]); d->firmware_size = Co_Get32(&r[132]);
    d->pending_version = Co_Get32(&r[136]); d->active_version = Co_Get32(&r[140]);
    d->update_state = Co_Get32(&r[144]); d->update_result = Co_Get32(&r[148]);
    d->active_slot = Co_Get32(&r[152]); d->pending_slot = Co_Get32(&r[156]);
    d->heartbeat_ms = Co_Get16(&r[160]);
    if (schema == 2U) {
        d->boot_request = Co_Get32(&r[164]); d->boot_node_id = Co_Get32(&r[168]);
        d->boot_bitrate = Co_Get32(&r[172]); d->boot_protocol = Co_Get32(&r[176]);
    }
    return Ps_Validate(d);
}

PsResult Ps_Load(PsStore *s, PsData *d)
{
    uint8_t page, r[PS_RECORD_SIZE];
    bool found = false, blank = true;
    PsData candidate;
    if ((s == NULL) || (d == NULL) || (s->io.read == NULL)) {
        return PS_INVALID;
    }
    s->loaded = false;
    for (page = 0U; page < 2U; ++page) {
        uint32_t i, sequence;
        if (!s->io.read(s->io.user, page, 0U, r, sizeof(r))) {
            return PS_IO_ERROR;
        }
        for (i = 0U; i < sizeof(r); ++i) {
            if (r[i] != 0xFFU) { blank = false; }
        }
        sequence = Co_Get32(&r[8]);
        if (decode(r, &candidate) && (!found ||
            (((sequence - s->sequence) != 0U) &&
             ((sequence - s->sequence) < UINT32_C(0x80000000))))) {
            *d = candidate;
            s->sequence = sequence;
            s->active_page = page;
            found = true;
        }
    }
    s->loaded = true;
    if (!found) {
        s->sequence = 0U;
        s->active_page = 1U;
    }
    return found ? PS_OK : (blank ? PS_EMPTY : PS_CORRUPT);
}

PsResult Ps_Save(PsStore *s, const PsData *d)
{
    uint8_t r[PS_RECORD_SIZE], check[PS_RECORD_SIZE], page;
    if ((s == NULL) || !Ps_Validate(d) || !s->loaded ||
        (s->io.erase == NULL) || (s->io.write == NULL) || (s->io.read == NULL)) {
        return PS_INVALID;
    }
    page = (uint8_t)(s->active_page ^ 1U);
    encode(r, d, s->sequence + 1U);
    if (!s->io.erase(s->io.user, page) ||
        !s->io.write(s->io.user, page, 0U, r, 248U) ||
        !s->io.read(s->io.user, page, 0U, check, 248U) ||
        (memcmp(r, check, 248U) != 0) ||
        !s->io.write(s->io.user, page, 248U, &r[248], 8U) ||
        !s->io.read(s->io.user, page, 0U, check, sizeof(check)) ||
        (memcmp(r, check, sizeof(r)) != 0)) {
        return PS_IO_ERROR;
    }
    s->sequence++;
    s->active_page = page;
    return PS_OK;
}
