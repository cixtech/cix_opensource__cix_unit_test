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
#include "test_hwaccel_mode.h"

static const char* hwaccel_mode_name(CME_HWACCEL_MODE mode)
{
    switch (mode) {
    case CME_HWACCEL_MODE_PERFORMANCE:
        return "PERFORMANCE";
    case CME_HWACCEL_MODE_ENERGY_EFFICIENCY:
        return "ENERGY_EFFICIENCY";
    case CME_HWACCEL_MODE_LOW_POWER:
        return "LOW_POWER";
    case CME_HWACCEL_MODE_CPU_AVOIDANT:
        return "CPU_AVOIDANT";
    default:
        return "?";
    }
}

static int check_mode(CME_HWACCEL_MODE expected)
{
    if (cme_hwaccel_mode != expected) {
        fprintf(stderr,
                "expected cme_hwaccel_mode=%s (%d), got %s (%d)\n",
                hwaccel_mode_name(expected),
                (int)expected,
                hwaccel_mode_name(cme_hwaccel_mode),
                (int)cme_hwaccel_mode);
        return EXIT_FAILURE;
    }
    printf("  mode %s: ok\n", hwaccel_mode_name(expected));
    return EXIT_SUCCESS;
}

static int test_valid_modes(void)
{
    int i;

    if (check_mode(CME_HWACCEL_MODE_PERFORMANCE) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    for (i = (int)CME_HWACCEL_MODE_PERFORMANCE; i < (int)CME_HWACCEL_MODE_MAX; i++) {
        CME_HWACCEL_MODE mode = (CME_HWACCEL_MODE)i;
        cme_set_hwaccel_mode(mode);
        if (check_mode(mode) != EXIT_SUCCESS)
            return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int test_invalid_mode_ignored(void)
{
    cme_set_hwaccel_mode(CME_HWACCEL_MODE_ENERGY_EFFICIENCY);
    if (check_mode(CME_HWACCEL_MODE_ENERGY_EFFICIENCY) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    cme_set_hwaccel_mode(CME_HWACCEL_MODE_MAX);
    if (check_mode(CME_HWACCEL_MODE_ENERGY_EFFICIENCY) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    cme_set_hwaccel_mode((CME_HWACCEL_MODE)-1);
    if (check_mode(CME_HWACCEL_MODE_ENERGY_EFFICIENCY) != EXIT_SUCCESS)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}

int test_hwaccel_mode(void)
{
    CME_RET ret;

    ret = cme_initialize();
    if (ret != CME_RET_SUCCESS) {
        fprintf(stderr, "cme_initialize() returned %d\n", ret);
        return EXIT_FAILURE;
    }

    printf("cme_set_hwaccel_mode tests:\n");

    if (test_valid_modes() != EXIT_SUCCESS)
        return EXIT_FAILURE;

    if (test_invalid_mode_ignored() != EXIT_SUCCESS)
        return EXIT_FAILURE;

    cme_set_hwaccel_mode(CME_HWACCEL_MODE_PERFORMANCE);
    printf("restored default mode: %s\n", hwaccel_mode_name(cme_hwaccel_mode));

    ret = cme_destroy();
    if (ret != CME_RET_SUCCESS) {
        fprintf(stderr, "cme_destroy() returned %d\n", ret);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
