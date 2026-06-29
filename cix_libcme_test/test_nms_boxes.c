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
#include "test_nms_boxes.h"

extern char* optarg;

static void nms_usage(const char* prog)
{
    fprintf(stderr, "Usage A (builtin, same 6 boxes as libcme sample test_nms_boxes.c):\n");
    fprintf(stderr, "  %s nms_boxes -g 1\n", prog);
    fprintf(stderr, "Usage B (raw float32 LE):\n");
    fprintf(stderr, "  %s nms_boxes -N <num_boxes> -b boxes.bin -S scores.bin [-u score_th] [-I nms_th] [-k top_k] [-o out.txt] [-n rounds]\n", prog);
    fprintf(stderr, "  boxes.bin: N*4 floats [x,y,w,h]; scores.bin: N floats\n");
}

static int read_float_file(const char* path, float* dst, size_t nfloats, const char* label)
{
    FILE* fp = fopen(path, "rb");
    size_t n;
    if (!fp) {
        fprintf(stderr, "test_nms_boxes: cannot open %s '%s'\n", label, path);
        return -1;
    }
    n = fread(dst, sizeof(float), nfloats, fp);
    fclose(fp);
    if (n != nfloats) {
        fprintf(stderr, "test_nms_boxes: %s '%s' need %zu floats, got %zu\n", label, path, nfloats, n);
        return -1;
    }
    return 0;
}

static int run_nms(cme_nms_boxes_ctrl* ctrl, int n_round, const char* out_path)
{
    int nb = ctrl->num_boxes;
    int* indices = (int*)malloc((size_t)nb * sizeof(int));
    cme_nms_boxes_ret ret;
    CME_RET status;
    int i, j;
    double total_ms = 0.0;

    if (!indices) {
        fprintf(stderr, "test_nms_boxes: out of memory\n");
        return EXIT_FAILURE;
    }
    memset(indices, 0xff, (size_t)nb * sizeof(int));
    memset(&ret, 0, sizeof(ret));
    ret.indices = indices;
    ret.num_output = 0;

    for (i = 0; i < n_round; i++) {
        unsigned long t0 = get_mono_time_ns();
        status = cme_2d_nms_boxes(ctrl, &ret, 1, NULL);
        unsigned long t1 = get_mono_time_ns();
        if (t1 >= t0)
            total_ms += (t1 - t0) / 1000000.0;
        printf("test_nms_boxes: cme_2d_nms_boxes time: %lu ns (%.3f ms)\n", t1 - t0, (t1 - t0) / 1000000.0);
        if (status != CME_RET_SUCCESS) {
            fprintf(stderr, "test_nms_boxes: API returned %d\n", (int)status);
            free(indices);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0)
        printf("test_nms_boxes: %d rounds, average %.3f ms\n", n_round, total_ms / n_round);

    printf("test_nms_boxes: num_output=%d\n", ret.num_output);
    for (j = 0; j < ret.num_output; j++) {
        int idx = indices[j];
        printf("  kept[%d]: index=%d score=%.6f box xywh=[%.4f,%.4f,%.4f,%.4f]\n", j, idx,
               ctrl->scores[idx],
               ctrl->bboxes[idx * 4 + 0], ctrl->bboxes[idx * 4 + 1],
               ctrl->bboxes[idx * 4 + 2], ctrl->bboxes[idx * 4 + 3]);
    }

    if (out_path) {
        FILE* fo = fopen(out_path, "w");
        if (!fo) {
            fprintf(stderr, "test_nms_boxes: cannot open '%s'\n", out_path);
            free(indices);
            return EXIT_FAILURE;
        }
        fprintf(fo, "%d\n", ret.num_output);
        for (j = 0; j < ret.num_output; j++)
            fprintf(fo, "%d\n", indices[j]);
        fclose(fo);
        printf("test_nms_boxes: wrote '%s'\n", out_path);
    }

    free(indices);
    printf("test_nms_boxes: OK\n");
    return EXIT_SUCCESS;
}

static int builtin_golden(int n_round, const char* out_path)
{
    static const float bboxes[] = {
        100.0f, 100.0f, 200.0f, 200.0f,
        110.0f, 110.0f, 190.0f, 190.0f,
        500.0f, 500.0f, 100.0f, 100.0f,
        520.0f, 520.0f, 80.0f, 80.0f,
        50.0f, 50.0f, 30.0f, 30.0f,
        105.0f, 105.0f, 180.0f, 180.0f,
    };
    static const float scores[] = {0.95f, 0.80f, 0.70f, 0.60f, 0.50f, 0.75f};
    const int num_boxes = 6;
    float* bb = (float*)malloc(sizeof(bboxes));
    float* sc = (float*)malloc(sizeof(scores));
    cme_nms_boxes_ctrl ctrl;
    int rc;

    if (!bb || !sc) {
        free(bb);
        free(sc);
        return EXIT_FAILURE;
    }
    memcpy(bb, bboxes, sizeof(bboxes));
    memcpy(sc, scores, sizeof(scores));

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.bboxes = bb;
    ctrl.scores = sc;
    ctrl.num_boxes = num_boxes;
    ctrl.score_threshold = 0.3f;
    ctrl.nms_threshold = 0.5f;
    ctrl.top_k = 0;

    printf("test_nms_boxes: builtin -g 1 (6 boxes, OpenCV-style NMSBoxes)\n");
    rc = run_nms(&ctrl, n_round, out_path);
    free(bb);
    free(sc);
    return rc;
}

int test_nms_boxes(int argc, char** argv)
{
    int c;
    int g_mode = 0;
    int N = 0;
    char* fb = NULL;
    char* fs = NULL;
    char* out_path = NULL;
    float score_th = 0.3f;
    float nms_th = 0.5f;
    int top_k = 0;
    int n_round = 1;
    float* bb = NULL;
    float* sc = NULL;
    cme_nms_boxes_ctrl ctrl;
    int rc;

    while ((c = getopt(argc, argv, "g:N:b:S:u:I:k:o:n:")) != -1) {
        switch (c) {
        case 'g': g_mode = atoi(optarg); break;
        case 'N': N = atoi(optarg); break;
        case 'b': fb = optarg; break;
        case 'S': fs = optarg; break;
        case 'u': score_th = (float)atof(optarg); break;
        case 'I': nms_th = (float)atof(optarg); break;
        case 'k': top_k = atoi(optarg); break;
        case 'o': out_path = optarg; break;
        case 'n': n_round = atoi(optarg); break;
        default:
            nms_usage(argv[0] ? argv[0] : "test_cme");
            return EXIT_FAILURE;
        }
    }

    if (g_mode == 1)
        return builtin_golden(n_round, out_path);

    if (!fb || !fs || N <= 0) {
        fprintf(stderr, "test_nms_boxes: use -g 1 or -N -b -S\n");
        nms_usage(argv[0] ? argv[0] : "test_cme");
        return EXIT_FAILURE;
    }

    bb = (float*)malloc((size_t)(N * 4) * sizeof(float));
    sc = (float*)malloc((size_t)N * sizeof(float));
    if (!bb || !sc) {
        free(bb);
        free(sc);
        return EXIT_FAILURE;
    }
    if (read_float_file(fb, bb, (size_t)(N * 4), "bboxes") != 0 ||
        read_float_file(fs, sc, (size_t)N, "scores") != 0) {
        free(bb);
        free(sc);
        return EXIT_FAILURE;
    }

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.bboxes = bb;
    ctrl.scores = sc;
    ctrl.num_boxes = N;
    ctrl.score_threshold = score_th;
    ctrl.nms_threshold = nms_th;
    ctrl.top_k = top_k;

    printf("test_nms_boxes: file mode N=%d top_k=%d\n", N, top_k);
    rc = run_nms(&ctrl, n_round, out_path);
    free(bb);
    free(sc);
    return rc;
}
