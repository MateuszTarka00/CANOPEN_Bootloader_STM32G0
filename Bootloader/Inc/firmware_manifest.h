/**
 * @file firmware_manifest.h
 * @brief Stable vendor-specific signed-image manifest wire format.
 */
#ifndef FIRMWARE_MANIFEST_H
#define FIRMWARE_MANIFEST_H

#include "bootloader_interface.h"

#define BL_MANIFEST_RECORD_SIZE 152U
#define BL_MANIFEST_SIGNED_SIZE 76U
#define BL_MANIFEST_FORMAT_VERSION 1U
#define BL_MANIFEST_SIGNATURE_ED25519 1U

bool BlManifest_Encode(const BlImageManifest *manifest,
                       uint8_t record[BL_MANIFEST_RECORD_SIZE]);
bool BlManifest_Decode(const uint8_t *record, uint32_t size,
                       BlImageManifest *manifest);

#endif
