/**
 * @file firmware_auth_key.h
 * @brief Prototype Ed25519 public key and firmware-acceptance gate.
 *
 * The matching prototype private key is stored outside the repository.
 * Initial programming must include a matching signed Slot-A manifest.
 */
#ifndef FIRMWARE_AUTH_KEY_H
#define FIRMWARE_AUTH_KEY_H

#define BL_SIGNING_PUBLIC_KEY_PROVISIONED 1

static const unsigned char bl_signing_public_key[32] = {
    0x09U, 0xC1U, 0xA4U, 0x34U, 0x4FU, 0xF8U, 0x38U, 0x0AU,
    0x29U, 0xDBU, 0x6FU, 0x9CU, 0x08U, 0x2EU, 0x98U, 0x31U,
    0xD8U, 0xC4U, 0x94U, 0x1FU, 0x8EU, 0x22U, 0xB9U, 0x6DU,
    0xFDU, 0x0CU, 0xA1U, 0x87U, 0x16U, 0xE4U, 0x39U, 0xB0U
};

#endif
