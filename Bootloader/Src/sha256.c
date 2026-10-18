/**
 * @file sha256.c
 * @brief SHA-256 for streamed image verification.
 */
#include "sha256.h"
#include <string.h>

static const uint32_t round_constant[64] = {
    UINT32_C(0x428A2F98), UINT32_C(0x71374491), UINT32_C(0xB5C0FBCF), UINT32_C(0xE9B5DBA5),
    UINT32_C(0x3956C25B), UINT32_C(0x59F111F1), UINT32_C(0x923F82A4), UINT32_C(0xAB1C5ED5),
    UINT32_C(0xD807AA98), UINT32_C(0x12835B01), UINT32_C(0x243185BE), UINT32_C(0x550C7DC3),
    UINT32_C(0x72BE5D74), UINT32_C(0x80DEB1FE), UINT32_C(0x9BDC06A7), UINT32_C(0xC19BF174),
    UINT32_C(0xE49B69C1), UINT32_C(0xEFBE4786), UINT32_C(0x0FC19DC6), UINT32_C(0x240CA1CC),
    UINT32_C(0x2DE92C6F), UINT32_C(0x4A7484AA), UINT32_C(0x5CB0A9DC), UINT32_C(0x76F988DA),
    UINT32_C(0x983E5152), UINT32_C(0xA831C66D), UINT32_C(0xB00327C8), UINT32_C(0xBF597FC7),
    UINT32_C(0xC6E00BF3), UINT32_C(0xD5A79147), UINT32_C(0x06CA6351), UINT32_C(0x14292967),
    UINT32_C(0x27B70A85), UINT32_C(0x2E1B2138), UINT32_C(0x4D2C6DFC), UINT32_C(0x53380D13),
    UINT32_C(0x650A7354), UINT32_C(0x766A0ABB), UINT32_C(0x81C2C92E), UINT32_C(0x92722C85),
    UINT32_C(0xA2BFE8A1), UINT32_C(0xA81A664B), UINT32_C(0xC24B8B70), UINT32_C(0xC76C51A3),
    UINT32_C(0xD192E819), UINT32_C(0xD6990624), UINT32_C(0xF40E3585), UINT32_C(0x106AA070),
    UINT32_C(0x19A4C116), UINT32_C(0x1E376C08), UINT32_C(0x2748774C), UINT32_C(0x34B0BCB5),
    UINT32_C(0x391C0CB3), UINT32_C(0x4ED8AA4A), UINT32_C(0x5B9CCA4F), UINT32_C(0x682E6FF3),
    UINT32_C(0x748F82EE), UINT32_C(0x78A5636F), UINT32_C(0x84C87814), UINT32_C(0x8CC70208),
    UINT32_C(0x90BEFFFA), UINT32_C(0xA4506CEB), UINT32_C(0xBEF9A3F7), UINT32_C(0xC67178F2)
};

static uint32_t rotateRight(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32U - count));
}

static uint32_t readBigEndian(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) | (uint32_t)data[3];
}

static void transform(BlSha256 *context, const uint8_t block[64])
{
    uint32_t words[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t i;
    for (i = 0U; i < 16U; ++i) {
        words[i] = readBigEndian(&block[i * 4U]);
    }
    for (i = 16U; i < 64U; ++i) {
        uint32_t x = words[i - 15U];
        uint32_t y = words[i - 2U];
        uint32_t s0 = rotateRight(x, 7U) ^ rotateRight(x, 18U) ^ (x >> 3U);
        uint32_t s1 = rotateRight(y, 17U) ^ rotateRight(y, 19U) ^ (y >> 10U);
        words[i] = words[i - 16U] + s0 + words[i - 7U] + s1;
    }
    a = context->state[0]; b = context->state[1];
    c = context->state[2]; d = context->state[3];
    e = context->state[4]; f = context->state[5];
    g = context->state[6]; h = context->state[7];
    for (i = 0U; i < 64U; ++i) {
        uint32_t s1 = rotateRight(e, 6U) ^ rotateRight(e, 11U) ^ rotateRight(e, 25U);
        uint32_t choice = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + choice + round_constant[i] + words[i];
        uint32_t s0 = rotateRight(a, 2U) ^ rotateRight(a, 13U) ^ rotateRight(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + majority;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    context->state[0] += a; context->state[1] += b;
    context->state[2] += c; context->state[3] += d;
    context->state[4] += e; context->state[5] += f;
    context->state[6] += g; context->state[7] += h;
}

void BlSha256_Init(BlSha256 *context)
{
    static const uint32_t initial[8] = {
        UINT32_C(0x6A09E667), UINT32_C(0xBB67AE85), UINT32_C(0x3C6EF372), UINT32_C(0xA54FF53A),
        UINT32_C(0x510E527F), UINT32_C(0x9B05688C), UINT32_C(0x1F83D9AB), UINT32_C(0x5BE0CD19)
    };
    memcpy(context->state, initial, sizeof(initial));
    context->total_bytes = 0U;
    context->block_size = 0U;
}

void BlSha256_Update(BlSha256 *context, const uint8_t *data, uint32_t size)
{
    uint32_t remaining = size;
    context->total_bytes += size;
    while (remaining > 0U) {
        uint32_t room = 64U - context->block_size;
        uint32_t copy = (remaining < room) ? remaining : room;
        memcpy(&context->block[context->block_size], data, copy);
        context->block_size += copy;
        data += copy;
        remaining -= copy;
        if (context->block_size == 64U) {
            transform(context, context->block);
            context->block_size = 0U;
        }
    }
}

void BlSha256_Final(BlSha256 *context, uint8_t digest[32])
{
    uint64_t bit_count = context->total_bytes * UINT64_C(8);
    uint32_t i;
    context->block[context->block_size++] = 0x80U;
    if (context->block_size > 56U) {
        memset(&context->block[context->block_size], 0, 64U - context->block_size);
        transform(context, context->block);
        context->block_size = 0U;
    }
    memset(&context->block[context->block_size], 0, 56U - context->block_size);
    for (i = 0U; i < 8U; ++i) {
        context->block[63U - i] = (uint8_t)(bit_count >> (i * 8U));
    }
    transform(context, context->block);
    for (i = 0U; i < 8U; ++i) {
        digest[i * 4U] = (uint8_t)(context->state[i] >> 24U);
        digest[i * 4U + 1U] = (uint8_t)(context->state[i] >> 16U);
        digest[i * 4U + 2U] = (uint8_t)(context->state[i] >> 8U);
        digest[i * 4U + 3U] = (uint8_t)context->state[i];
    }
    memset(context, 0, sizeof(*context));
}
