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
#include "test_resize_cvtcolor_dpu.h"

#if 0
#include <string.h>
#include <unistd.h>
#include <cme/cme.h>
#include "test_common.h"

extern char* optarg;

/* Must match drm_entry.c dpu_dev_strings when AVAILABLE_DPU_DEVICES==1 */
static const char k_default_dpu[] = "14080000.disp-controller";

static void rc_dpu_usage(const char* prog)
{
    fprintf(stderr, "Usage: %s resize_cvtcolor_dpu -i <input> -w <width> -h <height> -f <src_format> -F <dst_format> [-m colorspace] [-o] [-W -H together] [-D dpu] [-c] [-n]\n", prog);
    fprintf(stderr, "  (wraps cme_2d_resize_cvtcolor_by_dpu; DPU/DRM path)\n");
    fprintf(stderr, "  -i   input file (required)\n");
    fprintf(stderr, "  -w   source width (required)\n");
    fprintf(stderr, "  -h   source height (required)\n");
    fprintf(stderr, "  -f   source format: nv12, yuyv, rgb24, rgba (required)\n");
    fprintf(stderr, "  -F   destination format (required)\n");
    fprintf(stderr, "  -m   colorspace: default, bt601_limit, bt601_full, bt709_limit, bt709_full (default default)\n");
    fprintf(stderr, "  -o   dump destination to file (optional)\n");
    fprintf(stderr, "  -W   destination width (default: same as source)\n");
    fprintf(stderr, "  -H   destination height (default: same as source)\n");
    fprintf(stderr, "  -D   DPU device string (default %s); DRM layer must recognize it\n", k_default_dpu);
    fprintf(stderr, "  -c   use clmem (default 1)\n");
    fprintf(stderr, "  -n   API test rounds (default 1)\n");
}

int test_resize_cvtcolor_dpu(int argc, char** argv)
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
    int dst_w = 0, dst_h = 0;
    char* src_fmt_str = NULL;
    char* dst_fmt_str = NULL;
    char* mode_str = NULL;
    char* dpu_override = NULL;
    const char* dpu_device = k_default_dpu;
    IMG_FORMAT src_fmt = CME_FORMAT_UNKNOWN;
    IMG_FORMAT dst_fmt = CME_FORMAT_UNKNOWN;
    int cvt_mode = CME_COLOR_SPACE_DEFAULT;
    FILE* fin = NULL;
    FILE* fout = NULL;
    int use_clmem = 1;
    int n_round = 1;
    double total_time = 0.0;
    int i;

    while ((c = getopt(argc, argv, "i:w:h:f:F:m:o:W:H:D:c:n:")) != -1) {
        switch (c) {
        case 'i': input_file = optarg; break;
        case 'w': src_w = atoi(optarg); break;
        case 'h': src_h = atoi(optarg); break;
        case 'f': src_fmt_str = optarg; break;
        case 'F': dst_fmt_str = optarg; break;
        case 'm': mode_str = optarg; break;
        case 'o': output_file = optarg; break;
        case 'W': dst_w = atoi(optarg); break;
        case 'H': dst_h = atoi(optarg); break;
        case 'D': dpu_override = optarg; break;
        case 'c': use_clmem = atoi(optarg); break;
        case 'n': n_round = atoi(optarg); break;
        default:
            rc_dpu_usage(argv[0] ? argv[0] : "test_cme");
            return EXIT_FAILURE;
        }
    }

    if (dpu_override && dpu_override[0] != '\0')
        dpu_device = dpu_override;

    if (!input_file || src_w <= 0 || src_h <= 0 || !src_fmt_str || !dst_fmt_str) {
        fprintf(stderr, "test_resize_cvtcolor_dpu: missing -i -w -h -f -F\n");
        rc_dpu_usage(argv[0] ? argv[0] : "test_cme");
        return EXIT_FAILURE;
    }

    src_fmt = format_str_to_enum(src_fmt_str);
    dst_fmt = format_str_to_enum(dst_fmt_str);
    if (src_fmt == CME_FORMAT_UNKNOWN || dst_fmt == CME_FORMAT_UNKNOWN) {
        fprintf(stderr, "test_resize_cvtcolor_dpu: unknown format\n");
        return EXIT_FAILURE;
    }

    if (mode_str == NULL) {
        cvt_mode = CME_COLOR_SPACE_DEFAULT;
    } else {
        if (parse_cvtcolor_mode(mode_str, &cvt_mode) != 0) {
            fprintf(stderr, "test_resize_cvtcolor_dpu: unknown mode '%s'\n", mode_str);
            return EXIT_FAILURE;
        }
    }

    if (dst_w <= 0 || dst_h <= 0) {
        dst_w = src_w;
        dst_h = src_h;
    }

    cap = query_all_capability();
    if (!cap) {
        fprintf(stderr, "test_resize_cvtcolor_dpu: query_all_capability() returned NULL\n");
        return EXIT_FAILURE;
    }
    if (!format_supported(cap->supported_input_formats, src_fmt) ||
        !format_supported(cap->supported_output_formats, dst_fmt)) {
        fprintf(stderr, "test_resize_cvtcolor_dpu: format not supported\n");
        return EXIT_FAILURE;
    }

    memset(&src_ctrl, 0, sizeof(src_ctrl));
    src_ctrl.width = src_w;
    src_ctrl.height = src_h;
    src_ctrl.format = src_fmt;
    src_ctrl.ifconherent = 0;
    src_ctrl.colormode = cvt_mode;
    src_ctrl.use_clmem = use_clmem;

    memset(&dst_ctrl, 0, sizeof(dst_ctrl));
    dst_ctrl.width = dst_w;
    dst_ctrl.height = dst_h;
    dst_ctrl.format = dst_fmt;
    dst_ctrl.ifconherent = 0;
    dst_ctrl.colormode = cvt_mode;
    dst_ctrl.use_clmem = use_clmem;

    src = alloc_cme_img(&src_ctrl, &error);
    if (error || !src) {
        fprintf(stderr, "test_resize_cvtcolor_dpu: alloc_cme_img(src) failed (error=%d)\n", error);
        return EXIT_FAILURE;
    }
    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !dst) {
        fprintf(stderr, "test_resize_cvtcolor_dpu: alloc_cme_img(dst) failed (error=%d)\n", error);
        free_cme_img(src);
        return EXIT_FAILURE;
    }

    fin = fopen(input_file, "rb");
    if (!fin) {
        fprintf(stderr, "test_resize_cvtcolor_dpu: cannot open input '%s'\n", input_file);
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);
    fin = NULL;

    for (i = 0; i < n_round; i++) {
        unsigned long start = get_mono_time_ns();
        ret = cme_2d_resize_cvtcolor_by_dpu(src, dst, cvt_mode, dpu_device, 1, NULL);
        unsigned long end = get_mono_time_ns();
        if (end < start) {
            fprintf(stderr, "test_resize_cvtcolor_dpu: time error\n");
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        total_time += (end - start) / 1000000.0;
        printf("test_resize_cvtcolor_dpu: cme_2d_resize_cvtcolor_by_dpu time: %lu ns (%.3f ms)\n",
               end - start, (end - start) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_resize_cvtcolor_dpu: API returned %d\n", (int)ret);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_resize_cvtcolor_dpu: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_resize_cvtcolor_dpu: OK (DPU=%s, %dx%d %s -> %dx%d %s, mode=%s)\n",
           dpu_device, src_w, src_h, src_fmt_str, dst_w, dst_h, dst_fmt_str, mode_str ? mode_str : "default");

    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_resize_cvtcolor_dpu: cannot open output '%s'\n", output_file);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_resize_cvtcolor_dpu: dumped dst to '%s' (%d bytes)\n", output_file, dst->tlength);
    }

    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}
#endif
