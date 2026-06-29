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
#include "test_detection_post_process.h"

extern char* optarg;

static void dpp_usage(const char* prog)
{
    fprintf(stderr, "Usage A (builtin, same tensors as libcme sample):\n");
    fprintf(stderr, "  %s detection_post_process -g 1   # regular NMS (use_regular_nms=1)\n", prog);
    fprintf(stderr, "  %s detection_post_process -g 2   # fast NMS (use_regular_nms=0)\n", prog);
    fprintf(stderr, "Usage B (raw float32 LE files):\n");
    fprintf(stderr, "  %s detection_post_process -N <anchors> -C <classes> -e enc.bin -p cls.bin -a anc.bin [options]\n", prog);
    fprintf(stderr, "  enc.bin: N*4 floats [dy,dx,dh,dw]; cls.bin: N*(C+1); anc.bin: N*4 [y,x,h,w] centers/sizes\n");
    fprintf(stderr, "Options: -s scales (default 10,10,5,5) -u nms_score_thresh (0.3) -I iou_thresh (0.5)\n");
    fprintf(stderr, "  -M max_detections (4) -K max_classes_per_detection (1) -R use_regular_nms (0) -P detection_per_class (4)\n");
    fprintf(stderr, "  -o <txt> write detections -n rounds (1)\n");
}

static int parse_scales(const char* str, float* sy, float* sx, float* sh, float* sw)
{
    if (!str || sscanf(str, "%f,%f,%f,%f", sy, sx, sh, sw) != 4)
        return -1;
    return 0;
}

static int read_float_file(const char* path, float* dst, size_t nfloats, const char* label)
{
    FILE* fp = fopen(path, "rb");
    size_t n;
    if (!fp) {
        fprintf(stderr, "test_detection_post_process: cannot open %s '%s'\n", label, path);
        return -1;
    }
    n = fread(dst, sizeof(float), nfloats, fp);
    fclose(fp);
    if (n != nfloats) {
        fprintf(stderr, "test_detection_post_process: %s '%s' need %zu floats, got %zu\n",
                label, path, nfloats, n);
        return -1;
    }
    return 0;
}

static void print_detections(const float* boxes, const float* classes, const float* scores, int ndet)
{
    int i;
    for (i = 0; i < ndet; i++) {
        printf("  det[%d]: ymin,xmin,ymax,xmax=[%.6f,%.6f,%.6f,%.6f] class=%.0f score=%.6f\n",
               i, boxes[i * 4 + 0], boxes[i * 4 + 1], boxes[i * 4 + 2], boxes[i * 4 + 3],
               classes[i], scores[i]);
    }
}

static int run_one(cme_detection_post_process_ctrl* ctrl, int n_round, const char* out_path)
{
    int nout = ctrl->max_detections * ctrl->max_classes_per_detection;
    float* out_boxes = (float*)calloc((size_t)(nout * 4), sizeof(float));
    float* out_classes = (float*)calloc((size_t)nout, sizeof(float));
    float* out_scores = (float*)calloc((size_t)nout, sizeof(float));
    int* num_detections = (int*)calloc(1, sizeof(int));
    cme_detection_post_process_ret ret;
    CME_RET status;
    int i;
    double total_ms = 0.0;

    if (!out_boxes || !out_classes || !out_scores || !num_detections) {
        fprintf(stderr, "test_detection_post_process: out of memory\n");
        free(out_boxes);
        free(out_classes);
        free(out_scores);
        free(num_detections);
        return EXIT_FAILURE;
    }

    memset(&ret, 0, sizeof(ret));
    ret.output_boxes = out_boxes;
    ret.output_classes = out_classes;
    ret.output_scores = out_scores;
    ret.num_detections = num_detections;
    ret.max_detections = ctrl->max_detections;

    for (i = 0; i < n_round; i++) {
        unsigned long t0 = get_mono_time_ns();
        status = cme_2d_detection_post_process(ctrl, &ret, 1, NULL);
        unsigned long t1 = get_mono_time_ns();
        if (t1 >= t0)
            total_ms += (t1 - t0) / 1000000.0;
        printf("test_detection_post_process: cme_2d_detection_post_process time: %lu ns (%.3f ms)\n",
               t1 - t0, (t1 - t0) / 1000000.0);
        if (status != CME_RET_SUCCESS) {
            fprintf(stderr, "test_detection_post_process: API returned %d\n", (int)status);
            free(out_boxes);
            free(out_classes);
            free(out_scores);
            free(num_detections);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0)
        printf("test_detection_post_process: %d rounds, average %.3f ms\n", n_round, total_ms / n_round);

    printf("test_detection_post_process: num_detections=%d\n", *num_detections);
    print_detections(out_boxes, out_classes, out_scores, *num_detections);

    if (out_path) {
        FILE* fo = fopen(out_path, "w");
        int j;
        if (!fo) {
            fprintf(stderr, "test_detection_post_process: cannot open '%s'\n", out_path);
            free(out_boxes);
            free(out_classes);
            free(out_scores);
            free(num_detections);
            return EXIT_FAILURE;
        }
        fprintf(fo, "%d\n", *num_detections);
        for (j = 0; j < *num_detections; j++) {
            fprintf(fo, "%.8f %.8f %.8f %.8f %.0f %.8f\n",
                    out_boxes[j * 4 + 0], out_boxes[j * 4 + 1], out_boxes[j * 4 + 2], out_boxes[j * 4 + 3],
                    out_classes[j], out_scores[j]);
        }
        fclose(fo);
        printf("test_detection_post_process: wrote '%s'\n", out_path);
    }

    free(out_boxes);
    free(out_classes);
    free(out_scores);
    free(num_detections);
    return EXIT_SUCCESS;
}

static int builtin_case(int which, int n_round, const char* out_path)
{
    const int num_anchors = 6;
    const int num_classes = 2;
    int rc;
    static const float anchors[] = {
        0.5f, 0.5f, 0.2f, 0.2f,
        0.5f, 0.5f, 0.4f, 0.4f,
        0.3f, 0.3f, 0.1f, 0.1f,
        0.7f, 0.7f, 0.15f, 0.15f,
        0.5f, 0.5f, 0.3f, 0.3f,
        0.1f, 0.1f, 0.05f, 0.05f,
    };
    static const float box_encoding[] = {
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.5f, 0.5f, 0.1f, 0.1f,
        -0.3f, -0.3f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
    };
    static const float class_prediction[] = {
        0.1f, 0.9f, 0.05f,
        0.1f, 0.85f, 0.05f,
        0.5f, 0.3f, 0.2f,
        0.1f, 0.05f, 0.88f,
        0.1f, 0.8f, 0.05f,
        0.9f, 0.05f, 0.05f,
    };
    float* enc = (float*)malloc(sizeof(box_encoding));
    float* cls = (float*)malloc(sizeof(class_prediction));
    float* anc = (float*)malloc(sizeof(anchors));
    cme_detection_post_process_ctrl ctrl;

    if (!enc || !cls || !anc) {
        free(enc);
        free(cls);
        free(anc);
        return EXIT_FAILURE;
    }
    memcpy(enc, box_encoding, sizeof(box_encoding));
    memcpy(cls, class_prediction, sizeof(class_prediction));
    memcpy(anc, anchors, sizeof(anchors));

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.box_encoding = enc;
    ctrl.class_prediction = cls;
    ctrl.anchors = anc;
    ctrl.num_anchors = num_anchors;
    ctrl.num_classes = num_classes;
    ctrl.scale_y = 10.0f;
    ctrl.scale_x = 10.0f;
    ctrl.scale_h = 5.0f;
    ctrl.scale_w = 5.0f;
    ctrl.nms_score_threshold = 0.3f;
    ctrl.iou_threshold = 0.5f;
    ctrl.max_detections = 4;
    ctrl.max_classes_per_detection = 1;
    ctrl.use_regular_nms = (which == 1) ? 1 : 0;
    ctrl.detection_per_class = 4;

    printf("test_detection_post_process: builtin case %d (regular_nms=%d)\n", which, ctrl.use_regular_nms);
    rc = run_one(&ctrl, n_round, out_path);
    free(enc);
    free(cls);
    free(anc);
    return rc;
}

int test_detection_post_process(int argc, char** argv)
{
    int c;
    int g_mode = 0;
    int N = 0, C = 0;
    char* fe = NULL, * fp = NULL, * fa = NULL;
    char* out_path = NULL;
    char* scale_str = NULL;
    float sy = 10.f, sx = 10.f, sh = 5.f, sw = 5.f;
    float nms_th = 0.3f, iou_th = 0.5f;
    int maxdet = 4, maxk = 1, use_reg = 0, dpc = 4;
    int n_round = 1;
    float *enc = NULL, *cls = NULL, *anc = NULL;
    cme_detection_post_process_ctrl ctrl;
    int rc;

    while ((c = getopt(argc, argv, "g:N:C:e:p:a:s:u:I:M:K:R:P:o:n:")) != -1) {
        switch (c) {
        case 'g': g_mode = atoi(optarg); break;
        case 'N': N = atoi(optarg); break;
        case 'C': C = atoi(optarg); break;
        case 'e': fe = optarg; break;
        case 'p': fp = optarg; break;
        case 'a': fa = optarg; break;
        case 's': scale_str = optarg; break;
        case 'u': nms_th = (float)atof(optarg); break;
        case 'I': iou_th = (float)atof(optarg); break;
        case 'M': maxdet = atoi(optarg); break;
        case 'K': maxk = atoi(optarg); break;
        case 'R': use_reg = atoi(optarg); break;
        case 'P': dpc = atoi(optarg); break;
        case 'o': out_path = optarg; break;
        case 'n': n_round = atoi(optarg); break;
        default:
            dpp_usage(argv[0] ? argv[0] : "test_cme");
            return EXIT_FAILURE;
        }
    }

    if (scale_str && parse_scales(scale_str, &sy, &sx, &sh, &sw) != 0) {
        fprintf(stderr, "test_detection_post_process: bad -s (need sy,sx,sh,sw)\n");
        return EXIT_FAILURE;
    }

    if (g_mode == 1 || g_mode == 2)
        return builtin_case(g_mode, n_round, out_path);

    if (!fe || !fp || !fa || N <= 0 || C <= 0) {
        fprintf(stderr, "test_detection_post_process: use -g 1|2 or -N -C -e -p -a\n");
        dpp_usage(argv[0] ? argv[0] : "test_cme");
        return EXIT_FAILURE;
    }

    enc = (float*)malloc((size_t)(N * 4) * sizeof(float));
    cls = (float*)malloc((size_t)(N * (C + 1)) * sizeof(float));
    anc = (float*)malloc((size_t)(N * 4) * sizeof(float));
    if (!enc || !cls || !anc) {
        free(enc);
        free(cls);
        free(anc);
        return EXIT_FAILURE;
    }
    if (read_float_file(fe, enc, (size_t)(N * 4), "box_encoding") != 0 ||
        read_float_file(fp, cls, (size_t)(N * (C + 1)), "class_prediction") != 0 ||
        read_float_file(fa, anc, (size_t)(N * 4), "anchors") != 0) {
        free(enc);
        free(cls);
        free(anc);
        return EXIT_FAILURE;
    }

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.box_encoding = enc;
    ctrl.class_prediction = cls;
    ctrl.anchors = anc;
    ctrl.num_anchors = N;
    ctrl.num_classes = C;
    ctrl.scale_y = sy;
    ctrl.scale_x = sx;
    ctrl.scale_h = sh;
    ctrl.scale_w = sw;
    ctrl.nms_score_threshold = nms_th;
    ctrl.iou_threshold = iou_th;
    ctrl.max_detections = maxdet;
    ctrl.max_classes_per_detection = maxk;
    ctrl.use_regular_nms = use_reg;
    ctrl.detection_per_class = dpc;

    printf("test_detection_post_process: file mode N=%d C=%d regular_nms=%d\n", N, C, use_reg);
    rc = run_one(&ctrl, n_round, out_path);
    free(enc);
    free(cls);
    free(anc);
    return rc;
}
