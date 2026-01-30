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

#ifndef H264DECODE_H
#define H264DECODE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>

#define MAX_PICTURES 32

// H.264 NAL type
enum {
    NAL_UNKNOWN = 0,
    NAL_SLICE = 1,
    NAL_IDR_SLICE = 5,
    NAL_SEI = 6,
    NAL_SPS = 7,
    NAL_PPS = 8,
    NAL_AUD = 9
};

typedef struct {
    VADisplay va_dpy;
    VAConfigID config_id;
    VAContextID context_id;
    VASurfaceID *surfaces;
    VABufferID pic_param_buf;
    VABufferID iqmatrix_buf;
    CropInfo crop_info;
    int surface_count;
    int width;
    int height;
    int initialized;
    int nal_ref_idc;
    int num_ref_idx_l0_active_minus1;
    int num_ref_idx_l1_active_minus1;
    int max_ref_count;
    int ref_frame_count;
    int separate_colour_plane_flag;
    int log2_max_frame_num;
    int frame_mbs_only_flag;
    int pic_order_cnt_type;
    int log2_max_pic_order_cnt_lsb;
    int bottom_field_pic_order_in_frame_present_flag;
    int delta_pic_order_always_zero_flag;
    int redundant_pic_cnt_present_flag;
    int seq_update_flag;
} H264Decoder;

#endif /* H264DECODE_H */
