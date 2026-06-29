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
#include "test_cvtcolor.h"

extern char* optarg;

static void cvtcolor_usage(const char* prog)
{
    fprintf(stderr, "Usage: %s cvtcolor -i <input> -w <width> -h <height> -f <src_format> -F <dst_format> -m <mode> [-o <output>]\n", prog);
    fprintf(stderr, "  -i   input file (required)\n");
    fprintf(stderr, "  -w   width (required)\n");
    fprintf(stderr, "  -h   height (required)\n");
    fprintf(stderr, "  -f   source format: nv12, yuyv, rgb24, rgba (required)\n");
    fprintf(stderr, "  -F   destination format (required)\n");
    fprintf(stderr, "  -m   mode: default, bt601_limit, bt601_full, bt709_limit, bt709_full (required)\n");
    fprintf(stderr, "  -o   dump destination to file (optional)\n");
    fprintf(stderr, "  -n   API test round (default 1)\n");
}

int test_cvtcolor(int argc, char** argv)
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
    char* src_fmt_str = NULL;
    char* dst_fmt_str = NULL;
    char* mode_str = NULL;
    IMG_FORMAT src_fmt, dst_fmt;
    int cvt_mode = CME_COLOR_SPACE_DEFAULT;
    FILE* fin = NULL;
    FILE* fout = NULL;
    int n_round = 1;
    double total_time = 0.0;

    while ((c = getopt(argc, argv, "i:w:h:f:F:m:o:n:")) != -1) {
        switch (c) {
        case 'i': input_file = optarg; break;
        case 'w': w = atoi(optarg); break;
        case 'h': h = atoi(optarg); break;
        case 'f': src_fmt_str = optarg; break;
        case 'F': dst_fmt_str = optarg; break;
        case 'm': mode_str = optarg; break;
        case 'o': output_file = optarg; break;
        case 'n': n_round = atoi(optarg); break;
        default:
            cvtcolor_usage(argv[0] ? argv[0] : "test_cme");
            return EXIT_FAILURE;
        }
    }

    if (!input_file || w <= 0 || h <= 0 || !src_fmt_str || !dst_fmt_str || !mode_str) {
        fprintf(stderr, "test_cvtcolor: missing -i -w -h -f -F -m\n");
        cvtcolor_usage(argv[0] ? argv[0] : "test_cme");
        return EXIT_FAILURE;
    }

    src_fmt = format_str_to_enum(src_fmt_str);
    dst_fmt = format_str_to_enum(dst_fmt_str);
    if (src_fmt == CME_FORMAT_UNKNOWN || dst_fmt == CME_FORMAT_UNKNOWN) {
        fprintf(stderr, "test_cvtcolor: unknown format\n");
        return EXIT_FAILURE;
    }
    if (parse_cvtcolor_mode(mode_str, &cvt_mode) != 0) {
        fprintf(stderr, "test_cvtcolor: unknown mode '%s'\n", mode_str);
        return EXIT_FAILURE;
    }

    cap = query_all_capability();
    if (!cap) {
        fprintf(stderr, "test_cvtcolor: query_all_capability() returned NULL\n");
        return EXIT_FAILURE;
    }
    if (!format_supported(cap->supported_input_formats, src_fmt) ||
        !format_supported(cap->supported_output_formats, dst_fmt)) {
        fprintf(stderr, "test_cvtcolor: format not supported\n");
        return EXIT_FAILURE;
    }

    memset(&src_ctrl, 0, sizeof(src_ctrl));
    src_ctrl.width = w;
    src_ctrl.height = h;
    src_ctrl.format = src_fmt;
    src_ctrl.ifconherent = 0;
    src_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;

    memset(&dst_ctrl, 0, sizeof(dst_ctrl));
    dst_ctrl.width = w;
    dst_ctrl.height = h;
    dst_ctrl.format = dst_fmt;
    dst_ctrl.ifconherent = 0;
    dst_ctrl.colormode = cvt_mode;

    src = alloc_cme_img(&src_ctrl, &error);
    if (error || !src) {
        fprintf(stderr, "test_cvtcolor: alloc_cme_img(src) failed\n");
        return EXIT_FAILURE;
    }
    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !dst) {
        fprintf(stderr, "test_cvtcolor: alloc_cme_img(dst) failed\n");
        free_cme_img(src);
        return EXIT_FAILURE;
    }

    fin = fopen(input_file, "rb");
    if (!fin) {
        fprintf(stderr, "test_cvtcolor: cannot open '%s'\n", input_file);
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);
    fin = NULL;

    for (int i = 0; i < n_round; i++) {
        unsigned long start = get_mono_time_ns();
        ret = cme_2d_cvtcolor(src, dst, cvt_mode, 1, NULL);
        unsigned long end = get_mono_time_ns();
        if (end < start) {
            fprintf(stderr, "test_cvtcolor: time error\n");
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        total_time += (end - start) / 1000000.0;
        printf("test_cvtcolor: cme_2d_cvtcolor time: %lu ns (%.3f ms)\n", end - start, (end - start) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_cvtcolor: cme_2d_cvtcolor returned %d\n", (int)ret);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_cvtcolor: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_cvtcolor: OK (%dx%d %s -> %s mode=%s)\n", w, h, src_fmt_str, dst_fmt_str, mode_str);

    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_cvtcolor: cannot open output '%s'\n", output_file);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_cvtcolor: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }

    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}
