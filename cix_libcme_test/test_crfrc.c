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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cme/cme.h>
#include "test_common.h"
#include "test_crfrc.h"

extern char* optarg;

static void crfrc_usage(const char* prog)
{
    fprintf(stderr, "Usage: %s crfrc -i <input> -w <width> -h <height> -f <src_format> -F <dst_format> -m <flip> -r <rotate> [-M <colorspace>] [-o <output>] [-W -H] [-E -X -Y -P -Q] [-c] [-n]\n", prog);
    fprintf(stderr, "  (wraps cme_2d_crop_resize_flip_rotation_cvtcolor)\n");
    fprintf(stderr, "  -i   input file (required)\n");
    fprintf(stderr, "  -w   source width (required)\n");
    fprintf(stderr, "  -h   source height (required)\n");
    fprintf(stderr, "  -f   source format: nv12, yuyv, rgb24, rgba, p010 (required)\n");
    fprintf(stderr, "  -F   destination format (required)\n");
    fprintf(stderr, "  -m   flip: none, h, v, h_v (required)\n");
    fprintf(stderr, "  -r   rotate: 0, 90, 180, 270 (required)\n");
    fprintf(stderr, "  -M   dst colorspace: default, bt601_limit, bt601_full, bt709_limit, bt709_full (default default)\n");
    fprintf(stderr, "  -o   dump destination to file (optional)\n");
    fprintf(stderr, "  -W   destination width (default 1280)\n");
    fprintf(stderr, "  -H   destination height (default 720)\n");
    fprintf(stderr, "  -E   enable crop: 1 or 0 (default 0)\n");
    fprintf(stderr, "  -X -Y -P -Q   crop rect crop_x, crop_y, crop_w, crop_h (required when -E 1)\n");
    fprintf(stderr, "  -c   use clmem (default 1)\n");
    fprintf(stderr, "  -n   API test rounds (default 1)\n");
}

static int parse_rotate(const char* str, CME_ROTATE_MODE* out)
{
    if (strcmp(str, "0") == 0) {
        *out = CME_ROTATE_0;
        return 0;
    }
    if (strcmp(str, "90") == 0) {
        *out = CME_ROTATE_90;
        return 0;
    }
    if (strcmp(str, "180") == 0) {
        *out = CME_ROTATE_180;
        return 0;
    }
    if (strcmp(str, "270") == 0) {
        *out = CME_ROTATE_270;
        return 0;
    }
    return -1;
}

int test_crfrc(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl;
    alloc_img_ctrl dst_ctrl;
    cme_img* src = NULL;
    cme_img* dst = NULL;
    int error = 0;
    CME_RET ret;
    int c;
    char* input_file = NULL;
    char* output_file = NULL;
    int src_w = 0, src_h = 0;
    int dst_w = 1280, dst_h = 720;
    char* src_fmt_str = NULL;
    char* dst_fmt_str = NULL;
    char* flip_str = NULL;
    char* rotate_str = NULL;
    char* cs_str = NULL;
    IMG_FORMAT src_fmt = CME_FORMAT_UNKNOWN;
    IMG_FORMAT dst_fmt = CME_FORMAT_UNKNOWN;
    int flip_mode = CME_HAL_TRANSFORM_FLIP_NONE;
    CME_ROTATE_MODE rotate = CME_ROTATE_0;
    int dst_colormode = CME_COLOR_SPACE_DEFAULT;
    cme_crfrc_ctrl ctrl;
    FILE* fin = NULL;
    FILE* fout = NULL;
    int use_clmem = 1;
    int n_round = 1;
    int enable_crop = 0;
    int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
    double total_time = 0.0;

    while ((c = getopt(argc, argv, "i:w:h:f:F:m:r:M:o:W:H:E:X:Y:P:Q:c:n:")) != -1) {
        switch (c) {
        case 'i': input_file = optarg; break;
        case 'w': src_w = atoi(optarg); break;
        case 'h': src_h = atoi(optarg); break;
        case 'f': src_fmt_str = optarg; break;
        case 'F': dst_fmt_str = optarg; break;
        case 'm': flip_str = optarg; break;
        case 'r': rotate_str = optarg; break;
        case 'M': cs_str = optarg; break;
        case 'o': output_file = optarg; break;
        case 'W': dst_w = atoi(optarg); break;
        case 'H': dst_h = atoi(optarg); break;
        case 'E': enable_crop = atoi(optarg); break;
        case 'X': crop_x = atoi(optarg); break;
        case 'Y': crop_y = atoi(optarg); break;
        case 'P': crop_w = atoi(optarg); break;
        case 'Q': crop_h = atoi(optarg); break;
        case 'c': use_clmem = atoi(optarg); break;
        case 'n': n_round = atoi(optarg); break;
        default:
            crfrc_usage(argv[0] ? argv[0] : "test_cme");
            return EXIT_FAILURE;
        }
    }

    if (!input_file || src_w <= 0 || src_h <= 0 || !src_fmt_str || !dst_fmt_str || !flip_str || !rotate_str) {
        fprintf(stderr, "test_crfrc: missing or invalid -i -w -h -f -F -m -r\n");
        crfrc_usage(argv[0] ? argv[0] : "test_cme");
        return EXIT_FAILURE;
    }

    src_fmt = format_str_to_enum(src_fmt_str);
    dst_fmt = format_str_to_enum(dst_fmt_str);
    if (src_fmt == CME_FORMAT_UNKNOWN || dst_fmt == CME_FORMAT_UNKNOWN) {
        fprintf(stderr, "test_crfrc: unknown format\n");
        return EXIT_FAILURE;
    }
    if (parse_flip_mode(flip_str, &flip_mode) != 0) {
        fprintf(stderr, "test_crfrc: unknown flip '%s'\n", flip_str);
        return EXIT_FAILURE;
    }
    if (parse_rotate(rotate_str, &rotate) != 0) {
        fprintf(stderr, "test_crfrc: unknown rotate '%s'\n", rotate_str);
        return EXIT_FAILURE;
    }
    if (cs_str) {
        if (parse_cvtcolor_mode(cs_str, &dst_colormode) != 0) {
            fprintf(stderr, "test_crfrc: unknown colorspace '%s'\n", cs_str);
            return EXIT_FAILURE;
        }
    }

    if (enable_crop) {
        if (crop_x < 0 || crop_y < 0 || crop_w <= 0 || crop_h <= 0 ||
            crop_x + crop_w > src_w || crop_y + crop_h > src_h) {
            fprintf(stderr, "test_crfrc: invalid crop %d,%d %dx%d for source %dx%d\n",
                    crop_x, crop_y, crop_w, crop_h, src_w, src_h);
            return EXIT_FAILURE;
        }
    }

    if (dst_w <= 0 || dst_h <= 0) {
        fprintf(stderr, "test_crfrc: invalid destination size %dx%d\n", dst_w, dst_h);
        return EXIT_FAILURE;
    }

    cap = query_all_capability();
    if (!cap) {
        fprintf(stderr, "test_crfrc: query_all_capability() returned NULL\n");
        return EXIT_FAILURE;
    }
    if (!format_supported(cap->supported_input_formats, src_fmt) ||
        !format_supported(cap->supported_output_formats, dst_fmt)) {
        fprintf(stderr, "test_crfrc: format not supported by device\n");
        return EXIT_FAILURE;
    }

    memset(&src_ctrl, 0, sizeof(src_ctrl));
    src_ctrl.width = src_w;
    src_ctrl.height = src_h;
    src_ctrl.format = src_fmt;
    src_ctrl.ifconherent = 0;
    src_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    src_ctrl.use_clmem = use_clmem;

    memset(&dst_ctrl, 0, sizeof(dst_ctrl));
    dst_ctrl.width = dst_w;
    dst_ctrl.height = dst_h;
    dst_ctrl.format = dst_fmt;
    dst_ctrl.ifconherent = 0;
    dst_ctrl.colormode = dst_colormode;
    dst_ctrl.use_clmem = use_clmem;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.enable_crop = enable_crop ? 1 : 0;
    ctrl.crop_x = crop_x;
    ctrl.crop_y = crop_y;
    ctrl.crop_w = crop_w;
    ctrl.crop_h = crop_h;
    ctrl.rotate = rotate;
    ctrl.flip_mode = (CME_FLIP_MODE)flip_mode;

    src = alloc_cme_img(&src_ctrl, &error);
    if (error || !src) {
        fprintf(stderr, "test_crfrc: alloc_cme_img(src) failed (error=%d)\n", error);
        return EXIT_FAILURE;
    }

    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !dst) {
        fprintf(stderr, "test_crfrc: alloc_cme_img(dst) failed (error=%d)\n", error);
        free_cme_img(src);
        return EXIT_FAILURE;
    }

    fin = fopen(input_file, "rb");
    if (!fin) {
        fprintf(stderr, "test_crfrc: cannot open input '%s'\n", input_file);
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);
    fin = NULL;

    for (int i = 0; i < n_round; i++) {
        unsigned long start = get_mono_time_ns();
        ret = cme_2d_crop_resize_flip_rotation_cvtcolor(src, dst, &ctrl, 1, NULL);
        unsigned long end = get_mono_time_ns();
        if (end < start) {
            fprintf(stderr, "test_crfrc: time error\n");
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        total_time += (end - start) / 1000000.0;
        printf("test_crfrc: cme_2d_crop_resize_flip_rotation_cvtcolor time: %lu ns (%.3f ms)\n",
               end - start, (end - start) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_crfrc: API returned %d\n", (int)ret);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_crfrc: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_crfrc: OK (src %dx%d %s -> dst %dx%d %s, flip=%s rotate=%s crop=%s)\n",
           src_w, src_h, src_fmt_str, dst_w, dst_h, dst_fmt_str, flip_str, rotate_str,
           enable_crop ? "on" : "off");

    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_crfrc: cannot open output '%s'\n", output_file);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        fout = NULL;
        printf("test_crfrc: dumped dst to '%s' (%d bytes)\n", output_file, dst->tlength);
    }

    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}
