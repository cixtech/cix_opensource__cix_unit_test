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
#include "common/vadisplay.h"
#include "common/bitstream.h"
#include "common/dumpframe.h"
#include "hevcdecode.h"

static int frame_count = 0;
static int start_code_len = 4;
static int sps_real_size = 0;

// get HEVC NAL
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

// get HEVC NAL type
static int get_nal_type(uint8_t *nal) {
    //printf("get_nal %d%d%d%d%d\n",  nal[0], nal[1], nal[2], nal[3], nal[4]);
    if (nal[0] == 0 && nal[1] == 0) {
        if (nal[2] == 1)
            return (nal[3] & 0x7E) >> 1 ; // 1-6 bits
        else if (nal[2] == 0 && nal[3] == 1)
            return (nal[4] & 0x7E) >> 1; // 1-6 bits
    }
    return NAL_UNKNOWN;
}

// get HEVC start code length
static int get_start_code_len(uint8_t *nal) {
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

// remove emulation prevention bytes (0x03)
static int remove_emulation_prevention(uint8_t *data, int size)
{
    int i = 0;
    int j = 0;
    while (i < size) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0x03) {
            data[j++] = data[i++];
            data[j++] = data[i++];
            i++; // skip emulation prevention byte 0x03
        } else {
            data[j++] = data[i++];
        }
    }
    return j;
}

// determine if the current slice is the last slice in the picture
static int get_last_slice_in_pic(uint8_t *next_nal) {
    int is_last_slice = 0;
    if (next_nal){
        int next_nal_type = get_nal_type(next_nal);
        printf("next_nal_type: %d\n", next_nal_type);
        if (next_nal_type == NAL_VPS || next_nal_type == NAL_SPS || next_nal_type == NAL_PPS ||
            next_nal_type == NAL_PREFIX_SEI || next_nal_type == NAL_SUFFIX_SEI || next_nal_type == NAL_AUD ||
            next_nal_type == NAL_EOS_NUT || next_nal_type == NAL_EOB_NUT || next_nal_type == NAL_FD_NUT) {
            is_last_slice = 1;
        } else {
            int next_start_code_len = get_start_code_len(next_nal);
            next_nal += next_start_code_len;
            BitstreamParser next_slice_parser;
            init_bitstream_parser(&next_slice_parser, next_nal, 32);
            skip_bits(&next_slice_parser, 16);  // skip nalu header

            int next_first_slice_segment_in_pic_flag = get_bits(&next_slice_parser, 1);
            printf("next slice first_slice_segment_in_pic_flag: %d\n", next_first_slice_segment_in_pic_flag);
            if (next_first_slice_segment_in_pic_flag) {
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

// Initialize VA-API for HEVC decoding
static int va_init(HEVCDecoder *decoder) {
    VAConfigAttrib attrib;
    VAStatus va_status;

    VAProfile profile = (decoder->output_bit_depth == 10) ? VAProfileHEVCMain10 : VAProfileHEVCMain;
    unsigned int rt_format = (decoder->output_bit_depth == 10) ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;
    VAEntrypoint entrypoint = VAEntrypointVLD;

    if (decoder->initialized == 1) {
        return 0;
    }

    attrib.type = VAConfigAttribRTFormat;
    va_status = vaGetConfigAttributes(decoder->va_dpy, profile, entrypoint, &attrib, 1);
    CHECK_VASTATUS(va_status, "vaGetConfigAttributes");

    if ((attrib.value & rt_format) == 0) {
        fprintf(stderr, "va_init: RT format 0x%x not supported for profile (mask 0x%x)\n",
                rt_format, attrib.value);
        return -1;
    }

    attrib.type = VAConfigAttribRTFormat;
    attrib.value = rt_format;
    va_status = vaCreateConfig(decoder->va_dpy, profile, entrypoint,
                               &attrib, 1, &decoder->config_id);
    CHECK_VASTATUS(va_status, "vaCreateConfig");

    decoder->surface_count = MAX_PICTURES;
    decoder->surfaces = (VASurfaceID*)malloc(sizeof(VASurfaceID) * MAX_PICTURES);

    va_status = vaCreateSurfaces(
                    decoder->va_dpy,
                    rt_format, decoder->width, decoder->height,
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
void va_release(HEVCDecoder *decoder) {
    if (!decoder || !decoder->initialized) return;

    vaDestroyContext(decoder->va_dpy, decoder->context_id);
    vaDestroySurfaces(decoder->va_dpy, decoder->surfaces, decoder->surface_count);
    vaDestroyConfig(decoder->va_dpy, decoder->config_id);
    vaTerminate(decoder->va_dpy);

    free(decoder->surfaces);
    decoder->initialized = 0;
}

// Parse profile_tier_level from SPS
void parse_ptl(BitstreamParser *sps_parser, int sps_max_sub_layers_minus1)
{
    int i;
    int profile_compatibility_flag[32] = {0};

    int profile_space = get_bits(sps_parser, 2); // profile_space
    int tier_flag = get_bits(sps_parser, 1); // tier_flag
    int profile_idc   = get_bits(sps_parser, 5);
    printf("profile_space: %d, tier_flag: %d, profile_idc: %d\n", profile_space, tier_flag, profile_idc);
    //printf("profile_idc: %d\n", profile_idc);  // main: 1; main10: 2; mainstill: 3;

    for (i = 0; i < 32; i++) {
        profile_compatibility_flag[i] = get_bits(sps_parser, 1);
        //printf("profile_compatibility_flag[%d]: %d\n", i, profile_compatibility_flag[i]);

        if (profile_idc == 0 && i > 0 && profile_compatibility_flag[i]) {
            profile_idc = i;
            printf("profile_idc: %d\n", profile_idc);  // main: 1; main10: 2; mainstill: 3;
        }
    }

    int progressive_source_flag = get_bits(sps_parser, 1); // progressive_source_flag
    int interlaced_source_flag = get_bits(sps_parser, 1); // interlaced_source_flag
    int non_packed_constraint_flag = get_bits(sps_parser, 1); // non_packed_constraint_flag
    int frame_only_constraint_flag = get_bits(sps_parser, 1); // frame_only_constraint_flag
    printf("progressive_source_flag: %d, interlaced_source_flag: %d, non_packed_constraint_flag: %d, frame_only_constraint_flag: %d\n",
           progressive_source_flag, interlaced_source_flag, non_packed_constraint_flag, frame_only_constraint_flag);

    skip_bits(sps_parser, 7);
    skip_bits(sps_parser, 1);
    skip_bits(sps_parser, 35);
    skip_bits(sps_parser, 1); // inbld_flag

    int level_idc = get_bits(sps_parser, 8);
    printf("level_idc: %d\n", level_idc);

    int sub_layer_profile_present_flag[sps_max_sub_layers_minus1 - 1] = {0};
    int sub_layer_level_present_flag[sps_max_sub_layers_minus1 - 1] = {0};
    for (i = 0; i < sps_max_sub_layers_minus1 - 1; i++) {
        sub_layer_profile_present_flag[i] = get_bits(sps_parser, 1);
        sub_layer_level_present_flag[i]   = get_bits(sps_parser, 1);
    }

    if (sps_max_sub_layers_minus1 - 1 > 0) {
        for (i = sps_max_sub_layers_minus1 - 1; i < 8; i++) {
            skip_bits(sps_parser, 2); // reserved_zero_2bits[i]
        }
    }
    for (i = 0; i < sps_max_sub_layers_minus1 - 1; i++) {
        if (sub_layer_profile_present_flag[i]) {
            level_idc = get_bits(sps_parser, 8);
            skip_bits(sps_parser, 32);
            skip_bits(sps_parser, 4);
            skip_bits(sps_parser, 43);
            skip_bits(sps_parser, 1);
            if(sub_layer_level_present_flag[i]) {
                level_idc = get_bits(sps_parser, 8);
                printf("level_idc: %d\n", level_idc);
            }
        }
    }
}

// Parse HEVC SPS and PPS NALs
static int va_hevc_parse_sequence(HEVCDecoder *decoder, uint8_t *sps_nal, int sps_size, uint8_t *pps_nal, int pps_size) {
    VAStatus va_status;
    //sps params
    int chroma_format_idc = 1; // default yuv420
    int separate_colour_plane_flag = 0;
    int pic_width_in_luma_samples = 0;
    int pic_height_in_luma_samples = 0;
    int max_dec_pic_buffering = 0;
    int pcm_bit_depth   = 0;
    int pcm_bit_depth_chroma = 0;
    int pcm_log2_min_pcm_cb_size = 0;
    int pcm_log2_max_pcm_cb_size =  0;
    int pcm_loop_filter_disable_flag = 0;
    int num_long_term_ref_pics_sps = 0;
    int num_tile_columns_minus1 = 1;
    int num_tile_rows_minus1    = 1;
    int sps_scaling_list_data_present_flag = 0;

    VAPictureParameterBufferHEVC pic_param;
    memset(&pic_param, 0, sizeof(pic_param));
    VAIQMatrixBufferHEVC iq_matrix;
    memset(&iq_matrix, 16, sizeof(iq_matrix));
    ScalingList scaling_list;
    memset(&scaling_list, 16, sizeof(scaling_list));

    printf("Initialize default scaling lists\n");
    int i,j;
    for (i = 0; i < 6; i++) {
        for (j = 0; j < 16; j++)
            iq_matrix.ScalingList4x4[i][j] = scaling_list.sl[0][i][j] = scaling_list_4x4_default[j];
        for (j = 0; j < 64; j++) {
            if (i < 3) {
                iq_matrix.ScalingList8x8[i][j] = scaling_list.sl[1][i][j] = scaling_list_intra_default[j];
                iq_matrix.ScalingList16x16[i][j] = scaling_list.sl[2][i][j] = scaling_list_intra_default[j];
            } else {
                iq_matrix.ScalingList8x8[i][j] = scaling_list.sl[1][i][j] = scaling_list_inter_default[j];
                iq_matrix.ScalingList16x16[i][j] = scaling_list.sl[2][i][j] = scaling_list_inter_default[j];
            }
            if (i < 2)
                iq_matrix.ScalingList32x32[i][j] = scaling_list.sl[3][i * 3][j] = scaling_list_intra_default[j];
        }
    };

    if (!decoder || !sps_nal || !pps_nal) {
        return 1;
    }

    decoder->seq_update_flag = 1;

    uint8_t *sps_data = sps_nal;
    uint8_t *pps_data = pps_nal;
    if (sps_real_size == 0) {
        sps_real_size = sps_size;
    }

    sps_real_size = remove_emulation_prevention(sps_data, sps_real_size);
    printf("sps_size: %d, sps_real_size: %d\n", sps_size, sps_real_size);

    start_code_len = get_start_code_len(sps_data);
    sps_data += start_code_len; // remove start code

    start_code_len = get_start_code_len(pps_data);
    pps_data += start_code_len;

    BitstreamParser sps_parser;
    init_bitstream_parser(&sps_parser, sps_data, sps_real_size);

    skip_bits(&sps_parser, 16);  // skip nalu header

    int sps_video_parameter_set_id = get_bits(&sps_parser, 4); // sps_video_parameter_set_id
    printf("sps_video_parameter_set_id: %d\n", sps_video_parameter_set_id);
    int sps_max_sub_layers_minus1 = get_bits(&sps_parser, 3) + 1;
    printf("sps_max_sub_layers_minus1: %d\n", sps_max_sub_layers_minus1);
    int sps_temporal_id_nesting_flag = get_bits(&sps_parser, 1); // sps_temporal_id_nesting_flag
    printf("sps_temporal_id_nesting_flag: %d\n", sps_temporal_id_nesting_flag);

    int bits_left = sps_real_size * 8 - get_bits_count(&sps_parser);
    printf("bits left in sps: %d\n", bits_left);

    parse_ptl(&sps_parser, sps_max_sub_layers_minus1); // parse profile tier level

    bits_left = sps_real_size * 8 - get_bits_count(&sps_parser);
    printf("bits left in sps: %d\n", bits_left);

    int sps_id = get_ue_golomb(&sps_parser); // sps_id
    printf("sps_id: %d\n", sps_id);
    chroma_format_idc = get_ue_golomb(&sps_parser);
    printf("chroma_format_idc: %d\n", chroma_format_idc);
    if (chroma_format_idc == 3) {
        separate_colour_plane_flag = get_bits(&sps_parser, 1); // separate_colour_plane_flag
        printf("separate_colour_plane_flag: %d\n", separate_colour_plane_flag);
    }

    pic_width_in_luma_samples = get_ue_golomb(&sps_parser);
    pic_height_in_luma_samples = get_ue_golomb(&sps_parser);

    printf("get stream resolution: %dx%d\n", pic_width_in_luma_samples, pic_height_in_luma_samples);

    if (decoder->width && decoder->height &&
        (pic_width_in_luma_samples != decoder->width || pic_height_in_luma_samples != decoder->height)) {
        printf("stream resolution change from %dx%d to %dx%d\n",
        decoder->width, decoder->height, pic_width_in_luma_samples, pic_height_in_luma_samples);

        decoder->width = pic_width_in_luma_samples;
        decoder->height = pic_height_in_luma_samples;

        vaDestroySurfaces(decoder->va_dpy, decoder->surfaces, decoder->surface_count);
        vaDestroyContext(decoder->va_dpy, decoder->context_id);

        decoder->surfaces = (VASurfaceID*)malloc(sizeof(VASurfaceID) * MAX_PICTURES);

        unsigned int rt_format = (decoder->output_bit_depth == 10) ? VA_RT_FORMAT_YUV420_10
                                                                   : VA_RT_FORMAT_YUV420;
        va_status = vaCreateSurfaces(
                        decoder->va_dpy,
                        rt_format, decoder->width, decoder->height,
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
    }

    decoder->width = pic_width_in_luma_samples;
    decoder->height = pic_height_in_luma_samples;

    if (get_bits(&sps_parser, 1)) { // pic_conformance_flag
        decoder->conf_win.frame_crop_left_offset = get_ue_golomb(&sps_parser); //conf_win_left_offset
        decoder->conf_win.frame_crop_right_offset = get_ue_golomb(&sps_parser); //conf_win_right_offset
        decoder->conf_win.frame_crop_top_offset = get_ue_golomb(&sps_parser); //conf_win_top_offset
        decoder->conf_win.frame_crop_bottom_offset = get_ue_golomb(&sps_parser); //conf_win_bottom_offset
        printf("conf_win: left %d, right %d, top %d, bottom %d\n",
               decoder->conf_win.frame_crop_left_offset, decoder->conf_win.frame_crop_right_offset,
               decoder->conf_win.frame_crop_top_offset, decoder->conf_win.frame_crop_bottom_offset);
    }

    int bit_depth_luma   = get_ue_golomb(&sps_parser) + 8;
    int bit_depth_chroma = get_ue_golomb(&sps_parser) + 8;
    printf("bit_depth: %d, %d\n", bit_depth_luma, bit_depth_chroma);

    int log2_max_poc_lsb = get_ue_golomb(&sps_parser) + 4;
    int sublayer_ordering_info = get_bits(&sps_parser, 1);
    int start = sublayer_ordering_info ? 0 : sps_max_sub_layers_minus1 - 1;
    for (int i = start; i < sps_max_sub_layers_minus1; i++) {
        max_dec_pic_buffering = get_ue_golomb(&sps_parser); // max_dec_pic_buffering
        get_ue_golomb(&sps_parser); // num_reorder_pics
        get_ue_golomb(&sps_parser); // max_latency_increase
    }
    printf("max_dec_pic_buffering %d\n", max_dec_pic_buffering);

    int log2_min_cb_size                    = get_ue_golomb(&sps_parser) + 3;
    int log2_diff_max_min_coding_block_size = get_ue_golomb(&sps_parser);

    int log2_ctb_size = log2_diff_max_min_coding_block_size + log2_min_cb_size;
    printf("log2_ctb_size %d\n", log2_ctb_size);
    int ctb_width  = (decoder->width  + (1 << log2_ctb_size) - 1) >> log2_ctb_size;
    int ctb_height = (decoder->height + (1 << log2_ctb_size) - 1) >> log2_ctb_size;
    decoder->ctb_width = ctb_width;
    decoder->ctb_height = ctb_height;
    int ctb_size  = ctb_width * ctb_height;
    printf("ctb_width %d, ctb_height %d, ctb_size %d\n", ctb_width, ctb_height, ctb_size);

    decoder->slice_address_length = ceil_log2(ctb_size);
    printf("slice_address_length %d\n", decoder->slice_address_length);

    int log2_min_tb_size                    = get_ue_golomb(&sps_parser) + 2;
    int log2_diff_max_min_transform_block_size   = get_ue_golomb(&sps_parser);
    int log2_max_trafo_size                 = log2_diff_max_min_transform_block_size + log2_min_tb_size;
    int max_transform_hierarchy_depth_inter = get_ue_golomb(&sps_parser);
    int max_transform_hierarchy_depth_intra = get_ue_golomb(&sps_parser);
    int scaling_list_enable_flag = get_bits(&sps_parser, 1);
    if (scaling_list_enable_flag) {
        sps_scaling_list_data_present_flag = get_bits(&sps_parser, 1);
        printf("sps_scaling_list_data_present_flag %d\n", sps_scaling_list_data_present_flag);
        if (sps_scaling_list_data_present_flag) {
            for (int sizeId = 0; sizeId < 4; sizeId++) {
                for (int matrixId = 0; matrixId < 6; matrixId += (sizeId == 3) ? 3 : 1) {
                    int scaling_list_pred_mode_flag = get_bits(&sps_parser, 1);
                    if (!scaling_list_pred_mode_flag) {
                        int delta = get_ue_golomb(&sps_parser);
                        if (delta) {
                            delta *= (sizeId == 3) ? 3 : 1;
                            memcpy(scaling_list.sl[sizeId][matrixId],scaling_list.sl[sizeId][matrixId - delta],sizeId > 0 ? 64 : 16);
                            if (sizeId > 1)
                                scaling_list.sl_dc[sizeId - 2][matrixId] = scaling_list.sl_dc[sizeId - 2][matrixId - delta];
                        }
                    } else {
                        int nextCoef = 8;
                        int coefNum = (1 << (4 + (sizeId << 1)));
                        coefNum = (coefNum < 64) ? coefNum : 64;
                        if (sizeId > 1) {
                            int scaling_list_dc_coef_minus8 = get_se_golomb(&sps_parser);
                            nextCoef = scaling_list_dc_coef_minus8 + 8;
                            scaling_list.sl_dc[sizeId - 2][matrixId] = nextCoef;
                        }

                        int pos = 0;
                        for (int i = 0; i < coefNum; i++) {
                            if (sizeId == 0) {
                                pos = 4 * hevc_diag_scan4x4_y[i] + hevc_diag_scan4x4_x[i];
                            } else {
                                pos = 8 * hevc_diag_scan8x8_y[i] + hevc_diag_scan8x8_x[i];
                            }
                            int scaling_list_delta_coef = get_se_golomb(&sps_parser);
                            nextCoef = (nextCoef + scaling_list_delta_coef + 256) % 256;
                            scaling_list.sl[sizeId][matrixId][pos] = nextCoef;
                        }
                    }
                }
            }
        }
    }

    int amp_enabled_flag = get_bits(&sps_parser, 1);
    int sao_enabled      = get_bits(&sps_parser, 1);
    printf("sample_adaptive_offset_enabled_flag %d\n", sao_enabled);

    int pcm_enabled_flag = get_bits(&sps_parser, 1);
    if (pcm_enabled_flag) {
        pcm_bit_depth   = get_bits(&sps_parser, 4);
        pcm_bit_depth_chroma = get_bits(&sps_parser, 4);
        pcm_log2_min_pcm_cb_size = get_ue_golomb(&sps_parser);
        pcm_log2_max_pcm_cb_size =  get_ue_golomb(&sps_parser);
        pcm_loop_filter_disable_flag = get_bits(&sps_parser, 1);
    }
    printf("pcm_enabled_flag %d\n", pcm_enabled_flag);
    printf("pcm_bit_depth %d, pcm_bit_depth_chroma %d\n", pcm_bit_depth, pcm_bit_depth_chroma);
    printf("pcm_log2_min_pcm_cb_size %d, pcm_log2_max_pcm_cb_size %d\n", pcm_log2_min_pcm_cb_size, pcm_log2_max_pcm_cb_size);
    printf("pcm_loop_filter_disable_flag %d\n", pcm_loop_filter_disable_flag);

    int num_short_term_ref_pic_sets = get_ue_golomb(&sps_parser);
    printf("num_short_term_ref_pic_sets %d\n", num_short_term_ref_pic_sets);

    int long_term_ref_pics_present_flag = get_bits(&sps_parser, 1);
    if (long_term_ref_pics_present_flag) {
        num_long_term_ref_pics_sps = get_ue_golomb(&sps_parser);
        for (int i = 0; i < num_long_term_ref_pics_sps; i++) {
            skip_bits(&sps_parser, log2_max_poc_lsb); // lt_ref_pic_poc_lsb_sps
            skip_bits(&sps_parser, 1); // used_by_curr_pic_lt_sps_flag
        }
    }
    printf("long_term_ref_pics_present_flag %d\n", long_term_ref_pics_present_flag);
    printf("num_long_term_ref_pics_sps %d\n", num_long_term_ref_pics_sps);

    int sps_temporal_mvp_enabled_flag          = get_bits(&sps_parser, 1);
    printf("sps_temporal_mvp_enabled_flag %d\n", sps_temporal_mvp_enabled_flag);
    int sps_strong_intra_smoothing_enable_flag = get_bits(&sps_parser, 1);
    printf("parsed sps done\n");

    BitstreamParser pps_parser;
    init_bitstream_parser(&pps_parser, pps_data, pps_size);
    printf("pps_size: %d\n", pps_size);

    skip_bits(&pps_parser, 16);  // skip nalu header

    int loop_filter_across_tiles_enabled_flag = 1;
    int disable_dbf                           = 0;
    int beta_offset_div2                      = 0;
    int tc_offset_div2                        = 0;
    int deblocking_filter_override_enabled_flag = 0;
    int uniform_spacing_flag                  = 0;
    uint16_t column_width_minus1[19]        = {0};
    uint16_t row_height_minus1[21]          = {0};

    int pps_id = get_ue_golomb(&pps_parser); // pps_id
    printf("pps_id %d\n", pps_id);
    int pps_sps_id = get_ue_golomb(&pps_parser); // sps_id
    printf("pps_sps_id %d\n", pps_sps_id);

    int dependent_slice_segments_enabled_flag = get_bits(&pps_parser, 1);
    decoder->dependent_slice_segments_enabled_flag = dependent_slice_segments_enabled_flag;
    int output_flag_present_flag              = get_bits(&pps_parser, 1);
    decoder->output_flag_present_flag = output_flag_present_flag;
    printf("output_flag_present_flag %d\n", output_flag_present_flag);
    int num_extra_slice_header_bits           = get_bits(&pps_parser, 3);
    decoder->num_extra_slice_header_bits = num_extra_slice_header_bits;
    int sign_data_hiding_flag = get_bits(&pps_parser, 1);
    int cabac_init_present_flag = get_bits(&pps_parser, 1);

    int num_ref_idx_l0_default_active_minus1 = get_ue_golomb(&pps_parser);
    int num_ref_idx_l1_default_active_minus1 = get_ue_golomb(&pps_parser);
    decoder->num_ref_idx_l0_active_minus1 = num_ref_idx_l0_default_active_minus1;
    decoder->num_ref_idx_l1_active_minus1 = num_ref_idx_l1_default_active_minus1;
    printf("PPS: num_ref_idx_l0_default_active_minus1 %d\n", num_ref_idx_l0_default_active_minus1);
    printf("PPS: num_ref_idx_l1_default_active_minus1 %d\n", num_ref_idx_l1_default_active_minus1);

    int pic_init_qp_minus26 = get_se_golomb(&pps_parser);

    int constrained_intra_pred_flag = get_bits(&pps_parser, 1);
    int transform_skip_enabled_flag = get_bits(&pps_parser, 1);

    int cu_qp_delta_enabled_flag = get_bits(&pps_parser, 1);
    int diff_cu_qp_delta_depth   = 0;
    if (cu_qp_delta_enabled_flag) {
        diff_cu_qp_delta_depth = get_ue_golomb(&pps_parser);
    }
    int cb_qp_offset = get_se_golomb(&pps_parser);
    int cr_qp_offset = get_se_golomb(&pps_parser);
    int pic_slice_level_chroma_qp_offsets_present_flag = get_bits(&pps_parser, 1);

    int weighted_pred_flag   = get_bits(&pps_parser, 1);
    int weighted_bipred_flag = get_bits(&pps_parser, 1);

    int transquant_bypass_enable_flag    = get_bits(&pps_parser, 1);
    int tiles_enabled_flag               = get_bits(&pps_parser, 1);
    int entropy_coding_sync_enabled_flag = get_bits(&pps_parser, 1);

    if (tiles_enabled_flag) {
        num_tile_columns_minus1 = get_ue_golomb(&pps_parser);
        num_tile_rows_minus1    = get_ue_golomb(&pps_parser);
        printf("num_tile_columns_minus1 %d, num_tile_rows_minus1 %d\n", num_tile_columns_minus1, num_tile_rows_minus1);
        uniform_spacing_flag = get_bits(&pps_parser, 1);
        printf("uniform_spacing_flag %d\n", uniform_spacing_flag);
        if (!uniform_spacing_flag) {
            int i = 0;
            for (i = 0; i < num_tile_columns_minus1; i++) {
                column_width_minus1[i] = get_ue_golomb(&pps_parser); // column_width
            }
            for (i = 0; i < num_tile_rows_minus1; i++) {
                row_height_minus1[i] = get_ue_golomb(&pps_parser); // row_height
            }
        }
        loop_filter_across_tiles_enabled_flag = get_bits(&pps_parser, 1); // loop_filter_across_tiles_enabled_flag
    }

    int seq_loop_filter_across_slices_enabled_flag = get_bits(&pps_parser, 1);
    int deblocking_filter_control_present_flag = get_bits(&pps_parser, 1);
    printf("deblocking_filter_control_present_flag %d\n", deblocking_filter_control_present_flag);
    if (deblocking_filter_control_present_flag) {
        deblocking_filter_override_enabled_flag = get_bits(&pps_parser, 1); // deblocking_filter_override_enabled_flag
        disable_dbf      = get_bits(&pps_parser, 1);
        printf("deblocking_filter_override_enabled_flag %d, disable_dbf %d\n", deblocking_filter_override_enabled_flag, disable_dbf);
        if (!disable_dbf) {
            beta_offset_div2 = get_se_golomb(&pps_parser);
            tc_offset_div2   = get_se_golomb(&pps_parser);
            printf("beta_offset_div2 %d, tc_offset_div2 %d\n", beta_offset_div2, tc_offset_div2);
        }
    }

    int scaling_list_data_present_flag = get_bits(&pps_parser, 1);
    printf("scaling_list_data_present_flag %d\n", scaling_list_data_present_flag);
    if (scaling_list_data_present_flag) {
        memset(&scaling_list, 16, sizeof(ScalingList));
        for (int sizeId = 0; sizeId < 4; sizeId++) {
            for (int matrixId = 0; matrixId < 6; matrixId += (sizeId == 3) ? 3 : 1) {
                int scaling_list_pred_mode_flag = get_bits(&pps_parser, 1);
                if (!scaling_list_pred_mode_flag) {
                    int delta = get_ue_golomb(&pps_parser);
                    if (delta) {
                        delta *= (sizeId == 3) ? 3 : 1;

                        memcpy(scaling_list.sl[sizeId][matrixId],scaling_list.sl[sizeId][matrixId - delta],sizeId > 0 ? 64 : 16);
                        if (sizeId > 1)
                            scaling_list.sl_dc[sizeId - 2][matrixId] = scaling_list.sl_dc[sizeId - 2][matrixId - delta];
                    }
                } else {
                    int nextCoef = 8;
                    int coefNum = (1 << (4 + (sizeId << 1)));
                    coefNum = (coefNum < 64) ? coefNum : 64;
                    if (sizeId > 1) {
                        int scaling_list_dc_coef_minus8 = get_se_golomb(&pps_parser);
                        nextCoef = scaling_list_dc_coef_minus8 + 8;
                        scaling_list.sl_dc[sizeId - 2][matrixId] = nextCoef;
                    }

                    int pos = 0;
                    for (int i = 0; i < coefNum; i++) {
                        if (sizeId == 0) {
                            pos = 4 * hevc_diag_scan4x4_y[i] + hevc_diag_scan4x4_x[i];
                        } else {
                            pos = 8 * hevc_diag_scan8x8_y[i] + hevc_diag_scan8x8_x[i];
                        }
                        int scaling_list_delta_coef = get_se_golomb(&pps_parser);
                        nextCoef = (nextCoef + scaling_list_delta_coef + 256) % 256;
                        scaling_list.sl[sizeId][matrixId][pos] = nextCoef;
                    }
                }
            }
        }
    }
    int lists_modification_present_flag = get_bits(&pps_parser, 1);
    int log2_parallel_merge_level_minus2     = get_ue_golomb(&pps_parser);
    int slice_header_extension_present_flag = get_bits(&pps_parser, 1);
    printf("log2_parallel_merge_level_minus2 %d\n", log2_parallel_merge_level_minus2);
    printf("parsed pps done\n");

    pic_param.pic_width_in_luma_samples                    = decoder->width;
    pic_param.pic_height_in_luma_samples                   = decoder->height;
    pic_param.log2_min_luma_coding_block_size_minus3       = log2_min_cb_size - 3;
    pic_param.sps_max_dec_pic_buffering_minus1             = max_dec_pic_buffering;
    pic_param.log2_diff_max_min_luma_coding_block_size     = log2_diff_max_min_coding_block_size;
    pic_param.log2_min_transform_block_size_minus2         = log2_min_tb_size - 2;
    pic_param.log2_diff_max_min_transform_block_size       = log2_max_trafo_size  - log2_min_tb_size;
    pic_param.max_transform_hierarchy_depth_inter          = max_transform_hierarchy_depth_inter;
    pic_param.max_transform_hierarchy_depth_intra          = max_transform_hierarchy_depth_intra;
    pic_param.num_short_term_ref_pic_sets                  = num_short_term_ref_pic_sets;
    pic_param.num_long_term_ref_pic_sps                    = num_long_term_ref_pics_sps;
    pic_param.num_ref_idx_l0_default_active_minus1         = num_ref_idx_l0_default_active_minus1;
    pic_param.num_ref_idx_l1_default_active_minus1         = num_ref_idx_l1_default_active_minus1;
    pic_param.init_qp_minus26                              = pic_init_qp_minus26;
    pic_param.pps_cb_qp_offset                             = cb_qp_offset;
    pic_param.pps_cr_qp_offset                             = cr_qp_offset;
    pic_param.pcm_sample_bit_depth_luma_minus1             = pcm_bit_depth;
    pic_param.pcm_sample_bit_depth_chroma_minus1           = pcm_bit_depth_chroma;
    pic_param.log2_min_pcm_luma_coding_block_size_minus3   = pcm_log2_min_pcm_cb_size;
    pic_param.log2_diff_max_min_pcm_luma_coding_block_size = pcm_log2_max_pcm_cb_size;
    pic_param.diff_cu_qp_delta_depth                       = diff_cu_qp_delta_depth;
    pic_param.pps_beta_offset_div2                         = beta_offset_div2;
    pic_param.pps_tc_offset_div2                           = tc_offset_div2;
    pic_param.log2_parallel_merge_level_minus2             = log2_parallel_merge_level_minus2;
    pic_param.bit_depth_luma_minus8                        = bit_depth_luma - 8;
    pic_param.bit_depth_chroma_minus8                      = bit_depth_chroma - 8;
    pic_param.log2_max_pic_order_cnt_lsb_minus4            = log2_max_poc_lsb - 4;
    pic_param.num_extra_slice_header_bits                  = num_extra_slice_header_bits;
    pic_param.num_tile_columns_minus1                      = num_tile_columns_minus1;
    pic_param.num_tile_rows_minus1                         = num_tile_rows_minus1;

    if (!uniform_spacing_flag) {
        memcpy(pic_param.column_width_minus1, column_width_minus1, sizeof(column_width_minus1));
        memcpy(pic_param.row_height_minus1, row_height_minus1, sizeof(row_height_minus1));
    }
    else {
        /* when uniform_spacing_flag equals 1, application should populate
         * column_width_minus[], and row_height_minus1[] with approperiate values.*/
        int i = 0;
        for (i = 0; i < num_tile_columns_minus1; i++) {
            pic_param.column_width_minus1[i] = ((i + 1) * decoder->ctb_width)  / (num_tile_columns_minus1 + 1) -
                                              (i * decoder->ctb_width)  / (num_tile_columns_minus1 + 1) - 1;
            printf("ctb_width %d\n", decoder->ctb_width);
            printf("num_tile_columns_minus1 %d\n", num_tile_columns_minus1);
            printf("column_width_minus1[%d] %d\n", i, pic_param.column_width_minus1[i]);
        }
        for (i = 0; i < num_tile_rows_minus1; i++) {
            pic_param.row_height_minus1[i] = ((i + 1) * decoder->ctb_height) / (num_tile_rows_minus1 + 1) -
                                            (i * decoder->ctb_height) / (num_tile_rows_minus1 + 1) - 1;
            printf("ctb_height %d\n", decoder->ctb_height);
            printf("num_tile_rows_minus1 %d\n", num_tile_rows_minus1);
            printf("row_height_minus1[%d] %d\n", i, pic_param.row_height_minus1[i]);
        }
    }

    pic_param.pic_fields.bits.chroma_format_idc                          = chroma_format_idc;
    pic_param.pic_fields.bits.tiles_enabled_flag                         = tiles_enabled_flag;
    pic_param.pic_fields.bits.separate_colour_plane_flag                 = separate_colour_plane_flag;
    pic_param.pic_fields.bits.pcm_enabled_flag                           = pcm_enabled_flag;
    pic_param.pic_fields.bits.scaling_list_enabled_flag                  = scaling_list_enable_flag;
    pic_param.pic_fields.bits.transform_skip_enabled_flag                = transform_skip_enabled_flag;
    pic_param.pic_fields.bits.amp_enabled_flag                           = amp_enabled_flag;
    pic_param.pic_fields.bits.strong_intra_smoothing_enabled_flag        = sps_strong_intra_smoothing_enable_flag;
    pic_param.pic_fields.bits.sign_data_hiding_enabled_flag              = sign_data_hiding_flag;
    pic_param.pic_fields.bits.constrained_intra_pred_flag                = constrained_intra_pred_flag;
    pic_param.pic_fields.bits.cu_qp_delta_enabled_flag                   = cu_qp_delta_enabled_flag;
    pic_param.pic_fields.bits.weighted_pred_flag                         = weighted_pred_flag;
    pic_param.pic_fields.bits.weighted_bipred_flag                       = weighted_bipred_flag;
    pic_param.pic_fields.bits.transquant_bypass_enabled_flag             = transquant_bypass_enable_flag;
    pic_param.pic_fields.bits.entropy_coding_sync_enabled_flag           = entropy_coding_sync_enabled_flag;
    pic_param.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = seq_loop_filter_across_slices_enabled_flag;
    pic_param.pic_fields.bits.loop_filter_across_tiles_enabled_flag      = loop_filter_across_tiles_enabled_flag;
    pic_param.pic_fields.bits.pcm_loop_filter_disabled_flag              = pcm_loop_filter_disable_flag;

    pic_param.slice_parsing_fields.bits.lists_modification_present_flag             = lists_modification_present_flag;
    pic_param.slice_parsing_fields.bits.long_term_ref_pics_present_flag             = long_term_ref_pics_present_flag;
    pic_param.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag               = sps_temporal_mvp_enabled_flag;
    pic_param.slice_parsing_fields.bits.cabac_init_present_flag                     = cabac_init_present_flag;
    pic_param.slice_parsing_fields.bits.output_flag_present_flag                    = output_flag_present_flag;
    pic_param.slice_parsing_fields.bits.dependent_slice_segments_enabled_flag       = dependent_slice_segments_enabled_flag;
    pic_param.slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag    = pic_slice_level_chroma_qp_offsets_present_flag;
    pic_param.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag         = sao_enabled;
    pic_param.slice_parsing_fields.bits.deblocking_filter_override_enabled_flag     = deblocking_filter_override_enabled_flag;
    pic_param.slice_parsing_fields.bits.pps_disable_deblocking_filter_flag          = disable_dbf;
    pic_param.slice_parsing_fields.bits.slice_segment_header_extension_present_flag = slice_header_extension_present_flag;
    pic_param.slice_parsing_fields.bits.RapPicFlag                                  = 1; //IS_IRAP(h)
    pic_param.slice_parsing_fields.bits.IdrPicFlag                                  = 1; //IS_IDR(h)
    pic_param.slice_parsing_fields.bits.IntraPicFlag                                = 1; //IS_IRAP(h)

    if (va_init(decoder) != 0 || decoder->initialized != 1) {
        fprintf(stderr, "va_init failed\n");
        return 1;
    }

    if (decoder->seq_update_flag == 1) {
        vaDestroyBuffer(decoder->va_dpy, decoder->decode_param_buf);
        vaDestroyBuffer(decoder->va_dpy, decoder->pic_param_buf);
        vaDestroyBuffer(decoder->va_dpy, decoder->iqmatrix_buf);
        //decoder->seq_update_flag = 0;
    }

    if (sps_scaling_list_data_present_flag || scaling_list_data_present_flag) {
        printf("fill iq matrix from scaling list\n");
        int i,j;
        for (i = 0; i < 6; i++) {
            for (j = 0; j < 16; j++)
                iq_matrix.ScalingList4x4[i][j] = scaling_list.sl[0][i][j];
            for (j = 0; j < 64; j++) {
                iq_matrix.ScalingList8x8[i][j]   = scaling_list.sl[1][i][j];
                iq_matrix.ScalingList16x16[i][j] = scaling_list.sl[2][i][j];
                if (i < 2)
                    iq_matrix.ScalingList32x32[i][j] = scaling_list.sl[3][i * 3][j];
            }
            iq_matrix.ScalingListDC16x16[i] = scaling_list.sl_dc[0][i];
            if (i < 2)
                iq_matrix.ScalingListDC32x32[i] = scaling_list.sl_dc[1][i * 3];
        }
    }

    //printf("create sequence buffers, %08x \n", *(uint32_t*)decode_data);
    va_status = vaCreateBuffer(decoder->va_dpy, decoder->context_id,
                               VASequenceParameterBufferType,
                               sps_real_size - start_code_len,
                               1, sps_data,
                               &decoder->decode_param_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    va_status = vaCreateBuffer(decoder->va_dpy, decoder->context_id,
                               VAPictureParameterBufferType,
                               sizeof(VAPictureParameterBufferHEVC),
                               1, &pic_param,
                               &decoder->pic_param_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    va_status = vaCreateBuffer(decoder->va_dpy, decoder->context_id,
                               VAIQMatrixBufferType,
                               sizeof(VAIQMatrixBufferHEVC),
                               1, &iq_matrix,
                               &decoder->iqmatrix_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    return 0;
}


int main(int argc, char **argv)
{
    int major_ver, minor_ver;
    VABufferID slice_param_buf, slice_data_buf;
    VAStatus va_status;
    HEVCDecoder decoder;
    memset(&decoder, 0, sizeof(decoder));
    decoder.output_bit_depth = 8;
    const char *input_file = nullptr;
    const char *output_file = nullptr;
    FILE* file = nullptr;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Usage: %s [-b 8|10] <hevc_file> [output_file]\n", argv[0]);
                return -1;
            }
            decoder.output_bit_depth = atoi(argv[i]);
            if (decoder.output_bit_depth != 8 && decoder.output_bit_depth != 10) {
                fprintf(stderr, "-b must be 8 or 10\n");
                return -1;
            }
        } else if (!input_file) {
            input_file = argv[i];
        } else if (!output_file) {
            output_file = argv[i];
        } else {
            fprintf(stderr, "Usage: %s [-b 8|10] <hevc_file> [output_file]\n", argv[0]);
            return -1;
        }
    }

    if (!input_file) {
        fprintf(stderr, "Usage: %s [-b 8|10] <hevc_file> [output_file]\n", argv[0]);
        return -1;
    }

    FILE* fp = fopen(input_file, "rb");
    if (!fp) {
        fprintf(stderr, "Failed to open file: %s\n", input_file);
        return -1;
    }

    if (output_file) {
        printf("output_file: %s\n", output_file);
        file = fopen(output_file, "wb");
        if (!file) {
            fprintf(stderr, "Failed to open output file: %s\n", output_file);
            fclose(fp);
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
        printf("NAL: type %d, size %d\n", nal_type, nal_size);

        switch (nal_type) {
            case NAL_VPS:
                printf("Found VPS NAL, skipping\n");
                break;
            case NAL_SPS:
                sps_nal = nal;
                sps_size = nal_size;
                sps_real_size = 0;
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

    va_status = va_hevc_parse_sequence(&decoder, sps_nal, sps_size, pps_nal, pps_size);
    CHECK_VASTATUS(va_status, "vaProcessSequence");

    VASurfaceID surface_id = 0;
    VASurfaceID last_surface_id = 0;

    while (remaining_size > 4) {
        uint8_t *nal = find_next_nal(current_pos, remaining_size, &nal_size);
        if (!nal) break;

        int nal_type = get_nal_type(nal);
        start_code_len = get_start_code_len(nal);
        printf("NAL: type %d, size %d\n", nal_type, nal_size);

        if (nal_type != NAL_VPS && nal_type != NAL_SPS && nal_type != NAL_PPS &&
            nal_type != NAL_PREFIX_SEI && nal_type != NAL_SUFFIX_SEI && nal_type != NAL_AUD &&
            nal_type != NAL_EOS_NUT && nal_type != NAL_EOB_NUT && nal_type != NAL_FD_NUT && nal_type < 41 &&
            !(decoder.no_rasl_output_flag && (nal_type == NAL_RASL_N || nal_type == NAL_RASL_R))) {
            //41~47 reserved, 48~63 unspecified
            printf("Decoding frame %d (NAL type %d, size %d)\n", frame_count, nal_type, nal_size);

            VASliceParameterBufferHEVC slice_param;
            memset(&slice_param, 0, sizeof(slice_param));

            int slice_data_size = nal_size - start_code_len; // remove start code
            printf("slice_data_size: %d\n", slice_data_size);

            slice_nal = nal;
            slice_nal += start_code_len;     //remove start code
            BitstreamParser slice_parser;
            init_bitstream_parser(&slice_parser, slice_nal, slice_data_size);
            skip_bits(&slice_parser, 16);  // skip nalu header

            int slice_segment_address = 0;
            int slice_type = 2; // default I
            int pic_output_flag = 1;
            int dependent_slice_segment_flag = 0;
            int no_output_of_prior_pics_flag = 0;

            int first_slice_segment_in_pic_flag = get_bits(&slice_parser, 1);
            printf("first_slice_segment_in_pic_flag: %d\n", first_slice_segment_in_pic_flag);
            if (IS_IRAP_NAL(nal_type)) {
                no_output_of_prior_pics_flag = get_bits(&slice_parser, 1); // no_output_of_prior_pics_flag
                printf("no_output_of_prior_pics_flag: %d\n", no_output_of_prior_pics_flag);
                if ((frame_count == 0 && no_output_of_prior_pics_flag) || IS_IDR_NAL(nal_type)) {
                    no_output_of_prior_pics_flag = 0; // only affects not first frame or not IDR
                }
            }

            if (nal_type != NAL_RASL_N && nal_type != NAL_RASL_R) {
                decoder.no_rasl_output_flag = IS_IDR_NAL(nal_type) || IS_BLA_NAL(nal_type) ||
                                            (nal_type == NAL_CRA_NUT && decoder.last_eos_flag) || frame_count == 0;
            }
            printf("no_rasl_output_flag: %d\n", decoder.no_rasl_output_flag);

            if (nal_type == NAL_CRA_NUT && decoder.last_eos_flag) {
                decoder.last_eos_flag = 0;
            }
            printf("no_output_of_prior_pics_flag: %d\n", no_output_of_prior_pics_flag);

            int slice_pps_id = get_ue_golomb(&slice_parser); // pps_id
            printf("slice_pps_id: %d\n", slice_pps_id);

            if (!first_slice_segment_in_pic_flag) {
                if (decoder.dependent_slice_segments_enabled_flag) {
                    dependent_slice_segment_flag = get_bits(&slice_parser, 1); // dependent_slice_segment_flag
                }
                slice_segment_address = get_bits(&slice_parser, decoder.slice_address_length); // slice_segment_address
                printf("slice_segment_address: %d\n", slice_segment_address);
            }

            printf("num_extra_slice_header_bits: %d\n", decoder.num_extra_slice_header_bits);
            if (!dependent_slice_segment_flag) {
                for (int i = 0; i < decoder.num_extra_slice_header_bits; i++) {
                    skip_bits(&slice_parser, 1); // slice_reserved_flag
                }
                slice_type = get_ue_golomb(&slice_parser); // slice_type
                printf("slice_type: %d\n", slice_type);
                if (decoder.output_flag_present_flag) {
                    pic_output_flag = get_bits(&slice_parser, 1); // pic_output_flag
                    printf("pic_output_flag: %d\n", pic_output_flag);
                }
            }

            switch (slice_type) {
                case 0: slice_type = 0; break;  // P
                case 1: slice_type = 1; break;  // B
                case 2: slice_type = 2; break;  // I
            }

            slice_param.slice_data_size = slice_data_size;
            slice_param.slice_data_offset = 0;
            slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
            slice_param.slice_data_byte_offset = 0;
            slice_param.slice_segment_address = slice_segment_address;

            va_status = vaCreateBuffer(decoder.va_dpy, decoder.context_id,
                                    VASliceParameterBufferType,
                                    sizeof(VASliceParameterBufferHEVC),
                                    1, &slice_param,
                                    &slice_param_buf);
            CHECK_VASTATUS(va_status, "vaCreateBuffer");

            va_status = vaCreateBuffer(decoder.va_dpy, decoder.context_id,
                                    VASliceDataBufferType,
                                    slice_data_size,
                                    1, nal + start_code_len,
                                    &slice_data_buf);
            CHECK_VASTATUS(va_status, "vaCreateBuffer");

            if (first_slice_segment_in_pic_flag) {
               surface_id = decoder.surfaces[(frame_count) % decoder.surface_count];
               last_surface_id = surface_id;
            } else {
                surface_id = last_surface_id;
            }

            if (first_slice_segment_in_pic_flag) {

                va_status = vaBeginPicture(decoder.va_dpy, decoder.context_id, surface_id);
                CHECK_VASTATUS(va_status, "vaBeginPicture");
                printf("surface_id: %d, context_id: %d, config_id: %d\n", surface_id, decoder.context_id, decoder.config_id);

                if (decoder.seq_update_flag == 1) {
                    va_status = vaRenderPicture(decoder.va_dpy, decoder.context_id, &decoder.decode_param_buf, 1);
                    CHECK_VASTATUS(va_status, "vaRenderPicture");
                }

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
            } else {
                is_last_slice_in_pic = 1;
            }
            printf("is_last_slice_in_pic: %d\n", is_last_slice_in_pic);

            if (is_last_slice_in_pic) {

                va_status = vaEndPicture(decoder.va_dpy, decoder.context_id);
                CHECK_VASTATUS(va_status, "vaEndPicture");

                if (pic_output_flag && !no_output_of_prior_pics_flag) {
                    va_status = vaSyncSurface(decoder.va_dpy, surface_id);
                    printf("vaSyncSurface %d\n", surface_id);
                    CHECK_VASTATUS(va_status, "vaSyncSurface");
                }

                if (file && pic_output_flag && !no_output_of_prior_pics_flag) {
                    va_status = save_vaapi_surface(decoder.va_dpy, surface_id, decoder.conf_win, file,
                                                   decoder.output_bit_depth);
                    CHECK_VASTATUS(va_status, "vaSaveSurface");
                }

                printf("Decoded %d frame done\n", frame_count);
                decoder.seq_update_flag = 0;
                frame_count++;
            }

            vaDestroyBuffer(decoder.va_dpy, slice_param_buf);
            vaDestroyBuffer(decoder.va_dpy, slice_data_buf);
        } else if (nal_type == NAL_SPS || nal_type == NAL_PPS) {
            switch (nal_type) {
                case NAL_SPS:
                    sps_nal = nal;
                    sps_size = nal_size;
                    sps_real_size = 0;
                    break;
                case NAL_PPS:
                    pps_nal = nal;
                    pps_size = nal_size;
                    break;
                default:
                    break;
            }
            va_status = va_hevc_parse_sequence(&decoder, sps_nal, sps_size, pps_nal, pps_size);
            CHECK_VASTATUS(va_status, "vaProcessSequence");
        } else if (nal_type == NAL_EOS_NUT || nal_type == NAL_EOB_NUT) {
            decoder.last_eos_flag = 1;
            printf("Received EOS NAL\n");
        }  else {
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
