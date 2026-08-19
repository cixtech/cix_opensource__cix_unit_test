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
#include "test_argmax_val.h"

extern char* optarg;

#define ARGMAX_EPS 1e-5f

static void argmax_usage(const char* prog)
{
    fprintf(stderr, "Usage A (builtin vectors):\n");
    fprintf(stderr, "  %s argmax_val -g 1 [-n rounds]\n", prog);
    fprintf(stderr, "Usage B (float32 file):\n");
    fprintf(stderr, "  %s argmax_val -i <data.bin> -d <n> [-o out.txt] [-n rounds]\n", prog);
    fprintf(stderr, "  data.bin: n float32 values (little-endian)\n");
}

static float ref_max_val(const float* data, int n)
{
    float m = data[0];
    int i;

    for (i = 1; i < n; i++) {
        if (data[i] > m)
            m = data[i];
    }
    return m;
}

static int float_near(float a, float b)
{
    return fabsf(a - b) <= ARGMAX_EPS;
}

static int run_argmax_once(const float* data, int n, float* out_max)
{
    CME_RET ret = cme_argmax_val(data, n, out_max);

    if (ret != CME_RET_SUCCESS) {
        fprintf(stderr, "test_argmax_val: cme_argmax_val returned %d\n", (int)ret);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int run_argmax_rounds(const float* data, int n, int n_round, float* out_max)
{
    int i;
    double total_ms = 0.0;

    for (i = 0; i < n_round; i++) {
        unsigned long t0 = get_mono_time_ns();
        if (run_argmax_once(data, n, out_max) != EXIT_SUCCESS)
            return EXIT_FAILURE;
        unsigned long t1 = get_mono_time_ns();
        if (t1 >= t0)
            total_ms += (t1 - t0) / 1000000.0;
        if (i == 0) {
            printf("test_argmax_val: cme_argmax_val time: %lu ns (%.3f ms)\n",
                   t1 - t0, (t1 - t0) / 1000000.0);
        }
    }
    if (n_round > 0) {
        printf("test_argmax_val: %d rounds, average time: %.3f ms\n", n_round, total_ms / n_round);
    }
    return EXIT_SUCCESS;
}

static int check_case(const char* name, const float* data, int n, float expected, int n_round)
{
    float got = 0.0f;
    float ref = ref_max_val(data, n);

    if (!float_near(ref, expected)) {
        fprintf(stderr, "test_argmax_val: internal ref mismatch for '%s'\n", name);
        return EXIT_FAILURE;
    }
    if (run_argmax_rounds(data, n, n_round, &got) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (!float_near(got, expected)) {
        fprintf(stderr, "test_argmax_val: '%s' expected %.8f, got %.8f\n", name, expected, got);
        return EXIT_FAILURE;
    }
    printf("test_argmax_val: '%s' ok (n=%d max=%.8f)\n", name, n, got);
    return EXIT_SUCCESS;
}

static int test_invalid_params(void)
{
    float data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float max_val = 0.0f;
    CME_RET ret;

    ret = cme_argmax_val(NULL, 4, &max_val);
    if (ret != CME_RET_INVALID_PARAM) {
        fprintf(stderr, "test_argmax_val: NULL data should return INVALID_PARAM, got %d\n", (int)ret);
        return EXIT_FAILURE;
    }
    ret = cme_argmax_val(data, 0, &max_val);
    if (ret != CME_RET_INVALID_PARAM) {
        fprintf(stderr, "test_argmax_val: n=0 should return INVALID_PARAM, got %d\n", (int)ret);
        return EXIT_FAILURE;
    }
    ret = cme_argmax_val(data, 4, NULL);
    if (ret != CME_RET_INVALID_PARAM) {
        fprintf(stderr, "test_argmax_val: NULL max_val should return INVALID_PARAM, got %d\n", (int)ret);
        return EXIT_FAILURE;
    }
    printf("test_argmax_val: invalid param checks ok\n");
    return EXIT_SUCCESS;
}

static int builtin_golden(int n_round)
{
    static const float vec_n1[] = {3.14f};
    static const float vec_n4[] = {1.0f, 2.0f, 9.0f, 4.0f};
    static const float vec_n5[] = {1.0f, 2.0f, 3.0f, 4.0f, 100.0f};
    static const float vec_n7[] = {0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 42.0f};
    static const float vec_neg[] = {-5.0f, -1.0f, -10.0f, -2.0f};
    float vec_n512[512];
    int i;

    for (i = 0; i < 512; i++)
        vec_n512[i] = (float)(i % 17) * 0.25f;
    vec_n512[123] = 99.0f;

    printf("test_argmax_val: builtin -g 1\n");
    if (test_invalid_params() != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (check_case("n=1", vec_n1, 1, 3.14f, n_round) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (check_case("n=4 aligned", vec_n4, 4, 9.0f, n_round) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (check_case("n=5 tail", vec_n5, 5, 100.0f, n_round) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (check_case("n=7 tail", vec_n7, 7, 42.0f, n_round) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (check_case("negative values", vec_neg, 4, -1.0f, n_round) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    if (check_case("n=512", vec_n512, 512, 99.0f, n_round) != EXIT_SUCCESS)
        return EXIT_FAILURE;
    printf("test_argmax_val: OK\n");
    return EXIT_SUCCESS;
}

static int read_float_file(const char* path, float* dst, int n, const char* label)
{
    FILE* fp = fopen(path, "rb");
    size_t got;

    if (!fp) {
        fprintf(stderr, "test_argmax_val: cannot open %s '%s'\n", label, path);
        return -1;
    }
    got = fread(dst, sizeof(float), (size_t)n, fp);
    fclose(fp);
    if ((int)got != n) {
        fprintf(stderr, "test_argmax_val: %s '%s' need %d floats, got %zu\n", label, path, n, got);
        return -1;
    }
    return 0;
}

int test_argmax_val(int argc, char** argv)
{
    int c;
    int g_mode = 0;
    char* input_file = NULL;
    char* output_file = NULL;
    int dim = 0;
    int n_round = 1;
    float* data = NULL;
    float max_val = 0.0f;
    float expected = 0.0f;
    FILE* fout = NULL;

    while ((c = getopt(argc, argv, "g:i:d:o:n:")) != -1) {
        switch (c) {
        case 'g': g_mode = atoi(optarg); break;
        case 'i': input_file = optarg; break;
        case 'd': dim = atoi(optarg); break;
        case 'o': output_file = optarg; break;
        case 'n': n_round = atoi(optarg); break;
        default:
            argmax_usage(argv[0] ? argv[0] : "test_libcme");
            return EXIT_FAILURE;
        }
    }

    if (g_mode == 1)
        return builtin_golden(n_round);

    if (!input_file || dim <= 0) {
        fprintf(stderr, "test_argmax_val: use -g 1 or -i -d\n");
        argmax_usage(argv[0] ? argv[0] : "test_libcme");
        return EXIT_FAILURE;
    }

    data = (float*)malloc((size_t)dim * sizeof(float));
    if (!data) {
        fprintf(stderr, "test_argmax_val: out of memory\n");
        return EXIT_FAILURE;
    }
    if (read_float_file(input_file, data, dim, "data") != 0) {
        free(data);
        return EXIT_FAILURE;
    }

    expected = ref_max_val(data, dim);
    if (run_argmax_rounds(data, dim, n_round, &max_val) != EXIT_SUCCESS) {
        free(data);
        return EXIT_FAILURE;
    }
    if (!float_near(max_val, expected)) {
        fprintf(stderr, "test_argmax_val: expected %.8f, got %.8f\n", expected, max_val);
        free(data);
        return EXIT_FAILURE;
    }
    printf("test_argmax_val: OK (n=%d max=%.8f)\n", dim, max_val);

    if (output_file) {
        fout = fopen(output_file, "w");
        if (!fout) {
            fprintf(stderr, "test_argmax_val: cannot open output '%s'\n", output_file);
            free(data);
            return EXIT_FAILURE;
        }
        fprintf(fout, "%.8f\n", max_val);
        fclose(fout);
        printf("test_argmax_val: write output to '%s'\n", output_file);
    }

    free(data);
    return EXIT_SUCCESS;
}
