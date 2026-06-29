/*
 * Copyright 2026 Cix Technology Group Co., Ltd.
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

#ifndef __TEST_COMMON_H
#define __TEST_COMMON_H

#include "cme.h"

/* Return 1 if fmt is in the formats list, 0 otherwise. */
int format_supported(const IMG_FORMAT* formats, IMG_FORMAT fmt);

/* Parse format string (nv12, i420, yuyv, rg24, rgb24, rgb888, bgr24, bgr888, rgba, rgba8888). Returns CME_FORMAT_UNKNOWN if unknown. */
IMG_FORMAT format_str_to_enum(const char* str);

/* Parse flip mode (none, h, v, h_v, hv). Returns 0 on success, -1 on unknown. */
int parse_flip_mode(const char* str, int* out_mode);

/* Parse cvtcolor/colorspace mode (default, bt601_limit, bt601_full, bt709_limit, bt709_full). Returns 0 on success, -1 on unknown. */
int parse_cvtcolor_mode(const char* str, int* out_mode);

/* Get current time in nanoseconds since epoch. */
unsigned long get_mono_time_ns();

/* Read image data from file. */
void read_data_from_file(cme_img* img, FILE* file);

/* Write image data to file. */
void write_data_to_file(cme_img* img, FILE* file);

#endif
