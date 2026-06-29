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
#include "test_job_task.h"

extern char* optarg;

/* Implemented in libcme; used to drop a pending job if *_task fails before submit. */
CME_RET cme_free_pending_job(cme_job_handle handle);

#define ROUND_UP_SZ(x, a) (((x) + (size_t)(a)-1) / (a) * (a))
#define OVERLAY_MAX_LAYERS 4

static void job_task_usage(const char* exe)
{
    fprintf(stderr, "Usage: %s job_task [options...]\n", exe);
    fprintf(stderr, "  Runs cme_begin_task -> cme_2d_*_task / cme_img_copy_task -> cme_end_task(sync).\n");
    fprintf(stderr, "  resize                 -i -w -h -f [-o] [-W -H] [-c] [-n]\n");
    fprintf(stderr, "  cvtcolor               -i -w -h -f -F -m [-o] [-n]\n");
    fprintf(stderr, "  resize_cvtcolor_flip   -i -w -h -f -m [-o] [-W -H] [-c] [-n]\n");
    fprintf(stderr, "  overlay                -i -I -w -h -W -H -f -F [-a -A -x -y -o -c -n]\n");
    fprintf(stderr, "  crfrc                  -i -w -h -f -F -m -r [-M -o -W -H -E -X -Y -P -Q -c -n]\n");
    fprintf(stderr, "  fisheye_remap          -i -w -h -f [-o -W -H -c -s -e -k -l -a -x -y -n]\n");
    fprintf(stderr, "  est_sim_tfm            [-o] [-n]\n");
    fprintf(stderr, "  cosine_sim             -i -j -d [-t 0|1] [-n] [-c 1]\n");
    fprintf(stderr, "  normalize              -i -w -h -f [-o] [-c] [-n]\n");
    fprintf(stderr, "  affine_transform       -i -w -h [-o] [-W -H] [-c] [-n] [-r]\n");
    fprintf(stderr, "  imcopy                 -i -w -h -f [-o] [-c] [-n]  (same WxH format, cme_img_copy_task)\n");
}

static int cosine_sim_tmp_min_width(int vector_dim)
{
    const int workgroup_size = 128;
    int num_packed = (vector_dim + 3) / 4;
    int num_workgroups = (num_packed + workgroup_size - 1) / workgroup_size;
    size_t partial_size_raw = (size_t)num_workgroups * sizeof(float);
    size_t partial_size = ROUND_UP_SZ(partial_size_raw, 128);
    return (int)(3 * partial_size / sizeof(float));
}

static IMG_FORMAT normalize_format_str(const char* str)
{
    IMG_FORMAT f = format_str_to_enum(str);
    if (f != CME_FORMAT_UNKNOWN)
        return f;
    if (strcmp(str, "rgb_float") == 0 || strcmp(str, "float") == 0)
        return CME_FORMAT_RGB_FLOAT;
    return CME_FORMAT_UNKNOWN;
}

static int read_float_vector_file(const char* path, float* dst, int dim, const char* label)
{
    FILE* fp = fopen(path, "rb");
    size_t n;
    if (!fp) {
        fprintf(stderr, "test_job_task: cannot open %s '%s'\n", label, path);
        return -1;
    }
    n = fread(dst, sizeof(float), (size_t)dim, fp);
    fclose(fp);
    if ((int)n != dim) {
        fprintf(stderr, "test_job_task: %s '%s' expected %d floats, got %zu\n", label, path, dim, n);
        return -1;
    }
    return 0;
}

static int generate_remap_maps(int width, int height, float** out_map_x, float** out_map_y)
{
    size_t map_size;
    float* map_x;
    float* map_y;
    int i, j;

    if (width <= 0 || height <= 0 || !out_map_x || !out_map_y) {
        if (out_map_x)
            *out_map_x = NULL;
        if (out_map_y)
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

static int job_resize(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl, dst_ctrl;
    cme_img* src = NULL;
    cme_img* dst = NULL;
    int error = 0;
    CME_RET ret;
    int c;
    char* input_file = NULL;
    char* output_file = NULL;
    int src_w = 0, src_h = 0;
    int dst_w = 1280, dst_h = 720;
    char* format_str = NULL;
    IMG_FORMAT fmt = CME_FORMAT_UNKNOWN;
    FILE* fin = NULL;
    FILE* fout = NULL;
    int use_clmem = 1;
    int n_round = 1;
    double total_time = 0.0;

    optind = 1;
    while ((c = getopt(argc, argv, "i:w:h:f:o:W:H:c:n:")) != -1) {
        switch (c) {
        case 'i': input_file = optarg; break;
        case 'w': src_w = atoi(optarg); break;
        case 'h': src_h = atoi(optarg); break;
        case 'f': format_str = optarg; break;
        case 'o': output_file = optarg; break;
        case 'W': dst_w = atoi(optarg); break;
        case 'H': dst_h = atoi(optarg); break;
        case 'c': use_clmem = atoi(optarg); break;
        case 'n': n_round = atoi(optarg); break;
        default:
            return EXIT_FAILURE;
        }
    }

    if (!input_file || src_w <= 0 || src_h <= 0 || !format_str) {
        fprintf(stderr, "test_job_task resize: missing -i -w -h -f\n");
        return EXIT_FAILURE;
    }
    fmt = format_str_to_enum(format_str);
    if (fmt == CME_FORMAT_UNKNOWN) {
        fprintf(stderr, "test_job_task resize: unknown format '%s'\n", format_str);
        return EXIT_FAILURE;
    }
    cap = query_all_capability();
    if (!cap || !format_supported(cap->supported_input_formats, fmt) ||
        !format_supported(cap->supported_output_formats, fmt)) {
        fprintf(stderr, "test_job_task resize: capability/format error\n");
        return EXIT_FAILURE;
    }

    memset(&src_ctrl, 0, sizeof(src_ctrl));
    src_ctrl.width = src_w;
    src_ctrl.height = src_h;
    src_ctrl.format = fmt;
    src_ctrl.ifconherent = 0;
    src_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    src_ctrl.use_clmem = use_clmem;
    memset(&dst_ctrl, 0, sizeof(dst_ctrl));
    dst_ctrl.width = dst_w;
    dst_ctrl.height = dst_h;
    dst_ctrl.format = fmt;
    dst_ctrl.ifconherent = 0;
    dst_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    dst_ctrl.use_clmem = use_clmem;

    src = alloc_cme_img(&src_ctrl, &error);
    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !src || !dst) {
        fprintf(stderr, "test_job_task resize: alloc failed\n");
        if (dst) free_cme_img(dst);
        if (src) free_cme_img(src);
        return EXIT_FAILURE;
    }
    fin = fopen(input_file, "rb");
    if (!fin) {
        fprintf(stderr, "test_job_task resize: cannot open '%s'\n", input_file);
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);

    for (int i = 0; i < n_round; i++) {
        unsigned long t0 = get_mono_time_ns();
        cme_job_handle job = cme_begin_task();
        if (job < 0) {
            fprintf(stderr, "test_job_task resize: cme_begin_task failed %d\n", job);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_2d_resize_task(job, src, dst);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_job_task resize: cme_2d_resize_task failed %d\n", (int)ret);
            (void)cme_free_pending_job(job);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_end_task(job, 1, NULL);
        unsigned long t1 = get_mono_time_ns();
        total_time += (t1 - t0) / 1000000.0;
        printf("test_job_task resize: cme_2d_resize_task time: %lu ns (%.3f ms)\n", t1 - t0,
               (t1 - t0) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_job_task resize: cme_end_task failed %d\n", (int)ret);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_job_task resize: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_job_task resize: OK (%dx%d %s -> %dx%d %s)\n", src_w, src_h, format_str, dst_w, dst_h,
           format_str);

    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_job_task resize: cannot open output '%s'\n", output_file);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_job_task resize: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }
    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}

static int job_cvtcolor(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl, dst_ctrl;
    cme_img* src = NULL;
    cme_img* dst = NULL;
    int error = 0;
    CME_RET ret;
    int c;
    char* input_file = NULL;
    char* output_file = NULL;
    int w = 0, h = 0;
    char *src_fmt_str = NULL, *dst_fmt_str = NULL, *mode_str = NULL;
    IMG_FORMAT src_fmt, dst_fmt;
    int cvt_mode = CME_COLOR_SPACE_DEFAULT;
    FILE* fin = NULL;
    FILE* fout = NULL;
    int n_round = 1;
    double total_time = 0.0;

    optind = 1;
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
            return EXIT_FAILURE;
        }
    }
    if (!input_file || w <= 0 || h <= 0 || !src_fmt_str || !dst_fmt_str || !mode_str) {
        fprintf(stderr, "test_job_task cvtcolor: missing args\n");
        return EXIT_FAILURE;
    }
    src_fmt = format_str_to_enum(src_fmt_str);
    dst_fmt = format_str_to_enum(dst_fmt_str);
    if (src_fmt == CME_FORMAT_UNKNOWN || dst_fmt == CME_FORMAT_UNKNOWN ||
        parse_cvtcolor_mode(mode_str, &cvt_mode) != 0) {
        fprintf(stderr, "test_job_task cvtcolor: bad format/mode\n");
        return EXIT_FAILURE;
    }
    cap = query_all_capability();
    if (!cap || !format_supported(cap->supported_input_formats, src_fmt) ||
        !format_supported(cap->supported_output_formats, dst_fmt)) {
        fprintf(stderr, "test_job_task cvtcolor: format not supported\n");
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
    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !src || !dst) {
        if (dst) free_cme_img(dst);
        if (src) free_cme_img(src);
        return EXIT_FAILURE;
    }
    fin = fopen(input_file, "rb");
    if (!fin) {
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);

    for (int i = 0; i < n_round; i++) {
        unsigned long t0 = get_mono_time_ns();
        cme_job_handle job = cme_begin_task();
        if (job < 0) {
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_2d_cvtcolor_task(job, src, dst, cvt_mode);
        if (ret != CME_RET_SUCCESS) {
            (void)cme_free_pending_job(job);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_end_task(job, 1, NULL);
        unsigned long t1 = get_mono_time_ns();
        total_time += (t1 - t0) / 1000000.0;
        printf("test_job_task cvtcolor: cme_2d_cvtcolor_task time: %lu ns (%.3f ms)\n", t1 - t0,
               (t1 - t0) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_job_task cvtcolor: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_job_task cvtcolor: OK (%dx%d %s -> %s mode=%s)\n", w, h, src_fmt_str, dst_fmt_str, mode_str);
    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_job_task cvtcolor: cannot open output '%s'\n", output_file);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_job_task cvtcolor: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }
    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}

static int job_resize_cvtcolor_flip(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl, dst_ctrl;
    cme_img* src = NULL;
    cme_img* dst = NULL;
    int error = 0;
    CME_RET ret;
    int c;
    char* input_file = NULL;
    char* output_file = NULL;
    int src_w = 0, src_h = 0;
    int dst_w = 1280, dst_h = 720;
    char* format_str = NULL;
    char* mode_str = NULL;
    IMG_FORMAT src_fmt = CME_FORMAT_UNKNOWN;
    int flip_mode = CME_HAL_TRANSFORM_FLIP_NONE;
    FILE* fin = NULL;
    FILE* fout = NULL;
    int use_clmem = 1;
    int n_round = 1;
    double total_time = 0.0;

    optind = 1;
    while ((c = getopt(argc, argv, "i:w:h:f:m:o:W:H:c:n:")) != -1) {
        switch (c) {
        case 'i': input_file = optarg; break;
        case 'w': src_w = atoi(optarg); break;
        case 'h': src_h = atoi(optarg); break;
        case 'f': format_str = optarg; break;
        case 'm': mode_str = optarg; break;
        case 'o': output_file = optarg; break;
        case 'W': dst_w = atoi(optarg); break;
        case 'H': dst_h = atoi(optarg); break;
        case 'c': use_clmem = atoi(optarg); break;
        case 'n': n_round = atoi(optarg); break;
        default:
            return EXIT_FAILURE;
        }
    }
    if (!input_file || src_w <= 0 || src_h <= 0 || !format_str || !mode_str ||
        parse_flip_mode(mode_str, &flip_mode) != 0) {
        fprintf(stderr, "test_job_task resize_cvtcolor_flip: bad args\n");
        return EXIT_FAILURE;
    }
    src_fmt = format_str_to_enum(format_str);
    if (src_fmt == CME_FORMAT_UNKNOWN) {
        return EXIT_FAILURE;
    }
    cap = query_all_capability();
    if (!cap || !format_supported(cap->supported_output_formats, CME_FORMAT_RGB_888) ||
        !format_supported(cap->supported_input_formats, src_fmt)) {
        fprintf(stderr, "test_job_task resize_cvtcolor_flip: not supported\n");
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
    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !src || !dst) {
        if (dst) free_cme_img(dst);
        if (src) free_cme_img(src);
        return EXIT_FAILURE;
    }
    fin = fopen(input_file, "rb");
    if (!fin) {
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);

    for (int i = 0; i < n_round; i++) {
        unsigned long t0 = get_mono_time_ns();
        cme_job_handle job = cme_begin_task();
        if (job < 0) {
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_2d_resize_cvtcolor_and_flip_task(job, src, dst, flip_mode);
        if (ret != CME_RET_SUCCESS) {
            (void)cme_free_pending_job(job);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_end_task(job, 1, NULL);
        unsigned long t1 = get_mono_time_ns();
        total_time += (t1 - t0) / 1000000.0;
        printf("test_job_task resize_cvtcolor_flip: cme_2d_resize_cvtcolor_and_flip_task time: %lu ns (%.3f ms)\n",
               t1 - t0, (t1 - t0) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_job_task resize_cvtcolor_flip: %d rounds, average time: %.3f ms\n", n_round,
               total_time / n_round);
    }
    printf("test_job_task resize_cvtcolor_flip: OK (src %dx%d %s -> dst %dx%d RGB888 mode=%s)\n", src_w, src_h,
           format_str, dst_w, dst_h, mode_str);
    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_job_task resize_cvtcolor_flip: cannot open output '%s'\n", output_file);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_job_task resize_cvtcolor_flip: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }
    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}

static int job_est_sim_tfm(int argc, char** argv)
{
    cme_est_sim_tfm_ctrl ctrl;
    cme_est_sim_tfm_ret sim_ret;
    CME_RET ret;
    int c;
    char* output_file = NULL;
    int n_round = 1;
    FILE* fout = NULL;
    double total_time = 0.0;

    optind = 1;
    while ((c = getopt(argc, argv, "o:n:")) != -1) {
        switch (c) {
        case 'o': output_file = optarg; break;
        case 'n': n_round = atoi(optarg); break;
        default:
            return EXIT_FAILURE;
        }
    }

    ctrl.dst_point_x[0] = 222;
    ctrl.dst_point_y[0] = 203;
    ctrl.dst_point_x[1] = 423;
    ctrl.dst_point_y[1] = 207;
    ctrl.dst_point_x[2] = 371;
    ctrl.dst_point_y[2] = 298;
    ctrl.dst_point_x[3] = 263;
    ctrl.dst_point_y[3] = 425;
    ctrl.dst_point_x[4] = 398;
    ctrl.dst_point_y[4] = 426;
    ctrl.src_point_x[0] = 338;
    ctrl.src_point_y[0] = 331;
    ctrl.src_point_x[1] = 516;
    ctrl.src_point_y[1] = 407;
    ctrl.src_point_x[2] = 430;
    ctrl.src_point_y[2] = 463;
    ctrl.src_point_x[3] = 265;
    ctrl.src_point_y[3] = 523;
    ctrl.src_point_x[4] = 388;
    ctrl.src_point_y[4] = 586;
    ctrl.point_number = 5;

    for (int i = 0; i < n_round; i++) {
        memset(&sim_ret, 0, sizeof(sim_ret));
        unsigned long t0 = get_mono_time_ns();
        cme_job_handle job = cme_begin_task();
        if (job < 0)
            return EXIT_FAILURE;
        ret = cme_2d_est_sim_tfm_task(job, &ctrl, &sim_ret);
        if (ret != CME_RET_SUCCESS) {
            (void)cme_free_pending_job(job);
            return EXIT_FAILURE;
        }
        ret = cme_end_task(job, 1, NULL);
        unsigned long t1 = get_mono_time_ns();
        total_time += (t1 - t0) / 1000000.0;
        printf("test_job_task est_sim_tfm: cme_2d_est_sim_tfm_task time: %lu ns (%.3f ms)\n", t1 - t0,
               (t1 - t0) / 1000000.0);
        if (ret != CME_RET_SUCCESS)
            return EXIT_FAILURE;
    }
    if (n_round > 0) {
        printf("test_job_task est_sim_tfm: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_job_task est_sim_tfm: OK (a=%f b=%f tx=%f ty=%f)\n", sim_ret.a, sim_ret.b, sim_ret.tx, sim_ret.ty);
    if (output_file) {
        fout = fopen(output_file, "w");
        if (!fout) {
            fprintf(stderr, "test_job_task est_sim_tfm: cannot open output '%s'\n", output_file);
            return EXIT_FAILURE;
        }
        fprintf(fout, "%f %f %f %f\n", sim_ret.a, sim_ret.b, sim_ret.tx, sim_ret.ty);
        fflush(fout);
        {
            long dump_sz = ftell(fout);
            fclose(fout);
            fout = NULL;
            printf("test_job_task est_sim_tfm: dumped to '%s' (%ld bytes)\n", output_file, dump_sz);
        }
    }
    return EXIT_SUCCESS;
}

static int job_cosine_sim(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl vec_ctrl, tmp_ctrl;
    cme_img* img_a = NULL;
    cme_img* img_b = NULL;
    cme_img* tmp = NULL;
    int error = 0;
    CME_RET ret;
    int c;
    char* path_a = NULL;
    char* path_b = NULL;
    int dim = 0;
    int use_tmp = 0;
    int use_clmem = 1;
    int n_round = 1;
    float result = 0.0f;
    float* host_a = NULL;
    float* host_b = NULL;
    double total_time = 0.0;

    optind = 1;
    while ((c = getopt(argc, argv, "i:j:d:t:n:c:")) != -1) {
        switch (c) {
        case 'i': path_a = optarg; break;
        case 'j': path_b = optarg; break;
        case 'd': dim = atoi(optarg); break;
        case 't': use_tmp = atoi(optarg); break;
        case 'n': n_round = atoi(optarg); break;
        case 'c': use_clmem = atoi(optarg); break;
        default:
            return EXIT_FAILURE;
        }
    }
    if (!path_a || !path_b || dim <= 0 || use_clmem != 1) {
        fprintf(stderr, "test_job_task cosine_sim: need -i -j -d and -c 1\n");
        return EXIT_FAILURE;
    }
    cap = query_all_capability();
    if (!cap || !format_supported(cap->supported_input_formats, CME_FORMAT_FLOAT32_VECTOR)) {
        fprintf(stderr, "test_job_task cosine_sim: FLOAT32_VECTOR not supported\n");
        return EXIT_FAILURE;
    }
    host_a = (float*)malloc((size_t)dim * sizeof(float));
    host_b = (float*)malloc((size_t)dim * sizeof(float));
    if (!host_a || !host_b ||
        read_float_vector_file(path_a, host_a, dim, "vec_a") != 0 ||
        read_float_vector_file(path_b, host_b, dim, "vec_b") != 0) {
        free(host_a);
        free(host_b);
        return EXIT_FAILURE;
    }
    memset(&vec_ctrl, 0, sizeof(vec_ctrl));
    vec_ctrl.width = dim;
    vec_ctrl.height = 1;
    vec_ctrl.format = CME_FORMAT_FLOAT32_VECTOR;
    vec_ctrl.ifconherent = 0;
    vec_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    vec_ctrl.use_clmem = use_clmem;
    img_a = alloc_cme_img(&vec_ctrl, &error);
    img_b = alloc_cme_img(&vec_ctrl, &error);
    if (error || !img_a || !img_b) {
        if (img_b) free_cme_img(img_b);
        if (img_a) free_cme_img(img_a);
        free(host_a);
        free(host_b);
        return EXIT_FAILURE;
    }
    memcpy(img_a->vir_addr[0], host_a, (size_t)dim * sizeof(float));
    memcpy(img_b->vir_addr[0], host_b, (size_t)dim * sizeof(float));
    if (use_tmp) {
        int tw = cosine_sim_tmp_min_width(dim);
        memset(&tmp_ctrl, 0, sizeof(tmp_ctrl));
        tmp_ctrl.width = tw;
        tmp_ctrl.height = 1;
        tmp_ctrl.format = CME_FORMAT_FLOAT32_VECTOR;
        tmp_ctrl.ifconherent = 0;
        tmp_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
        tmp_ctrl.use_clmem = use_clmem;
        tmp = alloc_cme_img(&tmp_ctrl, &error);
        if (error || !tmp) {
            free_cme_img(img_b);
            free_cme_img(img_a);
            free(host_a);
            free(host_b);
            return EXIT_FAILURE;
        }
    }

    for (int i = 0; i < n_round; i++) {
        unsigned long t0 = get_mono_time_ns();
        cme_job_handle job = cme_begin_task();
        if (job < 0) {
            if (tmp) free_cme_img(tmp);
            free_cme_img(img_b);
            free_cme_img(img_a);
            free(host_a);
            free(host_b);
            return EXIT_FAILURE;
        }
        ret = cme_2d_cosine_sim_task(job, img_a, img_b, &result, tmp);
        if (ret != CME_RET_SUCCESS) {
            (void)cme_free_pending_job(job);
            if (tmp) free_cme_img(tmp);
            free_cme_img(img_b);
            free_cme_img(img_a);
            free(host_a);
            free(host_b);
            return EXIT_FAILURE;
        }
        ret = cme_end_task(job, 1, NULL);
        unsigned long t1 = get_mono_time_ns();
        total_time += (t1 - t0) / 1000000.0;
        printf("test_job_task cosine_sim: cme_2d_cosine_sim_task time: %lu ns (%.3f ms)\n", t1 - t0,
               (t1 - t0) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            if (tmp) free_cme_img(tmp);
            free_cme_img(img_b);
            free_cme_img(img_a);
            free(host_a);
            free(host_b);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_job_task cosine_sim: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_job_task cosine_sim: OK (dim=%d cosine=%.8f)\n", dim, result);
    if (tmp) free_cme_img(tmp);
    free_cme_img(img_b);
    free_cme_img(img_a);
    free(host_a);
    free(host_b);
    return EXIT_SUCCESS;
}

static int job_normalize(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl, dst_ctrl;
    cme_img* src = NULL;
    cme_img* dst = NULL;
    cme_normalize_ctrl nctrl;
    int error = 0;
    CME_RET ret;
    int c;
    char* input_file = NULL;
    char* output_file = NULL;
    int w = 0, h = 0;
    char* format_str = NULL;
    IMG_FORMAT src_fmt;
    FILE* fin = NULL;
    FILE* fout = NULL;
    int use_clmem = 1;
    int n_round = 1;
    double total_time = 0.0;

    optind = 1;
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
            return EXIT_FAILURE;
        }
    }
    if (!input_file || w <= 0 || h <= 0 || !format_str) {
        return EXIT_FAILURE;
    }
    src_fmt = normalize_format_str(format_str);
    if (src_fmt != CME_FORMAT_RGB_888 && src_fmt != CME_FORMAT_RGB_FLOAT) {
        fprintf(stderr, "test_job_task normalize: format must be rgb24 or rgb_float\n");
        return EXIT_FAILURE;
    }
    cap = query_all_capability();
    if (!cap || !format_supported(cap->supported_input_formats, src_fmt) ||
        !format_supported(cap->supported_output_formats, CME_FORMAT_RGB_FLOAT)) {
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
    memset(&nctrl, 0, sizeof(nctrl));
    nctrl.multi_factor = 1.0f / 255.0f;
    nctrl.add_factor = 0.0f;
    nctrl.max = 1.0f;
    nctrl.min = 0.0f;
    nctrl.enable_clamp = 1;
    src = alloc_cme_img(&src_ctrl, &error);
    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !src || !dst) {
        if (dst) free_cme_img(dst);
        if (src) free_cme_img(src);
        return EXIT_FAILURE;
    }
    fin = fopen(input_file, "rb");
    if (!fin) {
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);

    for (int i = 0; i < n_round; i++) {
        unsigned long t0 = get_mono_time_ns();
        cme_job_handle job = cme_begin_task();
        if (job < 0) {
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_2d_normalize_task(job, src, dst, &nctrl);
        if (ret != CME_RET_SUCCESS) {
            (void)cme_free_pending_job(job);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_end_task(job, 1, NULL);
        unsigned long t1 = get_mono_time_ns();
        total_time += (t1 - t0) / 1000000.0;
        printf("test_job_task normalize: cme_2d_normalize_task time: %lu ns (%.3f ms)\n", t1 - t0,
               (t1 - t0) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_job_task normalize: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_job_task normalize: OK (%dx%d %s -> RGB_FLOAT)\n", w, h, format_str);
    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_job_task normalize: cannot open output '%s'\n", output_file);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_job_task normalize: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }
    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}

static int job_affine(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl, dst_ctrl;
    cme_img* src = NULL;
    cme_img* dst = NULL;
    cme_affine_transform_ctrl actrl;
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

    optind = 1;
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
            return EXIT_FAILURE;
        }
    }
    if (!input_file || src_w <= 0 || src_h <= 0) {
        return EXIT_FAILURE;
    }
    if (dst_w <= 0) dst_w = src_w;
    if (dst_h <= 0) dst_h = src_h;
    cap = query_all_capability();
    if (!cap || !format_supported(cap->supported_input_formats, CME_FORMAT_RGB_888) ||
        !format_supported(cap->supported_output_formats, CME_FORMAT_RGB_888)) {
        return EXIT_FAILURE;
    }
    memset(&actrl, 0, sizeof(actrl));
    if (rotate_degree == 0.0) {
        actrl.tranform_matrix[0][0] = 1.0f;
        actrl.tranform_matrix[0][1] = 0.0f;
        actrl.tranform_matrix[0][2] = 0.0f;
        actrl.tranform_matrix[1][0] = 0.0f;
        actrl.tranform_matrix[1][1] = 1.0f;
        actrl.tranform_matrix[1][2] = 0.0f;
    } else if (rotate_degree == 30.0) {
        actrl.tranform_matrix[0][0] = 0.89660f;
        actrl.tranform_matrix[0][1] = -0.5f;
        actrl.tranform_matrix[0][2] = -100.0f;
        actrl.tranform_matrix[1][0] = 0.5f;
        actrl.tranform_matrix[1][1] = 0.8660f;
        actrl.tranform_matrix[1][2] = 100.0f;
    } else if (rotate_degree == 10.0) {
        actrl.tranform_matrix[0][0] = 0.98481f;
        actrl.tranform_matrix[0][1] = 0.5f;
        actrl.tranform_matrix[0][2] = -100.0f;
        actrl.tranform_matrix[1][0] = -0.17365f;
        actrl.tranform_matrix[1][1] = 0.98481f;
        actrl.tranform_matrix[1][2] = 100.0f;
    } else {
        fprintf(stderr, "test_job_task affine_transform: invalid -r %f (use 0, 30, 10)\n", rotate_degree);
        return EXIT_FAILURE;
    }
    actrl.border_mode = CME_BORDER_CONSTANT;
    actrl.border_value[0] = actrl.border_value[1] = actrl.border_value[2] = 0;

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
    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !src || !dst) {
        if (dst) free_cme_img(dst);
        if (src) free_cme_img(src);
        return EXIT_FAILURE;
    }
    fin = fopen(input_file, "rb");
    if (!fin) {
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);

    for (int i = 0; i < n_round; i++) {
        unsigned long t0 = get_mono_time_ns();
        cme_job_handle job = cme_begin_task();
        if (job < 0) {
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_2d_affine_transform_task(job, src, dst, &actrl);
        if (ret != CME_RET_SUCCESS) {
            (void)cme_free_pending_job(job);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_end_task(job, 1, NULL);
        unsigned long t1 = get_mono_time_ns();
        total_time += (t1 - t0) / 1000000.0;
        printf("test_job_task affine_transform: cme_2d_affine_transform_task time: %lu ns (%.3f ms)\n", t1 - t0,
               (t1 - t0) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_job_task affine_transform: %d rounds, average time: %.3f ms\n", n_round,
               total_time / n_round);
    }
    printf("test_job_task affine_transform: OK (%dx%d -> %dx%d RGB888 r=%g)\n", src_w, src_h, dst_w, dst_h,
           rotate_degree);
    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_job_task affine_transform: cannot open output '%s'\n", output_file);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_job_task affine_transform: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }
    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}

static int job_imcopy(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl, dst_ctrl;
    cme_img* src = NULL;
    cme_img* dst = NULL;
    int error = 0;
    CME_RET ret;
    int c;
    char* input_file = NULL;
    char* output_file = NULL;
    int w = 0, h = 0;
    char* format_str = NULL;
    IMG_FORMAT fmt;
    FILE* fin = NULL;
    FILE* fout = NULL;
    int use_clmem = 1;
    int n_round = 1;
    double total_time = 0.0;

    optind = 1;
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
            return EXIT_FAILURE;
        }
    }
    if (!input_file || w <= 0 || h <= 0 || !format_str) {
        fprintf(stderr, "test_job_task imcopy: missing -i -w -h -f\n");
        return EXIT_FAILURE;
    }
    fmt = format_str_to_enum(format_str);
    if (fmt == CME_FORMAT_UNKNOWN) {
        return EXIT_FAILURE;
    }
    cap = query_all_capability();
    if (!cap || !format_supported(cap->supported_input_formats, fmt) ||
        !format_supported(cap->supported_output_formats, fmt)) {
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
    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !src || !dst) {
        if (dst) free_cme_img(dst);
        if (src) free_cme_img(src);
        return EXIT_FAILURE;
    }
    fin = fopen(input_file, "rb");
    if (!fin) {
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);

    for (int i = 0; i < n_round; i++) {
        unsigned long t0 = get_mono_time_ns();
        cme_job_handle job = cme_begin_task();
        if (job < 0) {
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_img_copy_task(job, src, dst);
        if (ret != CME_RET_SUCCESS) {
            (void)cme_free_pending_job(job);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_end_task(job, 1, NULL);
        unsigned long t1 = get_mono_time_ns();
        total_time += (t1 - t0) / 1000000.0;
        printf("test_job_task imcopy: cme_img_copy_task time: %lu ns (%.3f ms)\n", t1 - t0,
               (t1 - t0) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_job_task imcopy: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_job_task imcopy: OK (%dx%d %s)\n", w, h, format_str);
    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "test_job_task imcopy: cannot open output '%s'\n", output_file);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_job_task imcopy: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }
    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}

static int job_overlay(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl;
    alloc_img_ctrl dst_ctrl;
    cme_img* src[OVERLAY_MAX_LAYERS];
    cme_img* src_ptrs[OVERLAY_MAX_LAYERS];
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

    optind = 1;
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
            return EXIT_FAILURE;
        }
    }

    if (!input_base || !input_overlay || w <= 0 || h <= 0 || !format_str || W <= 0 || H <= 0 ||
        !format_overlay_str) {
        fprintf(stderr, "test_job_task overlay: missing -i -I -w -h -f -W -H -F\n");
        return EXIT_FAILURE;
    }
    if (alpha0 < 0 || alpha0 > 65535 || alpha1 < 0 || alpha1 > 65535) {
        fprintf(stderr, "test_job_task overlay: alpha must be 0-65535\n");
        return EXIT_FAILURE;
    }
    if (nimgs <= 0 || nimgs > OVERLAY_MAX_LAYERS) {
        fprintf(stderr, "test_job_task overlay: layer count must be 1-%d\n", OVERLAY_MAX_LAYERS);
        return EXIT_FAILURE;
    }

    fmt = format_str_to_enum(format_str);
    fmt_overlay = format_str_to_enum(format_overlay_str);
    if (fmt == CME_FORMAT_UNKNOWN || fmt_overlay == CME_FORMAT_UNKNOWN) {
        fprintf(stderr, "test_job_task overlay: unknown format\n");
        return EXIT_FAILURE;
    }
    cap = query_all_capability();
    if (!cap || !format_supported(cap->supported_input_formats, fmt) ||
        !format_supported(cap->supported_output_formats, fmt) ||
        !format_supported(cap->supported_input_formats, fmt_overlay)) {
        fprintf(stderr, "test_job_task overlay: format not supported\n");
        return EXIT_FAILURE;
    }

    memset(&src_ctrl, 0, sizeof(src_ctrl));
    src_ctrl.width = w;
    src_ctrl.height = h;
    src_ctrl.format = fmt;
    src_ctrl.ifconherent = 0;
    src_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    src_ctrl.use_clmem = use_clmem;
    src[0] = alloc_cme_img(&src_ctrl, &error);
    if (error || !src[0]) {
        fprintf(stderr, "test_job_task overlay: alloc base failed\n");
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
        free_cme_img(src[0]);
        return EXIT_FAILURE;
    }
    src[1]->x = x;
    src[1]->y = y;

    memset(&dst_ctrl, 0, sizeof(dst_ctrl));
    dst_ctrl.width = w;
    dst_ctrl.height = h;
    dst_ctrl.format = fmt;
    dst_ctrl.ifconherent = 0;
    dst_ctrl.colormode = CME_COLOR_SPACE_DEFAULT;
    dst_ctrl.use_clmem = use_clmem;
    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !dst) {
        free_cme_img(src[1]);
        free_cme_img(src[0]);
        return EXIT_FAILURE;
    }

    fin = fopen(input_base, "rb");
    if (!fin) {
        free_cme_img(dst);
        free_cme_img(src[1]);
        free_cme_img(src[0]);
        return EXIT_FAILURE;
    }
    read_data_from_file(src[0], fin);
    fclose(fin);
    fin = fopen(input_overlay, "rb");
    if (!fin) {
        free_cme_img(dst);
        free_cme_img(src[1]);
        free_cme_img(src[0]);
        return EXIT_FAILURE;
    }
    read_data_from_file(src[1], fin);
    fclose(fin);

    alpha_arr[0] = alpha0;
    alpha_arr[1] = alpha1;
    for (i = 0; i < nimgs; i++)
        src_ptrs[i] = src[i];

    for (i = 0; i < n_round; i++) {
        unsigned long t0 = get_mono_time_ns();
        cme_job_handle job = cme_begin_task();
        if (job < 0) {
            free_cme_img(dst);
            free_cme_img(src[1]);
            free_cme_img(src[0]);
            return EXIT_FAILURE;
        }
        ret = cme_2d_overlay_task(job, nimgs, src_ptrs, alpha_arr, dst);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_job_task overlay: cme_2d_overlay_task failed %d\n", (int)ret);
            (void)cme_free_pending_job(job);
            free_cme_img(dst);
            free_cme_img(src[1]);
            free_cme_img(src[0]);
            return EXIT_FAILURE;
        }
        ret = cme_end_task(job, 1, NULL);
        unsigned long t1 = get_mono_time_ns();
        total_time += (t1 - t0) / 1000000.0;
        printf("test_job_task overlay: cme_2d_overlay_task time: %lu ns (%.3f ms)\n", t1 - t0,
               (t1 - t0) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            free_cme_img(dst);
            free_cme_img(src[1]);
            free_cme_img(src[0]);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_job_task overlay: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_job_task overlay: OK (%d layers %dx%d %s, overlay %dx%d %s, alpha %d %d)\n", nimgs, w, h,
           format_str, W, H, format_overlay_str, alpha0, alpha1);
    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            free_cme_img(dst);
            free_cme_img(src[1]);
            free_cme_img(src[0]);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_job_task overlay: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }
    free_cme_img(dst);
    free_cme_img(src[1]);
    free_cme_img(src[0]);
    return EXIT_SUCCESS;
}

static int job_crfrc(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl, dst_ctrl;
    cme_img* src = NULL;
    cme_img* dst = NULL;
    cme_crfrc_ctrl ctrl;
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
    FILE* fin = NULL;
    FILE* fout = NULL;
    int use_clmem = 1;
    int n_round = 1;
    int enable_crop = 0;
    int crop_x = 0, crop_y = 0, crop_w = 0, crop_h = 0;
    double total_time = 0.0;

    optind = 1;
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
            return EXIT_FAILURE;
        }
    }
    if (!input_file || src_w <= 0 || src_h <= 0 || !src_fmt_str || !dst_fmt_str || !flip_str || !rotate_str) {
        fprintf(stderr, "test_job_task crfrc: missing -i -w -h -f -F -m -r\n");
        return EXIT_FAILURE;
    }
    src_fmt = format_str_to_enum(src_fmt_str);
    dst_fmt = format_str_to_enum(dst_fmt_str);
    if (src_fmt == CME_FORMAT_UNKNOWN || dst_fmt == CME_FORMAT_UNKNOWN) {
        fprintf(stderr, "test_job_task crfrc: unknown format\n");
        return EXIT_FAILURE;
    }
    if (parse_flip_mode(flip_str, &flip_mode) != 0 || parse_rotate(rotate_str, &rotate) != 0) {
        fprintf(stderr, "test_job_task crfrc: bad flip/rotate\n");
        return EXIT_FAILURE;
    }
    if (cs_str && parse_cvtcolor_mode(cs_str, &dst_colormode) != 0) {
        fprintf(stderr, "test_job_task crfrc: unknown colorspace\n");
        return EXIT_FAILURE;
    }
    if (enable_crop) {
        if (crop_x < 0 || crop_y < 0 || crop_w <= 0 || crop_h <= 0 ||
            crop_x + crop_w > src_w || crop_y + crop_h > src_h) {
            fprintf(stderr, "test_job_task crfrc: invalid crop\n");
            return EXIT_FAILURE;
        }
    }
    cap = query_all_capability();
    if (!cap || !format_supported(cap->supported_input_formats, src_fmt) ||
        !format_supported(cap->supported_output_formats, dst_fmt)) {
        fprintf(stderr, "test_job_task crfrc: format not supported\n");
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
    dst = alloc_cme_img(&dst_ctrl, &error);
    if (error || !src || !dst) {
        if (dst) free_cme_img(dst);
        if (src) free_cme_img(src);
        return EXIT_FAILURE;
    }
    fin = fopen(input_file, "rb");
    if (!fin) {
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);

    for (int i = 0; i < n_round; i++) {
        unsigned long t0 = get_mono_time_ns();
        cme_job_handle job = cme_begin_task();
        if (job < 0) {
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_2d_crop_resize_flip_rotation_cvtcolor_task(job, src, dst, &ctrl);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_job_task crfrc: cme_2d_crop_resize_flip_rotation_cvtcolor_task failed %d\n",
                    (int)ret);
            (void)cme_free_pending_job(job);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_end_task(job, 1, NULL);
        unsigned long t1 = get_mono_time_ns();
        total_time += (t1 - t0) / 1000000.0;
        printf("test_job_task crfrc: cme_2d_crop_resize_flip_rotation_cvtcolor_task time: %lu ns (%.3f ms)\n",
               t1 - t0, (t1 - t0) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_job_task crfrc: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_job_task crfrc: OK (src %dx%d %s -> dst %dx%d %s, flip=%s rotate=%s crop=%s)\n", src_w, src_h,
           src_fmt_str, dst_w, dst_h, dst_fmt_str, flip_str, rotate_str, enable_crop ? "on" : "off");
    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_job_task crfrc: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }
    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}

static int job_fisheye_remap(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl src_ctrl, dst_ctrl;
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

    optind = 1;
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
            return EXIT_FAILURE;
        }
    }
    if (!input_file || src_w <= 0 || src_h <= 0 || !format_str) {
        fprintf(stderr, "test_job_task fisheye_remap: missing -i -w -h -f\n");
        return EXIT_FAILURE;
    }
    if (src_w < dst_w || src_h < dst_h) {
        fprintf(stderr, "test_job_task fisheye_remap: src size must be >= dst size\n");
        return EXIT_FAILURE;
    }
    src_fmt = format_str_to_enum(format_str);
    if (src_fmt == CME_FORMAT_UNKNOWN) {
        return EXIT_FAILURE;
    }
    cap = query_all_capability();
    if (!cap || !format_supported(cap->supported_input_formats, src_fmt) ||
        !format_supported(cap->supported_output_formats, CME_FORMAT_RGB_888)) {
        fprintf(stderr, "test_job_task fisheye_remap: format not supported\n");
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
    dst = alloc_cme_img(&dst_ctrl, &error);
    tmp = alloc_cme_img(&dst_ctrl, &error);
    if (error || !src || !dst || !tmp) {
        if (tmp) free_cme_img(tmp);
        if (dst) free_cme_img(dst);
        if (src) free_cme_img(src);
        return EXIT_FAILURE;
    }
    fin = fopen(input_file, "rb");
    if (!fin) {
        free_cme_img(tmp);
        free_cme_img(dst);
        free_cme_img(src);
        return EXIT_FAILURE;
    }
    read_data_from_file(src, fin);
    fclose(fin);

    {
        size_t map_size = (size_t)src_w * (size_t)src_h * sizeof(float);
        if (map_x_file) {
            map_x = (float*)malloc(map_size);
            if (!map_x) {
                free_cme_img(tmp);
                free_cme_img(dst);
                free_cme_img(src);
                return EXIT_FAILURE;
            }
            fin = fopen(map_x_file, "rb");
            if (!fin) {
                free(map_x);
                free_cme_img(tmp);
                free_cme_img(dst);
                free_cme_img(src);
                return EXIT_FAILURE;
            }
            n = fread(map_x, 1, map_size, fin);
            fclose(fin);
            if (n != map_size) {
                free(map_x);
                free_cme_img(tmp);
                free_cme_img(dst);
                free_cme_img(src);
                return EXIT_FAILURE;
            }
        }
        if (map_y_file) {
            map_y = (float*)malloc(map_size);
            if (!map_y) {
                free(map_x);
                free_cme_img(tmp);
                free_cme_img(dst);
                free_cme_img(src);
                return EXIT_FAILURE;
            }
            fin = fopen(map_y_file, "rb");
            if (!fin) {
                free(map_y);
                free(map_x);
                free_cme_img(tmp);
                free_cme_img(dst);
                free_cme_img(src);
                return EXIT_FAILURE;
            }
            n = fread(map_y, 1, map_size, fin);
            fclose(fin);
            if (n != map_size) {
                free(map_y);
                free(map_x);
                free_cme_img(tmp);
                free_cme_img(dst);
                free_cme_img(src);
                return EXIT_FAILURE;
            }
        }
        if (!map_x || !map_y) {
            float* gen_x = NULL;
            float* gen_y = NULL;
            if (generate_remap_maps(src_w, src_h, &gen_x, &gen_y) != 0) {
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
        unsigned long t0 = get_mono_time_ns();
        cme_job_handle job = cme_begin_task();
        if (job < 0) {
            free(map_y);
            free(map_x);
            free_cme_img(tmp);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_2d_fisheye_remap_task(job, src, dst, tmp, &ctrl);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_job_task fisheye_remap: cme_2d_fisheye_remap_task failed %d\n", (int)ret);
            (void)cme_free_pending_job(job);
            free(map_y);
            free(map_x);
            free_cme_img(tmp);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        ret = cme_end_task(job, 1, NULL);
        unsigned long t1 = get_mono_time_ns();
        total_time += (t1 - t0) / 1000000.0;
        printf("test_job_task fisheye_remap: cme_2d_fisheye_remap_task time: %lu ns (%.3f ms)\n", t1 - t0,
               (t1 - t0) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            free(map_y);
            free(map_x);
            free_cme_img(tmp);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_job_task fisheye_remap: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_job_task fisheye_remap: OK (%dx%d %s -> %dx%d RGB888)\n", src_w, src_h, format_str, dst_w, dst_h);
    free(map_y);
    free(map_x);
    if (output_file) {
        fout = fopen(output_file, "wb");
        if (!fout) {
            free_cme_img(tmp);
            free_cme_img(dst);
            free_cme_img(src);
            return EXIT_FAILURE;
        }
        write_data_to_file(dst, fout);
        fclose(fout);
        printf("test_job_task fisheye_remap: dumped to '%s' (%d bytes)\n", output_file, dst->tlength);
    }
    free_cme_img(tmp);
    free_cme_img(dst);
    free_cme_img(src);
    return EXIT_SUCCESS;
}

int test_job_task(int argc, char** argv)
{
    const char* exe = argv[0] ? argv[0] : "test_libcme";

    if (argc < 3) {
        job_task_usage(exe);
        return EXIT_FAILURE;
    }

    int sub_argc = argc - 2;
    char** sub_argv = argv + 2;
    const char* kind = sub_argv[0];

    if (strcmp(kind, "resize") == 0)
        return job_resize(sub_argc, sub_argv);
    if (strcmp(kind, "cvtcolor") == 0)
        return job_cvtcolor(sub_argc, sub_argv);
    if (strcmp(kind, "resize_cvtcolor_flip") == 0)
        return job_resize_cvtcolor_flip(sub_argc, sub_argv);
    if (strcmp(kind, "overlay") == 0)
        return job_overlay(sub_argc, sub_argv);
    if (strcmp(kind, "crfrc") == 0)
        return job_crfrc(sub_argc, sub_argv);
    if (strcmp(kind, "fisheye_remap") == 0)
        return job_fisheye_remap(sub_argc, sub_argv);
    if (strcmp(kind, "est_sim_tfm") == 0)
        return job_est_sim_tfm(sub_argc, sub_argv);
    if (strcmp(kind, "cosine_sim") == 0)
        return job_cosine_sim(sub_argc, sub_argv);
    if (strcmp(kind, "normalize") == 0)
        return job_normalize(sub_argc, sub_argv);
    if (strcmp(kind, "affine_transform") == 0)
        return job_affine(sub_argc, sub_argv);
    if (strcmp(kind, "imcopy") == 0)
        return job_imcopy(sub_argc, sub_argv);

    fprintf(stderr, "%s: job_task: unknown kind '%s'\n", exe, kind);
    job_task_usage(exe);
    return EXIT_FAILURE;
}
