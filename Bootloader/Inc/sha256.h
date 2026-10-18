/**
 * @file sha256.h
 * @brief Fixed-memory SHA-256 implementation.
 */
#ifndef SHA256_H
#define SHA256_H

#include "canopen_types.h"

typedef struct {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t block[64];
    uint32_t block_size;
} BlSha256;

void BlSha256_Init(BlSha256 *context);
void BlSha256_Update(BlSha256 *context, const uint8_t *data, uint32_t size);
void BlSha256_Final(BlSha256 *context, uint8_t digest[32]);

#endif
