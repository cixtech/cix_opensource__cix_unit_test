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
#include "test_normalize.h"

extern char* optarg;

static IMG_FORMAT normalize_format_str(const char* str)
{
    IMG_FORMAT f = format_str_to_enum(str);
    if (f != CME_FORMAT_UNKNOWN)
        return f;
    if (strcmp(str, "rgb_float") == 0 || strcmp(str, "float") == 0)
        return CME_FORMAT_RGB_FLOAT;
    return CME_FORMAT_UNKNOWN;
}

static void normalize_usage(const char* prog)
{
    fprintf(stderr, "Usage: %s normalize -i <input> -w <width> -h <height> -f <format> [-o <output>]\n", prog);
    fprintf(stderr, "  -i   input file (required)\n");
    fprintf(stderr, "  -w   width (required)\n");
    fprintf(stderr, "  -h   height (required)\n");
    fprintf(stderr, "  -f   source format: rgb24, rgb_float (required)\n");
    fprintf(stderr, "  -o   dump destination to file (optional, binary float)\n");
    fprintf(stderr, "  -c   use clmem (default 1)\n");
    fprintf(stderr, "  -n   API test round (default 1)\n");
    fprintf(stderr, "  Destination is always RGB_FLOAT. Uses multi=1/255, add=0, clamp [0,1].\n");
}

int test_normalize(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl;
    alloc_img_ctrl dst_ctrl;
    cme_img* src = NULL;
    cme_img* dst = NULL;
    cme_normalize_ctrl ctrl;
    int error = 0;
    CME_RET ret;
    int c;
    char* input_file = NULL;
    char* output_file = NULL;
    int w = 0, h = 0;
    char* format_str = NULL;
    IMG_FORMAT src_fmt = CME_FORMAT_UNKNOWN;
    FILE* fin = NULL;
    FILE* fout = NULL;
    int use_clmem = 1;
    int n_round = 1;
    double total_time = 0.0;

    while ((c = getopt(argc, argv, "i:w:h:f:o:c:n:")) != -1) {
        switch (c) {
        case 'i': input_file = optarg; break;
        case 'w': w = atoi(optarg); break;
        case 'h': h = atoi(optarg); break;
        case 'f': format_str = optarg; break;
        case 'o': output_file = optarg; break;
        case 'c': use_clmem = atoi(optarg); break;
        case 'n': n_round = atoi(optarg); break;
        default:
            normalize_usage(argv[0] ? argv[0] : "test_cme");
            return EXIT_FAILURE;
        }
    }

    if (!input_file || w <= 0 || h <= 0 || !format_str) {
        fprintf(stderr, "test_normalize: missing -i -w -h -f\n");
        normalize_usage(argv[0] ? argv[0] : "test_cme");
        return EXIT_FAILURE;
    }

    src_fmt = normalize_format_str(format_str);
    if (src_fmt != CME_FORMAT_RGB_888 && src_fmt != CME_FORMAT_RGB_FLOAT) {
        fprintf(stderr, "test_normalize: format must be rgb24 or rgb_float (got '%s')\n", format_str);
        return EXIT_FAILURE;
    }

    cap = query_all_capability();
    if (!cap) {
        fprintf(stderr, "test_normalize: query_all_capability() returned NULL\n");
        return EXIT_FAILURE;
    }
    if (!format_supported(cap->supported_input_formats, src_fmt)) {
        fprintf(stderr, "test_normalize: source format not supported\n");
        return EXIT_FAILURE;
    }
    if (!format_supported(cap->supported_output_formats, CME_FORMAT_RGB_FLOAT)) {
        fprintf(stderr, "test_normalize: RGB_FLOAT output not supported\n");
        return EXIT_FAILURE;
    }

    memset(&src_ctrl, 0, sizeof(src_ctrl));
    src_ctrl.width = w;
    src_ctrl.height = h;
    src_ctrl.format = src_fmt;
    src_ctrl.ifconherent = 0;
    src_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    src_ctrl.use_clmem = use_clmem;

    memset(&dst_ctrl, 0, sizeof(dst_ctrl));
    dst_ctrl.width = w;
    dst_ctrl.height = h;
    dst_ctrl.format = CME_FORMAT_RGB_FLOAT;
    dst_ctrl.ifconherent = 0;
    dst_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    dst_ctrl.use_clmem = use_clmem;

    src = alloc_cme_img(&src_ctrl, &error);
    if (error || !src) {
        fprintf(stderr, "test_normalize: alloc_cme_img(src) failed\n");
        return EXIT_FAILURE;
    }
    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !dst) {
        fprintf(stderr, "test_normalize: alloc_cme_img(dst) failed\n");
        free_cme_img(src);
        return EXIT_FAILURE;
    }

    /* Typical normalize: pixel * multi_factor + add_factor, clamp to [min,max]. For uint8->[0,1]: multi=1/255, add=0, clamp 0..1 */
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.multi_factor = 1.0f / 255.0f;
    ctrl.add_factor = 0.0f;
    ctrl.max = 1.0f;
    ctrl.min = 0.0f;
    ctrl.enable_clamp = 1;

    fin = fopen(input_file, "rb");
    if (!fin) {
        fprintf(stderr, "test_normalize: cannot open '%s'\n", input_file);
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);
    fin = NULL;

    for (int i = 0; i < n_round; i++) {
        unsigned long start = get_mono_time_ns();
        ret = cme_2d_normalize(src, dst, &ctrl, 1, NULL);
        unsigned long end = get_mono_time_ns();
        if (end < start) {
            fprintf(stderr, "test_normalize: time error\n");
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        total_time += (end - start) / 1000000.0;
        printf("test_normalize: cme_2d_normalize time: %lu ns (%.3f ms)\n", end - start, (end - start) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_normalize: cme_2d_normalize returned %d\n", (int)ret);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_normalize: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_normalize: OK (%dx%d %s -> RGB_FLOAT)\n", w, h, format_str);

    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_normalize: cannot open output '%s'\n", output_file);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_normalize: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }

    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}
