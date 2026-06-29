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
#include "test_draw_rectangle.h"

extern char* optarg;

static void draw_rect_usage(const char* prog)
{
    fprintf(stderr, "Usage: %s draw_rectangle -i <input> -w <width> -h <height> -f <format> -x <rx> -y <ry> -p <rw> -q <rh> -R <r> -G <g> -B <b> -t <thickness> [-o] [-c] [-n]\n", prog);
    fprintf(stderr, "  (wraps cme_2d_draw_rectangle; RGB888/BGR888 only)\n");
    fprintf(stderr, "  -i   input image (required)\n");
    fprintf(stderr, "  -w   image width (required)\n");
    fprintf(stderr, "  -h   image height (required)\n");
    fprintf(stderr, "  -f   format: rgb24 or bgr24 (required)\n");
    fprintf(stderr, "  -x -y -p -q   rectangle x, y, width, height (required)\n");
    fprintf(stderr, "  -R -G -B   border color 0..255 (required)\n");
    fprintf(stderr, "  -t   line thickness in pixels, >0 (required)\n");
    fprintf(stderr, "  -o   dump result to file (optional)\n");
    fprintf(stderr, "  -c   use clmem (default 1)\n");
    fprintf(stderr, "  -n   API rounds (default 1)\n");
}

int test_draw_rectangle(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl dst_ctrl;
    cme_img* dst = NULL;
    int error = 0;
    CME_RET ret;
    int c;
    char* input_file = NULL;
    char* output_file = NULL;
    int w = 0, h = 0;
    char* fmt_str = NULL;
    IMG_FORMAT fmt = CME_FORMAT_UNKNOWN;
    int rx = 0, ry = 0, rw = 0, rh = 0;
    int R = -1, G = -1, B = -1;
    int thickness = 0;
    int use_clmem = 1;
    int n_round = 1;
    cme_rect_t rect;
    unsigned char color[3];
    FILE* fin = NULL;
    FILE* fout = NULL;
    double total_time = 0.0;
    int i;

    while ((c = getopt(argc, argv, "i:w:h:f:x:y:p:q:R:G:B:t:o:c:n:")) != -1) {
        switch (c) {
        case 'i': input_file = optarg; break;
        case 'w': w = atoi(optarg); break;
        case 'h': h = atoi(optarg); break;
        case 'f': fmt_str = optarg; break;
        case 'x': rx = atoi(optarg); break;
        case 'y': ry = atoi(optarg); break;
        case 'p': rw = atoi(optarg); break;
        case 'q': rh = atoi(optarg); break;
        case 'R': R = atoi(optarg); break;
        case 'G': G = atoi(optarg); break;
        case 'B': B = atoi(optarg); break;
        case 't': thickness = atoi(optarg); break;
        case 'o': output_file = optarg; break;
        case 'c': use_clmem = atoi(optarg); break;
        case 'n': n_round = atoi(optarg); break;
        default:
            draw_rect_usage(argv[0] ? argv[0] : "test_cme");
            return EXIT_FAILURE;
        }
    }

    if (!input_file || w <= 0 || h <= 0 || !fmt_str || rw <= 0 || rh <= 0 ||
        R < 0 || R > 255 || G < 0 || G > 255 || B < 0 || B > 255 || thickness <= 0) {
        fprintf(stderr, "test_draw_rectangle: missing or invalid arguments\n");
        draw_rect_usage(argv[0] ? argv[0] : "test_cme");
        return EXIT_FAILURE;
    }

    fmt = format_str_to_enum(fmt_str);
    if (fmt != CME_FORMAT_RGB_888 && fmt != CME_FORMAT_BGR_888) {
        fprintf(stderr, "test_draw_rectangle: format must be rgb24 or bgr24 (got '%s')\n", fmt_str);
        return EXIT_FAILURE;
    }

    cap = query_all_capability();
    if (!cap) {
        fprintf(stderr, "test_draw_rectangle: query_all_capability() returned NULL\n");
        return EXIT_FAILURE;
    }
    if (!format_supported(cap->supported_output_formats, fmt)) {
        fprintf(stderr, "test_draw_rectangle: format not supported\n");
        return EXIT_FAILURE;
    }

    memset(&dst_ctrl, 0, sizeof(dst_ctrl));
    dst_ctrl.width = w;
    dst_ctrl.height = h;
    dst_ctrl.format = fmt;
    dst_ctrl.ifconherent = 0;
    dst_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    dst_ctrl.use_clmem = use_clmem;

    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !dst) {
        fprintf(stderr, "test_draw_rectangle: alloc_cme_img failed (error=%d)\n", error);
        return EXIT_FAILURE;
    }

    fin = fopen(input_file, "rb");
    if (!fin) {
        fprintf(stderr, "test_draw_rectangle: cannot open input '%s'\n", input_file);
        free_cme_img(dst);
        return EXIT_FAILURE;
    }
    read_data_from_file(dst, fin);
    fclose(fin);
    fin = NULL;

    rect.x = rx;
    rect.y = ry;
    rect.width = rw;
    rect.height = rh;
    color[0] = (unsigned char)R;
    color[1] = (unsigned char)G;
    color[2] = (unsigned char)B;

    for (i = 0; i < n_round; i++) {
        unsigned long start = get_mono_time_ns();
        ret = cme_2d_draw_rectangle(dst, &rect, color, thickness, 1, NULL);
        unsigned long end = get_mono_time_ns();
        if (end < start) {
            fprintf(stderr, "test_draw_rectangle: time error\n");
            free_cme_img(dst);
            return EXIT_FAILURE;
        }
        total_time += (end - start) / 1000000.0;
        printf("test_draw_rectangle: cme_2d_draw_rectangle time: %lu ns (%.3f ms)\n",
               end - start, (end - start) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_draw_rectangle: API returned %d\n", (int)ret);
            free_cme_img(dst);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_draw_rectangle: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_draw_rectangle: OK (rect %d,%d %dx%d color %d,%d,%d t=%d)\n",
           rx, ry, rw, rh, R, G, B, thickness);

    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_draw_rectangle: cannot open output '%s'\n", output_file);
            free_cme_img(dst);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_draw_rectangle: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }

    free_cme_img(dst);
    return EXIT_SUCCESS;
}
