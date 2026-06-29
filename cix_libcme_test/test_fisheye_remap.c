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
#include "test_fisheye_remap.h"

extern char* optarg;

/**
 * Allocates and fills identity remap maps for fisheye_remap.
 * map_x[y*width+x] = x, map_y[y*width+x] = y.
 * Returns 0 on success, -1 on allocation failure; on success *out_map_x and
 * *out_map_y must be freed by the caller.
 */
static int generate_remap_maps(int width, int height, float** out_map_x, float** out_map_y)
{
    size_t map_size;
    float* map_x;
    float* map_y;
    int i, j;

    if (width <= 0 || height <= 0 || !out_map_x || !out_map_y) {
        *out_map_x = NULL;
        *out_map_y = NULL;
        return -1;
    }
    *out_map_x = NULL;
    *out_map_y = NULL;

    map_size = (size_t)width * (size_t)height * sizeof(float);
    map_x = (float*)malloc(map_size);
    map_y = (float*)malloc(map_size);
    if (!map_x || !map_y) {
        free(map_x);
        free(map_y);
        return -1;
    }

    for (j = 0; j < height; j++) {
        for (i = 0; i < width; i++) {
            map_x[j * width + i] = (float)i;
            map_y[j * width + i] = (float)j;
        }
    }

    *out_map_x = map_x;
    *out_map_y = map_y;
    return 0;
}

static void fisheye_remap_usage(const char* prog)
{
    fprintf(stderr, "Usage: %s fisheye_remap -i <input> -w <width> -h <height> -f <format> [-o <output>] [-W <dst_w>] [-H <dst_h>]\n", prog);
    fprintf(stderr, "  -i   input file (required)\n");
    fprintf(stderr, "  -w   source width (required)\n");
    fprintf(stderr, "  -h   source height (required)\n");
    fprintf(stderr, "  -f   source format: nv12, yuyv, rgb24, rgba (required)\n");
    fprintf(stderr, "  -o   dump destination to file (optional)\n");
    fprintf(stderr, "  -W   destination width (default 1920)\n");
    fprintf(stderr, "  -H   destination height (default 1080)\n");
    fprintf(stderr, "  -c   use clmem (default 1)\n");
    fprintf(stderr, "  -s   scale (default 1.0)\n");
    fprintf(stderr, "  -e   enable sharpen (default 1)\n");
    fprintf(stderr, "  -k   enable 5x5 kernel (default 1)\n");
    fprintf(stderr, "  -l   enable linear sample (default 1)\n");
    fprintf(stderr, "  -a   amount (default 0.0)\n");
    fprintf(stderr, "  -x   map x file (binary file, x float values, default:original image)\n");
    fprintf(stderr, "  -y   map y file (binary file, y float values, default:original image)\n");
    fprintf(stderr, "  -n   API test round (default 1)\n");
    fprintf(stderr, "  Destination is always RGB888. src size must be >= dst size.\n");
}

int test_fisheye_remap(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl;
    alloc_img_ctrl dst_ctrl;
    cme_img* src = NULL;
    cme_img* dst = NULL;
    cme_img* tmp = NULL;
    cme_fisheye_remap_ctrl ctrl;
    float* map_x = NULL;
    float* map_y = NULL;
    int error = 0;
    CME_RET ret;
    int c;
    char* input_file = NULL;
    char* output_file = NULL;
    char* map_x_file = NULL;
    char* map_y_file = NULL;
    int src_w = 0, src_h = 0;
    int dst_w = 1920, dst_h = 1080;
    char* format_str = NULL;
    IMG_FORMAT src_fmt = CME_FORMAT_UNKNOWN;
    FILE* fin = NULL;
    FILE* fout = NULL;
    int use_clmem = 1;
    size_t n;
    float scale = 1.0f;
    int en_sharpen = 1;
    int en_5x5_kernel = 1;
    int en_linear_sample = 1;
    float amount = 0.0f;
    int n_round = 1;
    double total_time = 0.0;

    while ((c = getopt(argc, argv, "i:w:h:f:o:W:H:c:s:e:k:l:a:x:y:n:")) != -1) {
        switch (c) {
        case 'i': input_file = optarg; break;
        case 'w': src_w = atoi(optarg); break;
        case 'h': src_h = atoi(optarg); break;
        case 'f': format_str = optarg; break;
        case 'o': output_file = optarg; break;
        case 'W': dst_w = atoi(optarg); break;
        case 'H': dst_h = atoi(optarg); break;
        case 'c': use_clmem = atoi(optarg); break;
        case 's': scale = atof(optarg); break;
        case 'e': en_sharpen = atoi(optarg); break;
        case 'k': en_5x5_kernel = atoi(optarg); break;
        case 'l': en_linear_sample = atoi(optarg); break;
        case 'a': amount = atof(optarg); break;
        case 'x': map_x_file = optarg; break;
        case 'y': map_y_file = optarg; break;
        case 'n': n_round = atoi(optarg); break;
        default:
            fisheye_remap_usage(argv[0] ? argv[0] : "test_cme");
            return EXIT_FAILURE;
        }
    }

    if (!input_file || src_w <= 0 || src_h <= 0 || !format_str) {
        fprintf(stderr, "test_fisheye_remap: missing -i -w -h -f\n");
        fisheye_remap_usage(argv[0] ? argv[0] : "test_cme");
        return EXIT_FAILURE;
    }

    if (src_w < dst_w || src_h < dst_h) {
        fprintf(stderr, "test_fisheye_remap: src size must be >= dst size\n");
        return EXIT_FAILURE;
    }

    src_fmt = format_str_to_enum(format_str);
    if (src_fmt == CME_FORMAT_UNKNOWN) {
        fprintf(stderr, "test_fisheye_remap: unknown format '%s'\n", format_str);
        return EXIT_FAILURE;
    }

    cap = query_all_capability();
    if (!cap) {
        fprintf(stderr, "test_fisheye_remap: query_all_capability() returned NULL\n");
        return EXIT_FAILURE;
    }
    if (!format_supported(cap->supported_input_formats, src_fmt)) {
        fprintf(stderr, "test_fisheye_remap: input format not supported\n");
        return EXIT_FAILURE;
    }
    if (!format_supported(cap->supported_output_formats, CME_FORMAT_RGB_888)) {
        fprintf(stderr, "test_fisheye_remap: RGB_888 output not supported\n");
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
    dst_ctrl.format = CME_FORMAT_RGB_888;
    dst_ctrl.ifconherent = 0;
    dst_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    dst_ctrl.use_clmem = use_clmem;

    src = alloc_cme_img(&src_ctrl, &error);
    if (error || !src) {
        fprintf(stderr, "test_fisheye_remap: alloc_cme_img(src) failed\n");
        return EXIT_FAILURE;
    }
    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !dst) {
        fprintf(stderr, "test_fisheye_remap: alloc_cme_img(dst) failed\n");
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    tmp = alloc_cme_img(&dst_ctrl, &error);
    if (error || !tmp) {
        fprintf(stderr, "test_fisheye_remap: alloc_cme_img(tmp) failed\n");
        free_cme_img(src);
        free_cme_img(dst);
        return EXIT_FAILURE;
    }

    fin = fopen(input_file, "rb");
    if (!fin) {
        fprintf(stderr, "test_fisheye_remap: cannot open '%s'\n", input_file);
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);
    fin = NULL;

    size_t map_size = (size_t)src_w * (size_t)src_h * sizeof(float);
    FILE* fx = NULL;
    FILE* fy = NULL;

    if (map_x_file) {
        map_x = (float*)malloc(map_size);
        if (!map_x) {
            fprintf(stderr, "test_fisheye_remap: alloc map_x failed\n");
            free_cme_img(tmp);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        fx = fopen(map_x_file, "rb");
        if (!fx) {
            fprintf(stderr, "test_fisheye_remap: cannot open '%s'\n", map_x_file);
            free(map_x);
            map_x = NULL;
            free_cme_img(tmp);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        n = fread(map_x, 1, map_size, fx);
        fclose(fx);
        fx = NULL;
        if (n != map_size) {
            fprintf(stderr, "test_fisheye_remap: map_x read %zu bytes, expected %zu\n", n, map_size);
            free(map_x);
            map_x = NULL;
            free_cme_img(tmp);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (map_y_file) {
        map_y = (float*)malloc(map_size);
        if (!map_y) {
            fprintf(stderr, "test_fisheye_remap: alloc map_y failed\n");
            free(map_x);
            free_cme_img(tmp);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        fy = fopen(map_y_file, "rb");
        if (!fy) {
            fprintf(stderr, "test_fisheye_remap: cannot open '%s'\n", map_y_file);
            free(map_y);
            map_y = NULL;
            free(map_x);
            free_cme_img(tmp);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        n = fread(map_y, 1, map_size, fy);
        fclose(fy);
        fy = NULL;
        if (n != map_size) {
            fprintf(stderr, "test_fisheye_remap: map_y read %zu bytes, expected %zu\n", n, map_size);
            free(map_y);
            map_y = NULL;
            free(map_x);
            free_cme_img(tmp);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    /* If no map files, or only one map file, generate the missing map(s). */
    if (!map_x || !map_y) {
        float* gen_x = NULL;
        float* gen_y = NULL;
        if (generate_remap_maps(src_w, src_h, &gen_x, &gen_y) != 0) {
            fprintf(stderr, "test_fisheye_remap: alloc map failed\n");
            free(map_x);
            free(map_y);
            free_cme_img(tmp);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        if (!map_x)
            map_x = gen_x;
        else
            free(gen_x);
        if (!map_y)
            map_y = gen_y;
        else
            free(gen_y);
    }

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.map_x = map_x;
    ctrl.map_y = map_y;
    ctrl.scale = scale;
    ctrl.en_linear_sample = en_linear_sample;
    ctrl.en_sharpen = en_sharpen;
    ctrl.en_5x5_kernel = en_5x5_kernel;
    ctrl.amount = amount;

    for (int i = 0; i < n_round; i++) {
        unsigned long start = get_mono_time_ns();
        ret = cme_2d_fisheye_remap(src, dst, tmp, &ctrl, 1, NULL);
        unsigned long end = get_mono_time_ns();
        if (end < start) {
            fprintf(stderr, "test_fisheye_remap: time error\n");
            free_cme_img(tmp);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        total_time += (end - start) / 1000000.0;
        printf("test_fisheye_remap: cme_2d_fisheye_remap time: %lu ns (%.3f ms)\n", end - start, (end - start) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_fisheye_remap: cme_2d_fisheye_remap returned %d\n", (int)ret);
            free_cme_img(tmp);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_fisheye_remap: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_fisheye_remap: OK (%dx%d %s -> %dx%d RGB888)\n", src_w, src_h, format_str, dst_w, dst_h);
    free(map_y);
    free(map_x);

    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_fisheye_remap: cannot open output '%s'\n", output_file);
            free_cme_img(tmp);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_fisheye_remap: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }

    free_cme_img(tmp);
    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}
