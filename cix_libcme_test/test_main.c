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
#include "test_version_capability.h"
#include "test_hwaccel_mode.h"
#include "test_resize_cvtcolor_flip.h"
#include "test_resize.h"
#include "test_cvtcolor.h"
#include "test_est_sim_tfm.h"
#include "test_fisheye_remap.h"
#include "test_overlay.h"
#include "test_affine_transform.h"
#include "test_normalize.h"
#include "test_crfrc.h"
#include "test_cosine_sim.h"
/* #include "test_resize_cvtcolor_dpu.h" */
#include "test_draw_rectangle.h"
#include "test_detection_post_process.h"
#include "test_nms_boxes.h"
#include "test_job_task.h"
#include "test_multi_task.h"
#include "test_img_copy.h"

static void usage(const char* prog)
{
    printf("Usage: %s <test_name> [test_args...]\n", prog);
    printf("\n");
    printf("Available tests:\n");
    printf("  capability           Query API version and capability\n");
    printf("  hwaccel_mode         cme_set_hwaccel_mode: valid/invalid mode handling\n");
    printf("  resize               cme_2d_resize: -i -w -h -f [-o] [-W -H]\n");
    printf("  cvtcolor             cme_2d_cvtcolor: -i -w -h -f -F -m [-o]\n");
    printf("  resize_cvtcolor_flip cme_2d_resize_cvtcolor_and_flip: -i -w -h -f -m [-o] [-W -H]\n");
    printf("  est_sim_tfm          cme_2d_est_sim_tfm: [-o output]\n");
    printf("  fisheye_remap        cme_2d_fisheye_remap: -i -w -h -f [-o] [-W -H]\n");
    printf("  overlay              cme_2d_overlay: -i base -I overlay -w -h -f -W -H -F [-a -A] [-o]\n");
    printf("  affine_transform     cme_2d_affine_transform: -i -w -h [-o] [-W -H]\n");
    printf("  normalize            cme_2d_normalize: -i -w -h -f [-o]\n");
    printf("  img_copy             cme_img_copy: -i -w -h -f [-o] [-c] [-n]\n");
    printf("  crfrc                cme_2d_crop_resize_flip_rotation_cvtcolor: -i -w -h -f -F -m -r [-M -o -W -H -E -X -Y -P -Q]\n");
    printf("  cosine_sim           cme_2d_cosine_sim: -i vec_a.bin -j vec_b.bin -d <dim> [-t 0|1] [-n] [-c 1]\n");
    /* printf("  resize_cvtcolor_dpu  cme_2d_resize_cvtcolor_by_dpu: -i -w -h -f -F [-m] [-o] [-W -H pair or omit for same as source] [-D]\n"); */
    printf("  draw_rectangle       cme_2d_draw_rectangle: -i -w -h -f -x -y -p -q -R -G -B -t [-o]\n");
    printf("  detection_post_process cme_2d_detection_post_process: -g 1|2 OR -N -C -e -p -a [options]\n");
    printf("  nms_boxes            cme_2d_nms_boxes: -g 1 OR -N -b boxes.bin -S scores.bin [options]\n");
    printf("  job_task             cme_begin_task + cme_2d_*_task + cme_end_task: job_task <kind> ...\n");
    printf("                       kinds: resize, cvtcolor, resize_cvtcolor_flip, overlay, crfrc,\n");
    printf("                              fisheye_remap, est_sim_tfm, cosine_sim, normalize,\n");
    printf("                              affine_transform, imcopy\n");
    printf("  multi_task           pipeline: resize_cvtcolor_and_flip -> affine -> normalize\n");
    printf("                       (cme_begin_task + *_task + cme_end_task async + cme_wait_job)\n");
    printf("                       -i -w -h -f [-W -H] [-o] [-N arcface|byte255] [-r 0|10|30] [-n]\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s capability\n", prog);
    printf("  %s hwaccel_mode\n", prog);
    printf("  %s resize -i in.yuv -w 1920 -h 1080 -f nv12 -o out.yuv\n", prog);
    printf("  %s cvtcolor -i in.yuv -w 1920 -h 1080 -f nv12 -F rgb24 -m bt601_limit -o out.rgb\n", prog);
    printf("  %s resize_cvtcolor_flip -i in.yuv -w 1920 -h 1080 -f nv12 -m none -o out.rgb\n", prog);
    printf("  %s est_sim_tfm [-o result.txt]\n", prog);
    printf("  %s fisheye_remap -i in.rgb -w 1920 -h 1080 -f rgb24 -o out.rgb\n", prog);
    printf("  %s overlay -i base.rgb -I over.rgb -w 1920 -h 1080 -f rgb24 -W 1920 -H 1080 -F rgb24 -a 65535 -A 65535 -o out.rgb\n", prog);
    printf("  %s affine_transform -i in.rgb -w 1920 -h 1080 -o out.rgb\n", prog);
    printf("  %s normalize -i in.rgb -w 1920 -h 1080 -f rgb24 -o out.rgb\n", prog);
    printf("  %s img_copy -i in.yuv -w 1920 -h 1080 -f nv12 -o copy.yuv\n", prog);
    printf("  %s crfrc -i in.yuv -w 1920 -h 1080 -f nv12 -F rgb24 -m none -r 0 -M bt601_limit -o out.rgb\n", prog);
    printf("  %s cosine_sim -i a.f32 -j b.f32 -d 512\n", prog);
    /* printf("  %s resize_cvtcolor_dpu -i in.yuv -w 1920 -h 1080 -f nv12 -F nv12 -m bt601_limit -o out.nv12\n", prog); */
    printf("  %s draw_rectangle -i in.rgb -w 1920 -h 1080 -f rgb24 -x 100 -y 100 -p 400 -q 300 -R 255 -G 0 -B 0 -t 4 -o out.rgb\n", prog);
    printf("  %s detection_post_process -g 1\n", prog);
    printf("  %s nms_boxes -g 1\n", prog);
    printf("  %s job_task resize -i in.yuv -w 1920 -h 1080 -f nv12 -o out.yuv\n", prog);
    printf("  %s multi_task -i in.rgb -w 1920 -h 1080 -f rgb24 -W 112 -H 112 -o out.f32\n", prog);
}

int main(int argc, char* argv[])
{
    const char* test;

    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return EXIT_SUCCESS;
    }

    test = argv[1];
    if (strcmp(test, "capability") == 0) {
        return test_version_capability();
    }
    if (strcmp(test, "hwaccel_mode") == 0) {
        return test_hwaccel_mode();
    }
    if (strcmp(test, "resize") == 0) {
        return test_resize(argc, argv);
    }
    if (strcmp(test, "cvtcolor") == 0) {
        return test_cvtcolor(argc, argv);
    }
    if (strcmp(test, "resize_cvtcolor_flip") == 0) {
        return test_resize_cvtcolor_and_flip(argc, argv);
    }
    if (strcmp(test, "est_sim_tfm") == 0) {
        return test_est_sim_tfm(argc, argv);
    }
    if (strcmp(test, "fisheye_remap") == 0) {
        return test_fisheye_remap(argc, argv);
    }
    if (strcmp(test, "overlay") == 0) {
        return test_overlay(argc, argv);
    }
    if (strcmp(test, "affine_transform") == 0) {
        return test_affine_transform(argc, argv);
    }
    if (strcmp(test, "normalize") == 0) {
        return test_normalize(argc, argv);
    }
    if (strcmp(test, "img_copy") == 0) {
        return test_img_copy(argc, argv);
    }
    if (strcmp(test, "crfrc") == 0) {
        return test_crfrc(argc, argv);
    }
    if (strcmp(test, "cosine_sim") == 0) {
        return test_cosine_sim(argc, argv);
    }
    /* if (strcmp(test, "resize_cvtcolor_dpu") == 0) {
        return test_resize_cvtcolor_dpu(argc, argv);
    } */
    if (strcmp(test, "draw_rectangle") == 0) {
        return test_draw_rectangle(argc, argv);
    }
    if (strcmp(test, "detection_post_process") == 0) {
        return test_detection_post_process(argc, argv);
    }
    if (strcmp(test, "nms_boxes") == 0) {
        return test_nms_boxes(argc, argv);
    }
    if (strcmp(test, "job_task") == 0) {
        return test_job_task(argc, argv);
    }
    if (strcmp(test, "multi_task") == 0) {
        return test_multi_task(argc, argv);
    }

    fprintf(stderr, "%s: unknown test '%s'\n", argv[0], test);
    usage(argv[0]);
    return EXIT_FAILURE;
}
