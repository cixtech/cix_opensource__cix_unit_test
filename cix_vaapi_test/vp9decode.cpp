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
#include <stdint.h>
#include <va/va.h>
#include "vp9decode.h"
#include "common/vadisplay.h"
#include "common/bitstream.h"
#include "common/dumpframe.h"

// read 32-bit little-endian from IVF / buffer
static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// read 64-bit little-endian (IVF frame timestamp)
static uint64_t read_le64(const uint8_t *p)
{
    return (uint64_t)read_le32(p) | ((uint64_t)read_le32(p + 4) << 32);
}

// VP9 differential syntax: sign bit follows magnitude
static int vp9_get_sbits_inv(BitstreamParser *parser, int n)
{
    int v = (int)get_bits(parser, n);
    return get_bits(parser, 1) ? -v : v;
}

static int vp9_clip_uintp2(int v, int bits)
{
    int m = (1 << bits) - 1;
    if (v < 0)
        return 0;
    if (v > m)
        return m;
    return v;
}

// VP9 decode012() for log2_tile_rows
static int vp9_decode012(BitstreamParser *parser)
{
    /* Spec decode012: read second bit only when first bit is 1. */
    int v = (int)get_bits(parser, 1);
    if (v)
        v += (int)get_bits(parser, 1);
    return v;
}

// Profile 2: ten_or_twelve_bit (reject 12-bit), color_space, optional color_range; profile 0: color_space + range.
static int vp9_parse_color_config(BitstreamParser *parser, int vp9_profile, int *bit_depth_out)
{
    if (vp9_profile == 2) {
        int twelve = (int)get_bits(parser, 1);
        if (twelve) {
            fprintf(stderr, "vp9: profile 2 12-bit not supported (only 10-bit)\n");
            return -1;
        }
        *bit_depth_out = 10;
        int cs = (int)get_bits(parser, 3);
        if (cs != 7)
            skip_bits(parser, 1);
    } else {
        skip_bits(parser, 3);
        skip_bits(parser, 1);
        *bit_depth_out = 8;
    }
    return 0;
}

// VP9 superframe index: returns number of concatenated frames in one IVF packet
static int get_vp9_superframe_count(const uint8_t *buffer, int buffer_size, int layer_sizes[8])
{
    if (buffer_size < 1)
        return 0;
    uint8_t marker = buffer[buffer_size - 1];
    if ((marker & 0xe0) != 0xc0) {
        layer_sizes[0] = buffer_size;
        return 1;
    }
    int length_size = 1 + ((marker >> 3) & 0x3);
    int nb_frames = 1 + (marker & 0x7);
    int idx_size = 2 + nb_frames * length_size;
    if (buffer_size < idx_size || buffer[buffer_size - idx_size] != marker) {
        layer_sizes[0] = buffer_size;
        return 1;
    }
    const uint8_t *idx = buffer + buffer_size - idx_size + 1;
    int64_t total = 0;
    for (int i = 0; i < nb_frames; i++) {
        int frame_size = 0;
        for (int j = 0; j < length_size; j++)
            frame_size |= idx[i * length_size + j] << (j * 8);
        layer_sizes[i] = frame_size;
        total += frame_size;
        if (frame_size <= 0 || total > buffer_size - idx_size) {
            fprintf(stderr, "vp9: invalid superframe size\n");
            layer_sizes[0] = buffer_size;
            return 1;
        }
    }
    if (nb_frames > 1)
        printf("get_vp9_superframe_count: superframe marker=0x%02x, layers=%d, length_size=%d\n",
               marker, nb_frames, length_size);
    return nb_frames;
}

// Release VA-API VP9 decode resources (surfaces/context; display left to main)
static void va_release(VP9Decoder *decoder)
{
    if (!decoder || !decoder->initialized)
        return;
    printf("va_release: destroy VP9 VA context and %d surfaces\n", decoder->surface_count);
    vaDestroyContext(decoder->va_dpy, decoder->context_id);
    vaDestroySurfaces(decoder->va_dpy, decoder->surfaces, decoder->surface_count);
    vaDestroyConfig(decoder->va_dpy, decoder->config_id);
    free(decoder->surfaces);
    decoder->surfaces = NULL;
    decoder->surface_count = 0;
    decoder->initialized = 0;
    memset(decoder->ref_valid, 0, sizeof(decoder->ref_valid));
    for (int i = 0; i < 8; i++)
        decoder->ref_frames[i] = VA_INVALID_ID;
}

// Initialize or recreate VA context when coded size or selected output bit depth changes
static int va_init(VP9Decoder *decoder, int width, int height)
{
    VAStatus va_status;
    VAConfigAttrib attrib;
    VAProfile va_profile;
    unsigned int rt_format;
    int bit_depth = decoder->output_bit_depth;
    int vp9_profile = (bit_depth == 10) ? 2 : 0;

    va_profile = (bit_depth == 10) ? VAProfileVP9Profile2 : VAProfileVP9Profile0;

    if (bit_depth <= 8) {
        rt_format = VA_RT_FORMAT_YUV420;
    } else if (bit_depth == 10)
        rt_format = VA_RT_FORMAT_YUV420_10;
    else {
        fprintf(stderr, "va_init: unsupported bit depth %d (profile 2 supports 10-bit only)\n", bit_depth);
        return -1;
    }

    printf("va_init: request %dx%d profile=%d bit_depth=%d (initialized=%d current=%dx%d va_prof=%d bd=%d)\n",
           width, height, vp9_profile, bit_depth, decoder->initialized,
           decoder->initialized ? decoder->width : 0, decoder->initialized ? decoder->height : 0,
           decoder->initialized ? (int)decoder->active_va_profile : -1,
           decoder->initialized ? decoder->active_rt_bit_depth : 0);

    if (decoder->initialized && decoder->width == width && decoder->height == height &&
        decoder->active_va_profile == va_profile && decoder->active_rt_bit_depth == bit_depth)
        return 0;

    if (decoder->initialized) {
        printf("va_init: config change, recreating VA context (was %dx%d prof=%d bd=%d)\n",
               decoder->width, decoder->height, (int)decoder->active_va_profile, decoder->active_rt_bit_depth);
        va_release(decoder);
    }

    attrib.type = VAConfigAttribRTFormat;
    va_status = vaGetConfigAttributes(decoder->va_dpy, va_profile, VAEntrypointVLD, &attrib, 1);
    CHECK_VASTATUS(va_status, "vaGetConfigAttributes");
    if ((attrib.value & rt_format) == 0) {
        fprintf(stderr, "va_init: RT format 0x%x not supported for VA profile %d (mask 0x%x)\n",
                rt_format, (int)va_profile, attrib.value);
        return -1;
    }

    attrib.type = VAConfigAttribRTFormat;
    attrib.value = rt_format;
    va_status =
        vaCreateConfig(decoder->va_dpy, va_profile, VAEntrypointVLD, &attrib, 1, &decoder->config_id);
    CHECK_VASTATUS(va_status, "vaCreateConfig");

    decoder->surface_count = MAX_PICTURES;
    decoder->surfaces = (VASurfaceID *)malloc(sizeof(VASurfaceID) * decoder->surface_count);
    if (!decoder->surfaces)
        return -1;

    va_status =
        vaCreateSurfaces(decoder->va_dpy, rt_format, width, height, decoder->surfaces, decoder->surface_count,
                         NULL, 0);
    CHECK_VASTATUS(va_status, "vaCreateSurfaces");

    va_status = vaCreateContext(decoder->va_dpy, decoder->config_id, width, height, VA_PROGRESSIVE,
                                decoder->surfaces, decoder->surface_count, &decoder->context_id);
    CHECK_VASTATUS(va_status, "vaCreateContext");

    decoder->width = width;
    decoder->height = height;
    decoder->active_va_profile = va_profile;
    decoder->active_rt_bit_depth = bit_depth;
    decoder->initialized = 1;
    memset(decoder->ref_valid, 0, sizeof(decoder->ref_valid));
    for (int i = 0; i < 8; i++)
        decoder->ref_frames[i] = VA_INVALID_ID;

    printf("va_init: VA profile %d, %dx%d, RT format 0x%x, surfaces %d\n", (int)va_profile, width, height,
           rt_format, decoder->surface_count);
    return 0;
}

static VASurfaceID vp9_pick_decode_surface_id(const VP9Decoder *decoder)
{
    for (int i = 0; i < decoder->surface_count; i++) {
        VASurfaceID surface_id = decoder->surfaces[i];
        int used = 0;
        for (int j = 0; j < 8; j++) {
            if (decoder->ref_valid[j] && decoder->ref_frames[j] == surface_id) {
                used = 1;
                break;
            }
        }
        if (!used)
            return surface_id;
    }
    printf("vp9_pick_decode_surface_id: all surfaces in use, fallback surface[0]=%u\n",
           (unsigned)decoder->surfaces[0]);
    return decoder->surfaces[0];
}

/*
 * Parse VP9 uncompressed header + build VA picture/slice params.
 * Returns: 0 OK, -1 error, 2 show_existing_frame (no slice data).
 */
static int va_vp9_parse_picture_header(VP9Decoder *decoder, const uint8_t *frame_data, int frame_size,
                            VADecPictureParameterBufferVP9 *picture_param,
                            VASliceParameterBufferVP9 *slice_param,
                            int *show_existing_idx_out, uint8_t *refresh_mask_out)
{
    BitstreamParser bitstream_parser;
    int w = 0, h = 0;
    int profile, keyframe, invisible = 0, errorres = 0, intraonly = 0;
    int resetctx = 0;
    uint8_t refreshrefmask = 0;
    int refidx[3] = {0, 0, 0};
    int signbias[3] = {0, 0, 0};
    int highprecisionmvs = 0;
    int filtermode = VP9_FILTER_8TAP_REGULAR;
    int refreshctx = 0, parallelmode = 0, framectxid = 0;
    struct VP9LfDelta lf_delta = {};
    VP9Segmentation seg = {};
    int yac_qi = 0;
    int ydc_qdelta = 0, uvdc_qdelta = 0, uvac_qdelta = 0;
    int lossless = 0;
    int log2_tile_cols = 0, log2_tile_rows = 0;
    int frame_bit_depth = 8;
    const int16_t *dc_qtbl = vp9_dc_qlookup_8;
    const int16_t *ac_qtbl = vp9_ac_qlookup_8;

    if (show_existing_idx_out)
        *show_existing_idx_out = -1;

    memset(picture_param, 0, sizeof(*picture_param));
    memset(slice_param, 0, sizeof(*slice_param));

    if (frame_size < 1)
        return -1;

    init_bitstream_parser(&bitstream_parser, frame_data, frame_size);

    if (get_bits(&bitstream_parser, 2) != 0x2) {
        fprintf(stderr, "vp9: bad frame marker\n");
        return -1;
    }

    profile = (int)get_bits(&bitstream_parser, 1);
    profile |= (int)get_bits(&bitstream_parser, 1) << 1;
    if (profile == 3)
        profile += (int)get_bits(&bitstream_parser, 1);

    if (profile != 0 && profile != 2) {
        fprintf(stderr, "vp9: only profile 0 and profile 2 are supported (got %d)\n", profile);
        return -1;
    }

    if (get_bits(&bitstream_parser, 1)) {
        /* show_existing_frame */
        if (show_existing_idx_out)
            *show_existing_idx_out = (int)get_bits(&bitstream_parser, 3);
        return 2;
    }

    keyframe = !get_bits(&bitstream_parser, 1);
    invisible = !get_bits(&bitstream_parser, 1);
    errorres = (int)get_bits(&bitstream_parser, 1);

    if (keyframe) {
        if (get_bits(&bitstream_parser, 24) != VP9_SYNCCODE) {
            fprintf(stderr, "vp9: bad sync code\n");
            return -1;
        }
        if (vp9_parse_color_config(&bitstream_parser, profile, &frame_bit_depth) != 0)
            return -1;
        decoder->coded_bit_depth = frame_bit_depth;
        refreshrefmask = 0xff;
        w = (int)get_bits(&bitstream_parser, 16) + 1;
        h = (int)get_bits(&bitstream_parser, 16) + 1;
        if (get_bits(&bitstream_parser, 1))
            skip_bits(&bitstream_parser, 32);
    } else {
        intraonly = invisible ? (int)get_bits(&bitstream_parser, 1) : 0;
        resetctx = errorres ? 0 : (int)get_bits(&bitstream_parser, 2);
        (void)resetctx;
        if (intraonly) {
            if (get_bits(&bitstream_parser, 24) != VP9_SYNCCODE) {
                fprintf(stderr, "vp9: bad sync code (intra)\n");
                return -1;
            }
            if (vp9_parse_color_config(&bitstream_parser, profile, &frame_bit_depth) != 0)
                return -1;
            decoder->coded_bit_depth = frame_bit_depth;
            refreshrefmask = (uint8_t)get_bits(&bitstream_parser, 8);
            w = (int)get_bits(&bitstream_parser, 16) + 1;
            h = (int)get_bits(&bitstream_parser, 16) + 1;
            if (get_bits(&bitstream_parser, 1))
                skip_bits(&bitstream_parser, 32);
        } else {
            refreshrefmask = (uint8_t)get_bits(&bitstream_parser, 8);
            refidx[0] = (int)get_bits(&bitstream_parser, 3);
            signbias[0] = (int)get_bits(&bitstream_parser, 1) && !errorres;
            refidx[1] = (int)get_bits(&bitstream_parser, 3);
            signbias[1] = (int)get_bits(&bitstream_parser, 1) && !errorres;
            refidx[2] = (int)get_bits(&bitstream_parser, 3);
            signbias[2] = (int)get_bits(&bitstream_parser, 1) && !errorres;

            for (int i = 0; i < 3; i++) {
                if (!decoder->ref_valid[refidx[i]]) {
                    fprintf(stderr, "vp9: missing reference %d\n", refidx[i]);
                    return -1;
                }
            }

            if (get_bits(&bitstream_parser, 1)) {
                w = decoder->ref_width[refidx[0]];
                h = decoder->ref_height[refidx[0]];
            } else if (get_bits(&bitstream_parser, 1)) {
                w = decoder->ref_width[refidx[1]];
                h = decoder->ref_height[refidx[1]];
            } else if (get_bits(&bitstream_parser, 1)) {
                w = decoder->ref_width[refidx[2]];
                h = decoder->ref_height[refidx[2]];
            } else {
                w = (int)get_bits(&bitstream_parser, 16) + 1;
                h = (int)get_bits(&bitstream_parser, 16) + 1;
            }
            if (get_bits(&bitstream_parser, 1))
                skip_bits(&bitstream_parser, 32);
            if (profile == 2) {
                if (decoder->coded_bit_depth != 10) {
                    fprintf(stderr, "vp9 profile 2: missing color config before inter frame\n");
                    return -1;
                }
                frame_bit_depth = decoder->coded_bit_depth;
            } else {
                frame_bit_depth = 8;
            }
            highprecisionmvs = (int)get_bits(&bitstream_parser, 1);
            if (get_bits(&bitstream_parser, 1))
                filtermode = VP9_FILTER_SWITCHABLE;
            else
                filtermode = (int)get_bits(&bitstream_parser, 2);
        }
    }

    refreshctx = errorres ? 0 : (int)get_bits(&bitstream_parser, 1);
    parallelmode = errorres ? 1 : (int)get_bits(&bitstream_parser, 1);
    framectxid = (int)get_bits(&bitstream_parser, 2);
    if (keyframe || intraonly)
        framectxid = 0;

    if (keyframe || errorres || intraonly) {
        lf_delta.ref[0] = 1;
        lf_delta.ref[1] = 0;
        lf_delta.ref[2] = -1;
        lf_delta.ref[3] = -1;
        lf_delta.mode[0] = 0;
        lf_delta.mode[1] = 0;
        memset(seg.feat, 0, sizeof(seg.feat));
    }

    int filter_level = (int)get_bits(&bitstream_parser, 6);
    int sharpness = (int)get_bits(&bitstream_parser, 3);

    lf_delta.enabled = (int)get_bits(&bitstream_parser, 1);
    if (lf_delta.enabled) {
        lf_delta.updated = (int)get_bits(&bitstream_parser, 1);
        if (lf_delta.updated) {
            for (int i = 0; i < 4; i++) {
                if (get_bits(&bitstream_parser, 1))
                    lf_delta.ref[i] = (int8_t)vp9_get_sbits_inv(&bitstream_parser, 6);
            }
            for (int i = 0; i < 2; i++) {
                if (get_bits(&bitstream_parser, 1))
                    lf_delta.mode[i] = (int8_t)vp9_get_sbits_inv(&bitstream_parser, 6);
            }
        }
    }

    yac_qi = (int)get_bits(&bitstream_parser, 8);
    ydc_qdelta = get_bits(&bitstream_parser, 1) ? vp9_get_sbits_inv(&bitstream_parser, 4) : 0;
    uvdc_qdelta = get_bits(&bitstream_parser, 1) ? vp9_get_sbits_inv(&bitstream_parser, 4) : 0;
    uvac_qdelta = get_bits(&bitstream_parser, 1) ? vp9_get_sbits_inv(&bitstream_parser, 4) : 0;
    lossless = (yac_qi == 0 && ydc_qdelta == 0 && uvdc_qdelta == 0 && uvac_qdelta == 0);

    seg.enabled = (int)get_bits(&bitstream_parser, 1);
    if (seg.enabled) {
        seg.update_map = (int)get_bits(&bitstream_parser, 1);
        if (seg.update_map) {
            for (int i = 0; i < 7; i++)
                seg.prob[i] = get_bits(&bitstream_parser, 1) ? (uint8_t)get_bits(&bitstream_parser, 8) : 255;
            seg.temporal = (int)get_bits(&bitstream_parser, 1);
            if (seg.temporal) {
                for (int i = 0; i < 3; i++)
                    seg.pred_prob[i] = get_bits(&bitstream_parser, 1) ? (uint8_t)get_bits(&bitstream_parser, 8) : 255;
            }
        }
        if (get_bits(&bitstream_parser, 1)) {
            seg.absolute_vals = (int)get_bits(&bitstream_parser, 1);
            for (int i = 0; i < 8; i++) {
                seg.feat[i].q_enabled = (int)get_bits(&bitstream_parser, 1);
                if (seg.feat[i].q_enabled)
                    seg.feat[i].q_val = (int16_t)vp9_get_sbits_inv(&bitstream_parser, 8);
                seg.feat[i].lf_enabled = (int)get_bits(&bitstream_parser, 1);
                if (seg.feat[i].lf_enabled)
                    seg.feat[i].lf_val = (int8_t)vp9_get_sbits_inv(&bitstream_parser, 6);
                seg.feat[i].ref_enabled = (int)get_bits(&bitstream_parser, 1);
                if (seg.feat[i].ref_enabled)
                    seg.feat[i].ref_val = (uint8_t)get_bits(&bitstream_parser, 2);
                seg.feat[i].skip_enabled = (int)get_bits(&bitstream_parser, 1);
            }
        }
    } else {
        seg.temporal = 0;
        seg.update_map = 0;
    }

    if (frame_bit_depth == 10) {
        dc_qtbl = vp9_dc_qlookup_10;
        ac_qtbl = vp9_ac_qlookup_10;
    }

    for (int i = 0; i < (seg.enabled ? 8 : 1); i++) {
        int qyac, qydc, quvac, quvdc, lflvl, sh = filter_level >= 32 ? 1 : 0;

        if (seg.enabled && seg.feat[i].q_enabled) {
            if (seg.absolute_vals)
                qyac = vp9_clip_uintp2((int)seg.feat[i].q_val, 8);
            else
                qyac = vp9_clip_uintp2(yac_qi + (int)seg.feat[i].q_val, 8);
        } else {
            qyac = yac_qi;
        }
        qydc = vp9_clip_uintp2(qyac + ydc_qdelta, 8);
        quvdc = vp9_clip_uintp2(qyac + uvdc_qdelta, 8);
        quvac = vp9_clip_uintp2(qyac + uvac_qdelta, 8);
        qyac = vp9_clip_uintp2(qyac, 8);

        seg.feat[i].qmul[0][0] = dc_qtbl[qydc];
        seg.feat[i].qmul[0][1] = ac_qtbl[qyac];
        seg.feat[i].qmul[1][0] = dc_qtbl[quvdc];
        seg.feat[i].qmul[1][1] = ac_qtbl[quvac];

        if (seg.enabled && seg.feat[i].lf_enabled) {
            if (seg.absolute_vals)
                lflvl = vp9_clip_uintp2((int)seg.feat[i].lf_val, 6);
            else
                lflvl = vp9_clip_uintp2(filter_level + (int)seg.feat[i].lf_val, 6);
        } else {
            lflvl = filter_level;
        }

        if (lf_delta.enabled) {
            seg.feat[i].lflvl[0][0] = seg.feat[i].lflvl[0][1] =
                (uint8_t)vp9_clip_uintp2(lflvl + (lf_delta.ref[0] * (1 << sh)), 6);
            for (int j = 1; j < 4; j++) {
                seg.feat[i].lflvl[j][0] = (uint8_t)vp9_clip_uintp2(
                    lflvl + ((lf_delta.ref[j] + lf_delta.mode[0]) * (1 << sh)), 6);
                seg.feat[i].lflvl[j][1] = (uint8_t)vp9_clip_uintp2(
                    lflvl + ((lf_delta.ref[j] + lf_delta.mode[1]) * (1 << sh)), 6);
            }
        } else {
            memset(seg.feat[i].lflvl, (uint8_t)lflvl, sizeof(seg.feat[i].lflvl));
        }
    }

    int sb_cols = (w + 63) / 64;
    for (log2_tile_cols = 0; sb_cols > (64 << log2_tile_cols); log2_tile_cols++)
        ;
    int max = 0;
    while ((sb_cols >> max) >= 4)
        max++;
    max = max > 0 ? max - 1 : 0;
    while (max > log2_tile_cols) {
        if (!get_bits(&bitstream_parser, 1))
            break;
        log2_tile_cols++;
    }
    log2_tile_rows = vp9_decode012(&bitstream_parser);

    uint32_t compressed_header_size = get_bits(&bitstream_parser, 16);
    unsigned int uncompressed_header_size = ((unsigned)get_bits_count(&bitstream_parser) + 7) / 8;

    {
        int pad = (-get_bits_count(&bitstream_parser)) & 7;
        if (pad)
            skip_bits(&bitstream_parser, pad);
    }
    int aligned_byte_off = get_bits_count(&bitstream_parser) / 8;
    {
        int64_t hdr_end = aligned_byte_off;
        int64_t fps = (int64_t)compressed_header_size;
        int64_t total = (int64_t)frame_size;
        if (hdr_end > total) {
            fprintf(stderr, "vp9: invalid header end (hdr_end=%lld total=%lld)\n",
                    (long long)hdr_end, (long long)total);
            return -1;
        }
        if (hdr_end + fps > total) {
            int64_t remain = total - hdr_end;
            fprintf(stderr,
                    "vp9: header + first_partition exceeds frame (hdr_end=%lld fps=%lld total=%lld), "
                    "clamp first_partition to %lld\n",
                    (long long)hdr_end, (long long)fps, (long long)total, (long long)remain);
            compressed_header_size = (uint32_t)(remain > 0 ? remain : 0);
        }
    }

    picture_param->frame_width = (uint16_t)w;
    picture_param->frame_height = (uint16_t)h;
    picture_param->pic_fields.bits.subsampling_x = 1;
    picture_param->pic_fields.bits.subsampling_y = 1;
    picture_param->pic_fields.bits.frame_type = keyframe ? 0 : 1;
    picture_param->pic_fields.bits.show_frame = invisible ? 0 : 1;
    picture_param->pic_fields.bits.error_resilient_mode = errorres ? 1 : 0;
    picture_param->pic_fields.bits.intra_only = intraonly ? 1 : 0;
    picture_param->pic_fields.bits.allow_high_precision_mv = (!keyframe && !intraonly) ? (highprecisionmvs ? 1 : 0) : 0;
    picture_param->pic_fields.bits.mcomp_filter_type =
        (uint32_t)(filtermode ^ (unsigned int)((filtermode <= 1) ? 1 : 0));
    picture_param->pic_fields.bits.frame_parallel_decoding_mode = parallelmode ? 1 : 0;
    picture_param->pic_fields.bits.reset_frame_context = (uint32_t)(errorres ? 0 : resetctx);
    picture_param->pic_fields.bits.refresh_frame_context = refreshctx ? 1 : 0;
    picture_param->pic_fields.bits.frame_context_idx = (uint32_t)framectxid;
    picture_param->pic_fields.bits.segmentation_enabled = seg.enabled ? 1 : 0;
    picture_param->pic_fields.bits.segmentation_temporal_update = seg.temporal ? 1 : 0;
    picture_param->pic_fields.bits.segmentation_update_map = seg.update_map ? 1 : 0;
    picture_param->pic_fields.bits.last_ref_frame = (uint32_t)refidx[0];
    picture_param->pic_fields.bits.last_ref_frame_sign_bias = signbias[0] ? 1 : 0;
    picture_param->pic_fields.bits.golden_ref_frame = (uint32_t)refidx[1];
    picture_param->pic_fields.bits.golden_ref_frame_sign_bias = signbias[1] ? 1 : 0;
    picture_param->pic_fields.bits.alt_ref_frame = (uint32_t)refidx[2];
    picture_param->pic_fields.bits.alt_ref_frame_sign_bias = signbias[2] ? 1 : 0;
    picture_param->pic_fields.bits.lossless_flag = lossless ? 1 : 0;

    picture_param->filter_level = (uint8_t)filter_level;
    picture_param->sharpness_level = (uint8_t)sharpness;
    picture_param->log2_tile_rows = (uint8_t)log2_tile_rows;
    picture_param->log2_tile_columns = (uint8_t)log2_tile_cols;
    picture_param->frame_header_length_in_bytes = (uint8_t)uncompressed_header_size;
    picture_param->first_partition_size = (uint16_t)compressed_header_size;
    picture_param->profile = (uint8_t)profile;
    picture_param->bit_depth = (uint8_t)frame_bit_depth;

    memcpy(picture_param->mb_segment_tree_probs, seg.prob, sizeof(picture_param->mb_segment_tree_probs));
    if (seg.temporal)
        memcpy(picture_param->segment_pred_probs, seg.pred_prob, sizeof(picture_param->segment_pred_probs));
    else
        memset(picture_param->segment_pred_probs, 255, sizeof(picture_param->segment_pred_probs));

    for (int i = 0; i < 8; i++) {
        if (decoder->ref_valid[i])
            picture_param->reference_frames[i] = decoder->ref_frames[i];
        else
            picture_param->reference_frames[i] = VA_INVALID_ID;
    }

    slice_param->slice_data_size = (uint32_t)frame_size;
    slice_param->slice_data_offset = 0;
    slice_param->slice_data_flag = VA_SLICE_DATA_FLAG_ALL;

    for (int i = 0; i < 8; i++) {
        VASegmentParameterVP9 *sp = &slice_param->seg_param[i];
        memset(sp, 0, sizeof(*sp));
        sp->segment_flags.fields.segment_reference_enabled = seg.feat[i].ref_enabled ? 1 : 0;
        sp->segment_flags.fields.segment_reference = seg.feat[i].ref_val & 3;
        sp->segment_flags.fields.segment_reference_skipped = seg.feat[i].skip_enabled ? 1 : 0;
        memcpy(sp->filter_level, seg.feat[i].lflvl, sizeof(sp->filter_level));
        sp->luma_dc_quant_scale = seg.feat[i].qmul[0][0];
        sp->luma_ac_quant_scale = seg.feat[i].qmul[0][1];
        sp->chroma_dc_quant_scale = seg.feat[i].qmul[1][0];
        sp->chroma_ac_quant_scale = seg.feat[i].qmul[1][1];
    }

    if (refresh_mask_out)
        *refresh_mask_out = refreshrefmask;
    printf("va_vp9_parse_picture_header: ok %dx%d keyframe=%d show_frame=%d first_partition=%u "
           "frame_header_len=%u seg_enabled=%d\n",
           w, h, keyframe, invisible ? 0 : 1, (unsigned)compressed_header_size,
           (unsigned)uncompressed_header_size, seg.enabled);
    return 0;
}

static void va_vp9_update_reference_frames(VP9Decoder *decoder, uint8_t refresh_mask,
                                           VASurfaceID output_surface, int frame_width, int frame_height)
{
    for (int i = 0; i < 8; i++) {
        if (refresh_mask & (1 << i)) {
            decoder->ref_frames[i] = output_surface;
            decoder->ref_valid[i] = 1;
            decoder->ref_width[i] = frame_width;
            decoder->ref_height[i] = frame_height;
        }
    }
    if (refresh_mask)
        printf("va_vp9_update_reference_frames: refresh_mask=0x%02x surface=%u %dx%d\n",
               refresh_mask, (unsigned)output_surface, frame_width, frame_height);
}

static void va_vp9_sync_reference_surfaces(VP9Decoder *decoder)
{
    VASurfaceID seen[8];
    int n = 0;

    for (int i = 0; i < 8; i++) {
        if (!decoder->ref_valid[i] || decoder->ref_frames[i] == VA_INVALID_ID)
            continue;
        VASurfaceID ref_surface_id = decoder->ref_frames[i];
        int dup = 0;
        for (int j = 0; j < n; j++) {
            if (seen[j] == ref_surface_id) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;
        seen[n++] = ref_surface_id;
        vaSyncSurface(decoder->va_dpy, ref_surface_id);
    }
    if (n > 0)
        printf("va_vp9_sync_reference_surfaces: synced %d unique reference surface(s)\n", n);
}

static int va_vp9_decode_picture(VP9Decoder *decoder, const uint8_t *frame_data, int frame_size,
                                 FILE *output_file, int frame_index)
{
    VAStatus va_status;
    VADecPictureParameterBufferVP9 picture_param;
    VASliceParameterBufferVP9 slice_param;
    int show_existing_idx = -1;
    uint8_t refresh_mask = 0;

    printf("va_vp9_decode_picture: frame_index=%d frame_size=%d\n", frame_index, frame_size);

    int parse_hdr_result =
        va_vp9_parse_picture_header(decoder, frame_data, frame_size, &picture_param, &slice_param,
                                    &show_existing_idx, &refresh_mask);
    if (parse_hdr_result == 2) {
        if (show_existing_idx < 0 || show_existing_idx >= 8 || !decoder->ref_valid[show_existing_idx]) {
            fprintf(stderr, "vp9: show_existing_frame idx=%d missing reference\n", show_existing_idx);
            return -1;
        }
        VASurfaceID ref_surface = decoder->ref_frames[show_existing_idx];
        vaSyncSurface(decoder->va_dpy, ref_surface);
        if (output_file) {
            CropInfo crop = {0, 0, 0, 0};
            va_status = save_vaapi_surface(decoder->va_dpy, ref_surface, crop, output_file,
                                           decoder->output_bit_depth);
            CHECK_VASTATUS(va_status, "save_vaapi_surface show_existing");
        }
        printf("frame %d show_existing_frame idx=%d surface=%u %dx%d\n", frame_index, show_existing_idx,
               (unsigned)ref_surface, decoder->ref_width[show_existing_idx],
               decoder->ref_height[show_existing_idx]);
        return 0;
    }
    if (parse_hdr_result < 0)
        return -1;

    if (va_init(decoder, picture_param.frame_width, picture_param.frame_height) != 0)
        return -1;

    va_vp9_sync_reference_surfaces(decoder);

    VASurfaceID surface_id = vp9_pick_decode_surface_id(decoder);
    printf("va_vp9_decode_picture: surface_id=%u context_id=%u\n", (unsigned)surface_id,
           (unsigned)decoder->context_id);

    VABufferID pic_buf = VA_INVALID_ID, slice_buf = VA_INVALID_ID, data_buf = VA_INVALID_ID;

    va_status = vaCreateBuffer(decoder->va_dpy, decoder->context_id, VAPictureParameterBufferType,
                               sizeof(picture_param), 1, &picture_param, &pic_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer pic");
    va_status = vaCreateBuffer(decoder->va_dpy, decoder->context_id, VASliceParameterBufferType,
                               sizeof(slice_param), 1, &slice_param, &slice_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer slice");
    va_status = vaCreateBuffer(decoder->va_dpy, decoder->context_id, VASliceDataBufferType, frame_size, 1,
                               (void *)frame_data, &data_buf);
    CHECK_VASTATUS(va_status, "vaCreateBuffer data");

    va_status = vaBeginPicture(decoder->va_dpy, decoder->context_id, surface_id);
    CHECK_VASTATUS(va_status, "vaBeginPicture");
    va_status = vaRenderPicture(decoder->va_dpy, decoder->context_id, &pic_buf, 1);
    CHECK_VASTATUS(va_status, "vaRenderPicture pic");
    va_status = vaRenderPicture(decoder->va_dpy, decoder->context_id, &slice_buf, 1);
    CHECK_VASTATUS(va_status, "vaRenderPicture slice");
    va_status = vaRenderPicture(decoder->va_dpy, decoder->context_id, &data_buf, 1);
    CHECK_VASTATUS(va_status, "vaRenderPicture data");
    va_status = vaEndPicture(decoder->va_dpy, decoder->context_id);
    CHECK_VASTATUS(va_status, "vaEndPicture");

    va_status = vaSyncSurface(decoder->va_dpy, surface_id);
    CHECK_VASTATUS(va_status, "vaSyncSurface");
    printf("va_vp9_decode_picture: vaSyncSurface done surface_id=%u\n", (unsigned)surface_id);

    va_vp9_update_reference_frames(decoder, refresh_mask, surface_id, picture_param.frame_width,
                                    picture_param.frame_height);

    if (output_file && picture_param.pic_fields.bits.show_frame) {
        CropInfo crop = {0, 0, 0, 0};
        va_status =
            save_vaapi_surface(decoder->va_dpy, surface_id, crop, output_file, decoder->output_bit_depth);
        CHECK_VASTATUS(va_status, "save_vaapi_surface");
    }

    printf("frame %d decoded %ux%u\n", frame_index, picture_param.frame_width, picture_param.frame_height);

    vaDestroyBuffer(decoder->va_dpy, pic_buf);
    vaDestroyBuffer(decoder->va_dpy, slice_buf);
    vaDestroyBuffer(decoder->va_dpy, data_buf);

    return 0;
}

static void vp9_close_decode_session(VP9Decoder *decoder, FILE *fp, FILE *out_file)
{
    printf("vp9_close_decode_session: teardown\n");
    if (decoder && decoder->va_dpy) {
        va_release(decoder);
        vaTerminate(decoder->va_dpy);
        va_close_display(decoder->va_dpy);
        decoder->va_dpy = nullptr;
    }
    if (fp)
        fclose(fp);
    if (out_file)
        fclose(out_file);
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *output_path = NULL;
    FILE *input_file = NULL, *output_file = NULL;
    uint8_t ivf_file_header[32];
    VP9Decoder decoder;
    int major, minor;
    int frame_index = 0;
    VAStatus va_status;

    memset(&decoder, 0, sizeof(decoder));
    decoder.output_bit_depth = 8;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Usage: %s [-b 8|10] <file.ivf> [output_file]\n", argv[0]);
                return 1;
            }
            decoder.output_bit_depth = atoi(argv[i]);
            if (decoder.output_bit_depth != 8 && decoder.output_bit_depth != 10) {
                fprintf(stderr, "-b must be 8 or 10\n");
                return 1;
            }
        } else if (!input_path) {
            input_path = argv[i];
        } else if (!output_path) {
            output_path = argv[i];
        } else {
            fprintf(stderr, "Usage: %s [-b 8|10] <file.ivf> [output_file]\n", argv[0]);
            return 1;
        }
    }
    if (!input_path) {
        fprintf(stderr, "Usage: %s [-b 8|10] <file.ivf> [output_file]\n", argv[0]);
        return 1;
    }

    printf("main: input=%s output=%s output_bit_depth=%d (-b)\n", input_path,
           output_path ? output_path : "(none)", decoder.output_bit_depth);

    input_file = fopen(input_path, "rb");
    if (!input_file) {
        perror(input_path);
        return 1;
    }
    if (output_path) {
        output_file = fopen(output_path, "wb");
        if (!output_file) {
            perror(output_path);
            fclose(input_file);
            return 1;
        }
    }

    if (fread(ivf_file_header, 1, 32, input_file) != 32 || memcmp(ivf_file_header, "DKIF", 4) != 0) {
        fprintf(stderr, "not an IVF file (expected DKIF)\n");
        fclose(input_file);
        if (output_file)
            fclose(output_file);
        return 1;
    }
    if (memcmp(ivf_file_header + 8, "VP90", 4) != 0 && memcmp(ivf_file_header + 8, "VP9 ", 4) != 0) {
        fprintf(stderr, "ivf: codec fourcc is not VP9 (got %.4s)\n", ivf_file_header + 8);
        fclose(input_file);
        if (output_file)
            fclose(output_file);
        return 1;
    }
    printf("main: IVF header ok (VP9), entering decode loop\n");

    decoder.va_dpy = va_open_display();
    if (!decoder.va_dpy) {
        fprintf(stderr, "va_open_display failed\n");
        fclose(input_file);
        if (output_file)
            fclose(output_file);
        return 1;
    }
    va_status = vaInitialize(decoder.va_dpy, &major, &minor);
    CHECK_VASTATUS(va_status, "vaInitialize");
    printf("VAAPI %d.%d\n", major, minor);

    for (;;) {
        uint8_t ivf_frame_header[12];
        if (fread(ivf_frame_header, 1, 12, input_file) != 12)
            break;
        uint32_t ivf_payload_size = read_le32(ivf_frame_header);
        (void)read_le64(ivf_frame_header + 4);
        if (ivf_payload_size == 0 || ivf_payload_size > 256 * 1024 * 1024) {
            fprintf(stderr, "ivf: bad frame size %u\n", ivf_payload_size);
            break;
        }
        uint8_t *packet_buffer = (uint8_t *)malloc(ivf_payload_size);
        if (!packet_buffer) {
            fprintf(stderr, "oom\n");
            break;
        }
        if (fread(packet_buffer, 1, ivf_payload_size, input_file) != ivf_payload_size) {
            free(packet_buffer);
            break;
        }

        int vp9_layer_sizes[8];
        int vp9_frames_in_packet = get_vp9_superframe_count(packet_buffer, ivf_payload_size, vp9_layer_sizes);
        int layer_offset[8];
        layer_offset[0] = 0;
        for (int i = 1; i < vp9_frames_in_packet; i++)
            layer_offset[i] = layer_offset[i - 1] + vp9_layer_sizes[i - 1];

        printf("main: IVF packet bytes=%u contains %d VP9 frame(s)\n", ivf_payload_size, vp9_frames_in_packet);

        for (int layer_idx = 0; layer_idx < vp9_frames_in_packet; layer_idx++) {
            const uint8_t *layer_data = packet_buffer + layer_offset[layer_idx];
            int layer_size = vp9_layer_sizes[layer_idx];
            if (va_vp9_decode_picture(&decoder, layer_data, layer_size, output_file, frame_index) != 0) {
                fprintf(stderr, "decode failed at ivf frame index %d (packet had %d VP9 frame%s)\n",
                        frame_index, vp9_frames_in_packet, vp9_frames_in_packet > 1 ? "s" : "");
                free(packet_buffer);
                vp9_close_decode_session(&decoder, input_file, output_file);
                return 1;
            }
            frame_index++;
        }
        free(packet_buffer);
    }

    printf("main: decode loop finished, total_vp9_frames=%d\n", frame_index);
    vp9_close_decode_session(&decoder, input_file, output_file);
    return 0;
}
