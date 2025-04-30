#pragma once

#include <stdint.h>
#include <stddef.h>

#include "bit_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LEGIC_PRIME_CRC_SIZE sizeof(uint8_t)

void legic_prime_crc_append(BitBuffer* buf);

bool legic_prime_crc_check(const BitBuffer* buf);

void legic_prime_crc_trim(BitBuffer* buf);

#ifdef __cplusplus
}
#endif
