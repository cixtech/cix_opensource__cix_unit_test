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
#include "test_affine_transform.h"

extern char* optarg;

static void affine_transform_usage(const char* prog)
{
    fprintf(stderr, "Usage: %s affine_transform -i <input> -w <width> -h <height> [-o <output>] [-W <dst_w>] [-H <dst_h>]\n", prog);
    fprintf(stderr, "  -i   input file RGB888 (required)\n");
    fprintf(stderr, "  -w   source width (required)\n");
    fprintf(stderr, "  -h   source height (required)\n");
    fprintf(stderr, "  -o   dump destination to file (optional)\n");
    fprintf(stderr, "  -W   destination width (default same as source)\n");
    fprintf(stderr, "  -H   destination height (default same as source)\n");
    fprintf(stderr, "  -c   use clmem (default 1)\n");
    fprintf(stderr, "  -n   API test round (default 1)\n");
    fprintf(stderr, "  -r   rotate degree (default 0, 30, 10)\n");
    fprintf(stderr, "  Uses identity transform matrix. Destination is RGB888.\n");
}

int test_affine_transform(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl;
    alloc_img_ctrl dst_ctrl;
    cme_img* src = NULL;
    cme_img* dst = NULL;
    cme_affine_transform_ctrl ctrl;
    int error = 0;
    CME_RET ret;
    int c;
    char* input_file = NULL;
    char* output_file = NULL;
    int src_w = 0, src_h = 0;
    int dst_w = 0, dst_h = 0;
    FILE* fin = NULL;
    FILE* fout = NULL;
    int use_clmem = 1;
    int n_round = 1;
    double rotate_degree = 0.0;
    double total_time = 0.0;

    while ((c = getopt(argc, argv, "i:w:h:o:W:H:c:n:r:")) != -1) {
        switch (c) {
        case 'i': input_file = optarg; break;
        case 'w': src_w = atoi(optarg); break;
        case 'h': src_h = atoi(optarg); break;
        case 'o': output_file = optarg; break;
        case 'W': dst_w = atoi(optarg); break;
        case 'H': dst_h = atoi(optarg); break;
        case 'c': use_clmem = atoi(optarg); break;
        case 'n': n_round = atoi(optarg); break;
        case 'r': rotate_degree = atof(optarg); break;
        default:
            affine_transform_usage(argv[0] ? argv[0] : "test_cme");
            return EXIT_FAILURE;
        }
    }

    if (!input_file || src_w <= 0 || src_h <= 0) {
        fprintf(stderr, "test_affine_transform: missing -i -w -h\n");
        affine_transform_usage(argv[0] ? argv[0] : "test_cme");
        return EXIT_FAILURE;
    }

    if (dst_w <= 0) dst_w = src_w;
    if (dst_h <= 0) dst_h = src_h;

    cap = query_all_capability();
    if (!cap) {
        fprintf(stderr, "test_affine_transform: query_all_capability() returned NULL\n");
        return EXIT_FAILURE;
    }
    if (!format_supported(cap->supported_input_formats, CME_FORMAT_RGB_888) ||
        !format_supported(cap->supported_output_formats, CME_FORMAT_RGB_888)) {
        fprintf(stderr, "test_affine_transform: RGB_888 not supported\n");
        return EXIT_FAILURE;
    }

    memset(&src_ctrl, 0, sizeof(src_ctrl));
    src_ctrl.width = src_w;
    src_ctrl.height = src_h;
    src_ctrl.format = CME_FORMAT_RGB_888;
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
        fprintf(stderr, "test_affine_transform: alloc_cme_img(src) failed\n");
        return EXIT_FAILURE;
    }
    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !dst) {
        fprintf(stderr, "test_affine_transform: alloc_cme_img(dst) failed\n");
        free_cme_img(src);
        return EXIT_FAILURE;
    }

    memset(&ctrl, 0, sizeof(ctrl));
    if (rotate_degree == 0.0) { /* Identity matrix */
        ctrl.tranform_matrix[0][0] = 1.0f;
        ctrl.tranform_matrix[0][1] = 0.0f;
        ctrl.tranform_matrix[0][2] = 0.0f;
        ctrl.tranform_matrix[1][0] = 0.0f;
        ctrl.tranform_matrix[1][1] = 1.0f;
        ctrl.tranform_matrix[1][2] = 0.0f;
    } else if (rotate_degree == 30.0) { /* rotate 30 degree */
        ctrl.tranform_matrix[0][0] = 0.89660;
        ctrl.tranform_matrix[0][1] = -0.5;
        ctrl.tranform_matrix[0][2] = -100;
        ctrl.tranform_matrix[1][0] = 0.5;
        ctrl.tranform_matrix[1][1] = 0.8660;
        ctrl.tranform_matrix[1][2] = 100;
    } else if (rotate_degree == 10.0) { /* rotate 10 degree */
        ctrl.tranform_matrix[0][0] = 0.98481;
        ctrl.tranform_matrix[0][1] = 0.17365;
        ctrl.tranform_matrix[0][2] = -100;
        ctrl.tranform_matrix[1][0] = -0.17365;
        ctrl.tranform_matrix[1][1] = 0.98481;
        ctrl.tranform_matrix[1][2] = 100;
    } else {
        fprintf(stderr, "test_affine_transform: invalid rotate degree %f\n", rotate_degree);
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    ctrl.border_mode = CME_BORDER_CONSTANT;
    ctrl.border_value[0] = ctrl.border_value[1] = ctrl.border_value[2] = 0;

    fin = fopen(input_file, "rb");
    if (!fin) {
        fprintf(stderr, "test_affine_transform: cannot open '%s'\n", input_file);
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);
    fin = NULL;

    for (int i = 0; i < n_round; i++) {
        unsigned long start = get_mono_time_ns();
        ret = cme_2d_affine_transform(src, dst, &ctrl, 1, NULL);
        unsigned long end = get_mono_time_ns();
        if (end < start) {
            fprintf(stderr, "test_affine_transform: time error\n");
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        total_time += (end - start) / 1000000.0;
        printf("test_affine_transform: cme_2d_affine_transform time: %lu ns (%.3f ms)\n", end - start, (end - start) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_affine_transform: cme_2d_affine_transform returned %d\n", (int)ret);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_affine_transform: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_affine_transform: OK (%dx%d -> %dx%d RGB888)\n", src_w, src_h, dst_w, dst_h);

    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_affine_transform: cannot open output '%s'\n", output_file);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_affine_transform: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }

    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}
