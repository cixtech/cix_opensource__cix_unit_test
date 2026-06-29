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

#include <string.h>
#include <time.h>
#include <stdio.h>
#include "test_common.h"

int format_supported(const IMG_FORMAT* formats, IMG_FORMAT fmt)
{
    int i;
    for (i = 0; i < 32 && formats[i] != CME_FORMAT_UNKNOWN; i++) {
        if (formats[i] == fmt)
            return 1;
    }
    return 0;
}

IMG_FORMAT format_str_to_enum(const char* str)
{
    if (strcmp(str, "nv12") == 0) return CME_FORMAT_NV12;
    if (strcmp(str, "nv21") == 0) return CME_FORMAT_NV21;
    if (strcmp(str, "yuyv") == 0) return CME_FORMAT_YUYV_422;
    if (strcmp(str, "rgb24") == 0 || strcmp(str, "rgb888") == 0) return CME_FORMAT_RGB_888;
    if (strcmp(str, "bgr24") == 0 || strcmp(str, "bgr888") == 0) return CME_FORMAT_BGR_888;
    if (strcmp(str, "rgba") == 0 || strcmp(str, "rgba8888") == 0) return CME_FORMAT_RGBA_8888;
    if (strcmp(str, "abgr") == 0 || strcmp(str, "abgr8888") == 0) return CME_FORMAT_ABGR_8888;
    if (strcmp(str, "bgra") == 0 || strcmp(str, "bgra8888") == 0) return CME_FORMAT_BGRA_8888;
    if (strcmp(str, "argb") == 0 || strcmp(str, "argb8888") == 0) return CME_FORMAT_ARGB_8888;
    if (strcmp(str, "p010") == 0 || strcmp(str, "p010le") == 0) return CME_FORMAT_P010;
    return CME_FORMAT_UNKNOWN;
}

int parse_flip_mode(const char* str, int* out_mode)
{
    if (strcmp(str, "none") == 0) { *out_mode = CME_HAL_TRANSFORM_FLIP_NONE; return 0; }
    if (strcmp(str, "h") == 0)   { *out_mode = CME_HAL_TRANSFORM_FLIP_H;   return 0; }
    if (strcmp(str, "v") == 0)   { *out_mode = CME_HAL_TRANSFORM_FLIP_V;   return 0; }
    if (strcmp(str, "h_v") == 0 || strcmp(str, "hv") == 0) { *out_mode = CME_HAL_TRANSFORM_FLIP_H_V; return 0; }
    return -1;
}

int parse_cvtcolor_mode(const char* str, int* out_mode)
{
    if (strcmp(str, "bt601_limit") == 0) { *out_mode = CME_CS_BT601_LIMIT; return 0; }
    if (strcmp(str, "bt601_full") == 0)  { *out_mode = CME_CS_BT601_FULL;  return 0; }
    if (strcmp(str, "bt709_limit") == 0) { *out_mode = CME_CS_BT709_LIMIT; return 0; }
    if (strcmp(str, "bt709_full") == 0)  { *out_mode = CME_CS_BT709_FULL;  return 0; }
    if (strcmp(str, "default") == 0)     { *out_mode = CME_COLOR_SPACE_DEFAULT; return 0; }
    return -1;
}

unsigned long get_mono_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void read_data_from_file(cme_img* img, FILE* file)
{
    int row;
    char* p0 = (char*)img->vir_addr[0];
    char* p1 = (char*)img->vir_addr[1];
    switch (img->format) {
        case CME_FORMAT_RGB_888:
        case CME_FORMAT_BGR_888:
            for (row = 0; row < img->height; row++)
                 fread(p0 + (size_t)img->stride[0][0] * row, 1, (size_t)img->width*3, file);
            break;
        case CME_FORMAT_RGBA_8888:
        case CME_FORMAT_ARGB_8888:
        case CME_FORMAT_BGRA_8888:
        case CME_FORMAT_ABGR_8888:
            for (row = 0; row < img->height; row++)
                 fread(p0 + (size_t)img->stride[0][0] * row, 1, (size_t)img->width*4, file);
            break;
        case CME_FORMAT_NV12:
        case CME_FORMAT_NV21:
            for (row = 0; row < img->height; row++)
                 fread(p0 + (size_t)img->stride[0][0] * row, 1, (size_t)img->width, file);
            for (row = 0; row < img->height / 2; row++)
                 fread(p1 + (size_t)img->stride[1][0] * row, 1, (size_t)img->width, file);
            break;
        case CME_FORMAT_YUYV_422:
            for (row = 0; row < img->height; row++)
                 fread(p0 + (size_t)img->stride[0][0] * row, 1, (size_t)img->width*2, file);
            break;
        case CME_FORMAT_P010:
            for (row = 0; row < img->height; row++)
                 fread(p0 + (size_t)img->stride[0][0] * row, 1, (size_t)img->width*2, file);
            for (row = 0; row < img->height / 2; row++)
                 fread(p1 + (size_t)img->stride[1][0] * row, 1, (size_t)img->width*2, file);
            break;
        default:
            fread(img->vir_addr[0], 1, (size_t)img->tlength, file);
            break;
    }
}

void write_data_to_file(cme_img* img, FILE* file)
{
    int row;
    char* p0 = (char*)img->vir_addr[0];
    char* p1 = (char*)img->vir_addr[1];
    switch (img->format) {
        case CME_FORMAT_RGB_888:
        case CME_FORMAT_BGR_888:
            for (row = 0; row < img->height; row++)
                 fwrite(p0 + (size_t)img->stride[0][0] * row, 1, (size_t)img->width*3, file);
            break;
        case CME_FORMAT_RGBA_8888:
        case CME_FORMAT_ARGB_8888:
        case CME_FORMAT_BGRA_8888:
        case CME_FORMAT_ABGR_8888:
            for (row = 0; row < img->height; row++)
                 fwrite(p0 + (size_t)img->stride[0][0] * row, 1, (size_t)img->width*4, file);
            break;
        case CME_FORMAT_NV12:
        case CME_FORMAT_NV21:
            for (row = 0; row < img->height; row++)
                 fwrite(p0 + (size_t)img->stride[0][0] * row, 1, (size_t)img->width, file);
            for (row = 0; row < img->height / 2; row++)
                 fwrite(p1 + (size_t)img->stride[1][0] * row, 1, (size_t)img->width, file);
            break;
        case CME_FORMAT_YUYV_422:
            for (row = 0; row < img->height; row++)
                 fwrite(p0 + (size_t)img->stride[0][0] * row, 1, (size_t)img->width*2, file);
            break;
        case CME_FORMAT_P010:
            for (row = 0; row < img->height; row++)
                 fwrite(p0 + (size_t)img->stride[0][0] * row, 1, (size_t)img->width*2, file);
            for (row = 0; row < img->height / 2; row++)
                 fwrite(p1 + (size_t)img->stride[1][0] * row, 1, (size_t)img->width*2, file);
            break;
        default:
            fwrite(img->vir_addr[0], 1, (size_t)img->tlength, file);
            break;
    }
}
