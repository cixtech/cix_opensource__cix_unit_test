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

#ifndef BITSTREAM_H
#define BITSTREAM_H

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>

typedef struct {
    const uint8_t *buffer;
    int bit_offset;
    int buffer_size;
} BitstreamParser;

void init_bitstream_parser(BitstreamParser *parser, const uint8_t *buffer, int size);
unsigned int get_bits(BitstreamParser *parser, int bits);
void skip_bits(BitstreamParser *parser, int bits);
unsigned int get_ue_golomb(BitstreamParser *parser);
int get_se_golomb(BitstreamParser *parser);
int get_bits_count(BitstreamParser *parser);
int ceil_log2(uint32_t x);

#endif /* BITSTREAM_H */
