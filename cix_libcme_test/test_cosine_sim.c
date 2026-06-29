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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cme/cme.h>
#include "test_common.h"
#include "test_cosine_sim.h"

extern char* optarg;

#define ROUND_UP_SZ(x, a) (((x) + (size_t)(a)-1) / (a) * (a))

static void cosine_sim_usage(const char* prog)
{
    fprintf(stderr, "Usage: %s cosine_sim -i <vec_a.bin> -j <vec_b.bin> -d <dim> [-t 0|1] [-n rounds] [-c use_clmem]\n", prog);
    fprintf(stderr, "  (wraps cme_2d_cosine_sim; vectors are raw float32, length dim)\n");
    fprintf(stderr, "  -i   first vector file (required)\n");
    fprintf(stderr, "  -j   second vector file (required)\n");
    fprintf(stderr, "  -d   vector length (number of floats; both inputs must match) (required)\n");
    fprintf(stderr, "  -t   use explicit tmp FLOAT32_VECTOR buffer: 1 or 0 (default 0)\n");
    fprintf(stderr, "  -n   API test rounds (default 1)\n");
    fprintf(stderr, "  -c   use clmem (must be 1 for OpenCL path; default 1)\n");
}

/* Minimum tmp_img->width (floats) per cixcl cosine_sim tmp check. */
static int cosine_sim_tmp_min_width(int vector_dim)
{
    const int workgroup_size = 128;
    int num_packed = (vector_dim + 3) / 4;
    int num_workgroups = (num_packed + workgroup_size - 1) / workgroup_size;
    size_t partial_size_raw = (size_t)num_workgroups * sizeof(float);
    size_t partial_size = ROUND_UP_SZ(partial_size_raw, 128);
    return (int)(3 * partial_size / sizeof(float));
}

static float cpu_cosine_sim(const float* a, const float* b, int n)
{
    double dot = 0.0, na = 0.0, nb = 0.0;
    int i;
    for (i = 0; i < n; i++) {
        dot += (double)a[i] * (double)b[i];
        na += (double)a[i] * (double)a[i];
        nb += (double)b[i] * (double)b[i];
    }
    if (na < 1e-30 || nb < 1e-30)
        return 0.0f;
    return (float)(dot / (sqrt(na) * sqrt(nb)));
}

static int read_float_vector_file(const char* path, float* dst, int dim, const char* label)
{
    FILE* fp = fopen(path, "rb");
    size_t n;
    if (!fp) {
        fprintf(stderr, "test_cosine_sim: cannot open %s '%s'\n", label, path);
        return -1;
    }
    n = fread(dst, sizeof(float), (size_t)dim, fp);
    fclose(fp);
    if ((int)n != dim) {
        fprintf(stderr, "test_cosine_sim: %s '%s' expected %d floats, got %zu\n", label, path, dim, n);
        return -1;
    }
    return 0;
}

int test_cosine_sim(int argc, char** argv)
{
    const cme_capacity* cap;
    alloc_img_ctrl vec_ctrl;
    alloc_img_ctrl tmp_ctrl;
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
    int i;

    while ((c = getopt(argc, argv, "i:j:d:t:n:c:")) != -1) {
        switch (c) {
        case 'i': path_a = optarg; break;
        case 'j': path_b = optarg; break;
        case 'd': dim = atoi(optarg); break;
        case 't': use_tmp = atoi(optarg); break;
        case 'n': n_round = atoi(optarg); break;
        case 'c': use_clmem = atoi(optarg); break;
        default:
            cosine_sim_usage(argv[0] ? argv[0] : "test_cme");
            return EXIT_FAILURE;
        }
    }

    if (!path_a || !path_b || dim <= 0) {
        fprintf(stderr, "test_cosine_sim: missing -i -j or invalid -d\n");
        cosine_sim_usage(argv[0] ? argv[0] : "test_cme");
        return EXIT_FAILURE;
    }

    if (use_clmem != 1) {
        fprintf(stderr, "test_cosine_sim: OpenCL implementation requires -c 1 (clmem)\n");
        return EXIT_FAILURE;
    }

    cap = query_all_capability();
    if (!cap) {
        fprintf(stderr, "test_cosine_sim: query_all_capability() returned NULL\n");
        return EXIT_FAILURE;
    }
    if (!format_supported(cap->supported_input_formats, CME_FORMAT_FLOAT32_VECTOR)) {
        fprintf(stderr, "test_cosine_sim: FLOAT32_VECTOR not supported on this device\n");
        return EXIT_FAILURE;
    }

    host_a = (float*)malloc((size_t)dim * sizeof(float));
    host_b = (float*)malloc((size_t)dim * sizeof(float));
    if (!host_a || !host_b) {
        fprintf(stderr, "test_cosine_sim: out of memory\n");
        free(host_a);
        free(host_b);
        return EXIT_FAILURE;
    }
    if (read_float_vector_file(path_a, host_a, dim, "vec_a") != 0 ||
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
    if (error || !img_a) {
        fprintf(stderr, "test_cosine_sim: alloc_cme_img(a) failed (error=%d)\n", error);
        free(host_a);
        free(host_b);
        return EXIT_FAILURE;
    }
    img_b = alloc_cme_img(&vec_ctrl, &error);
    if (error || !img_b) {
        fprintf(stderr, "test_cosine_sim: alloc_cme_img(b) failed (error=%d)\n", error);
        free_cme_img(img_a);
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
            fprintf(stderr, "test_cosine_sim: alloc_cme_img(tmp) failed (error=%d), need width>=%d\n", error, tw);
            free_cme_img(img_b);
            free_cme_img(img_a);
            free(host_a);
            free(host_b);
            return EXIT_FAILURE;
        }
    }

    printf("test_cosine_sim: CPU reference cosine = %.8f (dim=%d)\n", cpu_cosine_sim(host_a, host_b, dim), dim);

    for (i = 0; i < n_round; i++) {
        unsigned long start = get_mono_time_ns();
        ret = cme_2d_cosine_sim(img_a, img_b, &result, tmp, 1, NULL);
        unsigned long end = get_mono_time_ns();
        if (end < start) {
            fprintf(stderr, "test_cosine_sim: time error\n");
            if (tmp) free_cme_img(tmp);
            free_cme_img(img_b);
            free_cme_img(img_a);
            free(host_a);
            free(host_b);
            return EXIT_FAILURE;
        }
        total_time += (end - start) / 1000000.0;
        printf("test_cosine_sim: cme_2d_cosine_sim time: %lu ns (%.3f ms)\n", end - start, (end - start) / 1000000.0);
        if (ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_cosine_sim: API returned %d\n", (int)ret);
            if (tmp) free_cme_img(tmp);
            free_cme_img(img_b);
            free_cme_img(img_a);
            free(host_a);
            free(host_b);
            return EXIT_FAILURE;
        }
    }

    if (n_round > 0) {
        printf("test_cosine_sim: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_cosine_sim: GPU cosine similarity = %.8f\n", result);
    printf("test_cosine_sim: OK\n");

    if (tmp)
        free_cme_img(tmp);
    free_cme_img(img_b);
    free_cme_img(img_a);
    free(host_a);
    free(host_b);
    return EXIT_SUCCESS;
}
