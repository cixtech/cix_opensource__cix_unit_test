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

#ifndef DUMPFRAME_H
#define DUMPFRAME_H

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdint>
#include <va/va.h>

#define CHECK_VASTATUS(va_status,func)                                  \
if (va_status != VA_STATUS_SUCCESS) {                                   \
    fprintf(stderr,"%s:%s (%d) failed,exit\n", __func__, func, __LINE__); \
    exit(1);                                                            \
}

typedef struct {
    int frame_crop_left_offset;
    int frame_crop_right_offset;
    int frame_crop_top_offset;
    int frame_crop_bottom_offset;
} CropInfo;

int save_frame(uint8_t *y_plane, uint8_t *uv_plane, int crop_width, int crop_height,
                                int uv_crop_width,int uv_crop_height, int y_stride, int uv_stride,FILE *file);
int save_vaapi_surface(VADisplay display, VASurfaceID surface, CropInfo cropinfo, FILE* file);

#endif /* DUMPFRAME_H */
