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
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "dumpframe.h"

static const char *va_image_format_name(uint32_t fourcc)
{
    if (fourcc == VA_FOURCC_NV12)
        return "NV12";
    if (fourcc == VA_FOURCC_P010)
        return "P010";
    return NULL;
}

static int dump_bit_depth_from_va_fourcc(uint32_t fourcc)
{
    if (fourcc == VA_FOURCC_P010)
        return 10;
    if (fourcc == VA_FOURCC_NV12)
        return 8;
    return 8;
}

// save dump frame to file (NV12 8-bit or P010 10-bit per dump_bit_depth)
int save_frame(uint8_t *y_plane, uint8_t *uv_plane, int crop_width, int crop_height,
               int uv_crop_width, int uv_crop_height, int y_stride, int uv_stride, FILE *file,
               int dump_bit_depth) {
    if (!file) {
        fprintf(stderr, "Error: file pointer is NULL\n");
        return -1;
    }

    int y_bytes = (dump_bit_depth == 10) ? (crop_width * 2) : crop_width;
    int uv_bytes = (dump_bit_depth == 10) ? (uv_crop_width * 4) : (uv_crop_width * 2);

    for (int i = 0; i < crop_height; i++) {
        fwrite(y_plane + i * y_stride, 1, y_bytes, file);
    }

    for (int i = 0; i < uv_crop_height; i++) {
        fwrite(uv_plane + i * uv_stride, 1, uv_bytes, file);
    }
    printf("Saved frame to output file (%s)\n", dump_bit_depth == 10 ? "P010" : "NV12");
    return 0;
}

// Save VAAPI surface with cropping (vaDeriveImage only; layout follows derived NV12 or P010)
int save_vaapi_surface(VADisplay display, VASurfaceID surface, CropInfo cropinfo, FILE *file,
                       int output_bit_depth) {
    (void)output_bit_depth;
    VAImage image;
    void *image_data;
    VAStatus va_status;

    va_status = vaDeriveImage(display, surface, &image);
    CHECK_VASTATUS(va_status, "vaDeriveImage failed");

    uint32_t img_fourcc = image.format.fourcc;
    int dump_bit_depth = dump_bit_depth_from_va_fourcc(img_fourcc);
    int bpp = (dump_bit_depth == 10) ? 2 : 1;

    int image_width = image.width;
    int image_height = image.height;
    int num_planes = image.num_planes;
    const char *fmt_name = va_image_format_name(img_fourcc);
    if (fmt_name)
        printf("image: %dx%d, num_planes: %d, format %s (0x%x), pitches: %d, %d\n",
               image_width, image_height, num_planes, fmt_name, (unsigned)img_fourcc,
               image.pitches[0], image.pitches[1]);
    else
        printf("image: %dx%d, num_planes: %d, format 0x%x, pitches: %d, %d\n",
               image_width, image_height, num_planes, (unsigned)img_fourcc,
               image.pitches[0], image.pitches[1]);

    va_status = vaMapBuffer(display, image.buf, &image_data);
    CHECK_VASTATUS(va_status, "vaMapBuffer failed");

    int sub_width_c = 2; // for 4:2:0
    int sub_height_c = 2;
    int crop_left = cropinfo.frame_crop_left_offset * sub_width_c;
    int crop_right = image_width - cropinfo.frame_crop_right_offset * sub_width_c;
    int crop_top = cropinfo.frame_crop_top_offset * sub_height_c;
    int crop_bottom = image_height - cropinfo.frame_crop_bottom_offset * sub_height_c;

    int crop_width = crop_right - crop_left;
    int crop_height = crop_bottom - crop_top;
    printf("crop: width %d, height %d\n", crop_width, crop_height);

    uint8_t *y_plane = (uint8_t *)image_data + image.offsets[0];
    uint8_t *uv_plane = (uint8_t *)image_data + image.offsets[1];

    uint8_t *cropped_y = y_plane + crop_top * image.pitches[0] + crop_left * bpp;

    int uv_stride = image.pitches[1];
    int uv_crop_width = (crop_width + 1) / 2;
    int uv_crop_height = (crop_height + 1) / 2;
    uint8_t *cropped_uv = uv_plane + (crop_top / 2) * uv_stride + crop_left * bpp;

    save_frame(cropped_y, cropped_uv, crop_width, crop_height, uv_crop_width, uv_crop_height,
               image.pitches[0], uv_stride, file, dump_bit_depth);

    vaUnmapBuffer(display, image.buf);
    vaDestroyImage(display, image.image_id);

    return 0;
}
