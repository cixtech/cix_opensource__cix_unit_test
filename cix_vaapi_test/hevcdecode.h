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

#ifndef HEVCDECODE_H
#define HEVCDECODE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>

#define MAX_PICTURES 32
#define IS_IRAP_NAL(nal_type) (nal_type >= 16 && nal_type <= 23)
#define IS_IDR_NAL(nal_type) (nal_type == 19 || nal_type == 20)
#define IS_BLA_NAL(nal_type) (nal_type == 16 || nal_type == 17 || nal_type == 18)

static const uint8_t hevc_sub_width_c[] = {
    1, 2, 2, 1
};

static const uint8_t hevc_sub_height_c[] = {
    1, 2, 1, 1
};

// HEVC NAL type
enum {
    NAL_TRAIL_N = 0,
    NAL_TRAIL_R = 1,
    NAL_RASL_N = 8,
    NAL_RASL_R = 9,
    NAL_IDR_W_RADL = 19,
    NAL_IDR_N_LP = 20,
    NAL_CRA_NUT = 21,
    NAL_VPS = 32,
    NAL_SPS = 33,
    NAL_PPS = 34,
    NAL_AUD = 35,
    NAL_EOS_NUT = 36,
    NAL_EOB_NUT = 37,
    NAL_FD_NUT  = 38,
    NAL_PREFIX_SEI = 39,
    NAL_SUFFIX_SEI = 40,
    NAL_UNKNOWN = 0xFF
};

typedef struct ScalingList {
    /* This is a little wasteful, since sizeID 0 only needs 8 coeffs,
     * and size ID 3 only has 2 arrays, not 6. */
    uint8_t sl[4][6][64];
    uint8_t sl_dc[2][6];
} ScalingList;

const uint8_t hevc_diag_scan4x4_x[16] = {
    0, 0, 1, 0,
    1, 2, 0, 1,
    2, 3, 1, 2,
    3, 2, 3, 3,
};

const uint8_t hevc_diag_scan4x4_y[16] = {
    0, 1, 0, 2,
    1, 0, 3, 2,
    1, 0, 3, 2,
    1, 3, 2, 3,
};

const uint8_t hevc_diag_scan8x8_x[64] = {
    0, 0, 1, 0,
    1, 2, 0, 1,
    2, 3, 0, 1,
    2, 3, 4, 0,
    1, 2, 3, 4,
    5, 0, 1, 2,
    3, 4, 5, 6,
    0, 1, 2, 3,
    4, 5, 6, 7,
    1, 2, 3, 4,
    5, 6, 7, 2,
    3, 4, 5, 6,
    7, 3, 4, 5,
    6, 7, 4, 5,
    6, 7, 5, 6,
    7, 6, 7, 7,
};

const uint8_t hevc_diag_scan8x8_y[64] = {
    0, 1, 0, 2,
    1, 0, 3, 2,
    1, 0, 4, 3,
    2, 1, 0, 5,
    4, 3, 2, 1,
    0, 6, 5, 4,
    3, 2, 1, 0,
    7, 6, 5, 4,
    3, 2, 1, 0,
    7, 6, 5, 4,
    3, 2, 1, 7,
    6, 5, 4, 3,
    2, 7, 6, 5,
    4, 3, 7, 6,
    5, 4, 7, 6,
    5, 7, 6, 7,
};

const uint8_t scaling_list_4x4_default[16] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16
};

const uint8_t scaling_list_intra_default[64] = {
    16, 16, 16, 16, 17, 18, 21, 24,
    16, 16, 16, 16, 17, 19, 22, 25,
    16, 16, 17, 18, 20, 22, 25, 29,
    16, 16, 18, 21, 24, 27, 31, 36,
    17, 17, 20, 24, 30, 35, 41, 47,
    18, 19, 22, 27, 35, 44, 54, 65,
    21, 22, 25, 31, 41, 54, 70, 88,
    24, 25, 29, 36, 47, 65, 88, 115
};

const uint8_t scaling_list_inter_default[64] = {
    16, 16, 16, 16, 17, 18, 20, 24,
    16, 16, 16, 17, 18, 20, 24, 25,
    16, 16, 17, 18, 20, 24, 25, 28,
    16, 17, 18, 20, 24, 25, 28, 33,
    17, 18, 20, 24, 25, 28, 33, 41,
    18, 20, 24, 25, 28, 33, 41, 54,
    20, 24, 25, 28, 33, 41, 54, 71,
    24, 25, 28, 33, 41, 54, 71, 91
};

typedef struct {
    VADisplay va_dpy;
    VAConfigID config_id;
    VAContextID context_id;
    VASurfaceID *surfaces;
    VABufferID pic_param_buf;
    VABufferID iqmatrix_buf;
    VABufferID decode_param_buf;
    CropInfo conf_win;
    int surface_count;
    int width;
    int height;
    int initialized;
    int ctb_width;
    int ctb_height;
    int num_ref_idx_l0_active_minus1;
    int num_ref_idx_l1_active_minus1;
    int max_ref_count;
    int ref_frame_count;
    int dependent_slice_segments_enabled_flag;
    int num_extra_slice_header_bits;
    int slice_address_length;
    int output_flag_present_flag;
    int no_rasl_output_flag;
    int last_eos_flag;
    int seq_update_flag;
} HEVCDecoder;

#endif /* HEVCDECODE_H */
