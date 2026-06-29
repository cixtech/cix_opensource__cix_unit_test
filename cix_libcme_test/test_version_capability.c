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
#include <cme/cme.h>
#include "test_version_capability.h"

static const char* format_name(IMG_FORMAT fmt)
{
    static const char* names[] = {
        "RGBA_8888",
        "ARGB_8888",
        "RGB_888",
        "BGRA_8888",
        "ABGR_8888",
        "BGR_888",
        "NV12",
        "NV21",
        "I420",
        "YUYV_422",
        "P010",
        "RGB_FLOAT",
        "COMPRESSED",
    };
    if ((unsigned)fmt >= sizeof(names) / sizeof(names[0]))
        return "?";
    return names[fmt];
}

static void print_formats(const char* title, const IMG_FORMAT* formats)
{
    int i;
    printf("  %s:", title);
    for (i = 0; i < 32 && formats[i] != CME_FORMAT_UNKNOWN; i++) {
        if (i > 0)
            printf(",");
        printf(" %s", format_name(formats[i]));
    }
    printf("\n");
}

int test_version_capability(void)
{
    const char* version;
    const cme_capacity* cap;
    CME_RET ret;

    ret = cme_initialize();
    if (ret != CME_RET_SUCCESS) {
        fprintf(stderr, "cme_initialize() returned %d\n", ret);
        return EXIT_FAILURE;
    }

    version = query_api_version();
    if (!version) {
        fprintf(stderr, "query_api_version() returned NULL\n");
        return EXIT_FAILURE;
    }
    printf("CME API version: %s\n", version);

    cap = query_all_capability();
    if (!cap) {
        fprintf(stderr, "query_all_capability() returned NULL\n");
        return EXIT_FAILURE;
    }

    printf("Capability:\n");
    printf("  max size: %d x %d\n", cap->max_img_width, cap->max_img_height);
    printf("  min size: %d x %d\n", cap->min_img_width, cap->min_img_height);
    print_formats("supported input formats", cap->supported_input_formats);
    print_formats("supported output formats", cap->supported_output_formats);

    ret = cme_destroy();
    if (ret != CME_RET_SUCCESS) {
        fprintf(stderr, "cme_destroy() returned %d\n", ret);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
