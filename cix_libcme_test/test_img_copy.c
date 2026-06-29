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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cme/cme.h>
#include "test_common.h"
#include "test_img_copy.h"

extern char* optarg;

static void img_copy_usage(const char* prog)
{
    fprintf(stderr, "Usage: %s img_copy -i <input> -w <width> -h <height> -f <format> [-o <output>] [-c use_clmem] [-n rounds]\n", prog);
    fprintf(stderr, "  Wraps cme_img_copy (sync). Source and destination must match width, height, and format.\n");
    fprintf(stderr, "  -i   input file (required)\n");
    fprintf(stderr, "  -w   width (required)\n");
    fprintf(stderr, "  -h   height (required)\n");
    fprintf(stderr, "  -f   format: nv12, yuyv, rgb24, rgba, ... (required)\n");
    fprintf(stderr, "  -o   dump copy result to file (optional)\n");
    fprintf(stderr, "  -c   use clmem (default 1)\n");
    fprintf(stderr, "  -n   API test round (default 1)\n");
}

int test_img_copy(int argc, char** argv)
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
    int w = 0, h = 0;
    char* format_str = NULL;
    IMG_FORMAT fmt = CME_FORMAT_UNKNOWN;
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
            img_copy_usage(argv[0] ? argv[0] : "test_libcme");
            return EXIT_FAILURE;
        }
    }

    if (!input_file || w <= 0 || h <= 0 || !format_str) {
        fprintf(stderr, "test_img_copy: missing -i -w -h -f\n");
        img_copy_usage(argv[0] ? argv[0] : "test_libcme");
        return EXIT_FAILURE;
    }

    fmt = format_str_to_enum(format_str);
    if (fmt == CME_FORMAT_UNKNOWN) {
        fprintf(stderr, "test_img_copy: unknown format '%s'\n", format_str);
        return EXIT_FAILURE;
    }

    cap = query_all_capability();
    if (!cap) {
        fprintf(stderr, "test_img_copy: query_all_capability() returned NULL\n");
        return EXIT_FAILURE;
    }
    if (!format_supported(cap->supported_input_formats, fmt) ||
        !format_supported(cap->supported_output_formats, fmt)) {
        fprintf(stderr, "test_img_copy: format not supported for copy\n");
        return EXIT_FAILURE;
    }

    memset(&src_ctrl, 0, sizeof(src_ctrl));
    src_ctrl.width = w;
    src_ctrl.height = h;
    src_ctrl.format = fmt;
    src_ctrl.ifconherent = 0;
    src_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    src_ctrl.use_clmem = use_clmem;

    memcpy(&dst_ctrl, &src_ctrl, sizeof(dst_ctrl));

    src = alloc_cme_img(&src_ctrl, &error);
    if (error || !src) {
        fprintf(stderr, "test_img_copy: alloc_cme_img(src) failed\n");
        return EXIT_FAILURE;
    }
    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !dst) {
        fprintf(stderr, "test_img_copy: alloc_cme_img(dst) failed\n");
        free_cme_img(src);
        return EXIT_FAILURE;
    }

    fin = fopen(input_file, "rb");
    if (!fin) {
        fprintf(stderr, "test_img_copy: cannot open '%s'\n", input_file);
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);
    fin = NULL;

    for (int i = 0; i < n_round; i++) {
        unsigned long start = get_mono_time_ns();
        ret = cme_img_copy(src, dst, true, NULL);
        unsigned long end = get_mono_time_ns();
        if (end < start) {
            fprintf(stderr, "test_img_copy: time error\n");
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        total_time += (end - start) / 1000000.0;
        printf("test_img_copy: cme_img_copy time: %lu ns (%.3f ms)\n", end - start, (end - start) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_img_copy: cme_img_copy returned %d\n", (int)ret);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }

    if (n_round > 0) {
        printf("test_img_copy: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_img_copy: OK (%dx%d %s)\n", w, h, format_str);

    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_img_copy: cannot open output '%s'\n", output_file);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_img_copy: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }

    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}
