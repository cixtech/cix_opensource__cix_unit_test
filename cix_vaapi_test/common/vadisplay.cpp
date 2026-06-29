/*
 * Copyright 2025 Cix Technology Group Co., Ltd.
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
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <va/va.h>
#include <va/va_drm.h>
#include "vadisplay.h"

static int drm_fd = -1;

// Open a VADisplay using available DRM device nodes
VADisplay va_open_display(void)
{
    VADisplay va_dpy;
    int i;
    static const char *drm_device_paths[] = {
        "/dev/dri/renderD128",
        "/dev/dri/card0",
        "/dev/dri/renderD129",
        "/dev/dri/card1",
        NULL
    };

    for (i = 0; drm_device_paths[i]; i++) {
        drm_fd = open(drm_device_paths[i], O_RDWR);
        if (drm_fd < 0)
            continue;

        va_dpy = vaGetDisplayDRM(drm_fd);

        if (va_dpy) {
            printf("Obtained VADisplay from DRM device: %s\n", drm_device_paths[i]);
            return va_dpy;
        }
        close(drm_fd);
        drm_fd = -1;
    }
    return NULL;
}

// Close the VADisplay
void va_close_display(VADisplay va_dpy)
{
    if (drm_fd < 0)
        return;

    close(drm_fd);
    drm_fd = -1;
}
