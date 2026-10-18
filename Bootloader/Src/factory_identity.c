/** @file factory_identity.c @brief Explicit little-endian factory record codec. */
#include "factory_identity.h"
#include "persistent_storage.h"
#include <string.h>
#define FI_MAGIC UINT32_C(0x46494431)
#define FI_COMMIT UINT32_C(0x46434D54)

bool Fi_Validate(const FiData *d)
{
    return (d != NULL) && (d->vendor_id != 0U) && (d->vendor_id != UINT32_MAX) &&
        (d->product_code != 0U) && (d->product_code != UINT32_MAX) &&
        (d->serial != 0U) && (d->serial != UINT32_MAX) &&
        (d->product_revision != UINT32_MAX) &&
        (d->hardware_variant != 0U) && (d->hardware_variant != UINT32_MAX) &&
        (d->hardware_major != UINT16_MAX) && (d->hardware_minor != UINT16_MAX);
}

FiResult Fi_Encode(const FiData *d, uint8_t *r, uint32_t size)
{
    if (!Fi_Validate(d) || (r == NULL) || (size != FI_RECORD_SIZE)) { return FI_INVALID; }
    memset(r, 255, size);
    Co_Put32(&r[0], FI_MAGIC); Co_Put32(&r[4], FI_SCHEMA);
    Co_Put32(&r[8], FI_RECORD_SIZE);
    Co_Put32(&r[12], d->vendor_id); Co_Put32(&r[16], d->product_code);
    Co_Put32(&r[20], d->product_revision); Co_Put32(&r[24], d->serial);
    Co_Put32(&r[28], d->hardware_variant);
    Co_Put16(&r[32], d->hardware_major); Co_Put16(&r[34], d->hardware_minor);
    Co_Put32(&r[120], Ps_Crc(r, 120U)); Co_Put32(&r[124], FI_COMMIT);
    return FI_OK;
}

FiResult Fi_Decode(const uint8_t *r, uint32_t size, FiData *d)
{
    uint32_t i;
    bool blank = true;
    if (d == NULL) { return FI_INVALID; }
    memset(d, 0, sizeof(*d));
    if ((r == NULL) || (size != FI_RECORD_SIZE)) { return FI_INVALID; }
    for (i = 0U; i < size; ++i) { if (r[i] != 255U) { blank = false; } }
    if (blank) { return FI_EMPTY; }
    if ((Co_Get32(&r[0]) != FI_MAGIC) || (Co_Get32(&r[4]) != FI_SCHEMA) ||
        (Co_Get32(&r[8]) != FI_RECORD_SIZE) || (Co_Get32(&r[124]) != FI_COMMIT) ||
        (Co_Get32(&r[120]) != Ps_Crc(r, 120U))) { return FI_CORRUPT; }
    for (i = 36U; i < 120U; ++i) { if (r[i] != 255U) { return FI_CORRUPT; } }
    d->vendor_id = Co_Get32(&r[12]); d->product_code = Co_Get32(&r[16]);
    d->product_revision = Co_Get32(&r[20]); d->serial = Co_Get32(&r[24]);
    d->hardware_variant = Co_Get32(&r[28]);
    d->hardware_major = Co_Get16(&r[32]); d->hardware_minor = Co_Get16(&r[34]);
    if (!Fi_Validate(d)) { memset(d, 0, sizeof(*d)); return FI_CORRUPT; }
    return FI_OK;
}

bool Fi_Compatible(const FiData *d, const FiCompatibility *image)
{
    uint32_t revision;
    if (!Fi_Validate(d) || (image == NULL)) { return false; }
    revision = ((uint32_t)d->hardware_major << 16U) | d->hardware_minor;
    return (image->hardware_min <= image->hardware_max) &&
        (d->vendor_id == image->vendor_id) && (d->product_code == image->product_code) &&
        (d->hardware_variant == image->hardware_variant) &&
        (revision >= image->hardware_min) && (revision <= image->hardware_max);
}

bool Fi_ApplicationRange(uint32_t address, uint32_t size)
{
    return (size != 0U) && (address >= FI_APPLICATION_START) &&
        (address < FI_FLASH_ADDRESS) && (size <= FI_FLASH_ADDRESS - address);
}

static uint32_t decimal(uint16_t value, char *text)
{
    char reversed[5];
    uint32_t count = 0U, i;
    do {
        reversed[count++] = (char)('0' + (value % 10U));
        value = (uint16_t)(value / 10U);
    } while (value != 0U);
    for (i = 0U; i < count; ++i) { text[i] = reversed[count - i - 1U]; }
    return count;
}

bool Fi_HardwareText(const FiData *d, char *text, uint32_t size)
{
    uint32_t length;
    if ((text == NULL) || (size < FI_TEXT_SIZE)) { return false; }
    text[0] = '\0';
    if (!Fi_Validate(d)) { return false; }
    length = decimal(d->hardware_major, text); text[length++] = '.';
    length += decimal(d->hardware_minor, &text[length]); text[length] = '\0';
    return true;
}
