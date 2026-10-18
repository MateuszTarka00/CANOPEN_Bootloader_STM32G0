/**
 * @file firmware_manifest.c
 * @brief Explicit-endian image manifest codec with CRC and commit markers.
 */
#include "firmware_manifest.h"
#include "persistent_storage.h"
#include <string.h>

#define BL_MANIFEST_MAGIC UINT32_C(0x31424756)
#define BL_MANIFEST_COMMIT UINT32_C(0x544D4342)
#define BL_MANIFEST_CRC_OFFSET 140U

bool BlManifest_Encode(const BlImageManifest *manifest,
                       uint8_t record[BL_MANIFEST_RECORD_SIZE])
{
    if ((manifest == NULL) || (record == NULL) ||
        (manifest->image_size == 0U) ||
        (manifest->signature_size != BL_MAX_SIGNATURE_SIZE) ||
        (manifest->compatibility.hardware_min > manifest->compatibility.hardware_max)) {
        return false;
    }
    memset(record, 0xFF, BL_MANIFEST_RECORD_SIZE);
    Co_Put32(&record[0], BL_MANIFEST_MAGIC);
    Co_Put16(&record[4], BL_MANIFEST_FORMAT_VERSION);
    Co_Put16(&record[6], BL_MANIFEST_RECORD_SIZE);
    Co_Put32(&record[8], manifest->image_size);
    Co_Put32(&record[12], manifest->image_crc);
    Co_Put32(&record[16], manifest->version);
    Co_Put32(&record[20], manifest->compatibility.vendor_id);
    Co_Put32(&record[24], manifest->compatibility.product_code);
    Co_Put32(&record[28], manifest->compatibility.hardware_variant);
    Co_Put32(&record[32], manifest->compatibility.hardware_min);
    Co_Put32(&record[36], manifest->compatibility.hardware_max);
    memcpy(&record[40], manifest->image_digest, BL_IMAGE_DIGEST_SIZE);
    record[72] = BL_MANIFEST_SIGNATURE_ED25519;
    record[73] = BL_MAX_SIGNATURE_SIZE;
    record[74] = 0U;
    record[75] = 0U;
    memcpy(&record[76], manifest->signature, BL_MAX_SIGNATURE_SIZE);
    Co_Put32(&record[BL_MANIFEST_CRC_OFFSET],
             Ps_Crc(record, BL_MANIFEST_CRC_OFFSET));
    Co_Put32(&record[144], BL_MANIFEST_COMMIT);
    Co_Put32(&record[148], ~BL_MANIFEST_COMMIT);
    return true;
}

bool BlManifest_Decode(const uint8_t *record, uint32_t size,
                       BlImageManifest *manifest)
{
    uint32_t i;
    if (manifest == NULL) {
        return false;
    }
    memset(manifest, 0, sizeof(*manifest));
    if ((record == NULL) || (size != BL_MANIFEST_RECORD_SIZE) ||
        (Co_Get32(&record[0]) != BL_MANIFEST_MAGIC) ||
        (Co_Get16(&record[4]) != BL_MANIFEST_FORMAT_VERSION) ||
        (Co_Get16(&record[6]) != BL_MANIFEST_RECORD_SIZE) ||
        (record[72] != BL_MANIFEST_SIGNATURE_ED25519) ||
        (record[73] != BL_MAX_SIGNATURE_SIZE) ||
        (record[74] != 0U) || (record[75] != 0U) ||
        (Co_Get32(&record[BL_MANIFEST_CRC_OFFSET]) !=
         Ps_Crc(record, BL_MANIFEST_CRC_OFFSET)) ||
        (Co_Get32(&record[144]) != BL_MANIFEST_COMMIT) ||
        (Co_Get32(&record[148]) != ~BL_MANIFEST_COMMIT)) {
        return false;
    }
    manifest->image_size = Co_Get32(&record[8]);
    manifest->image_crc = Co_Get32(&record[12]);
    manifest->version = Co_Get32(&record[16]);
    manifest->compatibility.vendor_id = Co_Get32(&record[20]);
    manifest->compatibility.product_code = Co_Get32(&record[24]);
    manifest->compatibility.hardware_variant = Co_Get32(&record[28]);
    manifest->compatibility.hardware_min = Co_Get32(&record[32]);
    manifest->compatibility.hardware_max = Co_Get32(&record[36]);
    memcpy(manifest->image_digest, &record[40], BL_IMAGE_DIGEST_SIZE);
    manifest->signature_size = BL_MAX_SIGNATURE_SIZE;
    memcpy(manifest->signature, &record[76], BL_MAX_SIGNATURE_SIZE);
    for (i = BL_MANIFEST_RECORD_SIZE; i < size; ++i) {
        if (record[i] != 0xFFU) {
            memset(manifest, 0, sizeof(*manifest));
            return false;
        }
    }
    if ((manifest->image_size == 0U) ||
        (manifest->compatibility.hardware_min > manifest->compatibility.hardware_max)) {
        memset(manifest, 0, sizeof(*manifest));
        return false;
    }
    return true;
}
