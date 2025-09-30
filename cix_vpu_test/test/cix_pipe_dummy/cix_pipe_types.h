/*
 * Copyright (c) 2024 Cix Technology Group Co., Ltd.. All rights reserved.
 * License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef _CIX_PIPE_TYPES_H_
#define _CIX_PIPE_TYPES_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "cix_pipe.h"

/* When set CpipeMsg.msg to this exit code, it means to close the connection */
#define CPIPE_EXIT_CODE -1

/* image/video color format */
typedef enum _CpipeColorFormat
{
    CPIPE_COLOR_FormatUnused,
    CPIPE_COLOR_FormatMonochrome,
    CPIPE_COLOR_FormatI420,
    CPIPE_COLOR_FormatNV12,
    CPIPE_COLOR_FormatP010,
    CPIPE_COLOR_FormatYUY2,
    CPIPE_COLOR_FormatUYVY,
    CPIPE_COLOR_Format32bitBGRA8888,
    CPIPE_COLOR_Format32bitARGB8888,
    CPIPE_COLOR_Format24BitRGB888,
    CPIPE_COLOR_Format24BitRGB888Planar,
    CPIPE_COLOR_Format24BitBGR888,
    CPIPE_COLOR_Format24BitBGR888Planar,
    CPIPE_COLOR_FormatMax
} CpipeColorFormat;

/* Data descriptor type */
typedef enum _CpipeDescType
{
    CPIPE_TYPE_1D_BUFFER = 0,   /* CpipeBufferInfo */
    CPIPE_TYPE_2D_SURFACE,      /* CpipeSurfaceInfo */
    CPIPE_TYPE_MESSAGE,         /* CpipeMsg */
    CPIPE_TYPE_TIME,            /* CpipeTimeInfo */
    CPIPE_TYPE_MAX
} CpipeDescType;

/* time type */
typedef enum _CpipeTimeType
{
    CPIPE_TIME_START = 0,       /* start time */
    CPIPE_TIME_CALIBRATION,     /* time delta for calibration */
    CPIPE_TIME_MAX
} CpipeTimeType;

#pragma pack(push, 1)
/* 1D buffer */
typedef struct _CpipeBufferInfo
{
    CpipeDescHdr header;
    uint32_t offset;
    uint32_t size;
} CpipeBufferInfo;

/* 2D surface */
typedef struct _CpipeSurfaceInfo
{
    CpipeDescHdr header;
    CpipeColorFormat format;
    uint32_t non_cougiguous;
    uint32_t width;
    uint32_t height;
    uint32_t offset[CPIPE_MAX_PLANES];
    uint32_t stride[CPIPE_MAX_PLANES];
    uint32_t size[CPIPE_MAX_PLANES];
} CpipeSurfaceInfo;

/* Used to send message */
typedef struct _CpipeMsg
{
    CpipeDescHdr header;
    int32_t msg;            /* When greater than 0, it means the id of buffer which can be released */
                            /* When set to 0, it means a buffer can be released, but id is unknown */
                            /* When set to CPIPE_EXIT_CODE, it means to close connection */
                            /* Other values are reserved */
} CpipeMsg;

/* time information */
typedef struct _CpipeTimeInfo
{
    CpipeDescHdr header;
    CpipeTimeType type;
    uint64_t seconds;
    uint64_t useconds;
} CpipeTimeInfo;
#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* !_CIX_PIPE_TYPES_H_ */
