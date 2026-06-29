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
#include "test_overlay.h"

extern char* optarg;

#define OVERLAY_MAX_LAYERS 4

static void overlay_usage(const char* prog)
{
    fprintf(stderr, "Usage: %s overlay -i <base_input> -I <overlay_input> -w <width> -h <height> -f <format> [-a alpha0] [-A alpha1] [-o <output>]\n", prog);
    fprintf(stderr, "  -i   base layer input file (required)\n");
    fprintf(stderr, "  -I   overlay layer input file (required)\n");
    fprintf(stderr, "  -w   width (base layer width required)\n");
    fprintf(stderr, "  -h   height (base layer height required)\n");
    fprintf(stderr, "  -W   width (overlay layer width required)\n");
    fprintf(stderr, "  -H   height (overlay layer height required)\n");
    fprintf(stderr, "  -l   overlay layer number (default 2)\n");
    fprintf(stderr, "  -f   format: nv12, yuyv, rgb24, rgba (base layer format required)\n");
    fprintf(stderr, "  -F   format: nv12, yuyv, rgb24, rgba (overlay layer format required)\n");
    fprintf(stderr, "  -a   alpha for layer 0 (0-65535, default 65535)\n");
    fprintf(stderr, "  -A   alpha for layer 1 (0-65535, default 65535)\n");
    fprintf(stderr, "  -x   x offset for layer 1 (default 0)\n");
    fprintf(stderr, "  -y   y offset for layer 1 (default 0)\n");
    fprintf(stderr, "  -c   use clmem (default 1)\n");
    fprintf(stderr, "  -o   dump destination to file (optional)\n");
    fprintf(stderr, "  -n   API test round (default 1)\n");
}

int test_overlay(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl;
    alloc_img_ctrl dst_ctrl;
    cme_img* src[OVERLAY_MAX_LAYERS];
    cme_img* dst = NULL;
    int alpha_arr[OVERLAY_MAX_LAYERS];
    int error = 0;
    CME_RET ret;
    int c;
    char* input_base = NULL;
    char* input_overlay = NULL;
    char* output_file = NULL;
    int w = 0, h = 0;
    int W = 0, H = 0;
    char* format_str = NULL;
    char* format_overlay_str = NULL;
    IMG_FORMAT fmt = CME_FORMAT_UNKNOWN;
    IMG_FORMAT fmt_overlay = CME_FORMAT_UNKNOWN;
    int alpha0 = 65535, alpha1 = 65535;
    FILE* fin = NULL;
    FILE* fout = NULL;
    int i;
    int nimgs = 2;
    int use_clmem = 1;
    int x = 0, y = 0;
    int n_round = 1;
    double total_time = 0.0;
    for (i = 0; i < OVERLAY_MAX_LAYERS; i++) {
        src[i] = NULL;
        alpha_arr[i] = 65535;
    }

    while ((c = getopt(argc, argv, "i:I:w:h:W:H:l:f:F:a:A:x:y:o:c:n:")) != -1) {
        switch (c) {
        case 'i': input_base = optarg; break;
        case 'I': input_overlay = optarg; break;
        case 'w': w = atoi(optarg); break;
        case 'h': h = atoi(optarg); break;
        case 'W': W = atoi(optarg); break;
        case 'H': H = atoi(optarg); break;
        case 'l': nimgs = atoi(optarg); break;
        case 'f': format_str = optarg; break;
        case 'F': format_overlay_str = optarg; break;
        case 'a': alpha0 = atoi(optarg); break;
        case 'A': alpha1 = atoi(optarg); break;
        case 'x': x = atoi(optarg); break;
        case 'y': y = atoi(optarg); break;
        case 'o': output_file = optarg; break;
        case 'c': use_clmem = atoi(optarg); break;
        case 'n': n_round = atoi(optarg); break;
        default:
            overlay_usage(argv[0] ? argv[0] : "test_cme");
            return EXIT_FAILURE;
        }
    }

    if (!input_base || !input_overlay || w <= 0 || h <= 0 || !format_str || W <= 0 || H <= 0 || !format_overlay_str) {
        fprintf(stderr, "test_overlay: missing -i -I -w -h -f -W -H -F\n");
        overlay_usage(argv[0] ? argv[0] : "test_cme");
        return EXIT_FAILURE;
    }

    if (alpha0 < 0 || alpha0 > 65535 || alpha1 < 0 || alpha1 > 65535) {
        fprintf(stderr, "test_overlay: alpha must be 0-65535\n");
        return EXIT_FAILURE;
    }

    if (nimgs <= 0 || nimgs > OVERLAY_MAX_LAYERS) {
        fprintf(stderr, "test_overlay: overlay layer number must be 1-%d\n", OVERLAY_MAX_LAYERS);
        return EXIT_FAILURE;
    }

    fmt = format_str_to_enum(format_str);
    if (fmt == CME_FORMAT_UNKNOWN) {
        fprintf(stderr, "test_overlay: unknown format '%s'\n", format_str);
        return EXIT_FAILURE;
    }

    fmt_overlay = format_str_to_enum(format_overlay_str);
    if (fmt_overlay == CME_FORMAT_UNKNOWN) {
        fprintf(stderr, "test_overlay: unknown format '%s'\n", format_overlay_str);
        return EXIT_FAILURE;
    }

    cap = query_all_capability();
    if (!cap) {
        fprintf(stderr, "test_overlay: query_all_capability() returned NULL\n");
        return EXIT_FAILURE;
    }
    if (!format_supported(cap->supported_input_formats, fmt) ||
        !format_supported(cap->supported_output_formats, fmt) ||
        !format_supported(cap->supported_input_formats, fmt_overlay)) {
        fprintf(stderr, "test_overlay: format not supported\n");
        return EXIT_FAILURE;
    }

    memset(&src_ctrl, 0, sizeof(src_ctrl));
    src_ctrl.width = w;
    src_ctrl.height = h;
    src_ctrl.format = fmt;
    src_ctrl.ifconherent = 0;
    src_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    src_ctrl.use_clmem = use_clmem;

    memset(&dst_ctrl, 0, sizeof(dst_ctrl));
    dst_ctrl.width = w;
    dst_ctrl.height = h;
    dst_ctrl.format = fmt;
    dst_ctrl.ifconherent = 0;
    dst_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    dst_ctrl.use_clmem = use_clmem;

    src[0] = alloc_cme_img(&src_ctrl, &error);
    if (error || !src[0]) {
        fprintf(stderr, "test_overlay: alloc_cme_img(src[0]) failed\n");
        return EXIT_FAILURE;
    }

    memset(&src_ctrl, 0, sizeof(src_ctrl));
    src_ctrl.width = W;
    src_ctrl.height = H;
    src_ctrl.format = fmt_overlay;
    src_ctrl.ifconherent = 0;
    src_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    src_ctrl.use_clmem = use_clmem;

    src[1] = alloc_cme_img(&src_ctrl, &error);
    if (error || !src[1]) {
        fprintf(stderr, "test_overlay: alloc_cme_img(src[1]) failed\n");
        free_cme_img(src[0]);
        return EXIT_FAILURE;
    }

    src[1]->x = x;
    src[1]->y = y;

    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !dst) {
        fprintf(stderr, "test_overlay: alloc_cme_img(dst) failed\n");
        free_cme_img(src[1]);
        free_cme_img(src[0]);
        return EXIT_FAILURE;
    }

    fin = fopen(input_base, "rb");
    if (!fin) {
        fprintf(stderr, "test_overlay: cannot open base '%s'\n", input_base);
        free_cme_img(dst);
        free_cme_img(src[1]);
        free_cme_img(src[0]);
        return EXIT_FAILURE;
    }
    read_data_from_file(src[0], fin);
    fclose(fin);
    fin = NULL;

    fin = fopen(input_overlay, "rb");
    if (!fin) {
        fprintf(stderr, "test_overlay: cannot open overlay '%s'\n", input_overlay);
        free_cme_img(dst);
        free_cme_img(src[1]);
        free_cme_img(src[0]);
        return EXIT_FAILURE;
    }
    read_data_from_file(src[1], fin);
    fclose(fin);
    fin = NULL;

    alpha_arr[0] = alpha0;
    alpha_arr[1] = alpha1;

    for (int i = 0; i < n_round; i++) {
        unsigned long start = get_mono_time_ns();
        ret = cme_2d_overlay(nimgs, src, dst, alpha_arr, 1, NULL);
        unsigned long end = get_mono_time_ns();
        if (end < start) {
            fprintf(stderr, "test_overlay: time error\n");
            free_cme_img(dst);
            free_cme_img(src[1]);
            free_cme_img(src[0]);
            return EXIT_FAILURE;
        }
        total_time += (end - start) / 1000000.0;
        printf("test_overlay: cme_2d_overlay time: %lu ns (%.3f ms)\n", end - start, (end - start) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_overlay: cme_2d_overlay returned %d\n", (int)ret);
            free_cme_img(dst);
            free_cme_img(src[1]);
            free_cme_img(src[0]);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_overlay: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_overlay: OK (%d layers %dx%d %s, overlay layer %dx%d %s, alpha %d %d)\n", nimgs, w, h, format_str, W, H, format_overlay_str, alpha0, alpha1);

    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_overlay: cannot open output '%s'\n", output_file);
            free_cme_img(dst);
            free_cme_img(src[1]);
            free_cme_img(src[0]);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_overlay: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }

    free_cme_img(dst);
    free_cme_img(src[1]);
    free_cme_img(src[0]);
    return EXIT_SUCCESS;
}
