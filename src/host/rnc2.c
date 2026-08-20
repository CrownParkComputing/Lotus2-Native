/* RNC2 (Rob Northen compression, method 2) decoder.
 *
 * Faithful C port of the method-2 unpack path from the public
 * decompilation of RNC ProPack (lab313ru/rnc_propack_source, file
 * main.c: read_source_byte / input_bits_m2 / decode_match_count /
 * decode_match_offset / unpack_data_m2 / do_unpack_data).
 *
 * The earlier clean-room attempt collapsed the byte reader and the bit
 * reader into one helper and got the decode cascade wrong; the slave's
 * install body then blew the back-reference guard on its first command.
 * This version keeps the reference structure exactly:
 *
 *   - the bit reader is MSB-first within each input byte, one byte at a
 *     time (input_bits_m2);
 *   - literal bytes are XORed with the low byte of a 16-bit rolling key
 *     that is rotated right by 1 after every literal (and once after
 *     each raw run);
 *   - back-reference offsets are decoded by a fixed bit cascade and
 *     completed with a trailing literal byte: ((hi << 8) | byte) + 1.
 *
 * The output buffer acts as the sliding window; match copies read back
 * from already-emitted output.
 *
 * Return codes: 0 success, -1 not an RNC2 stream, -2 stream needs a
 * decryption key, -3 input truncated, -4 packed-data CRC mismatch,
 * -5 unpacked-data CRC mismatch, -6 corrupt stream (bad back-reference
 * or output overrun).
 */
#include <string.h>
#include <stdint.h>

#include "rnc2.h"

#define RNC2_HEADER_SIZE 18

typedef struct {
    const uint8_t *src;
    uint32_t src_len;   /* bytes of packed data after the 18-byte header */
    uint32_t src_pos;
    uint32_t bit_buffer;
    int bit_count;
    uint16_t enc_key;
    uint8_t *out;
    uint32_t out_len;   /* expected unpacked size */
    uint32_t opos;
    uint16_t crc;       /* running CRC16 of unpacked data */
} rnc2_t;

static uint16_t crc16_update(uint16_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++)
        crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    return crc;
}

static int read_byte(rnc2_t *v, uint8_t *b)
{
    if (v->src_pos >= v->src_len) return -3;
    *b = v->src[v->src_pos++];
    return 0;
}

static int input_bits(rnc2_t *v, int count, uint32_t *bits_out)
{
    uint32_t bits = 0;

    while (count--) {
        if (!v->bit_count) {
            uint8_t b;
            int rc = read_byte(v, &b);
            if (rc) return rc;
            v->bit_buffer = b;
            v->bit_count = 8;
        }
        bits <<= 1;
        if (v->bit_buffer & 0x80) bits |= 1;
        v->bit_buffer = (v->bit_buffer << 1) & 0xFF;
        v->bit_count--;
    }

    *bits_out = bits;
    return 0;
}

static int emit(rnc2_t *v, uint8_t b)
{
    if (v->opos >= v->out_len) return -6;
    v->out[v->opos++] = b;
    v->crc = crc16_update(v->crc, b);
    return 0;
}

static void ror_w(uint16_t *x)
{
    *x = (uint16_t)((*x >> 1) | (*x << 15));
}

static int decode_match_count(rnc2_t *v, uint32_t *count_out)
{
    uint32_t b;
    int rc;

    if ((rc = input_bits(v, 1, &b))) return rc;
    uint32_t count = b + 4;

    if ((rc = input_bits(v, 1, &b))) return rc;
    if (b) {
        uint32_t c;
        if ((rc = input_bits(v, 1, &c))) return rc;
        count = ((count - 1) << 1) + c;
    }

    *count_out = count;
    return 0;
}

static int decode_match_offset(rnc2_t *v, uint32_t *offset_out)
{
    uint32_t b;
    int rc;
    uint32_t offset = 0;

    if ((rc = input_bits(v, 1, &b))) return rc;
    if (b) {
        if ((rc = input_bits(v, 1, &offset))) return rc;

        if ((rc = input_bits(v, 1, &b))) return rc;
        if (b) {
            uint32_t c;
            if ((rc = input_bits(v, 1, &c))) return rc;
            offset = ((offset << 1) | c) | 4;

            if ((rc = input_bits(v, 1, &b))) return rc;
            if (!b) {
                if ((rc = input_bits(v, 1, &c))) return rc;
                offset = (offset << 1) | c;
            }
        } else if (!offset) {
            if ((rc = input_bits(v, 1, &b))) return rc;
            offset = b + 2;
        }
    }

    uint8_t lo;
    if ((rc = read_byte(v, &lo))) return rc;
    *offset_out = ((offset << 8) | lo) + 1;
    return 0;
}

static int copy_match(rnc2_t *v, uint32_t count, uint32_t offset)
{
    if (offset > v->opos) return -6;
    while (count--) {
        uint8_t b = v->out[v->opos - offset];
        int rc = emit(v, b);
        if (rc) return rc;
    }
    return 0;
}

static int unpack_data_m2(rnc2_t *v, uint32_t unpacked_size)
{
    uint32_t processed = 0;

    while (processed < unpacked_size) {
        for (;;) {
            uint32_t b;
            int rc = input_bits(v, 1, &b);
            if (rc) return rc;

            if (!b) {
                uint8_t lit;
                if ((rc = read_byte(v, &lit))) return rc;
                if ((rc = emit(v, (uint8_t)(v->enc_key ^ lit)))) return rc;
                ror_w(&v->enc_key);
                processed++;
            } else {
                if ((rc = input_bits(v, 1, &b))) return rc;
                if (b) {
                    uint32_t count, offset;

                    if ((rc = input_bits(v, 1, &b))) return rc;
                    if (b) {
                        if ((rc = input_bits(v, 1, &b))) return rc;
                        if (b) {
                            uint8_t raw;
                            if ((rc = read_byte(v, &raw))) return rc;
                            count = (uint32_t)raw + 8;
                            if (count == 8) {
                                /* end-of-chunk marker; one pad bit */
                                if ((rc = input_bits(v, 1, &b))) return rc;
                                break;
                            }
                        } else {
                            count = 3;
                        }
                        if ((rc = decode_match_offset(v, &offset))) return rc;
                    } else {
                        uint8_t raw;
                        count = 2;
                        if ((rc = read_byte(v, &raw))) return rc;
                        offset = (uint32_t)raw + 1;
                    }

                    processed += count;
                    if ((rc = copy_match(v, count, offset))) return rc;
                } else {
                    uint32_t count;
                    if ((rc = decode_match_count(v, &count))) return rc;

                    if (count != 9) {
                        uint32_t offset;
                        if ((rc = decode_match_offset(v, &offset))) return rc;
                        processed += count;
                        if ((rc = copy_match(v, count, offset))) return rc;
                    } else {
                        /* raw run of (bits(4) << 2) + 12 literal bytes */
                        uint32_t len;
                        if ((rc = input_bits(v, 4, &len))) return rc;
                        len = (len << 2) + 12;
                        processed += len;
                        while (len--) {
                            uint8_t lit;
                            if ((rc = read_byte(v, &lit))) return rc;
                            if ((rc = emit(v, (uint8_t)(v->enc_key ^ lit))))
                                return rc;
                        }
                        ror_w(&v->enc_key);
                    }
                }
            }
        }
    }

    return 0;
}

int rnc2_unpack(const uint8_t *in, uint8_t *out, uint32_t *out_size)
{
    if (memcmp(in, "RNC\x02", 4) != 0) return -1;

    uint32_t unpacked = ((uint32_t)in[4] << 24) | ((uint32_t)in[5] << 16) |
                        ((uint32_t)in[6] << 8)  | (uint32_t)in[7];
    uint32_t packed = ((uint32_t)in[8] << 24) | ((uint32_t)in[9] << 16) |
                      ((uint32_t)in[10] << 8) | (uint32_t)in[11];
    uint16_t unpacked_crc = (uint16_t)((in[12] << 8) | in[13]);
    uint16_t packed_crc = (uint16_t)((in[14] << 8) | in[15]);
    /* in[16] = leeway, in[17] = chunk count; informational only */

    const uint8_t *body = in + RNC2_HEADER_SIZE;

    uint16_t crc = 0;
    for (uint32_t i = 0; i < packed; i++)
        crc = crc16_update(crc, body[i]);
    if (crc != packed_crc) return -4;

    rnc2_t v;
    memset(&v, 0, sizeof(v));
    v.src = body;
    v.src_len = packed;
    v.out = out;
    v.out_len = unpacked;

    uint32_t flag;
    int rc;
    if ((rc = input_bits(&v, 1, &flag))) return rc;  /* "already packed" */
    if ((rc = input_bits(&v, 1, &flag))) return rc;  /* key present */
    if (flag) return -2;                             /* keyed stream: unsupported */

    if ((rc = unpack_data_m2(&v, unpacked))) return rc;

    if (v.crc != unpacked_crc) return -5;

    *out_size = unpacked;
    return 0;
}
