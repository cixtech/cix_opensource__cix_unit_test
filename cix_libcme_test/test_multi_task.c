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
#include "test_multi_task.h"

extern char* optarg;

CME_RET cme_free_pending_job(cme_job_handle handle);

enum {
    NORM_MODE_ARCFACE = 0,
    NORM_MODE_BYTE255 = 1,
};

static void multi_task_usage(const char* prog)
{
    fprintf(stderr, "Usage: %s multi_task -i <input> -w <width> -h <height> -f <format> [options]\n", prog);
    fprintf(stderr, "  Pipeline (single job): resize_cvtcolor_and_flip -> affine_transform -> normalize\n");
    fprintf(stderr, "  Submitted via cme_begin_task + *_task + cme_end_task(async) + cme_wait_job\n");
    fprintf(stderr, "  -i   input file (required)\n");
    fprintf(stderr, "  -w   source width (required)\n");
    fprintf(stderr, "  -h   source height (required)\n");
    fprintf(stderr, "  -f   source format: nv12, rgb24, rgba, ... (required)\n");
    fprintf(stderr, "  -W   pipeline output width (default 112)\n");
    fprintf(stderr, "  -H   pipeline output height (default 112)\n");
    fprintf(stderr, "  -o   dump final RGB_FLOAT output (optional)\n");
    fprintf(stderr, "  -c   use clmem (default 1)\n");
    fprintf(stderr, "  -n   test rounds (default 1)\n");
    fprintf(stderr, "  -r   affine rotate degree: 0, 10, 30 (default 0)\n");
    fprintf(stderr, "  -N   normalize mode: arcface (default) or byte255\n");
}

static void free_all_imgs(cme_img* src, cme_img* mid_rgb, cme_img* mid_affine, cme_img* out_float)
{
    if (out_float)
        free_cme_img(out_float);
    if (mid_affine)
        free_cme_img(mid_affine);
    if (mid_rgb)
        free_cme_img(mid_rgb);
    if (src)
        free_cme_img(src);
}

static int setup_affine_ctrl(cme_affine_transform_ctrl* actrl, double rotate_degree)
{
    memset(actrl, 0, sizeof(*actrl));
    if (rotate_degree == 0.0) {
        actrl->tranform_matrix[0][0] = 1.0f;
        actrl->tranform_matrix[0][1] = 0.0f;
        actrl->tranform_matrix[0][2] = 0.0f;
        actrl->tranform_matrix[1][0] = 0.0f;
        actrl->tranform_matrix[1][1] = 1.0f;
        actrl->tranform_matrix[1][2] = 0.0f;
    } else if (rotate_degree == 30.0) {
        actrl->tranform_matrix[0][0] = 0.89660f;
        actrl->tranform_matrix[0][1] = -0.5f;
        actrl->tranform_matrix[0][2] = -100.0f;
        actrl->tranform_matrix[1][0] = 0.5f;
        actrl->tranform_matrix[1][1] = 0.8660f;
        actrl->tranform_matrix[1][2] = 100.0f;
    } else if (rotate_degree == 10.0) {
        actrl->tranform_matrix[0][0] = 0.98481f;
        actrl->tranform_matrix[0][1] = 0.17365f;
        actrl->tranform_matrix[0][2] = -100.0f;
        actrl->tranform_matrix[1][0] = -0.17365f;
        actrl->tranform_matrix[1][1] = 0.98481f;
        actrl->tranform_matrix[1][2] = 100.0f;
    } else {
        fprintf(stderr, "test_multi_task: invalid -r %f (use 0, 10, 30)\n", rotate_degree);
        return -1;
    }
    actrl->border_mode = CME_BORDER_CONSTANT;
    actrl->border_value[0] = actrl->border_value[1] = actrl->border_value[2] = 0;
    return 0;
}

static void setup_normalize_ctrl(cme_normalize_ctrl* nctrl, int norm_mode)
{
    memset(nctrl, 0, sizeof(*nctrl));
    nctrl->enable_clamp = 1;
    if (norm_mode == NORM_MODE_BYTE255) {
        nctrl->multi_factor = 1.0f / 255.0f;
        nctrl->add_factor = 0.0f;
        nctrl->min = 0.0f;
        nctrl->max = 1.0f;
    } else {
        nctrl->multi_factor = 1.0f / 127.5f;
        nctrl->add_factor = -1.0f;
        nctrl->min = -128.0f;
        nctrl->max = 127.0f;
    }
}

static const char* norm_mode_name(int norm_mode)
{
    return (norm_mode == NORM_MODE_BYTE255) ? "byte255" : "arcface";
}

int test_multi_task(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl;
    alloc_img_ctrl rgb_ctrl;
    alloc_img_ctrl float_ctrl;
    cme_img* src = NULL;
    cme_img* mid_rgb = NULL;
    cme_img* mid_affine = NULL;
    cme_img* out_float = NULL;
    cme_affine_transform_ctrl actrl;
    cme_normalize_ctrl nctrl;
    int error = 0;
    CME_RET ret;
    int c;
    char* input_file = NULL;
    char* output_file = NULL;
    char* format_str = NULL;
    char* norm_str = NULL;
    int src_w = 0, src_h = 0;
    int dst_w = 112, dst_h = 112;
    IMG_FORMAT src_fmt = CME_FORMAT_UNKNOWN;
    FILE* fin = NULL;
    FILE* fout = NULL;
    int use_clmem = 1;
    int n_round = 1;
    double rotate_degree = 0.0;
    int norm_mode = NORM_MODE_ARCFACE;
    double total_time = 0.0;

    while ((c = getopt(argc, argv, "i:w:h:f:W:H:o:c:n:r:N:")) != -1) {
        switch (c) {
        case 'i': input_file = optarg; break;
        case 'w': src_w = atoi(optarg); break;
        case 'h': src_h = atoi(optarg); break;
        case 'f': format_str = optarg; break;
        case 'W': dst_w = atoi(optarg); break;
        case 'H': dst_h = atoi(optarg); break;
        case 'o': output_file = optarg; break;
        case 'c': use_clmem = atoi(optarg); break;
        case 'n': n_round = atoi(optarg); break;
        case 'r': rotate_degree = atof(optarg); break;
        case 'N': norm_str = optarg; break;
        default:
            multi_task_usage(argv[0] ? argv[0] : "test_libcme");
            return EXIT_FAILURE;
        }
    }

    if (!input_file || src_w <= 0 || src_h <= 0 || !format_str) {
        fprintf(stderr, "test_multi_task: missing -i -w -h -f\n");
        multi_task_usage(argv[0] ? argv[0] : "test_libcme");
        return EXIT_FAILURE;
    }
    if (dst_w <= 0 || dst_h <= 0) {
        fprintf(stderr, "test_multi_task: invalid destination size %dx%d\n", dst_w, dst_h);
        return EXIT_FAILURE;
    }
    if (norm_str) {
        if (strcmp(norm_str, "arcface") == 0)
            norm_mode = NORM_MODE_ARCFACE;
        else if (strcmp(norm_str, "byte255") == 0)
            norm_mode = NORM_MODE_BYTE255;
        else {
            fprintf(stderr, "test_multi_task: unknown -N '%s' (use arcface or byte255)\n", norm_str);
            return EXIT_FAILURE;
        }
    }
    if (setup_affine_ctrl(&actrl, rotate_degree) != 0)
        return EXIT_FAILURE;
    setup_normalize_ctrl(&nctrl, norm_mode);

    src_fmt = format_str_to_enum(format_str);
    if (src_fmt == CME_FORMAT_UNKNOWN) {
        fprintf(stderr, "test_multi_task: unknown format '%s'\n", format_str);
        return EXIT_FAILURE;
    }
    cap = query_all_capability();
    if (!cap || !format_supported(cap->supported_input_formats, src_fmt) ||
        !format_supported(cap->supported_output_formats, CME_FORMAT_RGB_888) ||
        !format_supported(cap->supported_output_formats, CME_FORMAT_RGB_FLOAT)) {
        fprintf(stderr, "test_multi_task: format/capability error\n");
        return EXIT_FAILURE;
    }

    memset(&src_ctrl, 0, sizeof(src_ctrl));
    src_ctrl.width = src_w;
    src_ctrl.height = src_h;
    src_ctrl.format = src_fmt;
    src_ctrl.ifconherent = 0;
    src_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    src_ctrl.use_clmem = use_clmem;

    memset(&rgb_ctrl, 0, sizeof(rgb_ctrl));
    rgb_ctrl.width = dst_w;
    rgb_ctrl.height = dst_h;
    rgb_ctrl.format = CME_FORMAT_RGB_888;
    rgb_ctrl.ifconherent = 0;
    rgb_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    rgb_ctrl.use_clmem = use_clmem;

    memset(&float_ctrl, 0, sizeof(float_ctrl));
    float_ctrl.width = dst_w;
    float_ctrl.height = dst_h;
    float_ctrl.format = CME_FORMAT_RGB_FLOAT;
    float_ctrl.ifconherent = 0;
    float_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    float_ctrl.use_clmem = use_clmem;

    src = alloc_cme_img(&src_ctrl, &error);
    mid_rgb = alloc_cme_img(&rgb_ctrl, &error);
    mid_affine = alloc_cme_img(&rgb_ctrl, &error);
    out_float = alloc_cme_img(&float_ctrl, &error);
    if (error || !src || !mid_rgb || !mid_affine || !out_float) {
        fprintf(stderr, "test_multi_task: alloc failed\n");
        free_all_imgs(src, mid_rgb, mid_affine, out_float);
        return EXIT_FAILURE;
    }

    fin = fopen(input_file, "rb");
    if (!fin) {
        fprintf(stderr, "test_multi_task: cannot open '%s'\n", input_file);
        free_all_imgs(src, mid_rgb, mid_affine, out_float);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);

    for (int i = 0; i < n_round; i++) {
        int wait_fd = -1;
        unsigned long t0 = get_mono_time_ns();
        cme_job_handle job = cme_begin_task();
        if (job < 0) {
            fprintf(stderr, "test_multi_task: cme_begin_task failed %d\n", (int)job);
            free_all_imgs(src, mid_rgb, mid_affine, out_float);
            return EXIT_FAILURE;
        }

        ret = cme_2d_resize_cvtcolor_and_flip_task(job, src, mid_rgb, CME_HAL_TRANSFORM_FLIP_NONE);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_multi_task: resize_cvtcolor_and_flip_task failed %d\n", (int)ret);
            (void)cme_free_pending_job(job);
            free_all_imgs(src, mid_rgb, mid_affine, out_float);
            return EXIT_FAILURE;
        }

        ret = cme_2d_affine_transform_task(job, mid_rgb, mid_affine, &actrl);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_multi_task: affine_transform_task failed %d\n", (int)ret);
            (void)cme_free_pending_job(job);
            free_all_imgs(src, mid_rgb, mid_affine, out_float);
            return EXIT_FAILURE;
        }

        ret = cme_2d_normalize_task(job, mid_affine, out_float, &nctrl);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_multi_task: normalize_task failed %d\n", (int)ret);
            (void)cme_free_pending_job(job);
            free_all_imgs(src, mid_rgb, mid_affine, out_float);
            return EXIT_FAILURE;
        }

        ret = cme_end_task(job, 0, &wait_fd);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_multi_task: cme_end_task failed %d\n", (int)ret);
            free_all_imgs(src, mid_rgb, mid_affine, out_float);
            return EXIT_FAILURE;
        }
        if (wait_fd < 0) {
            fprintf(stderr, "test_multi_task: cme_end_task failed %d (wait_fd=%d)\n", (int)ret, wait_fd);
            free_all_imgs(src, mid_rgb, mid_affine, out_float);
            return EXIT_FAILURE;
        }

        ret = cme_wait_job(wait_fd);
        unsigned long t1 = get_mono_time_ns();
        total_time += (t1 - t0) / 1000000.0;
        printf("test_multi_task: pipeline time: %lu ns (%.3f ms)\n", t1 - t0, (t1 - t0) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_multi_task: cme_wait_job failed %d\n", (int)ret);
            free_all_imgs(src, mid_rgb, mid_affine, out_float);
            return EXIT_FAILURE;
        }
    }

    if (n_round > 0) {
        printf("test_multi_task: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_multi_task: OK (%dx%d %s -> %dx%d RGB888 -> %dx%d RGB888 (r=%g) -> RGB_FLOAT norm=%s)\n",
           src_w, src_h, format_str, dst_w, dst_h, dst_w, dst_h, rotate_degree, norm_mode_name(norm_mode));

    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_multi_task: cannot open output '%s'\n", output_file);
            free_all_imgs(src, mid_rgb, mid_affine, out_float);
            return EXIT_FAILURE;
        }
        write_data_to_file(out_float, fout);
        fclose(fout);
        printf("test_multi_task: dumped to '%s' (%d bytes)\n", output_file, out_float->tlength);
    }

    free_all_imgs(src, mid_rgb, mid_affine, out_float);
    return EXIT_SUCCESS;
}
