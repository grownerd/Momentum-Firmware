#include "legic_prime_crc.h"

#include <furi/furi.h>

#define BITMASK(x) (1 << (x))

/*
 ref  http://www.csm.ornl.gov/~dunigan/crc.html
 Returns the value v with the bottom b [0,32] bits reflected.
 Example: reflect(0x3e23L,3) == 0x3e26
*/
uint32_t reflect(uint32_t v, int b) {
    uint32_t t = v;
    for (int i = 0; i < b; ++i) {
        if (t & 1)
            v |=  BITMASK((b - 1) - i);
        else
            v &= ~BITMASK((b - 1) - i);
        t >>= 1;
    }
    return v;
}

// https://graphics.stanford.edu/~seander/bithacks.html#BitReverseTable

// Reverse the bits in a byte with 3 operations (64-bit multiply and modulus division):
uint8_t reflect8(uint8_t b) {
    return (b * 0x0202020202ULL & 0x010884422010ULL) % 1023;
}

void crc_init_ref(crc_t *crc, int order, uint32_t polynom, uint32_t initial_value, uint32_t final_xor, bool refin, bool refout) {
    crc_init(crc, order, polynom, initial_value, final_xor);
    crc->refin = refin;
    crc->refout = refout;
    crc_clear(crc);
}

void crc_init(crc_t *crc, int order, uint32_t polynom, uint32_t initial_value, uint32_t final_xor) {
    crc->order = order;
    crc->topbit = BITMASK(order - 1);
    crc->polynom = polynom;
    crc->initial_value = initial_value;
    crc->final_xor = final_xor;
    crc->mask = (1L << order) - 1;
    crc->refin = false;
    crc->refout = false;
    crc_clear(crc);
}

void crc_clear(crc_t *crc) {

    crc->state = crc->initial_value & crc->mask;
    if (crc->refin)
        crc->state = reflect(crc->state, crc->order);
}

void crc_update2(crc_t *crc, uint32_t data, int data_width) {

    if (crc->refin)
        data = reflect(data, data_width);

    // Bring the next byte into the remainder.
    crc->state ^= data << (crc->order - data_width);

    for (uint8_t bit = data_width; bit > 0; --bit) {

        if (crc->state & crc->topbit)
            crc->state = (crc->state << 1) ^ crc->polynom;
        else
            crc->state = (crc->state << 1);
    }
}

void crc_update(crc_t *crc, uint32_t data, int data_width) {
    if (crc->refin)
        data = reflect(data, data_width);

    int i;
    for (i = 0; i < data_width; i++) {
        int oldstate = crc->state;
        crc->state = crc->state >> 1;
        if ((oldstate ^ data) & 1) {
            crc->state ^= crc->polynom;
        }
        data >>= 1;
    }
}

uint32_t crc_finish(crc_t *crc) {
    uint32_t val = crc->state;
    if (crc->refout)
        val = reflect(val, crc->order);
    return (val ^ crc->final_xor) & crc->mask;
}

// width=4  poly=0xC, reversed poly=0x7  init=0x5   refin=true  refout=true  xorout=0x0000  check=  name="CRC-4/LEGIC"
uint32_t CRC4Legic(uint8_t *buff, size_t size) {
    UNUSED(size);

    crc_t crc;
    crc_init_ref(&crc, 4, 0x19 >> 1, 0x5, 0, true, true);
    crc_update2(&crc, 1, 1); /* CMD_READ */
    crc_update2(&crc, buff[0], 8);
    crc_update2(&crc, buff[1], 8);
    return reflect(crc_finish(&crc), 4);
}
// width=8  poly=0x63, reversed poly=0x8D  init=0x55  refin=true  refout=true  xorout=0x0000  check=0xC6  name="CRC-8/LEGIC"
// the CRC needs to be reversed before returned.
uint32_t CRC8Legic(uint8_t *buff, size_t size) {
    crc_t crc;
    crc_init_ref(&crc, 8, 0x63, 0x55, 0, true, true);
    for (size_t i = 0; i < size; ++i) {
        crc_update2(&crc, buff[i], 8);
    }
    return reflect8(crc_finish(&crc));
}
