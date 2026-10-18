/**
 * @file firmware_auth.c
 * @brief Constant-time Ed25519 manifest verification using Monocypher.
 */
#include "firmware_auth.h"
#include "firmware_auth_key.h"
#include "monocypher-ed25519.h"
#include <string.h>

bool BlAuth_IsReady(void)
{
    return BL_SIGNING_PUBLIC_KEY_PROVISIONED != 0;
}

bool BlAuth_VerifyManifest(const BlImageManifest *manifest)
{
    uint8_t record[BL_MANIFEST_RECORD_SIZE];
    int result;
    if (!BlAuth_IsReady() ||
        !BlManifest_Encode(manifest, record)) {
        return false;
    }
    result = crypto_ed25519_check(manifest->signature,
                                  bl_signing_public_key,
                                  record, BL_MANIFEST_SIGNED_SIZE);
    memset(record, 0, sizeof(record));
    return result == 0;
}
