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
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include "common/va_display.h"
#include "common/bitstream.h"
#include "common/dumpframe.h"
#include "h264decode.h"

static int frame_count = 0;
static int start_code_len = 4;

// get H.264 NAL
static uint8_t* find_next_nal(uint8_t *buffer, int size, int *nal_size) {
    uint8_t *start = NULL;
    *nal_size = 0;

    // 0x00000001 or 0x000001
    for (int i = 0; i < size - 3; i++) {
        if (buffer[i] == 0 && buffer[i+1] == 0) {
            if (buffer[i+2] == 0 && buffer[i+3] == 1) {
                start = &buffer[i];
                break;
            } else if (buffer[i+2] == 1) {
                start = &buffer[i];
                break;
            }
        }
    }

    if (!start) return NULL;

    // find next nal
    uint8_t *next_start = NULL;
    for (int i = (start - buffer) + 4; i < size - 3; i++) {
        if (buffer[i] == 0 && buffer[i+1] == 0) {
            if (buffer[i+2] == 0 && buffer[i+3] == 1) {
                next_start = &buffer[i];
                break;
            } else if (buffer[i+2] == 1) {
                next_start = &buffer[i];
                break;
            }
        }
    }

    if (next_start) {
        *nal_size = next_start - start;
    } else {
        // final nal
        *nal_size = size - (start - buffer);
    }

    return start;
}

// get H.264 NAL type
static int get_nal_type(unsigned char* nal) {
    if (nal[0] == 0 && nal[1] == 0) {
        if (nal[2] == 1)
            return nal[3] & 0x1F;
        else if (nal[2] == 0 && nal[3] == 1)
            return nal[4] & 0x1F; // low 5 bits
    }
    return NAL_UNKNOWN;
}

// get H.264 NAL ref_idc
static int get_nal_ref_idc(unsigned char* nal) {
    if (nal[0] == 0 && nal[1] == 0) {
        if (nal[2] == 1)
            return (nal[3] >> 5) & 0x03;
        else if (nal[2] == 0 && nal[3] == 1)
            return (nal[4] >> 5) & 0x03; // 2 bits
    }
    return 0;
}

// get H.264 start code length
static int get_start_code_len(unsigned char* nal) {
    if (nal[0] == 0 && nal[1] == 0 ) {
        if (nal[2] == 1) {
            start_code_len = 3;
        }
        else if (nal[2] == 0 && nal[3] == 1) {
            start_code_len = 4;
        }
    }
    printf("start_code_len: %d\n", start_code_len);
    return start_code_len; //default 0x0000001
}

// determine if the current slice is the last slice in the picture
static int get_last_slice_in_pic(uint8_t *next_nal) {
    int is_last_slice = 0;
    if (next_nal) {
        int next_nal_type = get_nal_type(next_nal);
        printf("next_nal_type: %d\n", next_nal_type);
        if (next_nal_type == NAL_SEI || next_nal_type == NAL_SPS || next_nal_type == NAL_PPS || next_nal_type == NAL_AUD ) {
            is_last_slice = 1;
        } else {
            int next_start_code_len = get_start_code_len(next_nal);
            next_nal += next_start_code_len;
            BitstreamParser next_slice_parser;
            init_bitstream_parser(&next_slice_parser, next_nal, 32);
            skip_bits(&next_slice_parser, 8); // skip nalu header

            int next_first_mb_in_slice = get_ue_golomb(&next_slice_parser);
            printf("next_first_mb_in_slice: %d\n", next_first_mb_in_slice);
            if (next_first_mb_in_slice == 0) {
                is_last_slice = 1;
            } else {
                is_last_slice = 0;
            }
        }
    }
    else {
        is_last_slice = 1;
    }
    return is_last_slice;
}

// Initialize VA-API for H.264 decoding
static int va_init(H264Decoder *decoder) {
    VAConfigAttrib attrib;
    VAStatus va_status;

    VAProfile profile = VAProfileH264High;
    VAEntrypoint entrypoint = VAEntrypointVLD;

    if (decoder->initialized == 1) {
        return 0;
    }

    va_status = vaGetConfigAttributes(decoder->va_dpy, profile, entrypoint, &attrib, 1);
    CHECK_VASTATUS(va_status, "vaGetConfigAttributes");

    va_status = vaCreateConfig(decoder->va_dpy, profile, entrypoint,
                               &attrib, 1, &decoder->config_id);
    CHECK_VASTATUS(va_status, "vaCreateConfig");

    decoder->surface_count = MAX_PICTURES;
    decoder->surfaces = (VASurfaceID*)malloc(sizeof(VASurfaceID) * MAX_PICTURES);

    va_status = vaCreateSurfaces(
                    decoder->va_dpy,
                    VA_RT_FORMAT_YUV420, decoder->width, decoder->height,
                    decoder->surfaces, decoder->surface_count,
                    NULL, 0
                );
    CHECK_VASTATUS(va_status, "vaCreateSurfaces");

    va_status = vaCreateContext(decoder->va_dpy, decoder->config_id,
                                decoder->width, decoder->height,
                                VA_PROGRESSIVE,
                                decoder->surfaces,
                                decoder->surface_count,
                                &decoder->context_id);
    CHECK_VASTATUS(va_status, "vaCreateContext");

    decoder->initialized = 1;
    return 0;
}

// Release VA-API resources
void va_release(H264Decoder *decoder) {
    if (!decoder || !decoder->initialized) return;

    vaDestroyContext(decoder->va_dpy, decoder->context_id);
    vaDestroySurfaces(decoder->va_dpy, decoder->surfaces, decoder->surface_count);
    vaDestroyConfig(decoder->va_dpy, decoder->config_id);
    vaTerminate(decoder->va_dpy);

    free(decoder->surfaces);
    decoder->initialized = 0;
}

// Parse H.264 SPS and PPS NALs
static int va_h264_parse_sequence(H264Decoder *decoder, uint8_t *sps_nal, int sps_size, uint8_t *pps_nal, int pps_size) {
    VAStatus va_status;
    //sps params
    int profile_idc, level_idc;
    int chroma_format_idc = 1; // default yuv420
    int constraint_set_flags = 0;
    int bit_depth_luma_minus8 = 0;
    int bit_depth_chroma_minus8 = 0;
    int max_num_ref_frames = 1;
    int log2_max_frame_num_minus4;
    int pic_order_cnt_type;
    int log2_max_pic_order_cnt_lsb_minus4 = 0;
    int pic_width_in_mbs_minus1;
    int pic_height_in_map_units_minus1;
    int frame_mbs_only_flag = 0;
    int mb_adaptive_frame_field_flag = 0;
    int direct_8x8_inference_flag;
    int delta_pic_order_always_zero_flag = 0;
    int separate_colour_plane_flag = 0;
    // pps params
    int transform_8x8_mode_flag = 0;
    int pic_scaling_matrix_present_flag = 0;
    int constrained_intra_pred_flag = 0;

    VAPictureParameterBufferH264 pic_param;
    memset(&pic_param, 0, sizeof(pic_param));
    VAIQMatrixBufferH264 iq_matrix;
    memset(&iq_matrix, 16, sizeof(iq_matrix));

    if (!decoder || !sps_nal || !pps_nal) {
        return 1;
    }

    uint8_t *sps_data = sps_nal;
    uint8_t *pps_data = pps_nal;

    start_code_len = get_start_code_len(sps_data);
    sps_data += start_code_len; // remove start code
    start_code_len = get_start_code_len(pps_data);
    pps_data += start_code_len;

    BitstreamParser sps_parser;
    init_bitstream_parser(&sps_parser, sps_data, sps_size);

    skip_bits(&sps_parser, 8);  // skip nalu type

    profile_idc = get_bits(&sps_parser, 8);
    constraint_set_flags |= get_bits(&sps_parser, 1) << 0; // constraint_set0_flag
    constraint_set_flags |= get_bits(&sps_parser, 1) << 1; // constraint_set1_flag
    constraint_set_flags |= get_bits(&sps_parser, 1) << 2; // constraint_set2_flag
    constraint_set_flags |= get_bits(&sps_parser, 1) << 3; // constraint_set3_flag
    constraint_set_flags |= get_bits(&sps_parser, 1) << 4; // constraint_set4_flag
    constraint_set_flags |= get_bits(&sps_parser, 1) << 5; // constraint_set5_flag
    skip_bits(&sps_parser, 2);  // reserved_zero_2bits
    level_idc = get_bits(&sps_parser, 8);
    get_ue_golomb(&sps_parser); // sps_id
    printf("profile_idc %d, level_idc %d\n", profile_idc, level_idc);

    if (profile_idc == 100 || // High profile
        profile_idc == 110 || // High10 profile
        profile_idc == 122 || // High422 profile
        profile_idc == 244 || // High444 Predictive profile
        profile_idc ==  44 || // Cavlc444 profile
        profile_idc ==  83 || // Scalable Constrained High profile (SVC)
        profile_idc ==  86 || // Scalable High Intra profile (SVC)
        profile_idc == 118 || // Stereo High profile (MVC)
        profile_idc == 128 || // Multiview High profile (MVC)
        profile_idc == 138 || // Multiview Depth High profile (MVCD)
        profile_idc == 144) { // old High444 profile
        chroma_format_idc = get_ue_golomb(&sps_parser);
        if (chroma_format_idc == 3) {
            separate_colour_plane_flag = get_bits(&sps_parser, 1); // separate_colour_plane_flag
            decoder->separate_colour_plane_flag = separate_colour_plane_flag;
            printf("separate_colour_plane_flag %d\n", separate_colour_plane_flag);
        }
        printf("chroma_format_idc %d\n", chroma_format_idc);

        bit_depth_luma_minus8 = get_ue_golomb(&sps_parser);
        bit_depth_chroma_minus8 = get_ue_golomb(&sps_parser);
        skip_bits(&sps_parser, 1); // qpprime_y_zero_transform_bypass_flag
        printf("bit_depth_luma_minus8 %d\n", bit_depth_luma_minus8);
        printf("bit_depth_chroma_minus8 %d\n", bit_depth_chroma_minus8);

        int seq_scaling_matrix_present_flag = get_bits(&sps_parser, 1);
        if (seq_scaling_matrix_present_flag) {
            int count = (chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < count; i++) {
                if (get_bits(&sps_parser, 1)) { // scaling_list_present_flag
                    int size = (i < 6) ? 16 : 64;
                    int last_scale = 8;
                    int next_scale = 8;

                    for (int j = 0; j < size; j++) {
                        if (next_scale != 0) {
                            int delta_scale = get_se_golomb(&sps_parser);
                            next_scale = (last_scale + delta_scale + 256) % 256;
                        }
                        last_scale = (next_scale == 0) ? last_scale : next_scale;
                    }
                }
            }
        }
    }

    log2_max_frame_num_minus4 = get_ue_golomb(&sps_parser);
    printf("log2_max_frame_num_minus4 %d\n", log2_max_frame_num_minus4);
    decoder->log2_max_frame_num = log2_max_frame_num_minus4 + 4;
    pic_order_cnt_type = get_ue_golomb(&sps_parser);
    decoder->pic_order_cnt_type = pic_order_cnt_type;
    printf("pic_order_cnt_type %d\n", pic_order_cnt_type);

    if (pic_order_cnt_type == 0) {
        log2_max_pic_order_cnt_lsb_minus4 = get_ue_golomb(&sps_parser);
        decoder->log2_max_pic_order_cnt_lsb = log2_max_pic_order_cnt_lsb_minus4 + 4;
        printf("log2_max_pic_order_cnt_lsb_minus4: %d\n", log2_max_pic_order_cnt_lsb_minus4);
    } else if (pic_order_cnt_type == 1) {
        delta_pic_order_always_zero_flag = get_bits(&sps_parser, 1);
        decoder->delta_pic_order_always_zero_flag = delta_pic_order_always_zero_flag;
        printf("delta_pic_order_always_zero_flag %d\n", delta_pic_order_always_zero_flag);

        get_se_golomb(&sps_parser); // offset_for_non_ref_pic
        get_se_golomb(&sps_parser); // offset_for_top_to_bottom_field

        int num_ref_frames_in_pic_order_cnt_cycle = get_ue_golomb(&sps_parser);
        for (int i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++) {
            get_se_golomb(&sps_parser); // offset_for_ref_frame[i]
        }
    } // pic_order_cnt_type == 2, do nothing

    max_num_ref_frames = get_ue_golomb(&sps_parser);
    decoder->max_ref_count = max_num_ref_frames;
    printf("max_num_ref_frames %d\n", max_num_ref_frames);
    skip_bits(&sps_parser, 1); // gaps_in_frame_num_allowed_flag

    pic_width_in_mbs_minus1 = get_ue_golomb(&sps_parser);
    pic_height_in_map_units_minus1 = get_ue_golomb(&sps_parser);

    frame_mbs_only_flag = get_bits(&sps_parser, 1);
    decoder->frame_mbs_only_flag = frame_mbs_only_flag;
    if (!frame_mbs_only_flag) {
        mb_adaptive_frame_field_flag = get_bits(&sps_parser, 1);
    }

    direct_8x8_inference_flag = get_bits(&sps_parser, 1);
    printf("direct_8x8_inference_flag %d\n", direct_8x8_inference_flag);

    int width = (pic_width_in_mbs_minus1 + 1) * 16;
    int height = (pic_height_in_map_units_minus1 + 1) * 16 * (frame_mbs_only_flag ? 1 : 2);
    printf("get stream resolution: %dx%d\n", width, height);

    decoder->width = width;
    decoder->height = height;

    if (get_bits(&sps_parser, 1)) { // frame_cropping_flag
        decoder->crop_info.frame_crop_left_offset = get_ue_golomb(&sps_parser); //frame_crop_left_offset
        decoder->crop_info.frame_crop_right_offset = get_ue_golomb(&sps_parser); //frame_crop_right_offset
        decoder->crop_info.frame_crop_top_offset = get_ue_golomb(&sps_parser); //frame_crop_top_offset
        decoder->crop_info.frame_crop_bottom_offset = get_ue_golomb(&sps_parser); //frame_crop_bottom_offset
        printf("crop_info: left %d, right %d, top %d, bottom %d\n",
               decoder->crop_info.frame_crop_left_offset, decoder->crop_info.frame_crop_right_offset,
               decoder->crop_info.frame_crop_top_offset, decoder->crop_info.frame_crop_bottom_offset);
    }
    printf("parsed sps done\n");

    BitstreamParser pps_parser;
    init_bitstream_parser(&pps_parser, pps_data, pps_size);
    skip_bits(&pps_parser, 8);  // skip nalu type
    get_ue_golomb(&pps_parser); // pic_parameter_set_id
    get_ue_golomb(&pps_parser); // seq_parameter_set_id

    int entropy_coding_mode_flag = get_bits(&pps_parser, 1);
    int pic_order_present_flag = get_bits(&pps_parser, 1); // bottom_field_pic_order_in_frame_present_flag
    decoder->bottom_field_pic_order_in_frame_present_flag = pic_order_present_flag;
    printf("bottom_field_pic_order_in_frame_present_flag %d\n", pic_order_present_flag);

    int num_slice_groups_minus1 = get_ue_golomb(&pps_parser);
    if (num_slice_groups_minus1 > 0) {
        int slice_group_map_type = get_ue_golomb(&pps_parser);
        if (slice_group_map_type == 0) {
            for (int i = 0; i <= num_slice_groups_minus1; i++) {
                get_ue_golomb(&pps_parser); // run_length_minus1[i]
            }
        } else if (slice_group_map_type == 2) {
            for (int i = 0; i <= num_slice_groups_minus1; i++) {
                get_ue_golomb(&pps_parser); // top_left[i]
                get_ue_golomb(&pps_parser); // bottom_right[i]
            }
        } else if (slice_group_map_type == 3 ||
                   slice_group_map_type == 4 ||
                   slice_group_map_type == 5) {
            get_bits(&pps_parser, 1); // slice_group_change_direction_flag
            get_ue_golomb(&pps_parser); // slice_group_change_rate_minus1
        } else if (slice_group_map_type == 6) {
            int pic_size_in_map_units_minus1 = get_ue_golomb(&pps_parser);
            int slice_group_idc_length = get_ue_golomb(&pps_parser);

            for (int i = 0; i <= pic_size_in_map_units_minus1; i++) {
                get_bits(&pps_parser, slice_group_idc_length); // slice_group_idc[i]
            }
        }
    }

    int num_ref_idx_l0_default_active_minus1 = get_ue_golomb(&pps_parser); // num_ref_idx_l0_default_active_minus1
    int num_ref_idx_l1_default_active_minus1 = get_ue_golomb(&pps_parser); // num_ref_idx_l1_default_active_minus1
    decoder->num_ref_idx_l0_active_minus1 = num_ref_idx_l0_default_active_minus1;
    decoder->num_ref_idx_l1_active_minus1 = num_ref_idx_l1_default_active_minus1;
    printf("PPS: num_ref_idx_l0_default_active_minus1 %d\n", num_ref_idx_l0_default_active_minus1);
    printf("PPS: num_ref_idx_l1_default_active_minus1 %d\n", num_ref_idx_l1_default_active_minus1);

    int weighted_pred_flag = get_bits(&pps_parser, 1);
    int weighted_bipred_idc = get_bits(&pps_parser, 2);
    int pic_init_qp_minus26 = get_se_golomb(&pps_parser);
    int pic_init_qs_minus26 = get_se_golomb(&pps_parser);
    int chroma_qp_index_offset = get_se_golomb(&pps_parser);
    int second_chroma_qp_index_offset = chroma_qp_index_offset;
    int deblocking_filter_control_present_flag = get_bits(&pps_parser, 1);
    constrained_intra_pred_flag = get_bits(&pps_parser, 1);
    int redundant_pic_cnt_present_flag = get_bits(&pps_parser, 1);
    decoder->redundant_pic_cnt_present_flag = redundant_pic_cnt_present_flag;
    printf("redundant_pic_cnt_present_flag %d\n", redundant_pic_cnt_present_flag);

    int more_rbsp_data = 0;
    int bits_left = pps_size * 8 - get_bits_count(&pps_parser);
    if ((profile_idc == 66 || profile_idc == 77 ||profile_idc == 88) && (constraint_set_flags & 7)) {
        more_rbsp_data = 0;
    } else {
        more_rbsp_data = 1;
    }

    if (bits_left > 0 && more_rbsp_data) {
        transform_8x8_mode_flag = get_bits(&pps_parser, 1);
        pic_scaling_matrix_present_flag = get_bits(&pps_parser, 1);
        if (pic_scaling_matrix_present_flag) {
            int scaling_list_count = 6 + ((chroma_format_idc != 3) ? 2 : 6) * transform_8x8_mode_flag;
            for (int i = 0; i < scaling_list_count; i++) {
                if (get_bits(&pps_parser, 1)) { // scaling_list_present_flag
                    int size = (i < 6) ? 16 : 64;
                    int last_scale = 8;
                    int next_scale = 8;
                    for (int j = 0; j < size; j++) {
                        if (next_scale != 0) {
                            int delta_scale = get_se_golomb(&pps_parser);
                            next_scale = (last_scale + delta_scale + 256) % 256;
                        }
                        last_scale = (next_scale == 0) ? last_scale : next_scale;
                    }
                }
            }
        }
        second_chroma_qp_index_offset = get_se_golomb(&pps_parser);
    }

    printf("entropy_coding_mode_flag %d\n", entropy_coding_mode_flag);
    printf("deblocking_filter_control_present_flag %d\n", deblocking_filter_control_present_flag);
    printf("transform_8x8_mode_flag %d\n", transform_8x8_mode_flag);
    printf("pic_scaling_matrix_present_flag %d\n", pic_scaling_matrix_present_flag);
    printf("pic_order_present_flag %d\n", pic_order_present_flag);
    printf("reference_pic_flag %d\n", decoder->nal_ref_idc);
    printf("parsed pps done\n");

    int qp_bd_offset = 6 * bit_depth_luma_minus8;

    pic_param.picture_width_in_mbs_minus1 = pic_width_in_mbs_minus1;
    pic_param.picture_height_in_mbs_minus1 = pic_height_in_map_units_minus1;
    pic_param.bit_depth_luma_minus8 = bit_depth_luma_minus8;
    pic_param.bit_depth_chroma_minus8 = bit_depth_chroma_minus8;
    pic_param.num_ref_frames = max_num_ref_frames;
    pic_param.seq_fields.bits.chroma_format_idc = chroma_format_idc;
    pic_param.seq_fields.bits.residual_colour_transform_flag = 0;
    pic_param.seq_fields.bits.gaps_in_frame_num_value_allowed_flag = 0;
    pic_param.seq_fields.bits.frame_mbs_only_flag = frame_mbs_only_flag;
    pic_param.seq_fields.bits.mb_adaptive_frame_field_flag = mb_adaptive_frame_field_flag;
    pic_param.seq_fields.bits.direct_8x8_inference_flag = direct_8x8_inference_flag;
    pic_param.seq_fields.bits.MinLumaBiPredSize8x8 = (level_idc >= 31);
    pic_param.seq_fields.bits.log2_max_frame_num_minus4 = log2_max_frame_num_minus4;
    pic_param.seq_fields.bits.pic_order_cnt_type = pic_order_cnt_type;
    pic_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = log2_max_pic_order_cnt_lsb_minus4;
    pic_param.seq_fields.bits.delta_pic_order_always_zero_flag = delta_pic_order_always_zero_flag;

    pic_param.pic_init_qp_minus26 = pic_init_qp_minus26 + qp_bd_offset;
    pic_param.pic_init_qs_minus26 = pic_init_qs_minus26 + qp_bd_offset;
    pic_param.chroma_qp_index_offset = chroma_qp_index_offset;
    pic_param.second_chroma_qp_index_offset = second_chroma_qp_index_offset;

    pic_param.pic_fields.bits.entropy_coding_mode_flag = entropy_coding_mode_flag;
    pic_param.pic_fields.bits.weighted_pred_flag = weighted_pred_flag;
    pic_param.pic_fields.bits.weighted_bipred_idc = weighted_bipred_idc;
    pic_param.pic_fields.bits.transform_8x8_mode_flag = transform_8x8_mode_flag;
    pic_param.pic_fields.bits.field_pic_flag = 0;
    pic_param.pic_fields.bits.constrained_intra_pred_flag = constrained_intra_pred_flag;
    pic_param.pic_fields.bits.pic_order_present_flag = pic_order_present_flag;
    pic_param.pic_fields.bits.deblocking_filter_control_present_flag = deblocking_filter_control_present_flag;
    pic_param.pic_fields.bits.redundant_pic_cnt_present_flag = redundant_pic_cnt_present_flag;
    pic_param.pic_fields.bits.reference_pic_flag = (decoder->nal_ref_idc != 0);
    pic_param.frame_num = 0;

    va_status = va_init(decoder);
    if (decoder->initialized != 1) {
        fprintf(stderr, "va_init failed\n");
        return 1;
    }

    if (decoder->seq_update_flag == 1) {
        vaDestroyBuffer(decoder->va_dpy, decoder->pic_param_buf);
        vaDestroyBuffer(decoder->va_dpy, decoder->iqmatrix_buf);
        //decoder->seq_update_flag = 0;
    }

    va_status = vaCreateBuffer(decoder->va_dpy, decoder->context_id,
                               VAPictureParameterBufferType,
                               sizeof(VAPictureParameterBufferH264),
                               1, &pic_param,
                               &decoder->pic_param_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 16; j++) {
            iq_matrix.ScalingList4x4[i][j] = 16;
        }
    }
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 64; j++) {
            iq_matrix.ScalingList8x8[i][j] = 16;
        }
    }

    va_status = vaCreateBuffer(decoder->va_dpy, decoder->context_id,
                               VAIQMatrixBufferType,
                               sizeof(VAIQMatrixBufferH264),
                               1, &iq_matrix,
                               &decoder->iqmatrix_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    decoder->seq_update_flag = 1;

    return 0;
}


int main(int argc, char **argv)
{
    int major_ver, minor_ver;
    VABufferID slice_param_buf, slice_data_buf;
    VAStatus va_status;
    H264Decoder decoder;
    memset(&decoder, 0, sizeof(decoder));
    const char *output_file = nullptr;
    FILE* file = nullptr;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <h264_file>\n", argv[0]);
        return -1;
    }

    FILE* fp = fopen(argv[1], "rb");
    if (!fp) {
        fprintf(stderr, "Failed to open file: %s\n", argv[1]);
        return -1;
    }

    if (argc == 3) {
        printf("output_file: %s\n", argv[2]);
        output_file = argv[2];
        file = fopen(output_file, "wb");
        if (!file) {
            fprintf(stderr, "Failed to open output file: %s\n", argv[2]);
            return -1;
        }
    }

    decoder.va_dpy = va_open_display();
    va_status = vaInitialize(decoder.va_dpy, &major_ver, &minor_ver);
    CHECK_VASTATUS(va_status, "vaInitialize");
    printf("VAAPI Version: %d.%d\n", major_ver, minor_ver);

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    unsigned char* file_buffer = (unsigned char*)malloc(file_size);
    if (!file_buffer) {
        fprintf(stderr, "Failed to allocate file buffer\n");
        fclose(fp);
        return -1;
    }

    fread(file_buffer, 1, file_size, fp);
    fclose(fp);
    fp = NULL;

    // SPS/PPS/IDR
    uint8_t *current_pos = file_buffer;
    int remaining_size = file_size;
    int nal_size = 0, sps_size = 0, pps_size = 0;
    uint8_t *sps_nal = NULL, *pps_nal = NULL, *slice_nal = NULL;
    int is_last_slice_in_pic = 0;

    while (remaining_size > 4) {
        uint8_t *nal = find_next_nal(current_pos, remaining_size, &nal_size);
        if (!nal) break;

        int nal_type = get_nal_type(nal);
        decoder.nal_ref_idc = get_nal_ref_idc(nal);

        printf("NAL: type %d, size %d\n", nal_type, nal_size);
        printf("NAL: nal_ref_idc %d\n", decoder.nal_ref_idc);

        switch (nal_type) {
            case NAL_SPS:
                sps_nal = nal;
                sps_size = nal_size;
                break;
            case NAL_PPS:
                pps_nal = nal;
                pps_size = nal_size;
                break;
            default:
                break;
        }

        current_pos += nal_size;
        remaining_size -= nal_size;

        if (sps_nal && pps_nal) {
            printf("found SPS and PPS\n");
            break;
        }
    }

    if (!sps_nal || !pps_nal) {
        fprintf(stderr, "Failed to find SPS/PPS in the stream\n");
        free(file_buffer);
        return -1;
    }

    va_status = va_h264_parse_sequence(&decoder, sps_nal, sps_size, pps_nal, pps_size);
    CHECK_VASTATUS(va_status, "vaProcessSequence");

    while (remaining_size > 4) {
        uint8_t *nal = find_next_nal(current_pos, remaining_size, &nal_size);
        if (!nal) break;

        int nal_type = get_nal_type(nal);
        start_code_len = get_start_code_len(nal);
        decoder.nal_ref_idc = get_nal_ref_idc(nal);

        if (nal_type == NAL_IDR_SLICE || nal_type == NAL_SLICE) {

            printf("Decoding frame %d (NAL type %d, size %d)\n", frame_count, nal_type, nal_size);
            VASliceParameterBufferH264 slice_param;
            memset(&slice_param, 0, sizeof(slice_param));

            int slice_data_size = nal_size - start_code_len; // remove start code
            printf("slice_data_size: %d\n", slice_data_size);

            slice_nal = nal;
            slice_nal += start_code_len; // remove start code
            BitstreamParser slice_parser;
            init_bitstream_parser(&slice_parser, slice_nal, slice_data_size);
            skip_bits(&slice_parser, 8); // skip nalu header
            int first_mb_in_slice = get_ue_golomb(&slice_parser);
            printf("first_mb_in_slice: %d\n", first_mb_in_slice);
            int slice_type = get_ue_golomb(&slice_parser);
            printf("slice_type: %d\n", slice_type);
            slice_type %= 5;

            int num_ref_idx_l0_active_minus1 = 0;
            int num_ref_idx_l1_active_minus1 = 0;
            int num_ref_idx_active_override_flag = 0;

            get_ue_golomb(&slice_parser); // pps_id
            if (decoder.separate_colour_plane_flag == 1) {
                skip_bits(&slice_parser, 2); // colour_plane_id
            }
            printf("log2_max_frame_num: %d\n", decoder.log2_max_frame_num);
            skip_bits(&slice_parser, decoder.log2_max_frame_num); // skip frame_num

            int field_pic_flag = 0;
            if (!decoder.frame_mbs_only_flag) {
                field_pic_flag = get_bits(&slice_parser, 1);
                if (field_pic_flag) {
                    skip_bits(&slice_parser, 1); // bottom_field_flag
                }
            }

            if (nal_type == NAL_IDR_SLICE) {
                get_ue_golomb(&slice_parser);
            }
            if (decoder.pic_order_cnt_type == 0) {
                skip_bits(&slice_parser, decoder.log2_max_pic_order_cnt_lsb); // pic_order_cnt_lsb
                if (decoder.bottom_field_pic_order_in_frame_present_flag && !field_pic_flag) {
                   get_se_golomb(&slice_parser); // delta_pic_order_cnt_bottom
                }
            } else if (decoder.pic_order_cnt_type == 1 && !decoder.delta_pic_order_always_zero_flag) {
                get_se_golomb(&slice_parser); // delta_pic_order_cnt[0]
                if (decoder.bottom_field_pic_order_in_frame_present_flag && !field_pic_flag) {
                    get_se_golomb(&slice_parser); // delta_pic_order_cnt[1]
                }
            }

            if (decoder.redundant_pic_cnt_present_flag) {
                get_ue_golomb(&slice_parser); // redundant_pic_cnt
            }

            if (slice_type == 1) { //B
                get_bits(&slice_parser, 1); // direct_spatial_mv_pred_flag
            }

            if (slice_type == 0 || slice_type == 1) { // P or B
                num_ref_idx_active_override_flag = get_bits(&slice_parser, 1);
                if (num_ref_idx_active_override_flag) {
                    num_ref_idx_l0_active_minus1 = get_ue_golomb(&slice_parser);
                    if (slice_type == 1) { //B
                        num_ref_idx_l1_active_minus1 = get_ue_golomb(&slice_parser);
                    }
                }
                printf("num_ref_idx_l0_active_minus1 %d\n", num_ref_idx_l0_active_minus1);
                printf("num_ref_idx_l1_active_minus1 %d\n", num_ref_idx_l1_active_minus1);
            }

            switch (slice_type) {
                case 0: slice_type = 0; break; // P
                case 1: slice_type = 1; break; // B
                case 2: slice_type = 2; break; // I
                case 3: slice_type = 3; break; // SP
                case 4: slice_type = 4; break; // SI
            }
            printf("slice_type: %d\n", slice_type);

            slice_param.slice_data_size = slice_data_size;
            slice_param.slice_data_offset = 0;
            slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
            slice_param.slice_data_bit_offset = 0;
            slice_param.first_mb_in_slice = 0;
            slice_param.slice_type = slice_type;
            slice_param.direct_spatial_mv_pred_flag = 0;

            if (num_ref_idx_active_override_flag) {
                slice_param.num_ref_idx_l0_active_minus1 = num_ref_idx_l0_active_minus1;
                slice_param.num_ref_idx_l1_active_minus1 = num_ref_idx_l1_active_minus1;
            }
            else {
                slice_param.num_ref_idx_l0_active_minus1 = decoder.num_ref_idx_l0_active_minus1;
                slice_param.num_ref_idx_l1_active_minus1 = decoder.num_ref_idx_l1_active_minus1;
            }

            va_status = vaCreateBuffer(decoder.va_dpy, decoder.context_id,
                                    VASliceParameterBufferType,
                                    sizeof(VASliceParameterBufferH264),
                                    1, &slice_param,
                                    &slice_param_buf);
            CHECK_VASTATUS(va_status, "vaCreateBuffer");

            va_status = vaCreateBuffer(decoder.va_dpy, decoder.context_id,
                                    VASliceDataBufferType,
                                    slice_data_size,
                                    1, nal + start_code_len,
                                    &slice_data_buf);
            CHECK_VASTATUS(va_status, "vaCreateBuffer");

            VASurfaceID surface_id = decoder.surfaces[(frame_count) % decoder.surface_count];
            if (first_mb_in_slice == 0) {
                va_status = vaBeginPicture(decoder.va_dpy, decoder.context_id, surface_id);
                CHECK_VASTATUS(va_status, "vaBeginPicture");
                printf("surface_id: %d, context_id: %d, config_id: %d\n", surface_id, decoder.context_id, decoder.config_id);

                va_status = vaRenderPicture(decoder.va_dpy, decoder.context_id, &decoder.pic_param_buf, 1);
                CHECK_VASTATUS(va_status, "vaRenderPicture");

                va_status = vaRenderPicture(decoder.va_dpy, decoder.context_id, &decoder.iqmatrix_buf, 1);
                CHECK_VASTATUS(va_status, "vaRenderPicture");

                va_status = vaRenderPicture(decoder.va_dpy, decoder.context_id, &slice_param_buf, 1);
                CHECK_VASTATUS(va_status, "vaRenderPicture");

                va_status = vaRenderPicture(decoder.va_dpy, decoder.context_id, &slice_data_buf, 1);
                CHECK_VASTATUS(va_status, "vaRenderPicture");
            }
            else {
                va_status = vaRenderPicture(decoder.va_dpy, decoder.context_id, &slice_param_buf, 1);
                CHECK_VASTATUS(va_status, "vaRenderPicture");

                va_status = vaRenderPicture(decoder.va_dpy, decoder.context_id, &slice_data_buf, 1);
                CHECK_VASTATUS(va_status, "vaRenderPicture");
            }

            // Find next NAL to determine if this is the last slice in the picture
            int next_nal_size = 0;
            uint8_t *next_pos = current_pos + nal_size;
            int next_remaining_size = remaining_size - nal_size;
            uint8_t *next_nal = find_next_nal(next_pos, next_remaining_size, &next_nal_size);
            if (next_nal_size > 4) {
                is_last_slice_in_pic = get_last_slice_in_pic(next_nal);
            }
            else {
                is_last_slice_in_pic = 1;
            }
            printf("is_last_slice_in_pic: %d\n", is_last_slice_in_pic);

            if (is_last_slice_in_pic) {
                va_status = vaEndPicture(decoder.va_dpy, decoder.context_id);
                CHECK_VASTATUS(va_status, "vaEndPicture");

                va_status = vaSyncSurface(decoder.va_dpy, surface_id);
                CHECK_VASTATUS(va_status, "vaSyncSurface");

                if (file) {
                    va_status = save_vaapi_surface(decoder.va_dpy, surface_id, decoder.crop_info, file);
                    CHECK_VASTATUS(va_status, "vaSaveSurface");
                }

                printf("Decoded %d frame done\n", frame_count);
                frame_count++;
            }

            vaDestroyBuffer(decoder.va_dpy, slice_param_buf);
            vaDestroyBuffer(decoder.va_dpy, slice_data_buf);
        }
        else if (nal_type == NAL_SPS || nal_type == NAL_PPS) {
            printf("NAL: type %d, size %d\n", nal_type, nal_size);
            switch (nal_type) {
                case NAL_SPS:
                    sps_nal = nal;
                    sps_size = nal_size;
                    break;
                case NAL_PPS:
                    pps_nal = nal;
                    pps_size = nal_size;
                    break;
                default:
                    break;
            }
            va_status = va_h264_parse_sequence(&decoder, sps_nal, sps_size, pps_nal, pps_size);
            CHECK_VASTATUS(va_status, "vaProcessSequence");
        } else {
            printf("NAL type %d, size %d\n", nal_type, nal_size);
        }

        current_pos += nal_size;
        remaining_size -= nal_size;
    }

    vaDestroySurfaces(decoder.va_dpy, decoder.surfaces, decoder.surface_count);
    vaDestroyConfig(decoder.va_dpy, decoder.config_id);
    vaDestroyContext(decoder.va_dpy, decoder.context_id);

    vaTerminate(decoder.va_dpy);
    va_close_display(decoder.va_dpy);

    va_release(&decoder);
    free(file_buffer);
    if (file) {
        fclose(file);
    }
    return 0;
}
