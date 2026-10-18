/**
 * @file firmware_auth.h
 * @brief Ed25519 authentication of the canonical image manifest.
 */
#ifndef FIRMWARE_AUTH_H
#define FIRMWARE_AUTH_H

#include "firmware_manifest.h"

bool BlAuth_IsReady(void);
bool BlAuth_VerifyManifest(const BlImageManifest *manifest);

#endif
