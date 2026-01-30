/*
 * Copyright 2025 Cix Technology Group Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include "bitstream.h"

// Initialize the bitstream parser
void init_bitstream_parser(BitstreamParser *parser, const uint8_t *buffer, int size) {
    parser->buffer = buffer;
    parser->bit_offset = 0;
    parser->buffer_size = size;
}

// Get specified number of bits from the bitstream
unsigned int get_bits(BitstreamParser *parser, int bits) {
    if (parser->bit_offset + bits > parser->buffer_size * 8) {
        return 0;
    }

    unsigned int result = 0;
    int byte_pos = parser->bit_offset / 8;
    int bit_pos = parser->bit_offset % 8;

    for (int i = 0; i < bits; i++) {
        result = (result << 1) | ((parser->buffer[byte_pos] >> (7 - bit_pos)) & 0x01);
        bit_pos++;
        if (bit_pos == 8) {
            bit_pos = 0;
            byte_pos++;
        }
    }

    parser->bit_offset += bits;
    return result;
}

// Skip specified number of bits in the bitstream
void skip_bits(BitstreamParser *parser, int bits) {
    parser->bit_offset += bits;
}

// Get unsigned Exp-Golomb coded integer
unsigned int get_ue_golomb(BitstreamParser *parser) {
    int leading_zeros = 0;

    while (get_bits(parser, 1) == 0 && parser->bit_offset < parser->buffer_size * 8) {
        leading_zeros++;
    }

    if (leading_zeros >= 32) {
        return 0;
    }

    return (1 << leading_zeros) - 1 + get_bits(parser, leading_zeros);
}

// Get signed Exp-Golomb coded integer
int get_se_golomb(BitstreamParser *parser) {
    unsigned int code_num = get_ue_golomb(parser);

    int sign = (code_num % 2 == 0) ? -1 : 1;
    int value = (code_num + 1) / 2;

    return sign * value;
}

// Get the number of bits read so far
int get_bits_count(BitstreamParser *parser) {
    return parser->bit_offset;
}

// Calculate the ceiling of log2(x)
int ceil_log2(uint32_t x) {
    if (x == 0) return 0;
    int bits = 0;
    while ((1U << bits) < x) bits++;
    return bits;
}
