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
#include "test_est_sim_tfm.h"
#include "test_common.h"

extern char* optarg;

static void est_sim_tfm_usage(const char* prog)
{
    fprintf(stderr, "Usage: %s est_sim_tfm [-o <output>]\n", prog);
    fprintf(stderr, "  Estimates similarity transform from default point pairs.\n");
    fprintf(stderr, "  -o   write result (a b tx ty) to file (optional)\n");
    fprintf(stderr, "  -n   API test round (default 1)\n");
}

int test_est_sim_tfm(int argc, char** argv)
{
    cme_est_sim_tfm_ctrl ctrl;
    cme_est_sim_tfm_ret ret;
    CME_RET api_ret;
    int c;
    char* output_file = NULL;
    FILE* fout = NULL;
    int n_round = 1;
    double total_time = 0.0;

    while ((c = getopt(argc, argv, "o:n:")) != -1) {
        switch (c) {
        case 'o': output_file = optarg; break;
        case 'n': n_round = atoi(optarg); break;
        default:
            est_sim_tfm_usage(argv[0] ? argv[0] : "test_cme");
            return EXIT_FAILURE;
        }
    }

    /* Default point pairs (src -> dst) from sample; point_number 3..10 required. */
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

    memset(&ret, 0, sizeof(ret));
    for (int i = 0; i < n_round; i++) {
        unsigned long start = get_mono_time_ns();
        api_ret = cme_2d_est_sim_tfm(&ctrl, &ret, 1, NULL);
        unsigned long end = get_mono_time_ns();
        if (end < start) {
            fprintf(stderr, "test_est_sim_tfm: time error\n");
            return EXIT_FAILURE;
        }
        total_time += (end - start) / 1000000.0;
        printf("test_est_sim_tfm: cme_2d_est_sim_tfm time: %lu ns (%.3f ms)\n", end - start, (end - start) / 1000000.0);
        if (api_ret != CME_RET_SUCCESS) {
            fprintf(stderr, "test_est_sim_tfm: cme_2d_est_sim_tfm returned %d\n", (int)api_ret);
            return EXIT_FAILURE;
        }
    }
    if (n_round > 0) {
        printf("test_est_sim_tfm: %d rounds, average time: %.3f ms\n", n_round, total_time / n_round);
    }
    printf("test_est_sim_tfm: OK a=%f b=%f tx=%f ty=%f\n", ret.a, ret.b, ret.tx, ret.ty);

    if (output_file) {
        fout = fopen(output_file, "w");
        if (!fout) {
            fprintf(stderr, "test_est_sim_tfm: cannot open '%s'\n", output_file);
            return EXIT_FAILURE;
        }
        fprintf(fout, "%f %f %f %f\n", ret.a, ret.b, ret.tx, ret.ty);
        fclose(fout);
        printf("test_est_sim_tfm: result written to '%s'\n", output_file);
    }

    return EXIT_SUCCESS;
}
