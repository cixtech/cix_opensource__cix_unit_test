/*
 * The confidential and proprietary information contained in this file may
 * only be used by a person authorised under and to the extent permitted
 * by a subsisting licensing agreement from Arm Technology (China) Co., Ltd.
 *
 *            (C) COPYRIGHT 2021-2021 Arm Technology (China) Co., Ltd.
 *                ALL RIGHTS RESERVED
 *
 * This entire notice must be reproduced on all copies of this file
 * and copies of this file may only be made by a person if such person is
 * permitted to do so under the terms of a subsisting license agreement
 * from Arm Technology (China) Co., Ltd.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <unistd.h>
#include <semaphore.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <arpa/inet.h>

#include <linux/videodev2.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#include "mvx_player.hpp"
#include "drm/drm_fourcc.h"

using namespace std;

/****************************************************************************
 * Defines
 ****************************************************************************/
// Define bit field extract and test macros
// SBFX = signed bitfield extract
//  BFS = bit field set
#define SBFX(Rn,lsb,width)   ((int32_t)((Rn)<<(32-(lsb)-(width)))>>(32-(width)))
#define BFS(Rn,lsb,width)    (((Rn)&((1u<<(width))-1))<<(lsb))
#define UBFX(Rn,lsb,width)   ((uint32_t)((Rn)<<(32-(lsb)-(width)))>>(32-(width)))

// Extract macros as above but with width macro automatically generated
#define GET(lsb,Rn)          UBFX(Rn,lsb,lsb ## _SZ)
#define SET(lsb,Rn)          BFS(Rn,lsb,lsb ## _SZ)
#define SGET(lsb,Rn)         SBFX(Rn,lsb,lsb ## _SZ)

#ifndef V4L2_BUF_FLAG_LAST
#define V4L2_BUF_FLAG_LAST 0x00100000
#endif

#define V4L2_ALLOCATE_BUFFER_ROI 1048576 * 3
#define V4L2_READ_LEN_BUFFER_ROI 1048576 * 2

#ifndef V4L2_EVENT_SOURCE_CHANGE
#define V4L2_EVENT_SOURCE_CHANGE 5
#endif

#ifndef EXT_MEM_DEV
#define EXT_MEM_DEV  "/dev/mem"
#endif

#define UEVENT_MSG_SIZE (64*1024)
#define MVX_FIRMWARE_PATH "/lib/firmware/"
#define SIZE_1M 0x100000
#define SIZE_4M 0x400000
#define SECURE_DECODING_PHASE_I_FW_BASE 0xbde00000
#define DMA_HEAP_SYSTEM             "/dev/dma_heap/system"
#define DMA_HEAP_CMA                "/dev/dma_heap/linux,cma"
#define DMA_HEAP_PRIVATE            "/dev/dma_heap/vpu_private"
#define DMA_HEAP_PROTECTED_BITS     "/dev/dma_heap/vpu_protected"
#define DMA_HEAP_PROTECTED_INTBUF   "/dev/dma_heap/vpu_protected"
#define DMA_HEAP_OUTBUF             "/dev/dma_heap/media_protected"

typedef reader::result result;
/****************************************************************************
 * Misc
 ****************************************************************************/

template<typename T, typename U>
static T divRoundUp(T value, U round)
{
    return (value + round - 1) / round;
}

template<typename T, typename U>
static T roundUp(T value, U round)
{
    return divRoundUp(value, round) * round;
}

static uint64_t getRawTick(void)
{
    struct timespec tv;
    uint64_t tm;

    clock_gettime(CLOCK_MONOTONIC_RAW, &tv);

    tm = (uint64_t)tv.tv_sec * (1000000000L) + tv.tv_nsec;

    return tm;
}

/****************************************************************************
 * Exception
 ****************************************************************************/

Exception::Exception(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
}

Exception::Exception(const string &str)
{
    strncpy(msg, str.c_str(), sizeof(msg));
}

Exception::~Exception() throw()
{}

const char *Exception::what() const throw()
{
    return msg;
}

/****************************************************************************
 * Input and output
 ****************************************************************************/

const uint32_t IVFHeader::signatureDKIF = v4l2_fourcc('D', 'K', 'I', 'F');
static std::mutex m_Mutex;
static sem_t m_sem_uevent_monitor;

//COnfiguration file maximum line lengths, for use by fgets()
//ROI requires 512
//ERP requires 10+1 + 7+1 + 11+1 + (22+1)*256 = 5919 (8k width)
#define CFG_FILE_LINE_SIZE (6144)
static char cfg_file_line_buf[CFG_FILE_LINE_SIZE];
//static int startcode_find_candidate(char *buf, int size);

#ifdef ENABLE_DISPLAY
static struct video_format pix_drm_formats[] = {
    {V4L2_PIX_FMT_YUV420,           DRM_FORMAT_YUV420,      1},
    {V4L2_PIX_FMT_NV12,             DRM_FORMAT_NV12,        1},
    {V4L2_PIX_FMT_NV12M,            DRM_FORMAT_NV12,        1},
    {V4L2_PIX_FMT_YUYV,             DRM_FORMAT_YUYV,        1},
    {V4L2_PIX_FMT_RGB24,            DRM_FORMAT_RGB888,      1},
    {V4L2_PIX_FMT_ABGR32,           DRM_FORMAT_ARGB8888,    1},
    {V4L2_PIX_FMT_YUV420_AFBC_8,    DRM_FORMAT_YUV420_8BIT, 1},
    {V4L2_PIX_FMT_YUV420_AFBC_10,   DRM_FORMAT_YUV420_10BIT, 1},
};

static uint32_t pix_to_drm_fmt(uint32_t fmt)
{
    uint32_t i;

    for (i = 0; i < ARRAY_SIZE(pix_drm_formats); i++) {
        if (!pix_drm_formats[i].enable)
            continue;
        if (pix_drm_formats[i].pix_fmt == fmt)
            return pix_drm_formats[i].drm_fmt;
    }

    return DRM_FORMAT_INVALID;
}
#endif

IVFHeader::IVFHeader() :
    signature(signatureDKIF),
    version(0),
    length(32),
    codec(0),
    width(0),
    height(0),
    frameRate(30 << 16),
    timeScale(1 << 16),
    frameCount(0),
    padding(0)
{}

IVFHeader::IVFHeader(uint32_t codec, uint16_t width, uint16_t height, uint32_t fps_n, uint32_t fps_d, uint32_t frameCount) :
    signature(signatureDKIF),
    version(0),
    length(32),
    codec(codec),
    width(width),
    height(height),
    frameRate(fps_n << 16),
    timeScale(fps_d << 16),
    frameCount(frameCount),
    padding(0)
{}

IVFFrame::IVFFrame() :
    size(0),
    timestamp(0)
{}

IVFFrame::IVFFrame(uint32_t size, uint64_t timestamp) :
    size(size),
    timestamp(timestamp)
{}

const uint8_t VC1SequenceLayerData::magic1 = 0xC5;
const uint32_t VC1SequenceLayerData::magic2 = 4;

VC1SequenceLayerData::VC1SequenceLayerData() :
    numFrames(0),
    signature1(0),
    signature2(0),
    headerC(),
    restOfSLD()
{}

VC1FrameLayerData::VC1FrameLayerData() :
    frameSize(0),
    reserved(0),
    key(0),
    timestamp(0)
{}

AFBCHeader::AFBCHeader()
{}

AFBCHeader::AFBCHeader(const v4l2_format &format, size_t frameSize, const v4l2_rect &crop, bool tiled, const int field) :
    magic(MAGIC),
    headerSize(sizeof(AFBCHeader)),
    version(VERSION),
    frameSize(frameSize),
    numComponents(0),
    subsampling(0),
    yuvTransform(false),
    blockSplit(false),
    yBits(0),
    cbBits(0),
    crBits(0),
    alphaBits(0),
    mbWidth(0),
    mbHeight(0),
    width(0),
    height(0),
    cropLeft(crop.left),
    cropTop(crop.top),
    param(0),
    fileMessage(field)
{
    uint32_t pixelformat;
    uint32_t formatWidth = 0;

    if (V4L2_TYPE_IS_MULTIPLANAR(format.type))
    {
        const v4l2_pix_format_mplane &f = format.fmt.pix_mp;
        pixelformat = f.pixelformat;
        formatWidth = f.width;
    }
    else
    {
        const v4l2_pix_format &f = format.fmt.pix;
        pixelformat = f.pixelformat;
        formatWidth = f.width;
    }
    width = crop.width;
    height = crop.height;

    if (field != FIELD_NONE)
    {
        height = divRoundUp(height, 2);
        cropTop =divRoundUp(cropTop, 2);
    }

    mbWidth = (formatWidth + 15) / 16;
    mbHeight = (height + cropTop + 15) / 16;

    switch (pixelformat)
    {
        case V4L2_PIX_FMT_YUV420_AFBC_8:
            numComponents = 3;
            subsampling = 1;
            yBits = 8;
            crBits = 8;
            cbBits = 8;
            alphaBits = 0;
            break;
        case V4L2_PIX_FMT_YUV420_AFBC_10:
            numComponents = 3;
            subsampling = 1;
            yBits = 10;
            crBits = 10;
            cbBits = 10;
            alphaBits = 0;
            break;
        case V4L2_PIX_FMT_YUV422_AFBC_8:
            numComponents = 3;
            subsampling = 2;
            yBits = 8;
            crBits = 8;
            cbBits = 8;
            alphaBits = 0;
            break;
        case V4L2_PIX_FMT_YUV422_AFBC_10:
            numComponents = 3;
            subsampling = 2;
            yBits = 10;
            crBits = 10;
            cbBits = 10;
            alphaBits = 0;
            break;
        case V4L2_PIX_FMT_Y_AFBC_8:
            numComponents = 1;
            subsampling = 0;
            yBits = 8;
            crBits = 8;
            cbBits = 8;
            alphaBits = 0;
            break;
        case V4L2_PIX_FMT_Y_AFBC_10:
            numComponents = 1;
            subsampling = 0;
            yBits = 10;
            crBits = 10;
            cbBits = 10;
            alphaBits = 0;
            break;
        default:
            throw Exception("Unsupported AFBC pixel format. pixelformat=0x%x.", pixelformat);
    }

    if (tiled)
    {
        param |= (PARAM_TILED_BODY | PARAM_TILED_HEADER);
    }
}

void AFBCHeader::setSize(size_t _width, size_t _height)
{
    width = _width;
    height = _height;
    if (fileMessage != FIELD_NONE)
    {
        height = divRoundUp(height, 2);
    }
    mbWidth = (width + cropLeft + 15) / 16;
    mbHeight = (height + cropTop + 15) / 16;
}

IO::IO(uint32_t format, size_t width, size_t height, size_t strideAlign) :
    format(format),
    width(width),
    height(height),
    strideAlign(strideAlign),
    securevideo(false),
    convert10bit(false),
    timestamp(0),
    base({0, 0}),
    count(0)
{}

void IO::controlFrameRate(unsigned int fps_n, unsigned int fps_d)
{
    if (fps_n == 0)
        return;

   if (!timerisset(&base))
    {
        /* Skip the 1st frame's control, just record time base */
        gettimeofday(&base, NULL);
        count++;
        return;
    }

    struct timeval next, now, d, t;
    d.tv_sec = ((time_t)count * fps_d / fps_n);
    d.tv_usec = (((time_t)count * fps_d % fps_n) * 1000000 / fps_n);
    timeradd(&base, &d, &next);

    gettimeofday(&now, NULL);
    if (timercmp(&now, &next, <))
    {
        timersub(&next, &now, &t);
        usleep(t.tv_sec * 1000000 + t.tv_usec);
    }
    else
    {
        timersub(&now, &next, &t);
        if (dir == 0)
            fprintf(stderr, "WARNING: Skip frame %d... input IO is %ld.%06ld ms late.\n",
                count, t.tv_sec + (t.tv_usec/1000), t.tv_usec % 1000);
        else
            fprintf(stderr, "WARNING: Skip frame %d... output IO is %ld.%06ld ms late.\n",
                count, t.tv_sec + (t.tv_usec/1000), t.tv_usec % 1000);
    }

    count++;
}

Input::Input(uint32_t format, size_t width, size_t height, size_t strideAlign) :
    IO(format, width, height, strideAlign)
{
    if (format == V4L2_PIX_FMT_VC1_ANNEX_G)
    {
        profile = 12;
    }
    dir = 0;
}

void Input::setFrameRate(unsigned int fps_n, unsigned int fps_d)
{
    if (fps_d > 0)
    {
        this->fps_n = fps_n;
        this->fps_d = fps_d;
    }
}

void Input::controlFrameRate()
{
    IO::controlFrameRate(fps_n, fps_d);
}

InputFile::InputFile(istream &input, uint32_t format) :
    Input(format),
    input(input)
{
    state = 7;
    offset=0;
    curlen=0;
    iseof = false;
    naluFmt = 0;
    remaining_bytes = 0;
    cached_frames = 0;
    skipRead = false;
    fileSize = 1;
    fileFrameNum = 1;
#ifdef ENABLE_CPIPE
    cpipe = NULL;
#endif
}

InputFile::InputFile(istream &input, uint32_t format, size_t width, size_t height, size_t strideAlign) :
    Input(format, width, height, strideAlign),
    input(input)
{
    state = 7;
    offset=0;
    curlen=0;
    iseof = false;
    naluFmt = 0;
    remaining_bytes = 0;
#ifdef ENABLE_CPIPE
    cpipe = NULL;
#endif
    rewind = false;
    cached_frames = 0;
    skipRead = false;
    input.seekg(0, std::ios_base::end);
    fileSize = input.tellg();
    input.seekg(0, std::ios_base::beg);
}

InputFile::~InputFile()
{
    if (inputBuf) {
        free(inputBuf);
    }
    if (reader) {
        delete reader;
    }
    if (encrypted_buffer) {
        munmap(encrypted_buffer, encrypted_buffer_len);
        close(encrypted_buffer_fd);
    }
}

void InputFile::prepare(Buffer &buf)
{
    vector<iovec> iov = buf.getImageSize();
    buf.getBuffer().flags = 0;
    if (getNaluFormat() != V4L2_OPT_NALU_FORMAT_START_CODES) {
        reader::result res;
        uint8_t *buffer_data = static_cast<uint8_t *>(iov[0].iov_base);
        uint32_t buffer_size = buf.getBuffer().length;
        uint32_t buf_offset = 0;
        int mode = 0, nalu_val = 0;
        {
            m_Mutex.lock();
            if (!reader) {
                reader = new start_code_reader(getFormat(), &input);
                switch(getNaluFormat()){
                    case V4L2_OPT_NALU_FORMAT_ONE_BYTE_LENGTH_FIELD:
                    case V4L2_OPT_NALU_FORMAT_TWO_BYTE_LENGTH_FIELD:
                    case V4L2_OPT_NALU_FORMAT_FOUR_BYTE_LENGTH_FIELD:
                        mode |= reader::RMODE_DELIMITED;
                        if (V4L2_OPT_NALU_FORMAT_ONE_BYTE_LENGTH_FIELD == getNaluFormat()) {
                            nalu_val = 1;
                        } else if (V4L2_OPT_NALU_FORMAT_TWO_BYTE_LENGTH_FIELD == getNaluFormat()) {
                            nalu_val = 2;
                        } else {
                            nalu_val = 4;
                        }
                        reader->set_metadata(reader::RMETA_DELIMITER_LENGTH, nalu_val);
                        break;
                    case V4L2_OPT_NALU_FORMAT_ONE_NALU_PER_BUFFER:
                        mode |= reader::RMODE_PACKETED;
                        send_end_of_subframe_flag = true;
                        send_end_of_frame_flag = true;
                        break;
                    case V4L2_OPT_NALU_FORMAT_ONE_FRAME_PER_BUFFER:
                        send_end_of_frame_flag = true;
                        break;
                }
                if (send_end_of_subframe_flag || send_end_of_frame_flag) {
                    mode |= reader::RMODE_FLAGS;
                }
                input.seekg(0, ios::end);
                reader->setFileLength((uint64_t)input.tellg());
                input.seekg(0,ios::beg);
                reader->set_mode(mode);
            }
            m_Mutex.unlock();
        }
        do {
            uint32_t filled_size;
            res = reader->read(0, buffer_data+buf_offset, buffer_size-buf_offset, &filled_size);
            buf_offset += filled_size;

            if(res == reader::RR_EOS)
            {
                break;
            }
            if(send_end_of_frame_flag && (res==reader::RR_EOP_FRAME || res==reader::RR_EOP_CODEC_CONFIG))
            {
                break;
            }
            bool eosf = res==reader::RR_EOP || res==reader::RR_EOP_FRAME || res==reader::RR_EOP_CODEC_CONFIG;
            if(send_end_of_subframe_flag && eosf)
            {
                break;
            }
        } while (buf_offset < buffer_size);
        if(res==reader::RR_EOP_CODEC_CONFIG)
        {
            buf.setCodecConfig( true );
        }
        if(res==reader::RR_EOP_FRAME && send_end_of_frame_flag)
        {
            buf.setEndOfFrame( true );
        }
        if((res==reader::RR_EOP || res == reader::RR_EOP_FRAME || res==reader::RR_EOP_CODEC_CONFIG) && send_end_of_subframe_flag)
        {
            buf.setEndOfSubFrame( true );
        }

        iov[0].iov_len = buf_offset;
        buf.setBytesUsed(iov);
    } else if (!securevideo) {
        if (frame_bytes.size() && frame_bytes.front()) {
            vector<iovec> iov_per_frame;
            iovec tmp_iovec = { .iov_base = iov[0].iov_base, .iov_len = frame_bytes.front() };
            input.read(static_cast<char *>(tmp_iovec.iov_base), tmp_iovec.iov_len);
            iov_per_frame.push_back(tmp_iovec);
            buf.setBytesUsed(iov_per_frame);
            frame_bytes.pop();
            if (frame_bytes.size() == 0) {
                iseof = true;
            }
        } else {
            for (size_t i = 0; i < iov.size(); ++i)
            {
                input.read(static_cast<char *>(iov[i].iov_base), iov[i].iov_len);
                if (((unsigned int)input.gcount() < iov[i].iov_len) && !rewind) {
                    iseof = true;

                }
                iov[i].iov_len = input.gcount();
            }
            buf.setBytesUsed(iov);
        }
    } else {
#ifdef ENABLE_DRM
        size_t read_len = min(encrypted_buffer_len, iov[0].iov_len);
        input.read(encrypted_buffer, read_len);
        read_len = input.gcount();
        struct decrypt_data_params stream_desc;
        stream_desc.enc_fd = encrypted_buffer_fd;
        stream_desc.enc_len = read_len;
        stream_desc.dec_fd = buf.getDmaFd(0);
        stream_desc.dec_len = iov[0].iov_len;
        int ret = cix_secvid_send_command(secvid_session, CIX_CA_CMD_SECVID_DECRYPT_DATA, &stream_desc);
        if (ret != OK) {
            throw Exception("Decrypt bitstream failed: %d.", ret);
        }
        iov[0].iov_len = stream_desc.dec_used;
        buf.setBytesUsed(iov);
        printf("Decrypt bitstream, enc_fd=%d, enc_len=%ld, dec_fd=%d, dec_len=%ld, dec_used=%ld\n",
                stream_desc.enc_fd, stream_desc.enc_len,
                stream_desc.dec_fd, stream_desc.dec_len,
                stream_desc.dec_used);
#else
        /*
         * In secure video case, should communicate with TA in OP-TEE to
         * load input bitstream. Before OP-TEE is up, just set bytesused
         * to buffer size for test with NULL HW driver.
         */
        buf.setBytesUsed(iov);
        iseof = true;
#endif
    }

    if (rewind && input.peek() == EOF) {
        cout << "reached the end of file, rewind to the beginning." << endl;
        input.clear();
        input.seekg(sizeof(0));
    }
}

void InputFile::finalize(Buffer &buf)
{
    if (isExtInput())
        returnExtBuffer(buf);
}

/*int startcode_find_candidate(char *buf, int size){
    int i = 0;
    for (; i < size; i++){
        if (!buf[i])
            break;
    }
    return i;
}*/
bool InputFile::eof()
{
    //return input.peek() == EOF;
    return iseof;
}

void InputFile::setSecureVideo(void *data)
{
    securevideo = true;

#ifdef ENABLE_DRM
    encrypted_buffer_len = SIZE_4M;
    secvid_session = static_cast<struct cix_secvid_session*>(data);

    int fd = open(DMA_HEAP_SYSTEM, O_RDWR);
    if (fd < 0)
    {
        throw Exception("Failed to open ion device.");
    }

    struct dma_heap_allocation_data heap_data {
        .len = encrypted_buffer_len,
        .fd_flags = O_RDWR | O_CLOEXEC,
    };
    if (ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &heap_data) < 0)
    {
        throw Exception("DMA_HEAP_IOCTL_ALLOC failed. errno=%d (%s).", errno, strerror(errno));
    }

    close(fd);
    encrypted_buffer_fd = heap_data.fd;
    encrypted_buffer = (char*)mmap(NULL, encrypted_buffer_len,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                encrypted_buffer_fd, 0);
#endif
}

InputIVF::InputIVF(istream &input, uint32_t informat) :
    InputFile(input, 0)
{
    IVFHeader header;
    left_bytes = 0;
    input.read(reinterpret_cast<char *>(&header), sizeof(header));

    if (header.signature != IVFHeader::signatureDKIF)
    {
        char *c = reinterpret_cast<char *>(&header.signature);
        throw Exception("Incorrect DKIF signature. signature=%c%c%c%c.", c[0], c[1], c[2], c[3]);
    }

    if (header.version != 0)
    {
        throw Exception("Incorrect DKIF version. version=%u.", header.version);
    }

    if (header.length != 32)
    {
        throw Exception("Incorrect DKIF length. length=%u.", header.length);
    }
    if (informat != header.codec) {
        format = informat;
    } else {
        format = header.codec;
    }
    width = (size_t)header.width;
    height = (size_t)header.height;
}

int InputFile::seekToKeyFrame(int offset)
{
    if (getNaluFormat() != V4L2_OPT_NALU_FORMAT_START_CODES)
        return -1;
    if (format != V4L2_PIX_FMT_HEVC && format != V4L2_PIX_FMT_H264)
        return -1;

    input.clear();
    input.seekg(offset, ios::beg);
    /* search for IDR frame start code */
    int zero_count = 0;
    char byte = -1;
    int found_sc = 0;
    iseof = 0;
    do {
        input.read(&byte, 1);
        if (input.gcount() < 1) {
            iseof = 1;
            break;
        }
        if (found_sc == 1) {
            int hevc_nalu_type = (byte >> 1) & 0x1F;
            if (((format == V4L2_PIX_FMT_H264) && ((byte & 0x1F) == 5)) ||
                ((format == V4L2_PIX_FMT_HEVC) && (hevc_nalu_type == 19 || hevc_nalu_type == 20)))
                break;
            found_sc = 0;
            zero_count = 0;
        } else if (byte == 0)
            zero_count++;
        else if (byte == 1 && zero_count >= 2)
            found_sc = 1;
        else
            zero_count = 0;
    } while(1);

    if (!iseof && found_sc)
        input.seekg(-(zero_count+2), ios::cur);
    else {
        cerr << "WARNING: No key frame was found." << endl;
        return -1;
    }

    return 0;
}

void InputFile::getExtBuffer(Buffer &buf, vector<iovec> &iov)
{
#ifdef ENABLE_CPIPE
    CpipePacket recvpkt;
    memset(&recvpkt, 0, sizeof(CpipePacket));
    do
    {
        CpipeStatus sts = CPIPE_recv(cpipe, 0, &recvpkt);
        if (sts != CPIPE_STATUS_OK || recvpkt.num != 1)
            throw Exception("CPIPE_recv failed (%d) or # of received packet is not 1 (%d).", sts, recvpkt.num);

        CpipeDescHdr *hdr = (CpipeDescHdr *)(recvpkt.list[0]);
        if (hdr->type == CPIPE_TYPE_2D_SURFACE)
        {
            CpipeSurfaceInfo *pic = (CpipeSurfaceInfo *)(recvpkt.list[0]);
            size_t nplanes = buf.getNumPlanes();
            size_t nfds = 0;
            for (; nfds < CPIPE_MAX_PLANES && recvpkt.fds[nfds] != CPIPE_INVALID_FD; nfds++);
            /* TODO: add pixel format, resolution and size check */
            if (nfds != nplanes)
                throw Exception("Expect %d planes while got %d fds from remote.", nplanes, nfds);

            v4l2_buffer &b = buf.getBuffer();
            for (size_t i = 0; i < nplanes; ++i)
            {
                unsigned int length = !V4L2_TYPE_IS_MULTIPLANAR(b.type) ?
                                    b.length : b.m.planes[i].length;
                if (length > pic->size[i])
                    throw Exception("Buffer got from remote is not big enough (%d < %d).", pic->size[i], length);
                buf.setDmaFd(recvpkt.fds[i], i);
                buf.setLength(length, i);
                buf.dmaMemoryMap(recvpkt.fds[i], i);
                iov[i].iov_len = length;
            }
            buf.setExtInputId(pic->header.id);
            break;
        }
        else if (hdr->type == CPIPE_TYPE_MESSAGE)
        {
            CpipeMsg *msg = (CpipeMsg *)(recvpkt.list[0]);
            if (msg->msg == CPIPE_EXIT_CODE)
            {
                iseof = true;
                break;
            }
        }
    } while (1);
#endif
}

void InputFile::returnExtBuffer(Buffer &buf)
{
#ifdef ENABLE_CPIPE
    CpipePacket sendpkt;
    memset(&sendpkt, 0, sizeof(CpipePacket));
    CpipeMsg msg;
    size_t nplanes = buf.getNumPlanes();
    msg.header.type = CPIPE_TYPE_MESSAGE;
    msg.header.size = DATA_SIZE(CpipeMsg);
    msg.msg = buf.getExtInputId();
    sendpkt.list[0] = &msg;
    sendpkt.num = 1;
    sendpkt.fds[0] = CPIPE_INVALID_FD;
    if (CPIPE_send(cpipe, 0, &sendpkt) == CPIPE_STATUS_ERROR)
        throw Exception("Failed to return buffer %d to remote.", buf.getExtInputId());
    for (size_t i = 0; i < nplanes; ++i)
        close(buf.getDmaFd(i));
#endif
}

bool InputIVF::eof()
{
    return input.peek() == EOF;
}

void InputIVF::prepare(Buffer &buf)
{
    uint32_t cal_size = 0;
    if (left_bytes == 0) {
        IVFFrame frame;
        input.read(reinterpret_cast<char *>(&frame), sizeof(frame));
        cal_size = frame.size;
        timestamp = frame.timestamp;

    } else {
        cal_size = left_bytes;
    }

    vector<iovec> iov = buf.getImageSize();
    uint32_t read_bytes = cal_size > iov[0].iov_len? iov[0].iov_len:cal_size;
    input.read(static_cast<char *>(iov[0].iov_base), read_bytes);
    if (input.gcount() < read_bytes){
        cout<<"read less than need!."<<endl;
    }
    iov[0].iov_len = input.gcount();
    buf.setEndOfFrame(cal_size == iov[0].iov_len);
    left_bytes = cal_size - iov[0].iov_len;
    buf.setBytesUsed(iov);
    buf.setTimeStamp(timestamp);

    if (rewind && input.peek() == EOF) {
        cout << "reached the end of file, rewind to the beginning." << endl;
        input.clear();
        input.seekg(sizeof(IVFHeader));
    }
}

InputRCV::InputRCV(istream &input) :
    InputFile(input, 0)
{
    input.read(reinterpret_cast<char *>(&sld), sizeof(sld));

    if (sld.signature1 != VC1SequenceLayerData::magic1 ||
        sld.signature2 != VC1SequenceLayerData::magic2)
    {
        cout<<"This is a raw stream of VC1!"<<endl;
        format = V4L2_PIX_FMT_VC1_ANNEX_G;
        profile = 12;
        codecConfigSent = true;
        isRcv = false;
        input.clear();
        input.seekg(0);
    } else {

        /* headerC is serialized in bigendian byteorder. */
        uint32_t q = sld.headerC;
        q = ntohl(q);

        HeaderC hc = reinterpret_cast<HeaderC &>(q);
        profile = hc.profile;

        format = V4L2_PIX_FMT_VC1_ANNEX_L;

        codecConfigSent = false;
        isRcv = true;
    }
}

bool InputRCV::eof()
{
    return input.peek() == EOF;
}

void InputRCV::prepare(Buffer &buf)
{
    vector<iovec> iov = buf.getImageSize();
    if (!isRcv) {
        InputFile::prepare(buf);
        return;
    }
    if (codecConfigSent != false)
    {
        uint32_t cal_size = 0;
        uint32_t read_bytes = 0;
        char * read_addr;
        uint32_t offset = 0;
        VC1FrameLayerData *fld = static_cast<VC1FrameLayerData *>(iov[0].iov_base);
        if (left_bytes == 0) {
            input.read(reinterpret_cast<char *>(fld), sizeof(*fld));
            cal_size = fld->frameSize;
            read_addr = reinterpret_cast<char *>(fld->data);
            offset = sizeof(*fld);
        } else {
            cal_size = left_bytes;
            read_addr = static_cast<char *>(iov[0].iov_base);
        }
        read_bytes = iov[0].iov_len - offset > cal_size?cal_size:iov[0].iov_len - offset;
        input.read(read_addr, read_bytes);
        iov[0].iov_len = input.gcount() + offset;
        left_bytes = cal_size > input.gcount()? cal_size - input.gcount(): 0;
        buf.setEndOfFrame(left_bytes == 0);
    }
    else
    {
        memcpy(iov[0].iov_base, &sld, sizeof(sld));
        iov[0].iov_len = sizeof(sld);

        buf.setCodecConfig(true);
        codecConfigSent = true;
    }

    buf.setBytesUsed(iov);

    if (rewind && input.peek() == EOF) {
        cout << "reached the end of file, rewind to the beginning." << endl;
        input.clear();
        input.seekg(sizeof(sld));
    }
}

InputAFBC::InputAFBC(istream &input, uint32_t format, size_t width, size_t height) :
    InputFile(input, format, width, height, 1)
{
    prepared_frames = 0;
}

void InputAFBC::prepare(Buffer &buf)
{
    if (eof()) {
        return;
    }

    if (skipRead) {
        prepared_frames++;
        buf.setTimeStamp(prepared_frames);
        return;
    }

    vector<iovec> iov = buf.getImageSize();
    AFBCHeader header;

    input.read(reinterpret_cast<char *>(&header), sizeof(header));

	if (header.version > AFBCHeader::VERSION) //if (header.version != AFBCHeader::VERSION)
    {
        throw Exception("Incorrect AFBC header version. got=%u, exptected <=%d.", header.version, AFBCHeader::VERSION);
    }

    if (header.headerSize < sizeof(header))
    {
        throw Exception("AFBC header size too small. size=%u.", header.headerSize);
    }

    if (6 == header.version && 64 != header.headerSize)
    {
        throw Exception("Incorrect AFBC headerSize, got=%u, exptected 64 in header.version=%d.", header.headerSize, header.version);
    }

    if (header.frameSize > iov[0].iov_len)
    {
        throw Exception("AFBC buffer too small. header_size=%u, frame_size=%u, buffer_size=%zu.",
                        header.headerSize, header.frameSize, iov[0].iov_len);
    }

    size_t skip = header.headerSize - sizeof(header);
    if (skip > 0)
    {
        input.ignore(skip);
    }

    input.read(reinterpret_cast<char *>(iov[0].iov_base), header.frameSize);
    if (input.gcount() != header.frameSize)
    {
        throw Exception("Too few AFBC bytes read. expected=%u, got=%zu.",
                        header.frameSize, input.gcount());
    }

    iov[0].iov_len = header.frameSize;

    buf.setBytesUsed(iov);

    bool tiled = header.param & AFBCHeader::PARAM_TILED_BODY;
    buf.setTiled(tiled);

    bool superblock = 2 < header.subsampling;
    buf.setSuperblock(superblock);

    prepared_frames++;
    buf.setTimeStamp(prepared_frames);

    if (rewind && input.peek() == EOF) {
        cout << "reached the end of file, rewind to the beginning." << endl;
        input.clear();
        input.seekg(0);
    }
    if (cached_frames && prepared_frames == cached_frames) {
        skipRead = true;
    }
}

bool InputAFBC::eof()
{
    return input.peek() == EOF;
}


InputFileFrame::InputFileFrame(istream &input, uint32_t format, size_t width, size_t height,
                               size_t strideAlign, size_t __stride[]) :
    InputFile(input, format, width, height, strideAlign),
    nplanes(0)
{
    memcpy(stride, __stride, sizeof(size_t) * 3);
    framesize = Codec::getSize(format, width, height, strideAlign, nplanes, stride, size, heights);
    fileFrameNum = fileSize / framesize;
    prepared_frames = 0;
}

bool InputFileFrame::eof()
{
    return input.peek() == EOF || iseof;
}

void InputFileFrame::prepare(Buffer &buf)
{
    if (chr_list){
        if (0 == prepared_frames) {
            chr_cur = chr_list->begin();
        }
        if (chr_list->end() != chr_cur && chr_cur->pic_index == prepared_frames) {
            buf.setChrCfg(*chr_cur);
            buf.setChrflag(true);
            chr_cur++;
        } else {
            buf.setChrflag(false);
        }
    }
    if (gop_list && gop_list->size()){
        struct v4l2_gop_config cfg = gop_list->front();

        if (cfg.gop_pic == prepared_frames) {
            buf.setGopResetCfg(cfg);
            gop_list->pop();
        }
    }

    if (ltr_list && ltr_list->size()){
        struct v4l2_reset_ltr_period_config cfg = ltr_list->front();

        if (cfg.reset_trigger_pic == prepared_frames) {
            buf.setLtrResetCfg(cfg);
            ltr_list->pop();
        }
    }

    if (enc_stats_list && enc_stats_list->size()){
        struct v4l2_enc_stats_cfg stats_cfg = enc_stats_list->front();

        if (stats_cfg.reset_pic == prepared_frames) {
            buf.setResetStatsMode(stats_cfg.reset_pic, stats_cfg.reset_cfg);
            enc_stats_list->pop();
        } else {
            buf.setResetStatsMode(stats_cfg.reset_pic, 0);
        }
    } else {
            buf.setResetStatsMode(prepared_frames, 0);
    }
    vector<iovec> iov = buf.getImageSize();

    if (nplanes != iov.size())
    {
        throw Exception("Frame format and buffer have different number of planes. format=%zu, buffer=%zu.",
                        nplanes, iov.size());
    }

    if (isExtInput())
    {
        getExtBuffer(buf, iov);
    }
    else if (!skipRead)
    {
        for (size_t i = 0; i < nplanes; ++i)
        {
            if (size[i] > iov[i].iov_len)
            {
                throw Exception("Frame plane is larger than buffer plane. plane=%zu, plane_size=%zu, buffer_size=%zu.",
                                i, size[i], iov[0].iov_len);
            }

            input.read(static_cast<char *>(iov[i].iov_base), size[i]);
            unsigned int readbytes = (unsigned int)input.gcount();
            if (rewind && input.peek() == EOF) {
                cout << "reached the end of file, rewind to the beginning." << endl;
                input.clear();
                input.seekg(0);
            }
            if (readbytes < iov[i].iov_len) {
                iseof = true;
            }
            iov[i].iov_len = readbytes;
        }
    }
    prepared_frames++;
    buf.setBytesUsed(iov);
    if (cached_frames && prepared_frames == cached_frames) {
        skipRead = true;
    }
    buf.setTimeStamp(prepared_frames);
    Input::controlFrameRate();
}

InputFileMiniFrame::InputFileMiniFrame(std :: istream & input, uint32_t format, size_t width, size_t height,
            size_t strideAlign, uint32_t cnt, size_t __stride[]):
    InputFileFrame(input, format, width, height, strideAlign, __stride),
        cnt_of_miniframe(cnt)
{
    for (size_t i = 0; i < 3; ++i)
    { offset[i] = 0; is_done[i] = true;}
    miniframe_height = roundUp(divRoundUp(height, cnt_of_miniframe), 64);
    setReadHeight(miniframe_height);
}

void InputFileMiniFrame::prepare(Buffer &buf)
{
    vector<iovec> iov = buf.getImageSize();
    size_t readlen = 0;
    size_t readheight = 0;
    v4l2_buffer &v_buf = buf.getBuffer();
    v_buf.reserved2 |= V4L2_BUF_FLAG_MVX_MINIFRAME;//use this to indicate miniframe mode
    if (nplanes != iov.size())
    {
        throw Exception("Frame format and buffer have different number of planes. format=%zu, buffer=%zu.",
                        nplanes, iov.size());
    }
    for (size_t i = 0; i < nplanes; ++i)
    {
        readheight = i >= 1 ? miniframe_height / 2 : miniframe_height;
        if (offset[i] + readheight >= heights[i]) {
            readheight = heights[i] - offset[i];
        }
        readlen = stride[i] * readheight;
        input.seekg(count * framesize + offset[i] * stride[i]);
        if (i == 1) input.seekg(size[i - 1], ios::cur);
        if (i == 2) input.seekg(size[i - 1] + size[i - 2], ios::cur);
        v_buf.m.planes[i].reserved[10] = offset[i] << 16;  //it's better that don't use miniframe mode with epr together.
        offset[i] += readheight;
        v_buf.m.planes[i].reserved[10] |= offset[i];
        input.read(static_cast<char *>(iov[i].iov_base), readlen);
        cout<<"read bytes in plane:"<<i<<" all bytes:"<<readlen<<" offset:"<<offset[i]<<endl;
        iov[i].iov_len = input.gcount();
        if (offset[i] == heights[i]) {
            is_done[i] = true;
            offset[i] = 0;
        } else {
            is_done[i] = false;
        }
    }
    if (is_done[0] && is_done[1] && is_done[2]) {
        count++;
        cout<<"frame read done:"<<count<<endl;
    }
    buf.setBytesUsed(iov);
}

bool InputFileMiniFrame::eof()
{
    return input.peek() == EOF;
}


InputFileFrameWithROI::InputFileFrameWithROI(std :: istream & input, uint32_t format, size_t width,
                                size_t height, size_t strideAlign, std :: istream & roi, size_t __stride[]) :
        InputFileFrame(input, format, width, height, strideAlign, __stride),
        roi_is(roi)
{
    load_roi_cfg();
    prepared_frames = 0;
    cur = roi_list->begin();
}

InputFileFrameWithROI::~InputFileFrameWithROI()
{
    if (roi_list) {
        delete roi_list;
    }
}

void InputFileFrameWithROI::prepare(Buffer & buf)
{
    if (roi_list->end() != cur && cur->pic_index == prepared_frames) {
        buf.setRoiCfg(*cur);
        buf.setROIflag(true);
        cur++;
    } else {
        buf.setROIflag(false);
    }
    InputFileFrame::prepare(buf);
}

void InputFileFrameWithROI::load_roi_cfg()
{
    int pic_index, qp, num_roi, prio_valid = 0;
    struct v4l2_mvx_roi_regions roi;
    if (!roi_list) {
        roi_list = new v4l2_roi_list_t;
    }
    while (roi_is.getline(cfg_file_line_buf, CFG_FILE_LINE_SIZE)){
        memset(&roi, 0, sizeof(struct v4l2_mvx_roi_regions));
        if (4 == sscanf(cfg_file_line_buf, "pic=%d qp=%d num_roi=%d prio_valid=%d",&pic_index,&qp,&num_roi, &prio_valid)) {
            roi.pic_index = pic_index;
            roi.qp = qp;
            roi.num_roi = num_roi;
            roi.roi_present  = true;
            roi.qp_present = true;
        } else if (4 == sscanf(cfg_file_line_buf, "pic=%d num_roi=%d qp=%d prio_valid=%d",&pic_index,&num_roi,&qp, &prio_valid)) {
            roi.pic_index = pic_index;
            roi.qp = qp;
            roi.num_roi = num_roi;
            roi.roi_present  = true;
            roi.qp_present = true;
        } else if (2 == sscanf(cfg_file_line_buf, "pic=%d num_roi=%d qp=%d prio_valid=%d",&pic_index,&num_roi,&qp, &prio_valid)) {
            roi.pic_index = pic_index;
            roi.qp = 0;
            roi.num_roi = num_roi;
            roi.roi_present  = true;
            roi.qp_present = false;
        } else if (2 == sscanf(cfg_file_line_buf, "pic=%d qp=%d num_roi=%d prio_valid=%d",&pic_index,&qp,&num_roi, &prio_valid)) {
            roi.pic_index = pic_index;
            roi.qp = qp;
            roi.num_roi = 0;
            roi.roi_present  = false;
            roi.qp_present = true;
        } else {
            cout <<"parse ROI config file ERROR!"<<endl;
        }
        if (roi.num_roi > V4L2_MVX_MAX_FRAME_REGIONS) {
            cout <<"invalid n_regions value:"<<roi.num_roi<<endl;
        }
        if (roi.num_roi > 0){
            char *sub_buf1 = cfg_file_line_buf;
            char *sub_buf2;
            int match = 0;
            for (int i = 0; i < roi.num_roi; i++) {
                sub_buf2 = strstr(sub_buf1," roi=");
                match = sscanf(sub_buf2, " roi={%hu,%hu,%hu,%hu,%hd,%hu,%hu}",
                    &roi.roi[i].mbx_left, &roi.roi[i].mbx_right,
                    &roi.roi[i].mby_top, &roi.roi[i].mby_bottom,
                    &roi.roi[i].qp_delta, &roi.roi[i].prio, &roi.roi[i].force_intra);
                if (match != 7){
                    cout<<"Error while parsing the ROI regions:"<<match<<endl;
                }
                if (!prio_valid) {
                    roi.roi[i].prio = 0;
                }
                cout<<roi.roi[i].mbx_left<<","<<
                roi.roi[i].mbx_right<<","<<
                roi.roi[i].mby_top<<","<<
                roi.roi[i].mby_bottom<<","<<
                roi.roi[i].qp_delta<<","<<
                roi.roi[i].prio<<endl;
                sub_buf1 = sub_buf2 + 5;
            }
        }
        roi_list->push_back(roi);
    }
}

InputFileFrameWithEPR::InputFileFrameWithEPR(std :: istream & input, uint32_t format, size_t width,
                                size_t height, size_t strideAlign, std :: istream & epr, uint32_t oformat, size_t __stride[]) :
        InputFileFrame(input, format, width, height, strideAlign, __stride),
        epr_is(epr),
        outformat(oformat)
{
    prepared_frames = 0;
    load_epr_cfg();
    cur = epr_list->begin();
}

InputFileFrameWithEPR::~InputFileFrameWithEPR(){
    if (epr_list) {
        delete epr_list;
    }
}

void InputFileFrameWithEPR::erp_adjust_bpr_to_64_64(
                                    struct v4l2_buffer_general_rows_uncomp_body* uncomp_body,
                                    int qp_delta,
                                    uint32_t bpr_base_idx,
                                    uint32_t row_off,
                                    uint8_t force,
                                    uint8_t quad_skip,
                                    int local_base)
{
    if (bpr_base_idx < local_base + row_off) {
        uncomp_body->bpr[bpr_base_idx].qp_delta = (uint16_t)(SET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_TOP_LEFT_16X16, qp_delta)
                                                            | SET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_TOP_RIGHT_16X16, qp_delta)
                                                            | SET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_BOT_LEFT_16X16, qp_delta)
                                                            | SET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_BOT_RIGHT_16X16, qp_delta)
                                                            | SET(V4L2_BLOCK_PARAM_RECORD_QP_FORCE_FIELD, force)
                                                            | SET(V4L2_BLOCK_PARAM_RECORD_QP_QUAD_SKIP, quad_skip));
    }
    if (bpr_base_idx + 1 < local_base + row_off)
        uncomp_body->bpr[bpr_base_idx + 1] = uncomp_body->bpr[bpr_base_idx];
    if (bpr_base_idx + row_off < local_base + 2 * row_off)
        uncomp_body->bpr[bpr_base_idx + row_off] = uncomp_body->bpr[bpr_base_idx];
    if (bpr_base_idx + row_off + 1 < local_base + 2 * row_off)
        uncomp_body->bpr[bpr_base_idx + row_off + 1] = uncomp_body->bpr[bpr_base_idx];
    //uncomp_body->bpr[bpr_base_idx].force = force;
    //uncomp_body->bpr[bpr_base_idx + 1] = uncomp_body->bpr[bpr_base_idx];
    //uncomp_body->bpr[bpr_base_idx + 1].force = force;
    //uncomp_body->bpr[bpr_base_idx + row_off] = uncomp_body->bpr[bpr_base_idx];
    //uncomp_body->bpr[bpr_base_idx + row_off].force = force;
    //uncomp_body->bpr[bpr_base_idx + row_off + 1] = uncomp_body->bpr[bpr_base_idx];
    //uncomp_body->bpr[bpr_base_idx + row_off + 1].force = force;
}

void InputFileFrameWithEPR::prepare(Buffer & buf)
{
    /*if (prepared_frames % 2 == 0) {
        prepareEPR(buf);
    } else {
        InputFileFrame::prepare(buf);
    }
    prepared_frames++;*/
    if (epr_list->end() != cur && cur->pic_index == prepared_frames) {
        prepareEPR(buf);
        cur++;
    } else {
        InputFileFrame::prepare(buf);
    }
}

void InputFileFrameWithEPR::prepareEPR(Buffer & buf)
{
    vector<iovec> iov = buf.getImageSize();
    v4l2_epr_list_t::iterator iter= cur;
    v4l2_epr_list_t::iterator end= epr_list->end();
    /*for (; iter != end; iter++) {
        if (iter->pic_index == prepared_frames / 2) {
            break;
        }
    }*/
    if (cur != end) {
        if (iter->qp_present) {
            printf("set epr qp:%d, iframe_enabled:%d\n", iter->qp.qp, iter->qp.epr_iframe_enable);
            buf.setQPofEPR(iter->qp);
        }
        if (iter->block_configs_present) {
            struct v4l2_buffer_general_block_configs block_configs;
            struct v4l2_core_buffer_header_general buffer_configs;
            unsigned int blk_cfg_size = 0;
            if (iter->block_configs.blk_cfg_type == V4L2_BLOCK_CONFIGS_TYPE_ROW_UNCOMP) {
                blk_cfg_size += sizeof(block_configs.blk_cfgs.rows_uncomp);

                int max_cols = (getWidth()+31) >> 5;
                int max_rows = (getHeight()+31) >> 5;
                if (getRotation() % 360 == 90 || getRotation() % 360 == 270) {
                    swap(max_cols,max_rows);
                }
                block_configs.blk_cfgs.rows_uncomp.n_cols_minus1 = max_cols - 1;
                block_configs.blk_cfgs.rows_uncomp.n_rows_minus1 = max_rows - 1;
                size_t bpru_body_size;
                if (outformat == V4L2_PIX_FMT_HEVC) {
                    if (getRotation() % 360 == 90 || getRotation() % 360 == 270)
                    {
                        max_rows = (getWidth() + 127) >> 7;
                    }
                    else
                    {
                        max_rows = (getHeight() + 127) >> 7;
                    }
                    max_rows *= 4;
                    bpru_body_size = max_rows * max_cols * sizeof(v4l2_buffer_general_rows_uncomp_body);
                } else {
                    bpru_body_size = max_rows * max_cols * sizeof(v4l2_buffer_general_rows_uncomp_body);
                }
                struct v4l2_buffer_general_rows_uncomp_body *uncomp_body =
                        static_cast<struct v4l2_buffer_general_rows_uncomp_body*>(iov[0].iov_base);
                iov[0].iov_len = bpru_body_size;
                cout<<"plane 0 size:"<<bpru_body_size<<endl;
                int n_cols = iter->block_configs.blk_cfgs.rows_uncomp.n_cols_minus1 + 1;
                int n_rows = iter->block_configs.blk_cfgs.rows_uncomp.n_rows_minus1 + 1;
                for (int j = 0; j < n_rows; j++)
                {
                    for (int i = 0; i < n_cols; i++)
                    {
                        int idx = (j * n_cols) + i;
                        unsigned int quad_qp_delta = iter->bc_row_body.uncomp->bpr[idx].qp_delta;
                        printf("   bpr[%u]={%d,%d,%d,%d,0x%x,0x%x,0x%x}\n", idx,
                            (int)SGET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_TOP_LEFT_16X16,  quad_qp_delta),
                            (int)SGET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_TOP_RIGHT_16X16, quad_qp_delta),
                            (int)SGET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_BOT_LEFT_16X16,  quad_qp_delta),
                            (int)SGET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_BOT_RIGHT_16X16, quad_qp_delta),
                            (uint8_t)GET(V4L2_BLOCK_PARAM_RECORD_QP_FORCE_FIELD, quad_qp_delta),
                            (uint8_t)GET(V4L2_BLOCK_PARAM_RECORD_QP_QUAD_SKIP, quad_qp_delta),
                            (uint8_t)GET(V4L2_BLOCK_PARAM_RECORD_QP_ABSOLUTE, quad_qp_delta));
                    }
               }
                //qp-set quad granularity is 32*32, the minimum qp-set granularity
                //is 16*16. These keep the same with H264. Pls not the FS flag in
                //ME QUADCFG register.
                if (true) {
                    for (int row = 0; row < n_rows; row++) {
                        memcpy(&uncomp_body->bpr[row * max_cols],
                               &iter->bc_row_body.uncomp->bpr[row * n_cols],
                               n_cols * sizeof(uncomp_body->bpr[0]));
                        for (int col = n_cols; col < max_cols; col++) {
                            memcpy(&uncomp_body->bpr[(row * max_cols) + col],
                                   &uncomp_body->bpr[(row * max_cols) + n_cols - 1],
                                   sizeof(uncomp_body->bpr[0]));
                            // bpr quad skip flag cant not inherit
                            uncomp_body->bpr[(row * max_cols) + col].qp_delta &= ~((uint32_t)SET(V4L2_BLOCK_PARAM_RECORD_QP_QUAD_SKIP, 1));
                            uncomp_body->bpr[(row * max_cols) + col].qp_delta &= ~((uint32_t)SET(V4L2_BLOCK_PARAM_RECORD_QP_ABSOLUTE, 1));
                            uncomp_body->bpr[(row * max_cols) + col].qp_delta &= ~((uint32_t)SET(V4L2_BLOCK_PARAM_RECORD_QUAD_FORCE_INTRA, 1));
                        }
                    }
                    for (int row = n_rows; row < max_rows; row++) {
                        memcpy(&uncomp_body->bpr[row * max_cols],
                               &uncomp_body->bpr[(n_rows - 1) * max_cols],
                               max_cols * sizeof(uncomp_body->bpr[0]));
                        // bpr quad skip and absolute QP  flag cant not inherit
                        for(int tmpj = 0; tmpj < max_cols; tmpj++) {
                            uncomp_body->bpr[row * max_cols + tmpj].qp_delta &= ~((uint32_t)SET(V4L2_BLOCK_PARAM_RECORD_QP_QUAD_SKIP, 1));
                            uncomp_body->bpr[(row * max_cols) + tmpj].qp_delta &= ~((uint32_t)SET(V4L2_BLOCK_PARAM_RECORD_QP_ABSOLUTE, 1));
                            uncomp_body->bpr[(row * max_cols) + tmpj].qp_delta &= ~((uint32_t)SET(V4L2_BLOCK_PARAM_RECORD_QUAD_FORCE_INTRA, 1));
                        }

                    }
                } else {
                    for (int row = 0; row < n_rows; row++)
                    {
                        for (int k=0; k<n_cols; k++) {
                            unsigned int quad_qp_delta = iter->bc_row_body.uncomp->bpr[row * n_cols + k].qp_delta;
                            int top_left =  (int)SGET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_TOP_LEFT_16X16,  quad_qp_delta);
                            int top_right = (int)SGET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_TOP_RIGHT_16X16, quad_qp_delta);
                            int bot_left =  (int)SGET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_BOT_LEFT_16X16,  quad_qp_delta);
                            int bot_right = (int)SGET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_BOT_RIGHT_16X16, quad_qp_delta);
                            uint8_t force = (uint8_t)GET(V4L2_BLOCK_PARAM_RECORD_QP_FORCE_FIELD, quad_qp_delta);
                            uint8_t quad_skip = (uint8_t)GET(V4L2_BLOCK_PARAM_RECORD_QP_QUAD_SKIP, quad_qp_delta);
                            // top left 64*64
                            erp_adjust_bpr_to_64_64(uncomp_body, top_left, 4*row*max_cols + 4*k, max_cols, force, quad_skip, 4*row*max_cols);
                            // top right 64*64
                            erp_adjust_bpr_to_64_64(uncomp_body, top_right, 4*row*max_cols + 4*k + 2, max_cols, force, quad_skip, 4*row*max_cols);
                            // bottom left 64*64
                            erp_adjust_bpr_to_64_64(uncomp_body, bot_left, 4*row*max_cols + 2*max_cols + 4*k, max_cols, force, quad_skip, 4*row*max_cols + 2*max_cols);
                            // bottom right 64*64
                            erp_adjust_bpr_to_64_64(uncomp_body, bot_right, 4*row*max_cols + 2*max_cols + 4*k + 2, max_cols, force, quad_skip, 4*row*max_cols + 2*max_cols);
                        }
                        // extend the end of a row
                        for (int col = 4*n_cols; col < max_cols; col++)
                        {
                            memcpy(&uncomp_body->bpr[(4*row * max_cols) + col],
                                &uncomp_body->bpr[(4*row * max_cols) + 4*n_cols - 1],
                                sizeof(uncomp_body->bpr[0]));
                            memcpy(&uncomp_body->bpr[(4*row * max_cols + max_cols) + col],
                                &uncomp_body->bpr[(4*row * max_cols + max_cols) + 4*n_cols - 1],
                                sizeof(uncomp_body->bpr[0]));
                            memcpy(&uncomp_body->bpr[(4*row * max_cols + 2*max_cols) + col],
                                &uncomp_body->bpr[(4*row * max_cols + 2*max_cols) + 4*n_cols - 1],
                                sizeof(uncomp_body->bpr[0]));
                            memcpy(&uncomp_body->bpr[(4*row * max_cols + 3*max_cols) + col],
                                &uncomp_body->bpr[(4*row * max_cols + 3*max_cols) + 4*n_cols - 1],
                                sizeof(uncomp_body->bpr[0]));
                            // bpr quad skip flag cant not inherit
                            uncomp_body->bpr[(4*row * max_cols) + col].qp_delta &= ~((uint32_t)SET(V4L2_BLOCK_PARAM_RECORD_QP_QUAD_SKIP, 1));
                            uncomp_body->bpr[(4*row * max_cols + max_cols) + col].qp_delta &= ~((uint32_t)SET(V4L2_BLOCK_PARAM_RECORD_QP_QUAD_SKIP, 1));
                            uncomp_body->bpr[(4*row * max_cols + 2*max_cols) + col].qp_delta &= ~((uint32_t)SET(V4L2_BLOCK_PARAM_RECORD_QP_QUAD_SKIP, 1));
                            uncomp_body->bpr[(4*row * max_cols + 3*max_cols) + col].qp_delta &= ~((uint32_t)SET(V4L2_BLOCK_PARAM_RECORD_QP_QUAD_SKIP, 1));
                        }
                    }
                    for (int row = 4*n_rows; row < max_rows; row++) {
                        // extend the last row
                        memcpy(&uncomp_body->bpr[row * max_cols],
                               &uncomp_body->bpr[(row - 4) * max_cols],
                               max_cols * sizeof(uncomp_body->bpr[0]));
                        // bpr quad skip flag cant not inherit
                        for(int tmpj = 0; tmpj < max_cols; tmpj++) {
                            uncomp_body->bpr[row * max_cols + tmpj].qp_delta &= ~((uint32_t)SET(V4L2_BLOCK_PARAM_RECORD_QP_QUAD_SKIP, 1));
                        }
                    }
                }

                block_configs.blk_cfg_type = iter->block_configs.blk_cfg_type;
                blk_cfg_size += sizeof(block_configs.blk_cfg_type);
                blk_cfg_size += sizeof(block_configs.reserved);

                buffer_configs.type = V4L2_BUFFER_GENERAL_TYPE_BLOCK_CONFIGS;
                buffer_configs.buffer_size = bpru_body_size;
                buffer_configs.config_size = blk_cfg_size;
                memcpy(&buffer_configs.config.config, &block_configs, blk_cfg_size);
                memcpy(&buf.getBuffer().m.planes[0].reserved[0], &buffer_configs, sizeof(buffer_configs.buffer_size) +
                        sizeof(buffer_configs.type) + sizeof(buffer_configs.config_size) + blk_cfg_size);

            }
        }
    }
    for (uint32_t i = 1; i < iov.size(); i++) {
        iov[i].iov_len = 0;
    }
    buf.setBytesUsed(iov);
    buf.setEPRflag();
}

void InputFileFrameWithEPR::read_row_cfg(char * buf, int row, int len, epr_config & config)
{
    char *sub_buf1, *sub_buf2;
    int quad_qp_delta[4];
    uint8_t force_tmp, force_qual_skip, absolute_qp, force_intra;
    int quad_min_qp[4]={0};
    struct v4l2_block_param_record *bpr = NULL;
    int n_cols = config.block_configs.blk_cfgs.rows_uncomp.n_cols_minus1 + 1;
    if (row == 0)
    {
        config.block_configs.blk_cfgs.rows_uncomp.n_cols_minus1 = len - 1;
    }
    bpr = &config.bc_row_body.uncomp->bpr[row * n_cols];
    sub_buf1 = buf;

    for (int i = 0; i< len; i++) {
        sub_buf2 = strstr(sub_buf1, " bpr=");
        //assert(sub_buf2);
        if (!sub_buf2) {
           printf("Error : the real bpr number is less than num_bpr for picture =%d (%s))\n",
                        config.pic_index, cfg_file_line_buf);
           return;
        }
        if (12 == sscanf(sub_buf2, " bpr={%d,%d,%d,%d,%hhi,%hhi,%hhi,%hhi,%d,%d,%d,%d}",
                        &quad_qp_delta[0],
                        &quad_qp_delta[1],
                        &quad_qp_delta[2],
                        &quad_qp_delta[3],
                        &force_tmp,
                        &force_qual_skip,
                        &absolute_qp,
                        &force_intra,
                        &quad_min_qp[0],
                        &quad_min_qp[1],
                        &quad_min_qp[2],
                        &quad_min_qp[3])) {
        } else if(8 == sscanf(sub_buf2, " bpr={%d,%d,%d,%d,%hhi,%hhi,%hhi,%hhi}",
                        &quad_qp_delta[0],
                        &quad_qp_delta[1],
                        &quad_qp_delta[2],
                        &quad_qp_delta[3],
                        &force_tmp,
                        &force_qual_skip,
                        &absolute_qp,
                        &force_intra)){
        } else {
            printf("Error while parsing the row BPR[%d]\n", i);
        }
        assert(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_TOP_LEFT_16X16_SZ == V4L2_BLOCK_PARAM_RECORD_QP_DELTA_TOP_RIGHT_16X16_SZ &&
               V4L2_BLOCK_PARAM_RECORD_QP_DELTA_TOP_LEFT_16X16_SZ == V4L2_BLOCK_PARAM_RECORD_QP_DELTA_BOT_LEFT_16X16_SZ &&
               V4L2_BLOCK_PARAM_RECORD_QP_DELTA_TOP_LEFT_16X16_SZ == V4L2_BLOCK_PARAM_RECORD_QP_DELTA_BOT_RIGHT_16X16_SZ);

        const int qpd_min = -(1 << (V4L2_BLOCK_PARAM_RECORD_QP_DELTA_TOP_LEFT_16X16_SZ - 1));
        const int qpd_max =  (1 << (V4L2_BLOCK_PARAM_RECORD_QP_DELTA_TOP_LEFT_16X16_SZ - 1)) - 1;
        if (quad_qp_delta[0] < qpd_min || quad_qp_delta[0] > qpd_max ||
            quad_qp_delta[1] < qpd_min || quad_qp_delta[1] > qpd_max ||
            quad_qp_delta[2] < qpd_min || quad_qp_delta[2] > qpd_max ||
            quad_qp_delta[3] < qpd_min || quad_qp_delta[3] > qpd_max) {

            printf("Error qp_deltas (%d,%d,%d,%d) are out of range (min=%d, max=%d)\n",
                quad_qp_delta[0], quad_qp_delta[1], quad_qp_delta[2], quad_qp_delta[3],
                qpd_min, qpd_max);

        }
        bpr[i].qp_delta  = SET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_TOP_LEFT_16X16,  quad_qp_delta[0]);
        bpr[i].qp_delta |= SET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_TOP_RIGHT_16X16, quad_qp_delta[1]);
        bpr[i].qp_delta |= SET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_BOT_LEFT_16X16,  quad_qp_delta[2]);
        bpr[i].qp_delta |= SET(V4L2_BLOCK_PARAM_RECORD_QP_DELTA_BOT_RIGHT_16X16, quad_qp_delta[3]);
        bpr[i].qp_delta |= SET(V4L2_BLOCK_PARAM_RECORD_QP_FORCE_FIELD, force_tmp);
        bpr[i].qp_delta |= SET(V4L2_BLOCK_PARAM_RECORD_QP_QUAD_SKIP, force_qual_skip);
        bpr[i].qp_delta |= SET(V4L2_BLOCK_PARAM_RECORD_QP_ABSOLUTE, absolute_qp);
        bpr[i].qp_delta |= SET(V4L2_BLOCK_PARAM_RECORD_QUAD_FORCE_INTRA, force_intra);
        if( quad_min_qp[0] < 0 || quad_min_qp[0] > 51 ||
            quad_min_qp[1] < 0 || quad_min_qp[1] > 51 ||
            quad_min_qp[2] < 0 || quad_min_qp[2] > 51 ||
            quad_min_qp[3] < 0 || quad_min_qp[3] > 51)
        {
            printf("Error min_qp (%d,%d,%d,%d) are out of range (min=0, max=51)\n",
                quad_min_qp[0], quad_min_qp[1], quad_min_qp[2], quad_min_qp[3]);
        }
        bpr[i].min_qp  = SET(V4L2_BLOCK_PARAM_RECORD_MIN_QP_TOP_LEFT_16X16,  quad_min_qp[0]);
        bpr[i].min_qp |= SET(V4L2_BLOCK_PARAM_RECORD_MIN_QP_TOP_RIGHT_16X16, quad_min_qp[1]);
        bpr[i].min_qp |= SET(V4L2_BLOCK_PARAM_RECORD_MIN_QP_BOT_LEFT_16X16,  quad_min_qp[2]);
        bpr[i].min_qp |= SET(V4L2_BLOCK_PARAM_RECORD_MIN_QP_BOT_RIGHT_16X16, quad_min_qp[3]);

        sub_buf1 = sub_buf2 + 5;
    }
    sub_buf2 = strstr(sub_buf1, " bpr=");
    if (sub_buf2) {
        printf("Error : the real bpr number is larger than num_bpr for picture =%d (%s)\n",
                    config.pic_index, cfg_file_line_buf);
    }
    /*
    for (int i=len; i<n_cols; i++) {
      bpr[i] = bpr[len-1];
    }*/
}

void InputFileFrameWithEPR::read_efp_cfg(char * buf, int num_epr, epr_config * config)
{
    char *sub_buf1;
    int epr_count = 0;
    // qp
    sub_buf1 = strstr(buf, " qp=");
    if (sub_buf1)
    {
        if (1 != sscanf(sub_buf1, " qp={%u}",
                        &config->qp.qp))
        {
            cout<<"Error while parsing qp:"<<sub_buf1<<endl;
        }
        config->qp_present = true;
        epr_count++;
    }
        // iframe enable
    config->qp.epr_iframe_enable = 1; // default is on
    sub_buf1 = strstr(buf, " iframe_enable=");
    if (sub_buf1)
    {
        if (1 != sscanf(sub_buf1, " iframe_enable=%u",
                        &config->qp.epr_iframe_enable))
        {
            printf("Error while parsing qp %s\n", sub_buf1);
        }
    }
    if (epr_count != num_epr)
    {
        cout<<"Error not parsed enough EPRs"<<endl;
    }

}
void InputFileFrameWithEPR::load_epr_cfg()
{
    unsigned int num;
    unsigned int num2;
    int pic_num;
    int last_pic_num = -1; // -1 to handle first time
    int epr_num_row = 0;
    int epr_real_row = 0;
    if (!epr_list) {
        epr_list = new v4l2_epr_list_t;
    }
    const size_t max_bprf_body_size = ((getWidth() +31) >> 5) * ((getHeight() +31) >> 5) *
                                        sizeof(struct v4l2_block_param_record);
    epr_config config(max_bprf_body_size);
    while (epr_is.getline(cfg_file_line_buf, CFG_FILE_LINE_SIZE)) {
        if (1 != sscanf(cfg_file_line_buf, "pic=%d", &pic_num)) {
            cout<<"Line:"<<cfg_file_line_buf<<endl;
            return;
        }
        if (pic_num < 0) {
            cout<<"pic index must not be less than zero!"<<endl;
            return;
        }
        if (last_pic_num != pic_num) {
            // not the first time
            if (last_pic_num != -1)
            {
                epr_list->push_back(config);
            }
            config.clear();
            epr_num_row = 0;
            epr_real_row = 0;
            memset(config.bc_row_body.uncomp->bpr, 0, max_bprf_body_size);
        }
        if (2 == sscanf(cfg_file_line_buf,"pic=%d num_efp=%d", &config.pic_index, &num)) {
            read_efp_cfg(cfg_file_line_buf, num, &config);
        } else if (3 == sscanf(cfg_file_line_buf, "pic=%d num_row=%d type=%i", &config.pic_index, &num, &num2)) {
            if (num == 0) {
                cout<<"num_row must be greater than 0"<<endl;
                return;
            }
            if (config.block_configs_present) {
                cout<<" block_configs_present flag already set to region for picture!"<<endl;
            }
            config.block_configs_present = true;

            if (num2 == V4L2_BLOCK_CONFIGS_TYPE_ROW_UNCOMP)
            {
                config.block_configs.blk_cfg_type = V4L2_BLOCK_CONFIGS_TYPE_ROW_UNCOMP;
                config.block_configs.blk_cfgs.rows_uncomp.n_rows_minus1 = num - 1;
            }
            else
            {
                cout<<"Error - Unsupported block_param_rows_format"<<endl;
                return;
            }
            epr_num_row = num;
        }else if (3 ==
                sscanf(cfg_file_line_buf, "pic=%d row=%u num_bpr=%u", &config.pic_index, &num, &num2)) {
            if (!config.block_configs_present)
            {
                cout<<"Error - block_configs_present is not set for picture" <<endl;
            }
            if (config.block_configs.blk_cfg_type == V4L2_BLOCK_CONFIGS_TYPE_ROW_UNCOMP) {
                static unsigned int last_row = 0;
                if (num <= last_row && num != 0) {
                    cout<<"Error : current row number is less than last_row for picture"<<endl;
                }
                while (num > (last_row + 1)) {
                    //duplicate row
                    int n_cols = config.block_configs.blk_cfgs.rows_uncomp.n_cols_minus1 + 1;
                    memcpy(&config.bc_row_body.uncomp->bpr[(last_row + 1) * n_cols],
                           &config.bc_row_body.uncomp->bpr[last_row * n_cols],
                           n_cols * sizeof(struct v4l2_block_param_record));
                                    // bpr quad skip flag cant not inherit
                    for (int tmpi = 0; tmpi < n_cols; tmpi++) {
                      config.bc_row_body.uncomp->bpr[(last_row + 1) * n_cols + tmpi].qp_delta &= ~((uint32_t)SET(V4L2_BLOCK_PARAM_RECORD_QP_QUAD_SKIP, 1));
                    }
                    config.block_configs.blk_cfgs.rows_uncomp.n_rows_minus1++;
                    last_row++;
                }
                epr_real_row++;
                read_row_cfg(cfg_file_line_buf, num, num2, config);
                last_row = num;
            } else {
            cout<<"Error - block_configs_type is set to an unsupported value"<<endl;
            }
        }else {
            cout<<"parse config file ERROR:"<<cfg_file_line_buf<<endl;
        }
        last_pic_num = pic_num;
    }

    if (epr_real_row != epr_num_row) {
         cout<<"Error: num_row [%d] in epr-cfg-file is not equal to the real-line-number"<<endl;
    }
    epr_list->push_back(config);
}

InputFrame::InputFrame(uint32_t format, size_t width, size_t height,
                       size_t strideAlign, size_t nframes) :
    Input(format, width, height, strideAlign),
    nframes(nframes),
    count(0)
{
    Codec::getSize(format, width, height, strideAlign, nplanes, stride, size, heights);
}

void InputFrame::prepare(Buffer &buf)
{
    vector<iovec> iov = buf.getImageSize();

    unsigned int color = (0xff << 24) | (count * 10);
    unsigned int rgba[4];
    for (int i = 0; i < 3; ++i)
    {
        rgba[i] = (color >> (i * 8)) & 0xff;
    }

    unsigned int yuv[3];
    rgb2yuv(yuv, rgba);

    unsigned int y = yuv[0];
    unsigned int u = yuv[1];
    unsigned int v = yuv[2];

    switch (format)
    {
        case V4L2_PIX_FMT_YUV420M:
        {
            if (iov.size() != 3)
            {
                throw Exception("YUV420 has 3 planes. planes=%zu.", iov.size());
            }

            /* Y plane. */
            if (iov[0].iov_len < size[0])
            {
                throw Exception("YUV420 Y plane has incorrect size. size=%zu, expected=%zu.",
                                iov[0].iov_len, size[0]);
            }

            memset(iov[0].iov_base, y, size[0]);
            iov[0].iov_len = size[0];

            /* U plane. */
            if (iov[1].iov_len < size[1])
            {
                throw Exception("YUV420 U plane has incorrect size. size=%zu, expected=%zu.",
                                iov[1].iov_len, size[1]);
            }

            memset(iov[1].iov_base, u, size[1]);
            iov[1].iov_len = size[1];

            /* V plane. */
            if (iov[2].iov_len < size[2])
            {
                throw Exception("YUV420 V plane has incorrect size. size=%zu, expected=%zu.",
                                iov[2].iov_len, size[2]);
            }

            memset(iov[2].iov_base, v, size[2]);
            iov[2].iov_len = size[2];

            break;
        }
        case V4L2_PIX_FMT_NV12M:
        {
            if (iov.size() != 2)
            {
                throw Exception("YUV420 NV12M has 2 planes. planes=%zu.", iov.size());
            }

            /* Y plane. */
            if (iov[0].iov_len < size[0])
            {
                throw Exception("YUV420 NV12M Y plane has incorrect size. size=%zu, expected=%zu.",
                                iov[0].iov_len, size[0]);
            }

            memset(iov[0].iov_base, y, size[0]);
            iov[0].iov_len = size[0];

            /* UV plane. */
            if (iov[1].iov_len < size[1])
            {
                throw Exception("YUV420 NV12M UV plane has incorrect size. size=%zu, expected=%zu.",
                                iov[1].iov_len, size[1]);
            }

            uint8_t *p = static_cast<uint8_t *>(iov[1].iov_base);
            for (size_t i = 0; i < iov[1].iov_len; i += 2)
            {
                *p++ = u;
                *p++ = v;
            }

            break;
        }
        case V4L2_PIX_FMT_NV21M:
        {
            if (iov.size() != 2)
            {
                throw Exception("YUV420 NV21M has 2 planes. planes=%zu.", iov.size());
            }

            /* Y plane. */
            if (iov[0].iov_len < size[0])
            {
                throw Exception("YUV420 NV21M Y plane has incorrect size. size=%zu, expected=%zu.",
                                iov[0].iov_len, size[0]);
            }

            memset(iov[0].iov_base, y, size[0]);
            iov[0].iov_len = size[0];

            /* UV plane. */
            if (iov[1].iov_len < size[1])
            {
                throw Exception("YUV420 NV21M UV plane has incorrect size. size=%zu, expected=%zu.",
                                iov[1].iov_len, size[1]);
            }

            uint8_t *p = static_cast<uint8_t *>(iov[1].iov_base);
            for (size_t i = 0; i < iov[1].iov_len; i += 2)
            {
                *p++ = v;
                *p++ = u;
            }

            break;
        }
        case V4L2_PIX_FMT_YUYV:
        {
            if (iov.size() != 1)
            {
                throw Exception("YUYV has 1 plane. planes=%zu.", iov.size());
            }

            if (iov[0].iov_len < size[0])
            {
                throw Exception("YUYV plane 1 has incorrect size. size=%zu, expected=%zu.",
                                iov[0].iov_len, size[0]);
            }

            uint8_t *p = static_cast<uint8_t *>(iov[0].iov_base);
            for (size_t i = 0; i < size[0]; i += 4)
            {
                *p++ = y;
                *p++ = u;
                *p++ = y;
                *p++ = v;
            }

            iov[0].iov_len = size[0];

            break;
        }
        case V4L2_PIX_FMT_UYVY:
        {
            if (iov.size() != 1)
            {
                throw Exception("UYVY has 1 plane. planes=%zu.", iov.size());
            }

            if (iov[0].iov_len < size[0])
            {
                throw Exception("UYVY plane 1 has incorrect size. size=%zu, expected=%zu.",
                                iov[0].iov_len, size[0]);
            }

            uint8_t *p = static_cast<uint8_t *>(iov[0].iov_base);
            for (size_t i = 0; i < size[0]; i += 4)
            {
                *p++ = u;
                *p++ = y;
                *p++ = v;
                *p++ = y;
            }

            iov[0].iov_len = size[0];

            break;
        }
        case V4L2_PIX_FMT_Y210:
        {
            uint16_t y16 = y << 8;
            uint16_t u16 = u << 8;
            uint16_t v16 = v << 8;

            if (iov.size() != 1)
            {
                throw Exception("Y210 has 1 plane. planes=%zu.", iov.size());
            }

            if (iov[0].iov_len < size[0])
            {
                throw Exception("Y210 plane 1 has incorrect size. size=%zu, expected=%zu.",
                                iov[0].iov_len, size[0]);
            }

            uint16_t *p = static_cast<uint16_t *>(iov[0].iov_base);
            for (size_t i = 0; i < (size[0] / sizeof(*p)); i += 4)
            {
                *p++ = y16;
                *p++ = u16;
                *p++ = y16;
                *p++ = v16;
            }

            iov[0].iov_len = size[0];

            break;
        }
        case V4L2_PIX_FMT_P010M:
        {
            uint16_t y16 = y << 8;
            uint16_t u16 = u << 8;
            uint16_t v16 = v << 8;

            if (iov.size() != 2)
            {
                throw Exception("P010 has 2 planes. planes=%zu.", iov.size());
            }

            /* Y plane. */
            if (iov[0].iov_len < size[0])
            {
                throw Exception("P010 Y plane has incorrect size. size=%zu, expected=%zu.",
                                iov[0].iov_len, size[0]);
            }

            uint16_t *p = static_cast<uint16_t *>(iov[0].iov_base);
            for (size_t i = 0; i < (size[0] / sizeof(*p)); ++i)
            {
                *p++ = y16;
            }

            iov[0].iov_len = size[0];

            /* UV plane. */
            if (iov[1].iov_len < size[1])
            {
                throw Exception("P010 UV plane has incorrect size. size=%zu, expected=%zu.",
                                iov[1].iov_len, size[1]);
            }

            p = static_cast<uint16_t *>(iov[1].iov_base);
            for (size_t i = 0; i < (size[1] / sizeof(*p)); i += 2)
            {
                *p++ = u16;
                *p++ = v16;
            }

            iov[1].iov_len = size[1];

            break;
        }
        case V4L2_PIX_FMT_Y0L2:
        {
            if (iov.size() != 1)
            {
                throw Exception("Y0L2 has 1 plane. planes=%zu.", iov.size());
            }

            if (iov[0].iov_len < size[0])
            {
                throw Exception("Y0L2 plane 1 has incorrect size. size=%zu, expected=%zu.",
                                iov[0].iov_len, size[0]);
            }

            uint64_t a = 3;
            uint64_t y10 = y << 2;
            uint64_t u10 = u << 2;
            uint64_t v10 = v << 2;
            uint64_t w = (a << 62) | (y10 << 52) | (v10 << 42) | (y10 << 32) | (a << 30) | (y10 << 20) | (u10 << 10) | y10;
            uint8_t *p = static_cast<uint8_t *>(iov[0].iov_base);
            for (size_t i = 0; i < iov[0].iov_len; i += 8)
            {
                *p++ = w & 0xff;
                *p++ = (w >> 8) & 0xff;
                *p++ = (w >> 16) & 0xff;
                *p++ = (w >> 24) & 0xff;
                *p++ = (w >> 32) & 0xff;
                *p++ = (w >> 40) & 0xff;
                *p++ = (w >> 48) & 0xff;
                *p++ = (w >> 56) & 0xff;
            }

            iov[0].iov_len = size[0];

            break;
        }
        case DRM_FORMAT_ABGR8888:
        {
            if (iov.size() != 1)
            {
                throw Exception("RGBA has 1 plane. planes=%zu.", iov.size());
            }

            if (iov[0].iov_len < size[0])
            {
                throw Exception("RGBA plane 1 has incorrect size. size=%zu, expected=%zu.",
                                iov[0].iov_len, size[0]);
            }

            uint8_t *p = static_cast<uint8_t *>(iov[0].iov_base);
            for (size_t i = 0; i < size[0]; i += 4)
            {
                *p++ = rgba[0];
                *p++ = rgba[1];
                *p++ = rgba[2];
                *p++ = rgba[3];
            }

            break;
        }
        case DRM_FORMAT_ARGB8888:
        {
            if (iov.size() != 1)
            {
                throw Exception("BGRA has 1 plane. planes=%zu.", iov.size());
            }

            if (iov[0].iov_len < size[0])
            {
                throw Exception("BGRA plane 1 has incorrect size. size=%zu, expected=%zu.",
                                iov[0].iov_len, size[0]);
            }

            uint8_t *p = static_cast<uint8_t *>(iov[0].iov_base);
            for (size_t i = 0; i < size[0]; i += 4)
            {
                *p++ = rgba[2];
                *p++ = rgba[1];
                *p++ = rgba[0];
                *p++ = rgba[3];
            }

            break;
        }
        case DRM_FORMAT_BGRA8888:
        {
            if (iov.size() != 1)
            {
                throw Exception("ARGB has 1 plane. planes=%zu.", iov.size());
            }

            if (iov[0].iov_len < size[0])
            {
                throw Exception("ABGR plane 1 has incorrect size. size=%zu, expected=%zu.",
                                iov[0].iov_len, size[0]);
            }

            uint8_t *p = static_cast<uint8_t *>(iov[0].iov_base);
            for (size_t i = 0; i < size[0]; i += 4)
            {
                *p++ = rgba[3];
                *p++ = rgba[0];
                *p++ = rgba[1];
                *p++ = rgba[2];
            }

            break;
        }
        case DRM_FORMAT_RGBA8888:
        {
            if (iov.size() != 1)
            {
                throw Exception("ABGR has 1 plane. planes=%zu.", iov.size());
            }

            if (iov[0].iov_len < size[0])
            {
                throw Exception("ABGR plane 1 has incorrect size. size=%zu, expected=%zu.",
                                iov[0].iov_len, size[0]);
            }

            uint8_t *p = static_cast<uint8_t *>(iov[0].iov_base);
            for (size_t i = 0; i < size[0]; i += 4)
            {
                *p++ = rgba[3];
                *p++ = rgba[2];
                *p++ = rgba[1];
                *p++ = rgba[0];
            }

            break;
        }
        default:
            throw Exception("Unsupport input frame format.");
    }

    ++count;
    buf.setBytesUsed(iov);
    buf.setTimeStamp(count);
}

bool InputFrame::eof()
{
    return count >= nframes;
}

InputFileOsd::InputFileOsd(const char  * filename, uint32_t format, size_t width, size_t height, size_t strideAlign, size_t __stride[]):
        osd_is(filename)
{
    if (__stride) {
        memcpy(stride, __stride, sizeof(size_t) * 3);
    } else {
        memset(stride, 0, sizeof(size_t) * 3);
    }
    framesize = Codec::getSize(format, width, height, strideAlign, nplanes, stride, size, heights);
}

InputFileOsd::~InputFileOsd()
{
    osd_is.close();
}

void InputFileOsd::prepare(Buffer & buf)
{
    vector<iovec> iov = buf.getImageSize();

    for (size_t i = 0; i < nplanes; ++i)
    {
        osd_is.read(static_cast<char *>(iov[i].iov_base), size[i]);
        osd_is.seekg(0, osd_is.end);
        osd_is.seekg(0, osd_is.beg);
        iov[i].iov_len = osd_is.gcount();
    }
    buf.setBytesUsed(iov);
}

bool InputFileOsd::eof()
{
    return osd_is.peek() == EOF;
}

InputFileFrameOSD::InputFileFrameOSD(istream &input, uint32_t format, size_t width,
                    size_t height, size_t strideAlign, size_t __stride[]) :
    InputFileFrame(input, format, width, height, strideAlign, __stride)
{
    refresh_index = 0;
    osd_file_1 = NULL;
    osd_file_2 = NULL;
}

void InputFileFrameOSD::prepare(Buffer & buf)
{
    bool prepared = false;
    if (osd_list){
        if (0 == prepared_frames && refresh_index == 0) {
            osd_cur = osd_list->begin();
        }
        if (osd_list->end() != osd_cur && osd_cur->pic_index == prepared_frames) {
            buf.setOsdCfg(*osd_cur);
            buf.setOsdCfgEnable(true);
            refresh_index = osd_cur->num_osd;
            osd_cur++;
        } else {
            buf.setOsdCfgEnable(false);
        }
    }
    while (!prepared) {
        switch (refresh_index & 0x3) {
            case 0x1:
            case 0x3:
                if (osd_file_1) {
                    if (!osd_file_1->eof()) {
                        osd_file_1->prepare(buf);
                        prepared = true;
                        buf.setOsdBufferflag(refresh_index);
                    }
                    refresh_index &= ~0x1;
                } else {
                    cerr<<"osd file 1 not exist"<< endl;
                    exit(1);
                }
                break;
            case 0x2:
                if (osd_file_2) {
                    if (!osd_file_2->eof()) {
                        osd_file_2->prepare(buf);
                        prepared = true;
                        buf.setOsdBufferflag(refresh_index);
                    }
                    refresh_index &= ~0x2;
                } else {
                    cerr<<"osd file 2 not exist"<< endl;
                    exit(1);
                }
                break;
            case 0x0:
                InputFileFrame::prepare(buf);
                prepared = true;
                buf.setOsdBufferflag(refresh_index);
                break;
            default:
                cerr<<"Unkonw refresh index:"<< refresh_index << endl;
                exit(1);
        }
    }
}

void InputFrame::rgb2yuv(unsigned int yuv[3], const unsigned int rgb[3])
{
    /* Y = Kr*R + Kg*G + Kb*B */
    /* U = (B-Y)/(1-Kb) = - R * Kr/(1-Kb) - G * Kg/(1-Kb) + B */
    /* V = (R-Y)/(1-Kr) = R - G * Kg/(1-Kr) - B * Kb/(1-Kr) */

    /* BT601 { 0.2990, 0.5870, 0.1140 } */
    /* BT709 { 0.2125, 0.7154, 0.0721 }; */

    float Kr = 0.299;
    float Kg = 0.587;
    float Kb = 0.114;

    float r = rgb[0] / 255.0;
    float g = rgb[1] / 255.0;
    float b = rgb[2] / 255.0;

    float y;
    float u;
    float v;

    /* RGB to YUV. */
    y = Kr * r + Kg * g + Kb * b;
    u = (b - y) / (1 - Kb);
    v = (r - y) / (1 - Kr);

    /* Map YUV to limited color space. */
    yuv[0] = y * 219 + 16;
    yuv[1] = u * 112 + 128;
    yuv[2] = v * 112 + 128;
}

Output::Output(uint32_t format) :
    IO(format),
    totalSize(0),
    fps_n(0),
    fps_d(1)
{
    dir = 1;
    packed = false;
}

Output::Output(uint32_t format, bool packed) :
    IO(format),
    totalSize(0),
    packed(packed),
    fps_n(0),
    fps_d(1)
{
    dir = 1;
}

Output::~Output()
{
    cout << "Total size " << totalSize << endl;
}

void Output::prepare(Buffer &buf)
{
    buf.clearBytesUsed();
}

void Output::finalize(Buffer &buf)
{
    v4l2_buffer &b = buf.getBuffer();
    if (V4L2_TYPE_IS_MULTIPLANAR(b.type) &&
            ((b.flags & V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT) != V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT ||
            (b.flags & V4L2_BUF_FLAG_MVX_DECODE_ONLY) == V4L2_BUF_FLAG_MVX_DECODE_ONLY)){
        return;
    }
    if (b.timestamp.tv_usec != 0 && b.timestamp.tv_usec != timestamp && b.timestamp.tv_usec != (timestamp + 1))
    {
        cerr << "Incorrect timestamp. got=" << b.timestamp.tv_usec << ", expected=" << timestamp << endl;
    }

    controlFrameRate();
    if (!V4L2_TYPE_IS_MULTIPLANAR(b.type)) {
        uint8_t frame_type = (b.reserved2 & 0xFF000000) >> 24;
        uint8_t src_transform  = (b.reserved2 & 0xFF0000) >> 16;
        uint16_t remaining_bytes = b.reserved2;
        switch(frame_type){
            case 0:
                printf("I frame.\n");
            break;
            case 1:
                printf("P frame.\n");
            break;
            case 2:
                printf("B frame.\n");
            break;
            case 3:
                printf("LOWER B frame.\n");
            break;
            case 4:
                printf("P key frame.\n");
            break;
            case 5:
                printf("No ref P frame.\n");
            break;
            case 6:
                printf("GDR frame.\n");
            break;
            default:
                printf("unrecognized frame type.\n");
            break;
        }
        switch(src_transform){
            case 0:
                printf("none src transform.\n");
            break;
            case 1:
                printf("rotate 90  degrees.\n");
            break;
            case 2:
                printf("rotate 180  degrees.\n");
            break;
            case 3:
                printf("rotate 270  degrees.\n");
            break;
            case 4:
                printf("vertical flip (no rotation).\n");
            break;
            case 5:
                printf("rotate 90  degrees and vertical flip.\n");
            break;
            case 6:
                printf("rotate 180  degrees and vertical flip.\n");
            break;
            case 7:
                printf("rotate 270  degrees and vertical flip.\n");
            break;
            default:
                printf("unrecognized src transform.\n");
            break;
        }
        printf("remaining bytes of this buffer:%d\n", remaining_bytes);
    }
    timestamp = b.timestamp.tv_usec;

    if (skipOutput)
        return;

    if (securevideo) {
        secureVideoFinalize(buf);
        return;
    }

    vector<iovec> iov;
    vector<char> img_buf;
    size_t y_width, y_height, frame_size, nplanes = 0;
    size_t src_stride[3] = {0}, dst_stride[3] = {0}, dst_heights[3] = {0}, dst_size[3] = {0};
    if (packed) {
        if (V4L2_TYPE_IS_MULTIPLANAR(b.type)) {
            y_width = b.reserved2 >> 16;
            y_height = b.reserved2 & 0xFFFF;

            for (uint32_t i = 0; i < b.length; i++) {
                src_stride[i] = b.m.planes[i].reserved[0];
            }
            frame_size = Codec::getSize(getFormat(), y_width, y_height, 1, nplanes, dst_stride, dst_size, dst_heights);
            img_buf.resize(frame_size);
            buf.getPackedBuffer(static_cast<char*>(&img_buf[0]), nplanes, dst_heights, dst_stride, src_stride);
            if (convert10bit && (getFormat() == V4L2_PIX_FMT_P010M)) {
                iov = buf._convert10Bit((unsigned short*)(&img_buf[0]), (unsigned short*)(&img_buf[dst_size[0]]),
                                dst_size[0], dst_size[1]);
            } else {
                iovec vec = { .iov_base = static_cast<char *>(&img_buf[0]), .iov_len = frame_size };
                iov.push_back(vec);
            }
        }
    } else {
        iov = buf.getBytesUsed();
    }
	const struct v4l2_mvx_seamless_target  &seamless = buf.getSeamless();
    for (size_t i = 0; i < iov.size(); ++i)
    {
        if (V4L2_TYPE_IS_MULTIPLANAR(b.type)&& seamless.seamless_mode !=0)
        {
            write(iov[i].iov_base, b.m.planes[i].length);
            memset(iov[i].iov_base,0,b.m.planes[i].length);
            totalSize += b.m.planes[i].length;
            //cerr << b.m.planes[i].length <<endl;
        }
        else
        {
            write(iov[i].iov_base, iov[i].iov_len);
            totalSize += iov[i].iov_len;
        }

    }

}

void Output::secureVideoFinalize(Buffer &buf)
{
#ifdef ENABLE_DRM
    vector<iovec> iov;
    vector<char> img_buf;
    struct dump_data_params params;
    int fd;
    size_t length;
    v4l2_buffer &b = buf.getBuffer();

    iov = buf.getBytesUsed();
    for (size_t i = 0; i < iov.size(); ++i)
    {
        if (V4L2_TYPE_IS_MULTIPLANAR(b.type))
            fd = b.m.planes[i].m.fd;
        else
            fd = b.m.fd;
        length = iov[i].iov_len;
        img_buf.resize(length);
        params.src_fd = fd;
        params.dst_virt = (uint8_t *)(&img_buf[0]);
        params.length = &length;
        int ret = cix_secvid_send_command(secvid_session, CIX_CA_CMD_SECVID_DUMP_DATA, &params);
        if (ret != OK) {
            printf("Dump protected data is not allowed\n");
            return;
        }
        if (length != iov[i].iov_len) {
            printf("Warning: dump protected data length is incorrect, expected %zu, actually %zu\n",
                    iov[i].iov_len, length);
            return;
        }
        write((void *)(&img_buf[0]), iov[i].iov_len);
        totalSize += iov[i].iov_len;
    }
#endif
}

void Output::setFrameRate(unsigned int fps_n, unsigned int fps_d)
{
    if (fps_d > 0)
    {
        this->fps_n = fps_n;
        this->fps_d = fps_d;
    }
}

void Output::setSkipOutput()
{
    skipOutput = true;
}

void Output::controlFrameRate()
{
    IO::controlFrameRate(fps_n, fps_d);
}

void Output::setSecureVideo(void *data)
{
    securevideo = true;

#ifdef ENABLE_DRM
    secvid_session = static_cast<struct cix_secvid_session*>(data);
#endif
}

OutputFile::OutputFile(ostream &output, uint32_t format) :
    Output(format),
    output(output)
{}

OutputFile::OutputFile(ostream &output, uint32_t format, bool packed) :
    Output(format, packed),
    output(output)
{}

void OutputFile::write(void *ptr, size_t nbytes)
{
    output.write(static_cast<char *>(ptr), nbytes);
    output.flush();
}

#ifdef ENABLE_DISPLAY
int OutputDisplay::parse_dargs(const char *arg)
{
    const char *delim = " ";
    char *strTmp = NULL;
    char *strTmpIn = NULL;

    dargc = 0;
    dargs = (char *)malloc(strlen(arg) + 1);
    if (!dargs) {
        cerr << "Error: fail to alloate memory for display args" << endl;
        return -1;
    }
    strncpy(dargs, arg, strlen(arg));

    strTmp = dargs;
    while (NULL != (strTmp = strtok_r(strTmp, delim, &strTmpIn)))
    {
        if (dargc >= MVX_MAX_DARG_NUM) {
            cerr << "Error: too many display args" << endl;
            free(dargs);
            dargs = NULL;
            return -1;
        }
        dargv[dargc] = strTmp;
        strTmp = NULL;
        dargc++;
    }

    return 0;
}
#endif

InputFileLayer::InputFileLayer(const char *filename, uint32_t format, size_t width, size_t height, size_t strideAlign, size_t __stride[]) :
        width(width),
        height(height),
        format(format),
        layer_is(filename)
{
    if (__stride) {
        memcpy(stride, __stride, sizeof(size_t) * 3);
    } else {
        memset(stride, 0, sizeof(size_t) * 3);
    }
    framesize = Codec::getSize(format, width, height, strideAlign, nplanes, stride, size, heights);
}

InputFileLayer::~InputFileLayer()
{
    layer_is.close();
}

void InputFileLayer::prepare(char *buf, size_t length)
{
    if (length >= framesize)
        layer_is.read(buf, framesize);
    else
        throw Exception("Error: too small buffer for loading a picture");
}

#ifdef ENABLE_DISPLAY
OutputDisplay::OutputDisplay(const char *arg, uint32_t format) :
    Output(format)
{
    dev = NULL;
    planes = NULL;
    nplanes = 0;
    fd = -1;
    bg_bo = NULL;

    if (parse_dargs(arg))
        throw Exception("Error: failed to parse display args");

    dev = cix_device_create();
    if (dev == NULL)
        throw Exception("Error: failed to create device cix drm device");

    if (cix_device_parse(dev, dargc, dargv))
        throw Exception("Error: %s failed to parse args.", dargv[0]);

    cix_device_get_drm_fd(dev, &fd);
    if (fd < 0)
        throw Exception("Error: failed to get drm fd");

    nplanes = cix_device_get_planes_usage(dev, &planes);
    if (nplanes <= 0)
        throw Exception("Error: failed to get display planes.");

    if (cix_device_set_mode(dev))
        throw Exception("Error: failed to set display mode.");
}

OutputDisplay::~OutputDisplay()
{
    if (dev != NULL)
    {
        remove_background();
        cix_device_clear_planes(dev);
        cix_device_clear_mode(dev);
        cix_device_commit(dev, 0);
        cix_device_close(dev);
    }

    if (dargs)
        free(dargs);
}

void OutputDisplay::finalize(Buffer &buf)
{
    v4l2_buffer &b = buf.getBuffer();
    if (!V4L2_TYPE_IS_MULTIPLANAR(b.type) || b.m.planes[0].bytesused == 0){
        return;
    }

    if (V4L2_TYPE_IS_MULTIPLANAR(b.type) &&
            ((b.flags & V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT) != V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT ||
            (b.flags & V4L2_BUF_FLAG_MVX_DECODE_ONLY) == V4L2_BUF_FLAG_MVX_DECODE_ONLY)){
        return;
    }
    if (b.timestamp.tv_usec != 0 && b.timestamp.tv_usec != timestamp && b.timestamp.tv_usec != (timestamp + 1))
    {
        cerr << "Incorrect timestamp. got=" << b.timestamp.tv_usec << ", expected=" << timestamp << endl;
    }

    timestamp = b.timestamp.tv_usec;

    controlFrameRate();
    show_single(nplanes-1, buf.getBo()); // put video on the top
}

void OutputDisplay::show_single(int planeid, struct cix_bo *bo)
{
    if (bo == NULL || planeid >= nplanes)
    {
        return;
    }

    if (cix_device_set_plane(dev, planes[planeid], bo))
    {
        cerr << "Error: failed to set plane" << endl;
        return;
    }

    if (cix_device_commit(dev, 0))
    {
        cerr << "Error: failed to display" << endl;
        return;
    }
}

void OutputDisplay::addBackground(InputFileLayer *bg_layer)
{
    if (fd < 0 || dev == NULL || nplanes < 2)
    {
        return;
    }

    bg_bo = cix_bo_new(fd, bg_layer->getWidth(),
                bg_layer->getHeight(),
                pix_to_drm_fmt(bg_layer->getFormat()));
    if (bg_bo == NULL)
    {
        cerr << "Error: failed to create bg_bo" << endl;
        return;
    }

    char *bg_buf = (char *)cix_bo_map(bg_bo);
    if (bg_buf == NULL)
    {
        cerr << "Error: failed to map bg_bo" << endl;
        cix_bo_del(bg_bo);
        return;
    }

    bg_layer->prepare(bg_buf, bg_layer->getFrameSize()); // TODO: get size from cix_device

    cix_bo_unmap(bg_bo);

    if (cix_bo_add_fb(fd, bg_bo, 0))
    {
        cerr << "Error: failed to add fb" << endl;
        cix_bo_del(bg_bo);
        return;
    }

    if (cix_device_set_plane(dev, planes[0], bg_bo))
    {
        cerr << "Error: failed to set plane" << endl;
        return;
    }
}

void OutputDisplay::remove_background()
{
    if (bg_bo != NULL)
    {
        cix_bo_del(bg_bo);
    }
}

OutputDisplayAFBC::OutputDisplayAFBC(const char *arg, uint32_t format, bool tiled) :
    OutputDisplay(arg, format),
    tiled(tiled)
{}

void OutputDisplayAFBC::prepare(Buffer &buf)
{
    buf.clearBytesUsed();
    buf.setTiled(tiled);
}
#endif

OutputIVF::OutputIVF(ofstream &output,
                     uint32_t format,
                     uint16_t width,
                     uint16_t height,
                     uint32_t fps_n,
                     uint32_t fps_d,
                     uint32_t frameCount) :
    OutputFile(output, format)
{
    IVFHeader header(format, width, height, fps_n, fps_d, frameCount);
    write(reinterpret_cast<char *>(&header), sizeof(header));
}

void OutputIVF::finalize(Buffer &buf)
{
    vector<iovec> iov = buf.getBytesUsed();
    v4l2_buffer &b = buf.getBuffer();
    if (V4L2_TYPE_IS_MULTIPLANAR(b.type) &&
            (b.flags & V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT) != V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT){
        return;
    }

    if (securevideo || skipOutput)
        return;

    if (b.flags & V4L2_BUF_FLAG_END_OF_SUB_FRAME) {
        for (size_t i = 0; i < iov.size(); ++i) {
            size_t n = temp.size();
            temp.resize(temp.size() + iov[i].iov_len);

            char *p = static_cast<char *>(iov[i].iov_base);
            copy(p, p + iov[i].iov_len, &temp[n]);
        }
    } else {
        size_t total_size = temp.size();
        for (size_t i = 0; i < iov.size(); ++i)
            total_size += iov[i].iov_len;

        IVFFrame frame(total_size, b.timestamp.tv_usec);
        write(&frame, sizeof(frame));
        write(&temp[0], temp.size());
        temp.clear();
        for (size_t i = 0; i < iov.size(); ++i)
            write(iov[i].iov_base, iov[i].iov_len);
    }
}

OutputAFBC::OutputAFBC(std::ofstream &output, uint32_t format, bool tiled) :
    OutputFile(output, format),
    tiled(tiled)
{}

void OutputAFBC::prepare(Buffer &buf)
{
    buf.clearBytesUsed();
    buf.setTiled(tiled);
}

void OutputAFBC::finalize(Buffer &buf)
{
    vector<iovec> iov = buf.getBytesUsed();
    const v4l2_format &fmt= buf.getFormat();
    v4l2_buffer &b = buf.getBuffer();
    if (V4L2_TYPE_IS_MULTIPLANAR(b.type) &&
            (b.flags & V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT) != V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT){
        return;
    }

    if (securevideo) {
        secureVideoFinalize(buf);
        return;
    }

    tiled = ((b.flags & V4L2_BUF_FLAG_MVX_AFBC_TILED_HEADERS) || (b.flags & V4L2_BUF_FLAG_MVX_AFBC_TILED_BODY)) ? true : false;

    if(fmt.fmt.pix_mp.field == V4L2_FIELD_SEQ_TB || fmt.fmt.pix_mp.field == V4L2_FIELD_SEQ_BT)
    {
        if (iov[0].iov_len > 0 && !skipOutput)
        {
            size_t top_len = roundUp(iov[0].iov_len / 2, 32);
            AFBCHeader top_header(buf.getFormat(), top_len, buf.getCrop(), tiled, AFBCHeader::FIELD_TOP);
            write(&top_header, sizeof(top_header));
            write(iov[0].iov_base, top_len);
            totalSize += top_len;

            size_t bot_len = iov[0].iov_len - top_len;
            AFBCHeader bot_header(buf.getFormat(), bot_len, buf.getCrop(), tiled, AFBCHeader::FIELD_BOTTOM);
            write(&bot_header, sizeof(bot_header));
            write(static_cast<char *>(iov[0].iov_base) + top_len, bot_len);
            totalSize += bot_len;
        }
    }
    else
    {
        if (iov[0].iov_len > 0)
        {
            if (!skipOutput)
            {
                AFBCHeader header(buf.getFormat(), iov[0].iov_len, buf.getCrop(), tiled);
                if (V4L2_TYPE_IS_MULTIPLANAR(b.type)) {
                    size_t y_width = (b.m.planes[0].reserved[1] & 0xFFFF0000) >> 16;
                    size_t y_height = b.m.planes[0].reserved[1] & 0xFFFF;
                    if (y_width > 0 && y_height > 0) header.setSize(y_width, y_height);
                }
                write(&header, sizeof(header));
            }
            OutputFile::finalize(buf);
        }
    }
}

OutputAFBCInterlaced::OutputAFBCInterlaced(std::ofstream &output, uint32_t format, bool tiled) :
    OutputAFBC(output, format, tiled)
{}

void OutputAFBCInterlaced::finalize(Buffer &buf)
{
    vector<iovec> iov = buf.getBytesUsed();
    v4l2_buffer &b = buf.getBuffer();
    if (V4L2_TYPE_IS_MULTIPLANAR(b.type) &&
            (b.flags & V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT) == 0){
        return;
    }

    if (securevideo || skipOutput)
        return;

    if (iov[0].iov_len == 0)
    {
        return;
    }

    tiled = ((b.flags & V4L2_BUF_FLAG_MVX_AFBC_TILED_HEADERS) || (b.flags & V4L2_BUF_FLAG_MVX_AFBC_TILED_BODY)) ? true : false;
    size_t top_len = roundUp(iov[0].iov_len / 2, 32);
    AFBCHeader top_header(buf.getFormat(), top_len, buf.getCrop(), tiled, AFBCHeader::FIELD_TOP);

    write(&top_header, sizeof(top_header));
    write(iov[0].iov_base, top_len);

    size_t bot_len = iov[0].iov_len - top_len;
    AFBCHeader bot_header(buf.getFormat(), bot_len, buf.getCrop(), tiled, AFBCHeader::FIELD_BOTTOM);

    write(&bot_header, sizeof(bot_header));
    write(static_cast<char *>(iov[0].iov_base) + top_len, bot_len);
}

OutputFileFrameStats::OutputFileFrameStats(std::ostream &output, uint32_t format,
            uint32_t stats_mode, const std::string& filename, uint32_t width, uint32_t height):
    OutputFile(output, format)
{
    queued_buffer = 0;
    //open all enc stats file by default for convenience
    std::string file_name_mms = std::string( filename) + ".mms";
    file_mms.open( file_name_mms.c_str(), std::ios_base::binary );
    printf( "Output: statistics(SATD, MSE, MAD) file, %s\n", file_name_mms.c_str() );

    std::string file_name_bitcost = std::string( filename) + ".bc";
    file_bitcost.open( file_name_bitcost.c_str(), std::ios_base::binary );
    printf( "Output: statistics(bitcost) file, %s\n", file_name_bitcost.c_str() );

    std::string file_name_qp = std::string( filename) + ".qp";
    file_qp.open( file_name_qp.c_str(), std::ios_base::binary );
    printf( "Output: statistics(bitcost) file, %s\n", file_name_qp.c_str() );

}

void OutputFileFrameStats::finalize(Buffer & buf)
{
    if ((buf.getBuffer().flags & V4L2_BUF_FLAG_MVX_BUFFER_ENC_STATS) != V4L2_BUF_FLAG_MVX_BUFFER_ENC_STATS)
    {
        OutputFile::finalize(buf);
        return;
    }

    if (securevideo)
        return;

    vector<iovec> iov = buf.getBytesUsed();
    struct v4l2_buffer_param_enc_stats *stats =static_cast<struct v4l2_buffer_param_enc_stats *>(iov[0].iov_base);
    uint32_t offset = sizeof(struct v4l2_buffer_param_enc_stats);
    if (stats->flags & V4L2_BUFFER_ENC_STATS_FLAG_DROP)
    {
        return;
    }

    if ( stats->flags & V4L2_BUFFER_ENC_STATS_FLAG_MMS )
    {
        file_mms.write(static_cast<char *>(iov[0].iov_base) + offset,
                       stats->mms_buffer_size);
        file_mms << std::flush;
    }
    if ( stats->flags & V4L2_BUFFER_ENC_STATS_FLAG_BITCOST )
    {
        file_bitcost.write(static_cast<char *>(iov[0].iov_base) + offset + stats->mms_buffer_size,
                       stats->bitcost_buffer_size);
        file_bitcost << std::flush;
    }
    if ( stats->flags & V4L2_BUFFER_ENC_STATS_FLAG_QP )
    {
        file_qp.write(static_cast<char *>(iov[0].iov_base) + offset + stats->mms_buffer_size + stats->bitcost_buffer_size,
                       stats->qp_buffer_size);
        file_qp << std::flush;
    }
    printf("stats mb size of this buffer, width:%u, height:%u\n", (stats->pic_index_or_mb_size & 0xFFFF), (stats->pic_index_or_mb_size & 0xFFFF0000)>>16);
}

OutputFileFrameStats::~OutputFileFrameStats()
{
    if (file_mms.is_open())
    {
        file_mms.close();
    }
    if (file_bitcost.is_open())
    {
        file_bitcost.close();
    }
    if (file_qp.is_open())
    {
        file_qp.close();
    }
}

/****************************************************************************
 * Buffer
 ****************************************************************************/

Buffer::Buffer(const v4l2_format &format) :
    format(format)
{
    isRoiCfg = false;
    epr_qp.qp = 0;
    bo = NULL;
    in_queue = false;
    memset(&seamless,0,sizeof(seamless));
    memset(ptr, 0, sizeof(ptr));
}

Buffer::Buffer(v4l2_buffer &buf, int fd, const v4l2_format &format, enum v4l2_memory memory_type) :
    buf(buf),
    format(format),
    memory_type(memory_type)
{
    memset(ptr, 0, sizeof(ptr));

    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        memcpy(planes, buf.m.planes, sizeof(planes[0]) * buf.length);
        this->buf.m.planes = planes;
    }
    isRoiCfg = false;
    epr_qp.qp = 0;
    bo = NULL;
    in_queue = false;
    memset(&seamless,0,sizeof(seamless));
    //memoryMap(fd);
}

Buffer::~Buffer()
{
    memoryUnmap();
}

v4l2_buffer &Buffer::getBuffer()
{
    return buf;
}

const v4l2_format &Buffer::getFormat() const
{
    return format;
}

void Buffer::setCrop(const v4l2_rect &crop)
{
    this->crop = crop;
}

const v4l2_rect &Buffer::getCrop() const
{
    return crop;
}

void Buffer::setSeamless(const struct v4l2_mvx_seamless_target &seamless)
{
    this->seamless = seamless;
}

const v4l2_mvx_seamless_target &Buffer::getSeamless()const
{
    return seamless;
}
vector<iovec> Buffer::getImageSize() const
{
    vector<iovec> iova;

    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        for (unsigned int i = 0; i < buf.length; ++i)
        {
            iovec iov = { .iov_base = ptr[i], .iov_len = buf.m.planes[i].length };
            iova.push_back(iov);
        }
    }
    else
    {
        iovec iov = { .iov_base = ptr[0], .iov_len = buf.length };
        iova.push_back(iov);
    }

    return iova;
}

vector<iovec> Buffer::convert10Bit(){
    return _convert10Bit(static_cast<unsigned short*>(ptr[0]) + buf.m.planes[0].data_offset,
        static_cast<unsigned short *>(ptr[1]) + buf.m.planes[1].data_offset,
        buf.m.planes[0].bytesused - buf.m.planes[0].data_offset,
        buf.m.planes[1].bytesused - buf.m.planes[1].data_offset);
}

vector<iovec> Buffer::_convert10Bit(unsigned short* ptr_y, unsigned short* ptr_uv, size_t size_y, size_t size_uv){
    unsigned int i=0;
    unsigned short *y, *tmp_uv, *u, *v;
    size_t y_size, uv_size;
    vector<iovec> iova;
    //v4l2_plane &y_p = buf.m.planes[0];
    //y = static_cast<unsigned short *>(ptr[0]) + y_p.data_offset;
    //y_size = y_p.bytesused - y_p.data_offset;
    //v4l2_plane &u_p = buf.m.planes[1];
    //u = static_cast<unsigned short *>(ptr[1]) + u_p.data_offset;
    //uv_size = u_p.bytesused - u_p.data_offset;
    y = ptr_y;
    y_size = size_y;
    u = ptr_uv;
    uv_size = size_uv;


    v = u + uv_size/(2*sizeof(short));
    //buf.length = 3;
    iovec iov;

    tmp_uv = static_cast<unsigned short *>(calloc(uv_size / sizeof(short), sizeof(short)));
    if(NULL == tmp_uv){
        memset(y, 0xcc, y_size);
        iov = { .iov_base = y, .iov_len = y_size};
        iova.push_back(iov);
    } else {
        memcpy(tmp_uv, u , uv_size);
        memset(u, 0, uv_size);
        for( i=0; i < y_size / sizeof(short); i++){
            y[i] = y[i] >> 6;
        }
        iov = { .iov_base = y, .iov_len = y_size};
        iova.push_back(iov);
        for(i = 0; i < uv_size / (2*sizeof(short)); i++){
            u[i] = tmp_uv[2*i] >> 6;
            v[i] = tmp_uv[2*i+1] >> 6;
        }
        iov = { .iov_base = u, .iov_len = (uv_size)/2};
        iova.push_back(iov);
        iov = { .iov_base = v, .iov_len = (uv_size)/2};
        iova.push_back(iov);
        free(tmp_uv);
    }
    return iova;
}
vector<iovec> Buffer::getBytesUsed() const
{
    vector<iovec> iova;

    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        for (unsigned int i = 0; i < buf.length; ++i)
        {
            v4l2_plane &p = buf.m.planes[i];

            iovec iov = { .iov_base = static_cast<char *>(ptr[i]) + p.data_offset, .iov_len = p.bytesused - p.data_offset };

            if (p.bytesused < p.data_offset)
            {
                iov.iov_len = 0;
            }

            iova.push_back(iov);
        }
    }
    else
    {
        iovec iov = { .iov_base = ptr[0], .iov_len = buf.bytesused };

        /*
         * Single planar buffers has no support for offset, but for HEVC and VP9
         * encode we must find a way to relay the offset from the code.
         *
         * For MMAP we use the lower 12 bits (assuming 4k page size) to relay
         * the offset.
         *
         * For userptr the actual pointer is updated to point at the first byte
         * of the data.
         *
         * Because there is no offset 'bytesused' does not have to be adjusted
         * similar to multi planar buffers.
         */
        switch (buf.memory)
        {
            case V4L2_MEMORY_MMAP:
                iov.iov_base = static_cast<char *>(iov.iov_base) + (buf.m.offset & ((1 << 12) - 1));
                break;
            case V4L2_MEMORY_USERPTR:
                iov.iov_base = reinterpret_cast<void *>(buf.m.userptr);
                break;
            default:
                break;
        }

        iova.push_back(iov);
    }

    return iova;
}

void Buffer::getPackedBuffer(char* img, size_t nplanes, size_t dst_heights[], size_t dst_stride[], size_t src_stride[])
{
    char *dst = img;
    for (uint32_t i = 0; i < nplanes; i++) {
        char *src = static_cast<char*>(ptr[i]);
        for (uint32_t row = 0; row < dst_heights[i]; row++) {
            memcpy(dst, src, dst_stride[i]);
            dst += dst_stride[i];
            src += src_stride[i];
        }
    }

}

void Buffer::setBytesUsed(vector<iovec> &iov)
{
    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        if (iov.size() > buf.length)
        {
            throw Exception("iovec vector size is larger than V4L2 buffer number of planes. size=%u, planes=%u",
                            iov.size(), buf.length);
        }

        size_t i;
        for (i = 0; i < iov.size(); ++i)
        {
            buf.m.planes[i].bytesused = iov[i].iov_len;
        }

        for (; i < buf.length; ++i)
        {
            buf.m.planes[i].bytesused = 0;
        }
    }
    else
    {
        buf.bytesused = 0;

        for (size_t i = 0; i < iov.size(); ++i)
        {
            buf.bytesused += iov[i].iov_len;

            if (buf.bytesused > buf.length)
            {
                throw Exception("V4L2 buffer size too small. length=%u, buyteused=%u.", buf.length, buf.bytesused);
            }
        }
    }
}

void Buffer::clearBytesUsed()
{
    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        for (size_t i = 0; i < buf.length; ++i)
        {
            buf.m.planes[i].bytesused = 0;
        }
    }
    else
    {
        buf.bytesused = 0;
    }
}

void Buffer::resetVendorFlags()
{
    buf.flags &= ~V4L2_BUF_FLAG_MVX_MASK;
}

void Buffer::setCodecConfig(bool codecConfig)
{
    buf.flags &= ~V4L2_BUF_FLAG_MVX_CODEC_CONFIG;
    buf.flags |= codecConfig ? V4L2_BUF_FLAG_MVX_CODEC_CONFIG : 0;
}

void Buffer::setTimeStamp(unsigned int timeUs)
{
    buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
    buf.timestamp.tv_sec = timeUs / 1000000;
    buf.timestamp.tv_usec = timeUs % 1000000;
}

void Buffer::setEndOfFrame(bool eof)
{
    buf.flags &= ~V4L2_BUF_FLAG_KEYFRAME;
    buf.flags |= eof ? V4L2_BUF_FLAG_KEYFRAME : 0;
}

void Buffer::setEndOfSubFrame(bool eosf)
{
    buf.flags &= ~V4L2_BUF_FLAG_END_OF_SUB_FRAME;
    buf.flags |= eosf ? V4L2_BUF_FLAG_END_OF_SUB_FRAME : 0;
}

void Buffer::setRotation(int rotation)
{
    if( rotation%90 !=0) {
        return;
    }

    switch (rotation%360) {
        case 90:
            buf.flags &= ~V4L2_BUF_FRAME_FLAG_ROTATION_MASK;
            buf.flags |= V4L2_BUF_FRAME_FLAG_ROTATION_90;
            break;
        case 180:
            buf.flags &= ~V4L2_BUF_FRAME_FLAG_ROTATION_MASK;
            buf.flags |= V4L2_BUF_FRAME_FLAG_ROTATION_180;
            break;
        case 270:
            buf.flags &= ~V4L2_BUF_FRAME_FLAG_ROTATION_MASK;
            buf.flags |= V4L2_BUF_FRAME_FLAG_ROTATION_270;
            break;
        default:
            break;
    }
    return;
}

void Buffer::setEncRotation(int rotation)
{
    if( rotation%90 !=0) {
        return;
    }

    switch (rotation%360) {
        case 90:
            buf.flags &= ~V4L2_BUF_ENCODE_FLAG_ROTATION_MASK;
            buf.flags |= V4L2_BUF_ENCODE_FLAG_ROTATION_90;
            break;
        case 180:
            buf.flags &= ~V4L2_BUF_ENCODE_FLAG_ROTATION_MASK;
            buf.flags |= V4L2_BUF_ENCODE_FLAG_ROTATION_180;
            break;
        case 270:
            buf.flags &= ~V4L2_BUF_ENCODE_FLAG_ROTATION_MASK;
            buf.flags |= V4L2_BUF_ENCODE_FLAG_ROTATION_270;
            break;
        default:
            break;
    }
    return;
}

void Buffer::setDownScale(int scale)
{
    if (scale == 1) {
        return;
    }
    switch (scale) {
        case 2:
            buf.flags &= ~V4L2_BUF_FRAME_FLAG_SCALING_MASK;
            buf.flags |= V4L2_BUF_FRAME_FLAG_SCALING_2;
            break;
        case 4:
            buf.flags &= ~V4L2_BUF_FRAME_FLAG_SCALING_MASK;
            buf.flags |= V4L2_BUF_FRAME_FLAG_SCALING_4;
            break;
        default:
            printf("didnot support this scale factor :%d",scale);
            break;
    }
    return;
}

void Buffer::setMirror(int mirror)
{
    if (mirror == 0) {
        return;
    } else {
        if (mirror == 1) {
            buf.flags &= ~V4L2_BUF_FRAME_FLAG_MIRROR_MASK;
            buf.flags |= V4L2_BUF_FRAME_FLAG_MIRROR_HORI;
        } else if (mirror == 2) {
            buf.flags &= ~V4L2_BUF_FRAME_FLAG_MIRROR_MASK;
            buf.flags |= V4L2_BUF_FRAME_FLAG_MIRROR_VERT;
        }
    }
    return;
}

void Buffer::setEndOfStream(bool eos)
{
    buf.flags &= ~V4L2_BUF_FLAG_LAST;
    buf.flags |= eos ? V4L2_BUF_FLAG_LAST : 0;
}

void Buffer::set_force_idr_flag(bool idr)
{
    buf.flags &= ~V4L2_BUF_FLAG_KEYFRAME;
    buf.flags |= idr ? V4L2_BUF_FLAG_KEYFRAME : 0;
}

void Buffer::set_reset_rc_flag(bool reset)
{
    buf.flags &= ~V4L2_BUF_FLAG_MVX_BUFFER_RESET_RC;
    buf.flags |= reset ? V4L2_BUF_FLAG_MVX_BUFFER_RESET_RC : 0;
}

void Buffer::setROIflag(bool roi_valid){
    buf.flags &= ~V4L2_BUF_FLAG_MVX_BUFFER_ROI;
    if (roi_valid) {
        buf.flags |= V4L2_BUF_FLAG_MVX_BUFFER_ROI;
        isRoiCfg = true;
    } else {
        isRoiCfg = false;
    }
}

void Buffer::setChrflag(bool chr_valid){
    buf.flags &= ~V4L2_BUF_FLAG_MVX_BUFFER_CHR;
    if (chr_valid) {
        buf.flags |= V4L2_BUF_FLAG_MVX_BUFFER_CHR;
        isChrCfg = true;
    } else {
        isChrCfg = false;
    }
}

void Buffer::setEPRflag(){
    buf.flags &= ~V4L2_BUF_FLAG_MVX_BUFFER_EPR;
    buf.flags |= V4L2_BUF_FLAG_MVX_BUFFER_EPR;
}

void Buffer::setOsdBufferflag(uint32_t index)
{
    buf.reserved2 &= ~V4L2_BUF_FLAG_MVX_OSD_MASK;
    if (index & 0x1) {
        buf.reserved2 |= V4L2_BUF_FLAG_MVX_OSD_1;
        return;
    }
    if (index & 0x2) {
        buf.reserved2 |= V4L2_BUF_FLAG_MVX_OSD_2;
        return;
    }
}

void Buffer::update(v4l2_buffer &b)
{
    buf = b;

    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        buf.m.planes = planes;
        for (size_t i = 0; i < buf.length; ++i)
        {
            buf.m.planes[i] = b.m.planes[i];
        }
    }
}

void Buffer::memoryMap(int fd)
{
    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        for (uint32_t i = 0; i < buf.length; ++i)
        {
            v4l2_plane &p = buf.m.planes[i];

            if (p.length > 0)
            {
                ptr[i] = mmap(NULL,
                              p.length,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED,
                              fd,
                              p.m.mem_offset);
                if (ptr[i] == MAP_FAILED)
                {
                    throw Exception("Failed to mmap multi memory.");
                }
            }
        }
    }
    else
    {
        if (buf.length > 0)
        {
            ptr[0] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                          MAP_SHARED, fd, buf.m.offset);
            if (ptr[0] == MAP_FAILED)
            {
                throw Exception("Failed to mmap memory.");
            }
        }
    }
}

void Buffer::memoryUnmap()
{
    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        for (uint32_t i = 0; i < buf.length; ++i)
        {
            if (buf.memory == V4L2_MEMORY_DMABUF)
            {
                close(buf.m.planes[i].m.fd);
            }
            if (ptr[i] != 0)
            {
                munmap(ptr[i], buf.m.planes[i].length);
            }
        }
    }
    else
    {
        if (ptr[0])
        {
            if (buf.memory == V4L2_MEMORY_DMABUF)
            {
                close(buf.m.fd);
            }
            munmap(ptr[0], buf.length);
        }
    }
}

size_t Buffer::getLength(unsigned int plane)
{
    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        if (buf.length <= plane)
        {
            return 0;
        }

        return buf.m.planes[plane].length;
    }
    else
    {
        if (plane > 0)
        {
            return 0;
        }

        return buf.length;
    }
}

void Buffer::setInterlaced(bool interlaced)
{
    buf.field = interlaced ? V4L2_FIELD_SEQ_TB : V4L2_FIELD_NONE;
}

void Buffer::setTiled(bool tiled)
{
    buf.flags &= ~(V4L2_BUF_FLAG_MVX_AFBC_TILED_HEADERS | V4L2_BUF_FLAG_MVX_AFBC_TILED_BODY);
    if (tiled)
    {
        buf.flags |= V4L2_BUF_FLAG_MVX_AFBC_TILED_HEADERS;
        buf.flags |= V4L2_BUF_FLAG_MVX_AFBC_TILED_BODY;
    }
}

void Buffer::setRoiCfg(struct v4l2_mvx_roi_regions roi)
{
    roi_cfg = roi;
    isRoiCfg = true;
}

void Buffer::setChrCfg(struct v4l2_mvx_chr_config chr)
{
    chr_cfg = chr;
    isChrCfg = true;
}

void Buffer::setOsdCfg(struct v4l2_osd_config osd)
{
    osd_cfg = osd;
}

void Buffer::setOsdCfgEnable(bool enable)
{
    isOsdCfg = enable;
}
void Buffer::setGopResetCfg(struct v4l2_gop_config cfg)
{
    gop_cfg = cfg;
    isGopCfg = true;
}

void Buffer::setLtrResetCfg(struct v4l2_reset_ltr_period_config cfg)
{
    ltr_cfg = cfg;
    isLtrCfg = true;
}

void Buffer::setSuperblock(bool superblock)
{
    buf.flags &= ~(V4L2_BUF_FLAG_MVX_AFBC_32X8_SUPERBLOCK);
    if (superblock)
    {
        buf.flags |= V4L2_BUF_FLAG_MVX_AFBC_32X8_SUPERBLOCK;
    }
}

void Buffer::setDmaFd(int fd, const unsigned int plane, const unsigned offset)
{
    if (buf.memory != V4L2_MEMORY_DMABUF)
    {
        throw Exception("Failed to set file descriptor. Illegal memory type. memory=%d.", buf.memory);
    }

    if (plane < VIDEO_MAX_PLANES)
    {
        fds[plane] = fd;
    }

    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        buf.m.planes[plane].m.fd = fd;
        buf.m.planes[plane].data_offset = offset;
    }
    else
    {
        buf.m.fd = fd;
    }
}

void Buffer::setLength(const unsigned int length, const unsigned int plane)
{
    if (plane < VIDEO_MAX_PLANES)
    {
        lengths[plane] = length;
    }

    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        buf.m.planes[plane].length = length;
    }
    else
    {
        buf.length = length;
    }
}

void Buffer::dmaMemoryMap(int dma_fd, const unsigned int plane)
{
    unsigned int length = getLength(plane);
    ptr[plane] = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, dma_fd, 0);
    if (ptr[plane] == MAP_FAILED)
    {
        throw Exception("Failed to mmap buffer.");
    }
}

void Buffer::dmaMemoryunMap(void *p, const unsigned int plane)
{
    unsigned int length = getLength(plane);
    munmap(p, length);
}

int Buffer::dmaMemorySync(unsigned int flags, bool ext_input)
{
    int ret;
    uint32_t nfds;
    struct dma_buf_sync sync = { 0 };

    if (memory_type != V4L2_MEMORY_DMABUF || ext_input)
        return 0;

    nfds = getNumPlanes();
    for (unsigned int i = 0; i < nfds; i++)
    {
        sync.flags = flags;
        ret = ioctl(fds[i], DMA_BUF_IOCTL_SYNC, &sync);
        if (ret)
        {
            printf("sync start failed %d\n", errno);
            return ret;
        }
    }

    return 0;
}

void Buffer::createBoFromDmaFds(int fd, size_t width, size_t height, uint32_t pixfmt)
{
#ifdef ENABLE_DISPLAY
    uint32_t pix_fmt;
    uint32_t nfds;

    if (fd < 0)
        return;

    width = max(width, (size_t)64);
    height = max(height, (size_t)64);
    nfds = getNumPlanes();
    pix_fmt = V4L2_TYPE_IS_MULTIPLANAR(buf.type) ? format.fmt.pix_mp.pixelformat :
                    format.fmt.pix.pixelformat;
    if (Codec::isAFBC(pixfmt))
    {
        uint32_t pitches[VIDEO_MAX_PLANES] = {0};
        uint32_t offsets[VIDEO_MAX_PLANES] = {0};
        struct afbc_info afbc;
        afbc.modifier = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16);
        afbc.crop_left = crop.left;
        afbc.crop_top = crop.top;
        afbc.crop_right = 0;
        afbc.crop_bottom = 0;
        pitches[0] = (uint32_t)width;
        bo = cix_bo_from_dmabuf(fd, width, height, pix_to_drm_fmt(pix_fmt), nfds, fds,
                                pitches, offsets, lengths, &afbc);
    }
    else
    {
        bo = cix_bo_from_dmabuf(fd, width, height, pix_to_drm_fmt(pix_fmt), nfds, fds,
                                NULL, NULL, lengths, NULL);
    }
    if (bo == NULL)
    {
        throw Exception("Failed to create bo from dmabuf fds: %dx%d.", width, height);
    }

    if (cix_bo_add_fb(fd, bo, DRM_MODE_FB_MODIFIERS))
    {
        throw Exception("Failed to add fb.");
    }
#endif
}

unsigned int Buffer::getNumPlanes() const
{
    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        return buf.length;
    }
    else
    {
        return 1;
    }
}

void Buffer::allocMemory(const unsigned int plane)
{
    unsigned int length = getLength(plane);
    ptr[plane] = NULL;

    if (length == 0)
        return;

    uint32_t allocSize = roundUp(length, MVE_PAGE_SIZE);
    ptr[plane] = aligned_alloc(MVE_PAGE_SIZE, allocSize);
    if (ptr[plane] == NULL)
        throw Exception("Failed to allocate buffer.");

    if (!V4L2_TYPE_IS_MULTIPLANAR(buf.type))
        buf.m.userptr = (unsigned long)ptr[plane];
    else
        buf.m.planes[plane].m.userptr = (unsigned long)ptr[plane];
}

void Buffer::freeMemory(const unsigned int plane)
{
    if (ptr[plane] != NULL) {
        free(ptr[plane]);
        ptr[plane] = NULL;
    }
}

void Buffer::createBo(int fd, size_t width, size_t height)
{
#ifdef ENABLE_DISPLAY
    uint32_t pix_fmt;

    if (fd < 0)
        return;

    width = max(width, (size_t)64);
    height = max(height, (size_t)64);
    pix_fmt = V4L2_TYPE_IS_MULTIPLANAR(buf.type) ? format.fmt.pix_mp.pixelformat :
                    format.fmt.pix.pixelformat;
    bo = cix_bo_new(fd, width, height, pix_to_drm_fmt(pix_fmt));
    if (bo == NULL)
    {
        throw Exception("Failed to create bo: %dx%d.", width, height);
    }

    ptr[0] = cix_bo_map(bo);
    if (ptr[0] == NULL)
    {
        throw Exception("Failed to map bo: %dx%d.", width, height);
    }

    if (cix_bo_add_fb(fd, bo, 0))
    {
        throw Exception("Failed to add fb.");
    }

    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        buf.m.planes[0].m.userptr = (unsigned long)ptr[0];
        for (uint32_t i = 1; i < buf.length; ++i)
        {
            v4l2_plane &p = buf.m.planes[i];

            if (p.length > 0)
            {
                ptr[i] = (char *)ptr[i-1] + buf.m.planes[i-1].length;
                buf.m.planes[i].m.userptr = (unsigned long)ptr[i];
            }
        }
    } else {
        buf.m.userptr = (unsigned long)ptr[0];
    }
#endif
}

void Buffer::destroyBo()
{
#ifdef ENABLE_DISPLAY
    if (bo != NULL)
    {
        cix_bo_unmap(bo);
        cix_bo_del(bo);
    }
#endif
}

void Buffer::zeroBuffer()
{
    for (int i = 0; i < VIDEO_MAX_PLANES; i++)
    {
        if (ptr[i] != 0)
        {
            memset(ptr[i], 0, getLength(i));
        }
    }
}

/****************************************************************************
 * Transcoder, decoder, encoder
 ****************************************************************************/

Codec::Codec(const char *dev,
             enum v4l2_buf_type inputType,
             enum v4l2_buf_type outputType,
             ostream &log,
             bool nonblock,
             bool freerun) :
    input(this, fd, inputType, log),
    output(this, fd, outputType, log),
    log(log),
#ifdef ENABLE_CPIPE
    cpipe(NULL),
#endif
    nonblock(nonblock),
    freerun(freerun),
    outputThreadDone(false)
{
    openDev(dev);
    mini_frame_cnt = 0;
    memory_type = V4L2_MEMORY_MMAP;
    max_process_time = 0;
    min_process_time = 0xffffffff;
    avg_process_time = 0;
}

Codec::Codec(const char *dev,
             Input &input,
             enum v4l2_buf_type inputType,
             Output &output,
             enum v4l2_buf_type outputType,
             ostream &log,
             bool nonblock,
             bool freerun) :
    input(this, fd, input, inputType, log),
    output(this, fd, output, outputType, log),
    log(log),
    csweo(false),
    fps_n(0),
    fps_d(1),
    bps(0),
    minqp(0),
    maxqp(0),
    fixedqp(0),
#ifdef ENABLE_CPIPE
    cpipe(NULL),
#endif
    nonblock(nonblock),
    freerun(freerun),
    outputThreadDone(false)
{
    openDev(dev);
    mini_frame_cnt = 0;
    memory_type = V4L2_MEMORY_MMAP;
    max_process_time = 0;
    min_process_time = 0xffffffff;
    avg_process_time = 0;
}

Codec::~Codec()
{
    closeDev();
}

int Codec::stream()
{
    /* Set NALU. */
    if (isVPx(input.io->getFormat()) ||
        input.io->getFormat() == V4L2_PIX_FMT_VC1_ANNEX_L)
    {
        input.setNALU(NALU_FORMAT_ONE_NALU_PER_BUFFER);
    }
    else if (input.io->getNaluFormat() <= NALU_FORMAT_FOUR_BYTE_LENGTH_FIELD)
    {
        input.setNALU((NaluFormat)input.io->getNaluFormat());
    }

    if ((input.io->getFormat() == V4L2_PIX_FMT_VC1_ANNEX_L) ||
        (input.io->getFormat() == V4L2_PIX_FMT_VC1_ANNEX_G))
    {
        struct v4l2_control control;
        int profile = 0xff;

        switch (input.io->getProfile())
        {
            case 0:
            {
                profile = 0;
                break;
            }
            case 4:
            {
                profile = 1;
                break;
            }
            case 12:
            {
                profile = 2;
                break;
            }
            default:
            {
                throw Exception("Unsupported VC1 profile.\n");
            }
        }

        log << "VC1 decoding profile( " << profile << " )" << endl;

        memset(&control, 0, sizeof(control));

        control.id = V4L2_CID_MVE_VIDEO_VC1_PROFILE;
        control.value = profile;

        if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
        {
            throw Exception("Failed to set profile=%u for fmt: %u .", profile, input.io->getFormat());
        }
    }

    /* Add VPx file header. */
    if (isVPx(output.io->getFormat()))
    {
        output.setNALU(NALU_FORMAT_ONE_NALU_PER_BUFFER);
    }

    try
    {
        queryCapabilities();
        /* enumerateFormats(); */
        enumerateFramesizes(output.io->getFormat());
        setFormats();
        subscribeEvents();
        allocateBuffers(memory_type);
        queueBuffers();
        streamon();

        if (nonblock)
        {
            runPoll();
        }
        else
        {
            runThreads();
        }
        streamoff();
        freeBuffers();
#ifdef ENABLE_CPIPE
        if (cpipe)
            CPIPE_close(cpipe, 0);
#endif
    }
    catch (Exception &e)
    {
        cerr << "Error: " << e.what() << endl;
        streamoff();
        freeBuffers();
        return 1;
    }

    return 0;
}

uint32_t Codec::to4cc(const string &str)
{
    if (str.compare("yuv420_afbc_8") == 0)
    {
        return v4l2_fourcc('Y', '0', 'A', '8');
    }
    else if (str.compare("yuv420_afbc_10") == 0)
    {
        return v4l2_fourcc('Y', '0', 'A', 'A');
    }
    else if (str.compare("yuv422_afbc_8") == 0)
    {
        return v4l2_fourcc('Y', '2', 'A', '8');
    }
    else if (str.compare("yuv422_afbc_10") == 0)
    {
        return v4l2_fourcc('Y', '2', 'A', 'A');
    }
    else if (str.compare("gray_afbc_8") == 0)
    {
        return V4L2_PIX_FMT_Y_AFBC_8;
    }
    else if (str.compare("gray_afbc_10") == 0)
    {
        return V4L2_PIX_FMT_Y_AFBC_10;
    }
    else if (str.compare("yuv420") == 0)
    {
        return V4L2_PIX_FMT_YUV420M;
    }
    else if (str.compare("yuv420_nv12") == 0)
    {
        return V4L2_PIX_FMT_NV12M;
    }
    else if (str.compare("yuv420_nv21") == 0)
    {
        return V4L2_PIX_FMT_NV21M;
    }
    else if (str.compare("yuv420_p010") == 0)
    {
        return V4L2_PIX_FMT_P010M;
    }
    else if (str.compare("yuv420_y0l2") == 0)
    {
        return V4L2_PIX_FMT_Y0L2;
    }
    else if (str.compare("yuv422_yuy2") == 0)
    {
        return V4L2_PIX_FMT_YUYV;
    }
    else if (str.compare("yuv422_uyvy") == 0)
    {
        return V4L2_PIX_FMT_UYVY;
    }
    else if (str.compare("yuv422_y210") == 0)
    {
        return V4L2_PIX_FMT_Y210;
    }
    else if (str.compare("rgba") == 0)
    {
        return DRM_FORMAT_ABGR8888;
    }
    else if (str.compare("bgra") == 0)
    {
        return DRM_FORMAT_ARGB8888;
    }
    else if (str.compare("argb") == 0)
    {
        return DRM_FORMAT_BGRA8888;
    }
    else if (str.compare("abgr") == 0)
    {
        return DRM_FORMAT_RGBA8888;
    }
    else if (str.compare("bgr") == 0)
    {
        return V4L2_PIX_FMT_BGR24;
    }
    else if (str.compare("rgb") == 0)
    {
        return V4L2_PIX_FMT_RGB24;
    }
    else if (str.compare("rgb3p") == 0)
    {
        return V4L2_PIX_FMT_RGB_3P;
    }
    else if (str.compare("argb1555") == 0)
    {
        return V4L2_PIX_FMT_ARGB555;
    }
    else if (str.compare("argb4444") == 0)
    {
        return V4L2_PIX_FMT_ARGB444;
    }
    else if (str.compare("rgb565") == 0)
    {
        return V4L2_PIX_FMT_RGB565;
    }
    else if (str.compare("gray") == 0)
    {
        return V4L2_PIX_FMT_GREY;
    }
    else if (str.compare("gray_10") == 0)
    {
        return V4L2_PIX_FMT_Y10_LE;
    }
    else if (str.compare("yuv444") == 0)
    {
        return V4L2_PIX_FMT_YUV444M;
    }
    else if (str.compare("yuv444_10") == 0)
    {
        return V4L2_PIX_FMT_YUV444_10;
    }
    else if (str.compare("yuv420_2p10") == 0)
    {
        return V4L2_PIX_FMT_YUV420_2P_10;
    }
    else if (str.compare("yuv422_1p10") == 0)
    {
        return V4L2_PIX_FMT_YUV422_1P_10;
    }
    else if (str.compare("yuv_i42010") == 0)
    {
        return V4L2_PIX_FMT_YUV420_I420_10;
    }
    else if (str.compare("avs2") == 0)
    {
        return V4L2_PIX_FMT_AVS2;
    }
    else if (str.compare("avs") == 0)
    {
        return V4L2_PIX_FMT_AVS;
    }
    else if (str.compare("h263") == 0)
    {
        return V4L2_PIX_FMT_H263;
    }
    else if (str.compare("h264") == 0)
    {
        return V4L2_PIX_FMT_H264;
    }
    else if (str.compare("h264_mvc") == 0)
    {
        return V4L2_PIX_FMT_H264_MVC;
    }
    else if (str.compare("h264_no_sc") == 0)
    {
        return V4L2_PIX_FMT_H264_NO_SC;
    }
    else if (str.compare("hevc") == 0)
    {
        return V4L2_PIX_FMT_HEVC;
    }
    else if (str.compare("mjpeg") == 0)
    {
        return V4L2_PIX_FMT_MJPEG;
    }
    else if (str.compare("jpeg") == 0)
    {
        return V4L2_PIX_FMT_JPEG;
    }
    else if (str.compare("mpeg2") == 0)
    {
        return V4L2_PIX_FMT_MPEG2;
    }
    else if (str.compare("mpeg4") == 0)
    {
        return V4L2_PIX_FMT_MPEG4;
    }
    else if (str.compare("rv") == 0)
    {
        return V4L2_PIX_FMT_RV;
    }
    else if (str.compare("vc1") == 0)
    {
        return V4L2_PIX_FMT_VC1_ANNEX_G;
    }
    else if (str.compare("vc1_l") == 0)
    {
        return V4L2_PIX_FMT_VC1_ANNEX_L;
    }
    else if (str.compare("vp8") == 0)
    {
        return V4L2_PIX_FMT_VP8;
    }
    else if (str.compare("vp9") == 0)
    {
        return V4L2_PIX_FMT_VP9;
    }
    else if (str.compare("av1") == 0)
    {
        return V4L2_PIX_FMT_AV1;
    }
    else
    {
        throw Exception("Not a valid format '%s'.\n", str.c_str());
    }

    return 0;
}

bool Codec::isVPx(uint32_t format)
{
    return format == V4L2_PIX_FMT_VP8 || format == V4L2_PIX_FMT_VP9 || format == V4L2_PIX_FMT_AV1;
}

bool Codec::isAFBC(uint32_t format)
{
    switch (format)
    {
        case V4L2_PIX_FMT_YUV420_AFBC_8:
        case V4L2_PIX_FMT_YUV420_AFBC_10:
        case V4L2_PIX_FMT_YUV422_AFBC_8:
        case V4L2_PIX_FMT_YUV422_AFBC_10:
        case V4L2_PIX_FMT_Y_AFBC_8:
        case V4L2_PIX_FMT_Y_AFBC_10:
            return true;
        default:
            return false;
    }
}

uint32_t Codec::calcAfbcSize(uint32_t format, unsigned int width, unsigned int height, bool tiled_headers,
                             bool tiled_body, bool superblock, bool interlaced) {
    const unsigned int mb_header_size = 16;
    unsigned int payload_align = 128;
    unsigned int mb_size;
    int size;
    if (/* superblock != false */ false) {
        width = divRoundUp(width, 32);
        height = divRoundUp(height, 8) + 1;
    } else {
        width = divRoundUp(width, 16);
        height = divRoundUp(height, 16) + 1;
    }

    /* Round up size to 8x8 tiles. */
    if (tiled_headers != false || tiled_body != false) {
        width = roundUp(width, 8);
        height = roundUp(height, 8);
    }

    switch (format) {
    case V4L2_PIX_FMT_YUV420_AFBC_8:
        mb_size = 384;
        break;
    case V4L2_PIX_FMT_YUV420_AFBC_10:
        mb_size = 480;
        break;
    case V4L2_PIX_FMT_YUV422_AFBC_8:
        mb_size = 512;
        break;
    case V4L2_PIX_FMT_YUV422_AFBC_10:
        mb_size = 656;
        break;
    case V4L2_PIX_FMT_Y_AFBC_8:
        mb_size = 256;
        break;
    case V4L2_PIX_FMT_Y_AFBC_10:
        mb_size = 320;
        break;
    default:
        return 0;
    }

    /* Round up tiled body to 128 byte boundary. */
    if (tiled_body != false)
        mb_size = roundUp(mb_size, payload_align);

    if (interlaced != false)
        height = divRoundUp(height, 2);

    /* Calculate size of AFBC makroblock headers. */
    size = roundUp(width * height * mb_header_size, payload_align);
    size += roundUp(width * height * mb_size, payload_align);

    if (interlaced != false)
        size *= 2;

    // for odd resolution:
    return roundUp(size, 4096);
}

bool Codec::isYUV422(uint32_t format)
{
    switch (format)
    {
        case V4L2_PIX_FMT_YUYV:
        case V4L2_PIX_FMT_UYVY:
        case V4L2_PIX_FMT_Y210:
        case V4L2_PIX_FMT_YUV422_1P_10:
        case V4L2_PIX_FMT_YUV422_AFBC_8:
        case V4L2_PIX_FMT_YUV422_AFBC_10:
            return true;
        default:
            return false;
    }
}

size_t Codec::getBytesUsed(v4l2_buffer &buf)
{
    size_t size = 0;

    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        for (uint32_t i = 0; i < buf.length; ++i)
        {
            size += buf.m.planes[i].bytesused;
        }
    }
    else
    {
        size = buf.bytesused;
    }

    return size;
}

void Codec::openDev(const char *dev)
{
    int flags = O_RDWR;

    log << "Opening '" << dev << "'." << endl;

    if (nonblock)
    {
        flags |= O_NONBLOCK;
    }

    /* Open the video device in read/write mode. */
    fd = open(dev, flags);
    if (fd < 0)
    {
        throw Exception("Failed to open device.");
    }
}

void Codec::closeDev()
{
    log << "Closing fd " << fd << "." << endl;
    close(fd);
    fd = -1;
}

bool Codec::findAvailableDevice(uint32_t out_fmt, uint32_t cap_fmt, char dev_name[])
{
    int i;
    int fd;
    struct v4l2_capability cap;
    uint32_t out_type;
    uint32_t cap_type;

    for (i = 0; i < 64; i++) {
        snprintf(dev_name, MVX_MAX_PATH_LEN, "/dev/video%d", i);
        fd = open(dev_name, O_RDWR | O_NONBLOCK);
        if (fd < 0)
            continue;

        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0)
            continue;

        if ((cap.capabilities & (V4L2_CAP_VIDEO_CAPTURE_MPLANE | V4L2_CAP_VIDEO_OUTPUT_MPLANE)) ||
            (cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
            out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        } else {
            out_type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        }

        if (isDeviceSupportFmt(fd, out_type, out_fmt) &&
            isDeviceSupportFmt(fd, cap_type, cap_fmt)) {
            close(fd);
            return true;
        }

        close(fd);
    }

    return false;
}

bool Codec::isDeviceSupportFmt(int fd, uint32_t type, uint32_t format)
{
    struct v4l2_fmtdesc fmt_desc;
    fmt_desc.type = type;
    fmt_desc.index = 0;

    while (ioctl(fd, VIDIOC_ENUM_FMT, &fmt_desc) == 0) {
        if(fmt_desc.pixelformat == format)
            return true;

        fmt_desc.index++;
    }

    return false;
}

void Codec::queryCapabilities()
{
    struct v4l2_capability cap;
    int ret;

    /* Query capabilities. */
    ret = ioctl(fd, VIDIOC_QUERYCAP, &cap);
    if (ret != 0)
    {
        throw Exception("Failed to query for capabilities");
    }

    if ((cap.capabilities &
         (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) == 0)
    {
        throw Exception("Device is missing m2m support.");
    }
}

void Codec::enumerateFormats()
{
    input.enumerateFormats();
    output.enumerateFormats();
}

void Codec::Port::enumerateFormats()
{
    struct v4l2_fmtdesc fmtdesc;
    int ret;

    fmtdesc.index = 0;
    fmtdesc.type = type;

    while (1)
    {
        ret = ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc);
        if (ret != 0)
        {
            break;
        }

        log << "fmt: index=" << fmtdesc.index <<
        ", type=" << fmtdesc.type <<
        " , flags=" << hex << fmtdesc.flags <<
        ", pixelformat=" << fmtdesc.pixelformat <<
        ", description=" << fmtdesc.description <<
        endl;

        fmtdesc.index++;
    }

    printf("\n");
}

void Codec::enumerateFramesizes(uint32_t format)
{
    struct v4l2_frmsizeenum frmsize;

    frmsize.index = 0;
    frmsize.pixel_format = format;

    int ret = ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize);
    if (ret != 0)
    {
        throw Exception("Failed to enumerate frame sizes. ret=d.\n",
                        ret);
    }

    log << "Enumerate frame size." <<
    " index=" << frmsize.index <<
    ", pixel_format=" << hex << frmsize.pixel_format << dec;

    switch (frmsize.type)
    {
        case V4L2_FRMIVAL_TYPE_DISCRETE:
            break;
        case V4L2_FRMIVAL_TYPE_CONTINUOUS:
        case V4L2_FRMIVAL_TYPE_STEPWISE:
            log << ", min_width=" << frmsize.stepwise.min_width <<
            ", max_width=" << frmsize.stepwise.max_width <<
            ", step_width=" << frmsize.stepwise.step_width <<
            ", min_height=" << frmsize.stepwise.min_height <<
            ", max_height=" << frmsize.stepwise.max_height <<
            ", step_height=" << frmsize.stepwise.step_height;
            break;
        default:
            throw Exception("Unsupported enumerate frame size type. type=d.\n",
                            frmsize.type);
    }

    log << endl;
}

const v4l2_format &Codec::Port::getFormat()
{
    /* Get and print format. */
    format.type = type;
    int ret = ioctl(fd, VIDIOC_G_FMT, &format);
    if (ret != 0)
    {
        throw Exception("Failed to get format.");
    }

    return format;
}

void Codec::Port::tryFormat(v4l2_format &format)
{
    int ret = ioctl(fd, VIDIOC_TRY_FMT, &format);
    if (ret != 0)
    {
        throw Exception("Failed to try format.");
    }
}

void Codec::Port::setFormat(v4l2_format &format)
{
    int ret = ioctl(fd, VIDIOC_S_FMT, &format);
    if (ret != 0)
    {
        throw Exception("Failed to set format.");
    }

    this->format = format;

    if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
        alloc_width = format.fmt.pix_mp.width;
        alloc_height = format.fmt.pix_mp.height;
        if (Codec::isAFBC(format.fmt.pix_mp.pixelformat)) {
            alloc_afbc_bytes = format.fmt.pix_mp.plane_fmt[0].sizeimage;
        }
    } else {
        alloc_width = format.fmt.pix.width;
        alloc_height = format.fmt.pix.height;
        if (Codec::isAFBC(format.fmt.pix.pixelformat)) {
            alloc_afbc_bytes = format.fmt.pix.sizeimage;
        }
    }
}

void Codec::Port::setPixelFormat()
{
    v4l2_format fmt = getFormat();

    if (V4L2_TYPE_IS_MULTIPLANAR(type))
    {
        struct v4l2_pix_format_mplane &f = fmt.fmt.pix_mp;

        f.pixelformat = io->getFormat();
        // update non-AFBC pix_mp num_planes from pixelformat
        if (!Codec::isAFBC(f.pixelformat)) {
            size_t nplanes = 3;
            size_t tmpStride[3][2];
            getStride(f.pixelformat, nplanes, tmpStride);
            f.num_planes = nplanes;
        }
        for (int i = 0; i < 3; ++i)
        {
            f.plane_fmt[i].bytesperline = __stride[i];
            f.plane_fmt[i].sizeimage = 0;
        }
    }
    else
    {
        struct v4l2_pix_format &f = fmt.fmt.pix;

        f.pixelformat = io->getFormat();
        f.bytesperline = 0;
        f.sizeimage = 0;
    }
    setFormat(fmt);
    printFormat(fmt);
}

void Codec::Port::getTrySetFormat()
{
    size_t width = 0, height = 0;

    v4l2_format fmt = getFormat();
    if (V4L2_TYPE_IS_MULTIPLANAR(type))
    {
        struct v4l2_pix_format_mplane &f = fmt.fmt.pix_mp;

        f.pixelformat = io->getFormat();
        f.width = io->getWidth();
        f.height = io->getHeight();
        f.num_planes = 3;
        f.field = interlaced ? V4L2_FIELD_SEQ_TB : V4L2_FIELD_NONE;

        if (!Codec::isAFBC(f.pixelformat))
        {
            size_t size[3];
            size_t heights[3];
            size_t nplanes;
            Codec::getSize(f.pixelformat, f.width, f.height, io->getStrideAlign(), nplanes, __stride, size, heights);
        }

        for (int i = 0; i < 3; ++i)
        {
            f.plane_fmt[i].bytesperline = __stride[i];
            f.plane_fmt[i].sizeimage = 0;
        }

        if (colorspace > 0 || xfer_func > 0 || ycbcr_enc > 0 || quantization > 0)
        {
            f.colorspace = colorspace;
            f.xfer_func = xfer_func;
            f.ycbcr_enc = ycbcr_enc;
            f.quantization = quantization;
        }
    }
    else
    {
        struct v4l2_pix_format &f = fmt.fmt.pix;

        f.pixelformat = io->getFormat();
        f.width = io->getWidth();
        f.height = io->getHeight();
        f.bytesperline = 0;
        f.sizeimage = getCaptureSize();

        f.field = interlaced ? V4L2_FIELD_SEQ_TB : V4L2_FIELD_NONE;

        if (colorspace > 0 || xfer_func > 0 || ycbcr_enc > 0 || quantization > 0)
        {
            f.colorspace = colorspace;
            f.xfer_func = xfer_func;
            f.ycbcr_enc = ycbcr_enc;
            f.quantization = quantization;
        }
    }

    /* Try format. */
    tryFormat(fmt);

    if (V4L2_TYPE_IS_MULTIPLANAR(type))
    {
        struct v4l2_pix_format_mplane &f = fmt.fmt.pix_mp;
        width = f.width;
        height = f.height;
        if (mini_frame_cnt >= 2) {
            for (int i = 0; i < 3; ++i)
            {
                f.plane_fmt[i].sizeimage = f.plane_fmt[i].bytesperline * io->getReadHeight();
                if (i > 0) f.plane_fmt[i].sizeimage /= 2;
            }
        }
    }
    else
    {
        struct v4l2_pix_format &f = fmt.fmt.pix;
        width = f.width;
        height = f.height;
    }
    // for dsl frame case, this is not suitable, remove this.
    if (V4L2_TYPE_IS_OUTPUT(type) && (width != io->getWidth() || height != io->getHeight()))
    {
        //throw Exception("Selected resolution is not supported for this format width:%d, io width:%d", width, io->getWidth());
    }

    io->setWidth(width);
    io->setHeight(height);
    // For decode IVF with maximum resolution in IVF header to AFBC, set sizeimage before the first set format
    if (Codec::isAFBC(io->getFormat()) && io->getDir() == 1) {
        alloc_afbc_bytes = Codec::calcAfbcSize(io->getFormat(), width, height,
                            true, true, true, interlaced);
        if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
            fmt.fmt.pix_mp.plane_fmt[0].sizeimage = alloc_afbc_bytes;
        } else {
            fmt.fmt.pix.sizeimage = alloc_afbc_bytes;
        }
    }
    setFormat(fmt);
    printFormat(fmt);
}

void Codec::setFormats()
{
    input.getTrySetFormat();
    output.getTrySetFormat();
    output.enableDualAfbcDownScale();
    output.enableDSLFrame();
    input.enableEncCrop();
    output.enableEncCrop();
}

void Codec::Port::printFormat(const struct v4l2_format &format)
{
    if (V4L2_TYPE_IS_MULTIPLANAR(format.type))
    {
        const struct v4l2_pix_format_mplane &f = format.fmt.pix_mp;

        log << "Format:" << dec <<
        " type=" << format.type <<
        ", format=" << f.pixelformat <<
        ", width=" << f.width <<
        ", height=" << f.height <<
        ", nplanes=" << int(f.num_planes) <<
        ", bytesperline=[" << f.plane_fmt[0].bytesperline <<
        ", " << f.plane_fmt[1].bytesperline <<
        ", " << f.plane_fmt[2].bytesperline << "]" <<
        ", sizeimage=[" << f.plane_fmt[0].sizeimage <<
        ", " << f.plane_fmt[1].sizeimage <<
        ", " << f.plane_fmt[2].sizeimage << "]" <<
        ", colorspace:" << f.colorspace <<
        ", xfer_func:" << (int)f.xfer_func <<
        ", ycbcr_enc:" << (int)f.ycbcr_enc <<
        ", qunatization:" << (int)f.quantization <<
        ", interlaced:" << interlaced <<
        endl;
    }
    else
    {
        const struct v4l2_pix_format &f = format.fmt.pix;

        log << "Format:" << dec <<
        " type=" << format.type <<
        ", format=" << f.pixelformat <<
        ", width=" << f.width <<
        ", height=" << f.height <<
        ", sizeimage=" << f.sizeimage <<
        ", bytesperline=" << f.bytesperline <<
        ", colorspace:" << f.colorspace <<
        ", xfer_func:" << (int)f.xfer_func <<
        ", ycbcr_enc:" << (int)f.ycbcr_enc <<
        ", qunatization:" << (int)f.quantization <<
        ", interlaced:" << interlaced << endl;
    }
}

const v4l2_rect Codec::Port::getCrop()
{
    struct v4l2_selection sel;

    sel.type = type;
    sel.target = V4L2_SEL_TGT_COMPOSE;
    sel.flags = 0;
    ioctl(fd, VIDIOC_G_SELECTION, &sel);

    return sel.r;
}

void Codec::Port::setInterlaced(bool interlaced)
{
    this->interlaced = interlaced;
}

void Codec::Port::tryEncStopCmd(bool tryStop)
{
    this->tryEncStop = tryStop;
}

void Codec::Port::tryDecStopCmd(bool tryStop)
{
    this->tryDecStop = tryStop;
}

#if KERNEL_VERSION(5, 15, 0) <= LINUX_VERSION_CODE
void Codec::getHDR10Info()
{
    struct v4l2_ctrl_hdr10_cll_info cll;
    struct v4l2_ctrl_hdr10_mastering_display mastering;
    struct v4l2_ext_control control[] = {
        {
            .id = V4L2_CID_COLORIMETRY_HDR10_CLL_INFO,
            .size = sizeof(cll),
            .ptr = &cll,
        },
        {
            .id = V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY,
            .size = sizeof(mastering),
            .ptr = &mastering,
        },
    };
    struct v4l2_ext_controls controls = {
        .which = V4L2_CTRL_WHICH_CUR_VAL,
        .count = sizeof(control)/sizeof(control[0]),
        .controls = control,
    };

    int ret = ioctl(fd, VIDIOC_G_EXT_CTRLS, &controls);
    if (ret != 0)
    {
        throw Exception("Failed to get HDR10 info: %d.", ret);
    }

    printHDR10Info(&cll, &mastering);

    return;
}

void Codec::printHDR10Info(struct v4l2_ctrl_hdr10_cll_info *cll,
                        struct v4l2_ctrl_hdr10_mastering_display *mastering)
{
    if (mastering && mastering->max_display_mastering_luminance > 0)
    {
        log << "mastering display={" <<
        "r={x=" << mastering->display_primaries_x[0] <<
        ", y=" << mastering->display_primaries_y[0] << "}" <<
        ", g={x=" << mastering->display_primaries_x[1] <<
        ", y=" << mastering->display_primaries_y[1] << "}" <<
        ", b={x=" << mastering->display_primaries_x[2] <<
        ", y=" << mastering->display_primaries_y[2] << "}" <<
        ", w={x=" << mastering->white_point_x <<
        ", y=" << mastering->white_point_y << "}" <<
        ", lumiance_max=" << mastering->max_display_mastering_luminance * 0.0001 <<
        ", luminance_min=" << mastering->min_display_mastering_luminance * 0.0001 << "}" << endl;
    }

    if (cll && cll->max_content_light_level > 0)
    {
        log << "content={luminance_max=" << cll->max_content_light_level <<
        ", luminance_average=" << cll->max_pic_average_light_level << "}" << endl;
    }
}
#endif

void Codec::subscribeEvents()
{
    subscribeEvents(V4L2_EVENT_EOS);
    subscribeEvents(V4L2_EVENT_SOURCE_CHANGE);
}

void Codec::subscribeEvents(uint32_t event)
{
    struct v4l2_event_subscription sub = {
        .type = event, .id = 0
    };
    int ret;

    ret = ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    if (ret != 0)
    {
        throw Exception("Failed to subscribe for event.");
    }
}

void Codec::unsubscribeEvents()
{
    unsubscribeEvents(V4L2_EVENT_ALL);
}

void Codec::unsubscribeEvents(uint32_t event)
{
    struct v4l2_event_subscription sub;
    int ret;

    sub.type = event;
    ret = ioctl(fd, VIDIOC_UNSUBSCRIBE_EVENT, &sub);
    if (ret != 0)
    {
        throw Exception("Failed to unsubscribe for event.");
    }
}
void Codec::setInputThread(int input_thread)
{
    if (input_thread)
    {
       input.startInputThread();
    }
}

void Codec::setMemoryType(enum v4l2_memory mem_type)
{
    memory_type = mem_type;
    input.setPortMemoryType(mem_type);
    output.setPortMemoryType(mem_type);
}

void Codec::allocateBuffers(enum v4l2_memory m_type)
{
    //0 is default value, let port handle buffer cnt
    // Set buffer count as 1 for jpeg encode to avoid mmap failure
    size_t default_buffer_cnt = (output.io->getFormat() == V4L2_PIX_FMT_JPEG || output.io->getFormat() == V4L2_PIX_FMT_MJPEG)? 1 : 6;
    size_t count = mini_frame_cnt >= 2? mini_frame_cnt : input.getBufferCnt() ? input.getBufferCnt() : default_buffer_cnt;
    if (freerun == false)
        count = 1;
    input.allocateBuffers(count, m_type);
    output.allocateBuffers(output.getBufferCnt()? output.getBufferCnt() : default_buffer_cnt, m_type);
}

void Codec::setJobFrames(uint32_t frames)
{
    output.setPortJobFrames(frames);
}

void Codec::setPriority(int val){
    output.setPriority(val);
}


uint32_t Codec::Port::getInputBufferIdx(pthread_mutex_t *mutex, pthread_cond_t *cond, std::queue<uint32_t> *input_queue)
{
    uint32_t index;
    pthread_mutex_lock(mutex);
    while(input_queue->size() == 0)
    {
        pthread_cond_wait(cond, mutex);
    }
    index = input_queue->front();
    input_queue->pop();
    pthread_mutex_unlock(mutex);
    return index;
}

void Codec::Port::appendInputBufferIdx(pthread_mutex_t *mutex, pthread_cond_t *cond, std::queue<uint32_t> *input_queue, uint32_t index)
{
    pthread_mutex_lock(mutex);
    input_queue->push(index);
    pthread_cond_broadcast(cond);
    pthread_mutex_unlock(mutex);
}

void* Codec::Port::fillInputThread(void *arg)
{
    Codec::Port *input = static_cast<Codec::Port *>(arg);
    pthread_detach(pthread_self());
    input->_fillInputThread();
    pthread_exit((void *)NULL);
}

void Codec::Port::_fillInputThread()
{
    while(!io->eof()){
        uint32_t index = getInputBufferIdx(&input_producer_mutex, &input_producer_cond, &input_producer_queue);
        Buffer &buffer = *(buffers.at(index));
        buffer.dmaMemorySync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE, io->isExtInput());
        io->prepare(buffer);
        buffer.dmaMemorySync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE, io->isExtInput());
        buffer.setEndOfStream(io->eof());
        appendInputBufferIdx(&input_consumer_mutex, &input_consumer_cond, &input_consumer_queue, index);
    }
}

void Codec::Port::startInputThread()
{
    int ret;
    isInputThread = true;
    pthread_mutex_init(&input_consumer_mutex, NULL);
    pthread_mutex_init(&input_producer_mutex, NULL);
    pthread_cond_init(&input_consumer_cond, NULL);
    pthread_cond_init(&input_producer_cond, NULL);
    ret = pthread_create(&tid, NULL, fillInputThread, this);
    if (ret != 0)
    {
        throw Exception("Failed to create input thread.");
    }
}

void Codec::Port::setPortMemoryType(enum v4l2_memory mem_type)
{
    memory_type_port = mem_type;
}

int Codec::Port::allocateDMABuf(size_t size)
{
    const char *dma_heap = DMA_HEAP_SYSTEM;
    if (securevideo) {
        if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE || type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
            dma_heap = DMA_HEAP_OUTBUF;
        else
            dma_heap = DMA_HEAP_PROTECTED_BITS;
    }

    int fd = open(dma_heap, O_RDWR);
    if (fd < 0)
    {
        throw Exception("Failed to open ion device.");
    }

    struct dma_heap_allocation_data heap_data {
        .len = size,
        .fd_flags = O_RDWR | O_CLOEXEC,
    };
    if (ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &heap_data) < 0)
    {
        throw Exception("DMA_HEAP_IOCTL_ALLOC failed. errno=%d (%s).", errno, strerror(errno));
    }

    close(fd);
    return heap_data.fd;
}

void Codec::Port::allocateBuffers(size_t count, enum v4l2_memory mem_type, bool append)
{
    uint32_t i;
    uint32_t total = io->needDoubleCount()?count * 2 : count;
    uint32_t start = buffers.size();
    int ret;

    if (append == false) {
        struct v4l2_requestbuffers reqbuf;
        /* Free existing meta buffer. */
        freeBuffers();
        start = 0;
        /* Request new buffer to be allocated. */
        reqbuf.count = total;
        reqbuf.type = type;
        reqbuf.memory = mem_type;
        ret = ioctl(fd, VIDIOC_REQBUFS, &reqbuf);
        if (ret != 0)
        {
            throw Exception("Failed to request buffers.%d", mem_type);
        }

        total = reqbuf.count;

        log << "Request buffers." <<
        " type=" << reqbuf.type <<
        ", count=" << reqbuf.count <<
        ", memory=" << reqbuf.memory << endl;

        /* Reset number of buffers queued to driver. */
        pending = 0;
    } else if (total > start) {
        struct v4l2_create_buffers createbuf;
        memset(&createbuf, 0, sizeof(v4l2_create_buffers));
        /* Create extra buffers*/
        createbuf.count = total - start;
        createbuf.memory = mem_type;
        createbuf.format = format;
        ret = ioctl(fd, VIDIOC_CREATE_BUFS, &createbuf);
        if (ret != 0)
        {
            throw Exception("Failed to create buffers.%d", mem_type);
        }

        log << "Create buffers." <<
        " type=" << createbuf.format.type <<
        ", index=" << createbuf.index <<
        ", count=" << createbuf.count <<
        ", capabilities=" << hex << createbuf.capabilities << dec <<
        ", memory=" << createbuf.memory << endl;

        start = createbuf.index;
        total = createbuf.index + createbuf.count;
    }

    /* Query each buffer and create a new meta buffer. */
    for (i = start; i < total; ++i)
    {
        v4l2_buffer buf;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];

        buf.type = type;
        buf.memory = mem_type;
        buf.index = i;
        buf.length = 3;
        buf.m.planes = planes;
        ret = ioctl(fd, VIDIOC_QUERYBUF, &buf);
        if (ret != 0)
        {
            throw Exception("Failed to query buffer.ret=%d", ret);
        }

        printBuffer(buf, "Query");

        buffers[buf.index] = new Buffer(buf, fd, format, mem_type);
        buffers[buf.index]->setCrop(crop);
        size_t nplanes = buffers[buf.index]->getNumPlanes();
        if (mem_type == V4L2_MEMORY_MMAP) {
            buffers[buf.index]->memoryMap(fd);
        } else if (mem_type == V4L2_MEMORY_DMABUF) {
            // In external input mode, meta buffer will be updated before qbuf
            if (io->isExtInput())
                return;

            for (unsigned int plane = 0; plane < nplanes; ++plane)
            {
                unsigned int length = !V4L2_TYPE_IS_MULTIPLANAR(buf.type) ?
                                    buf.length : buf.m.planes[plane].length;
                int dma_fd = allocateDMABuf(length);

                buffers[buf.index]->setDmaFd(dma_fd, plane);
                buffers[buf.index]->setLength(length, plane);
                buffers[buf.index]->dmaMemoryMap(dma_fd, plane);
            }
            if (io->isDisplay()) {
                int dfd = io->getFd();
                size_t width = io->getWidth();
                size_t height = io->getHeight();
                uint32_t pixfmt = io->getFormat();
                if (width > 2 && height > 2)
                    buffers[buf.index]->createBoFromDmaFds(dfd, width, height, pixfmt);
            }
        } else if (mem_type == V4L2_MEMORY_USERPTR) {
            if (io->isDisplay()) {
                int dfd = io->getFd();
                size_t width = io->getWidth();
                size_t height = io->getHeight();
                buffers[buf.index]->createBo(dfd, width, height);
            } else {
                for (unsigned int plane = 0; plane < nplanes; ++plane)
                {
                    unsigned int length = 1;
                    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
                        length = buf.m.planes[plane].length;
                    else
                        length = buf.length;
                    buffers[buf.index]->setLength(length, plane);
                    buffers[buf.index]->allocMemory(plane);
                }
            }
        } else {
            cerr<<"didnot support this v4l2 memory type on this mode:"<<mem_type<<endl;
        }
        if (zeroafbc && io->getDir() == 1) {
            buffers[buf.index]->zeroBuffer();
        }
    }
}

void Codec::freeBuffers()
{
    input.allocateBuffers(0, memory_type);
    output.allocateBuffers(0, memory_type);
}

void Codec::Port::freeBuffers()
{
    while (!buffers.empty())
    {
        BufferMap::iterator it = buffers.begin();
        Buffer *buf = it->second;
        if (memory_type_port == V4L2_MEMORY_USERPTR)
        {
            if (io->isDisplay())
            {
                buf->destroyBo();
            }
            else
            {
                size_t nplanes = buf->getNumPlanes();
                for (unsigned int plane = 0; plane < nplanes; ++plane)
                    buf->freeMemory(plane);
            }
        }
        delete(it->second);
        buffers.erase(it);
    }
}

unsigned int Codec::Port::getRequiredBufferCount()
{
    struct v4l2_control control;
    uint32_t value;

    control.id = V4L2_TYPE_IS_OUTPUT(type) ? V4L2_CID_MIN_BUFFERS_FOR_OUTPUT : V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    if (-1 == ioctl(fd, VIDIOC_G_CTRL, &control))
    {
        throw Exception("Failed to get minimum buffers.");
    }
    value = control.value;
    if (io->isDisplay())
    {
        value++; // allocate one extra buffer since display has one frame delay
    }
    return value > getBufferCnt() ? value : getBufferCnt();
}

void Codec::queueBuffers()
{
    output.queueBuffers();
    input.queueBuffers();
}

void Codec::resetBuffers()
{
    output.resetBuffers();
    input.resetBuffers();
}

void Codec::Port::resetBuffers()
{
    for (BufferMap::iterator it = buffers.begin();
         it != buffers.end(); ++it)
    {
        Buffer &buffer = *(it->second);
        vector<iovec> iov = buffer.getImageSize();
        for (size_t i = 0; i < iov.size(); ++i)
            iov[i].iov_len = 0;
        buffer.setBytesUsed(iov);
        buffer.setInQueue(false);
        buffer.resetVendorFlags();
    }
}

void Codec::Port::queueBuffers()
{
    for (BufferMap::iterator it = buffers.begin();
         it != buffers.end(); ++it)
    {
        Buffer &buffer = *(it->second);
        if (!io->eof() && !buffer.IsInQueue())
        {
            /* Remove vendor custom flags. */
            buffer.resetVendorFlags();

            if (isInputThread) {
                appendInputBufferIdx(&input_producer_mutex, &input_producer_cond, &input_producer_queue, buffer.getBuffer().index);
                uint32_t index = getInputBufferIdx(&input_consumer_mutex, &input_consumer_cond, &input_consumer_queue);
                Buffer &buffer_input = *(buffers.at(index));
                queueBuffer(buffer_input);
            } else {
                buffer.dmaMemorySync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE, io->isExtInput());
                io->prepare(buffer);
                buffer.dmaMemorySync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE, io->isExtInput());
                buffer.setEndOfStream(io->eof());
                queueBuffer(buffer);
                if (io->getDir() == 0) {
                    Input *is = dynamic_cast<Input*>(io);
                    if (is->frame_bytes.size()) {
                        break;
                    }
                }
            }
        }
    }
}

void Codec::Port::queueBuffer(Buffer &buf)
{
    v4l2_buffer &b = buf.getBuffer();
    int ret;
    int retry_times = 0;
    buf.setInterlaced(interlaced);
    if(io->getDir() == 0 && V4L2_TYPE_IS_MULTIPLANAR(b.type)){
        buf.setEncRotation(rotation);
    }
    if(io->getDir() == 1 && V4L2_TYPE_IS_MULTIPLANAR(b.type)){
        buf.setRotation(rotation);
    }
    buf.setMirror(mirror);
    buf.setDownScale(scale);

    if (buf.getRoiCfgflag() && getBytesUsed(b) != 0) {
        struct v4l2_mvx_roi_regions roi = buf.getRoiCfg();
        ret = ioctl(fd, VIDIOC_S_MVX_ROI_REGIONS, &roi);
        if (ret != 0)
        {
            throw Exception("Failed to queue roi param.");
        }
    }

    if (buf.getChrCfgflag() && getBytesUsed(b) != 0) {
        struct v4l2_mvx_chr_config chr = buf.getChrCfg();
        ret = ioctl(fd, VIDIOC_S_MVX_CHR_CFG, &chr);
        if (ret != 0)
        {
            throw Exception("Failed to queue chr param.");
        }
    }

    if (buf.getGopResetCfgflag() && getBytesUsed(b) != 0) {
        struct v4l2_gop_config gop = buf.getGopResetCfg();
        setGopResetPframes(gop.gop_pframes);
    }

    if (buf.getLtrResetCfgflag() && getBytesUsed(b) != 0) {
        struct v4l2_reset_ltr_period_config ltr = buf.getLtrResetCfg();
        setLtrResetPeriod(ltr.reset_ltr_period);
    }
    if (buf.getResetStatsMode() > 0  && getBytesUsed(b) != 0) {
        setStatsMode(buf.getResetStatsMode(), buf.getResetStatsPicIndex());
    }

    if (buf.getOsdCfgEnable() > 0  && getBytesUsed(b) != 0) {
        setOsdCfg(buf.getOsdCfg());
    }

    if (buf.getQPofEPR().qp > 0) {
        struct v4l2_buffer_param_qp _epr_qp = buf.getQPofEPR();
        ret = ioctl(fd, VIDIOC_S_MVX_QP_EPR, &_epr_qp);
        if (ret != 0)
        {
            throw Exception("Failed to queue roi param.");
        }
        _epr_qp.qp = 0;
        buf.setQPofEPR(_epr_qp);
    }
    /* Mask buffer offset. */
    if (!V4L2_TYPE_IS_MULTIPLANAR(b.type))
    {
        switch (b.memory)
        {
            case V4L2_MEMORY_MMAP:
                b.m.offset &= ~((1 << 12) - 1);
                break;
            default:
                break;
        }
    }
    //encoder specfied frames count to be processed
    if (io->getDir() == 0 && frames_count > 0 && frames_processed >= frames_count - 1 && !buf.isGeneralBuffer() && !buf.isOsdBuffer()) {
        if (frames_processed >= frames_count) {
            buf.clearBytesUsed();
            buf.resetVendorFlags();
            return;
        }
        buf.setEndOfStream(true);
    }
    if (io->getDir() == 0 && V4L2_TYPE_IS_MULTIPLANAR(b.type) &&
           !buf.isGeneralBuffer() && !buf.isOsdBuffer()) {
        Input *is = dynamic_cast<Input*>(io);
        if (is->idr_list.size())
        {
            if (is->idr_list.front() == frames_processed){
                is->idr_list.pop();
                buf.set_force_idr_flag(true);
                buf.set_reset_rc_flag(true);
            } else {
                buf.set_force_idr_flag(false);
                buf.set_reset_rc_flag(false);
            }
        }
        else
        {
            buf.set_force_idr_flag(false);
            buf.set_reset_rc_flag(false);
        }
        frames_processed++;
    }
    if (io->getDir() == 1) {
        Output *is = dynamic_cast<Output*>(io);
        if (is->drc_list.size())
        {
            if (is->drc_list.front().frame_index == buffers_sent){
                struct RateControlParams rc = is->drc_list.front();
                if (rc.target_bit_rate > 0)
                    setEncBitrate(rc.target_bit_rate);
                if (rc.frame_rate > 0)
                    setEncFramerate(rc.frame_rate, 1);
                if (rc.minqp < 1000)
                    setEncMinQP(rc.minqp);
                if (rc.maxqp < 1000)
                    setEncMaxQP(rc.maxqp);
                is->drc_list.pop();
            }
        }
        buffers_sent++;
    }
    printBuffer(b, "->");
    do {
        ret = ioctl(fd, VIDIOC_QBUF, &b);
        if (ret != 0 && errno == EAGAIN) {
            retry_times++;
            log << "Queue buffer again. type=" << b.type << ", memory=" << b.memory
                << ", retry=" << retry_times << endl;
            continue;
        }
        break;
    } while (retry_times < 10);
    if (ret != 0)
    {
        throw Exception("Failed to queue buffer. type=%u, memory=%u, errno=%d",
                        b.type, b.memory, errno);
    }

    buf.setInQueue(true);
    ++pending;
    if (io->getDir() == 0 && V4L2_TYPE_IS_MULTIPLANAR(b.type) && owner->freerun == false) {
        uint64_t tick = getRawTick();
        owner->bufInQueue.push(tick);
    }
}

Buffer &Codec::Port::dequeueBuffer()
{
    v4l2_plane planes[VIDEO_MAX_PLANES];
    v4l2_buffer buf;
    buf.m.planes = planes;
    int retry_times = 0;
    int ret;

    buf.type = type;
    buf.memory = memory_type_port;
    buf.length = 3;

    do {
        ret = ioctl(fd, VIDIOC_DQBUF, &buf);
        if (ret != 0 && errno == EAGAIN) {
            retry_times++;
            log << "Dequeue buffer again. type=" << buf.type << ", memory=" << buf.memory
                << ", retry=" << retry_times << endl;
            continue;
        }
        break;
    } while (retry_times < 10);
    if (ret != 0)
    {
        throw Exception("Failed to dequeue buffer. type=%u, memory=%u, errno=%d",
                        buf.type, buf.memory, errno);
    }

    if(io->getDir() == 1) {
        if (!owner->bufInQueue.empty()) {
            uint64_t process_time = getRawTick() - owner->bufInQueue.front();
            owner->bufInQueue.pop();
            owner->avg_process_time += process_time;
            owner->max_process_time = (process_time > owner->max_process_time) ? process_time : owner->max_process_time;
            owner->min_process_time = (process_time < owner->min_process_time) ? process_time : owner->min_process_time;
        }
    }
    --pending;
    printBuffer(buf, "<-");

    Buffer &buffer = *(buffers.at(buf.index));
    buffer.setInQueue(false);
    buffer.update(buf);

    buffer.setCrop(getCrop());
    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        buffer.setSeamless(this->seamless);
    }
    return buffer;
}

void Codec::Port::printBuffer(const v4l2_buffer &buf, const char *prefix)
{
    log << prefix << ": " <<
    "type=" << buf.type <<
    ", index=" << buf.index <<
    ", sequence=" << buf.sequence <<
    ", timestamp={" << buf.timestamp.tv_sec << ", " << buf.timestamp.tv_usec << "}" <<
    ", flags=" << hex << buf.flags << dec;

    if (V4L2_TYPE_IS_MULTIPLANAR(buf.type))
    {
        const char *delim;

        log << ", num_planes=" << buf.length;

        delim = "";
        log << ", bytesused=[";
        for (unsigned int i = 0; i < buf.length; ++i)
        {
            log << delim << buf.m.planes[i].bytesused;
            delim = ", ";
        }
        log << "]";

        delim = "";
        log << ", length=[";
        for (unsigned int i = 0; i < buf.length; ++i)
        {
            log << delim << buf.m.planes[i].length;
            delim = ", ";
        }
        log << "]";

        delim = "";
        log << ", offset=[";
        for (unsigned int i = 0; i < buf.length; ++i)
        {
            log << delim << buf.m.planes[i].data_offset;
            delim = ", ";
        }
        log << "]";
        if(seamless.seamless_mode != 0 && buf.m.planes[0].bytesused >0)
        {
            log << ", out video w:h=[";
            log<<((buf.reserved2 & 0xffff0000) >> 16);
            delim = ", ";
            log << delim << (buf.reserved2 & 0xffff);
            log << "]";
            log << ", stride=[";
            delim ="";
            for (unsigned int i = 0; i < buf.length; ++i)
            {
                log << delim << buf.m.planes[i].reserved[0];
                delim = ", ";
            }
            log << "]";
        }
        if (memory_type_port == V4L2_MEMORY_USERPTR)
        {
            delim = "";
            cout << ", userptr=[";
            for (unsigned int i = 0; i < buf.length; ++i)
            {
                cout << delim << hex << buf.m.planes[i].m.userptr << dec;
                delim = ", ";
            }
            cout << "]";
        }
    }
    else
    {
        log << ", bytesused=" << buf.bytesused <<
        ", length=" << buf.length;
    }

    log << endl;
}

void Codec::streamon()
{
    input.streamon();
    output.streamon();
}

void Codec::Port::streamon()
{
    log << "Stream on " << dec << type << endl;

    int ret = ioctl(fd, VIDIOC_STREAMON, &type);
    if (ret != 0)
    {
        throw Exception("Failed to stream on.");
    }
}

void Codec::streamoff()
{
    int process_frames = output.getFramesProcessed();
    input.streamoff();
    output.streamoff();
    if (process_frames && avg_process_time) {
        avg_process_time /= process_frames;
        printf("avg encoding delay is %ld.%ld us\n",avg_process_time/1000,avg_process_time%1000);
    }
}

void Codec::Port::streamoff()
{
    log << "Stream off " << dec << type << endl;

    int ret = ioctl(fd, VIDIOC_STREAMOFF, &type);
    if (ret != 0)
    {
        throw Exception("Failed to stream off.");
    }
}

void Codec::Port::sendEncStopCommand()
{
    v4l2_encoder_cmd cmd = { .cmd = V4L2_ENC_CMD_STOP };

    if (tryEncStop)
    {
        if (0 != ioctl(fd, VIDIOC_TRY_ENCODER_CMD, &cmd))
        {
            throw Exception("Failed to send try encoder stop command.");
        }
        if (0 != ioctl(fd, VIDIOC_ENCODER_CMD, &cmd))
        {
           throw Exception("Failed to send encoding stop command.");
        }
    }
}

void Codec::Port::sendDecCommand(uint32_t command)
{
    v4l2_decoder_cmd cmd = { .cmd = command };

    if (tryDecStop)
    {
        if (0 != ioctl(fd, VIDIOC_TRY_DECODER_CMD, &cmd))
        {
            throw Exception("Failed to send try decoder command.");
        }
    }

    if (0 != ioctl(fd, VIDIOC_DECODER_CMD, &cmd))
    {
        throw Exception("Failed to send decoder command.");
    }
}

void Codec::Port::setH264DecIntBufSize(uint32_t ibs)
{
    log << "setH264DecIntBufSize( " << ibs << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_INTBUF_SIZE;
    control.value = ibs;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set H264 ibs=%u.", ibs);
    }
}

void Codec::Port::setDecFrameReOrdering(uint32_t fro)
{
    log << "setDecFrameReOrdering( " << fro << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_FRAME_REORDERING;
    control.value = fro;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set decoding fro=%u.", fro);
    }
}

void Codec::Port::setDecIgnoreStreamHeaders(uint32_t ish)
{
    log << "setDecIgnoreStreamHeaders( " << ish << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_IGNORE_STREAM_HEADERS;
    control.value = ish;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set decoding ish=%u.", ish);
    }
}

void Codec::Port::setNALU(NaluFormat nalu)
{
    log << "Set NALU " << nalu << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_NALU_FORMAT;
    control.value = nalu;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set NALU. nalu=%u.", nalu);
    }
}

size_t Codec::Port::getCaptureSize()
{
    size_t baseSize = 1 * 1024 * 1024;
    if (io->getWidth() && io->getHeight()) {
        size_t inputBufSize = std::max(baseSize, io->getWidth() * io->getHeight());
        inputBufSize = roundUp(inputBufSize, baseSize);
        return inputBufSize;
    }
    return baseSize;
}

void Codec::Port::setEncFramerate(unsigned int fps_n, unsigned int fps_d)
{
    log << "setEncFramerate( " << fps_n << "/" << fps_d << " )" << endl;

    struct v4l2_streamparm streamparm;

    memset (&streamparm, 0x00, sizeof (struct v4l2_streamparm));
    streamparm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    streamparm.parm.output.timeperframe.numerator = fps_d;
    streamparm.parm.output.timeperframe.denominator = fps_n;

    if (-1 == ioctl(fd, VIDIOC_S_PARM, &streamparm))
    {
        throw Exception("Failed to set frame_rate=%u/%u.", fps_n,fps_d);
    }
}

void Codec::Port::setEncBitrate(uint32_t bit_rate)
{
    log << "setEncBitrate( " << bit_rate << " )" << endl;
    log << "setRctype( " << rc_type << " )" << endl;
    if (bit_rate == 0 && rc_type == 0) {
        return;
    }
    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MPEG_VIDEO_BITRATE;
    control.value = bit_rate;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set bit_rate=%u.", bit_rate);
    }
}

void Codec::Port::setRateControl(bool rc_enabled, int rc_mode,
            int target_bitrate, int maximum_bitrate)
{
    struct v4l2_control control;
    log << "setRateControl( " << rc_enabled << "," << rc_mode << ",";
    log << target_bitrate << "," << maximum_bitrate << ")" << endl;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE;
    control.value = rc_enabled;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set FRAME_RC_ENABLE=%u.", rc_enabled);
    }

    if (!rc_enabled)
        return;

    control.id = V4L2_CID_MPEG_VIDEO_BITRATE_MODE;
    control.value = rc_mode;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set BITRATE_MODE=%u.", rc_mode);
    }

    control.id = V4L2_CID_MPEG_VIDEO_BITRATE;
    control.value = target_bitrate;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set BITRATE=%u.", target_bitrate);
    }

    if (rc_mode != V4L2_MPEG_VIDEO_BITRATE_MODE_CVBR)
        return;

    control.id = V4L2_CID_MPEG_VIDEO_BITRATE_PEAK;
    control.value = maximum_bitrate;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set BITRATE_PEAK=%u.", maximum_bitrate);
    }

    return;

}

void Codec::Port::setEncGOPSize(uint32_t gopSize)
{
    log << "setEncGOPSize( " << gopSize << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MPEG_VIDEO_GOP_SIZE;
    control.value = gopSize;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set gopSize=%u.", gopSize);
    }
}

void Codec::Port::setEncBFrames(uint32_t bframes)
{
    log << "setEncBFrames( " << bframes << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MPEG_VIDEO_B_FRAMES;
    control.value = bframes;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set bframes=%u.", bframes);
    }
}

void Codec::Port::setEncSliceSpacing(uint32_t spacing)
{
    log << "setEncSliceSpacing( " << spacing << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MAX_MB;
    control.value = spacing;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set slice spacing=%u.", spacing);
    }

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MPEG_VIDEO_MULTI_SLICE_MODE;
    control.value = spacing != 0;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set slice mode.");
    }
}

void Codec::Port::setEncForceChroma(uint32_t fmt)
{
    log << "setEncForceChroma( " << fmt << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_FORCE_CHROMA_FORMAT;
    control.value = fmt;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set Force chroma fmt=%u.", fmt);
    }
}

void Codec::Port::setEncBitdepth(uint32_t bd)
{
    log << "setEncBitdepth( " << bd << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_BITDEPTH_LUMA;
    control.value = bd;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set encode video frame luma bd=%u.", bd);
    }

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_BITDEPTH_CHROMA;
    control.value = bd;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set encode video frame chroma bd=%u.", bd);
    }
}

void Codec::Port::setH264EncIntraMBRefresh(uint32_t period)
{
    log << "setH264EncIntraMBRefresh( " << period << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MPEG_VIDEO_CYCLIC_INTRA_REFRESH_MB;
    control.value = period;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set H264 period=%u.", period);
    }
}

void Codec::Port::setEncProfile(uint32_t profile)
{
    log << "setEncProfile( " << profile << " )" << endl;

    bool setProfile = false;
    struct v4l2_control control;

    memset(&control, 0, sizeof(control));

    if (io->getFormat() == to4cc("h264"))
    {
        setProfile = true;
        control.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
        control.value = profile;
    }
    else if (io->getFormat() == to4cc("hevc"))
    {
        setProfile = true;
        control.id = V4L2_CID_MPEG_VIDEO_HEVC_PROFILE;
        control.value = profile;
    }

    if (setProfile)
    {
        if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
        {
            throw Exception("Failed to set profile=%u for fmt: %u .", profile, io->getFormat());
        }
    }
    else
    {
        log << "Profile cannot be set for this codec" << endl;
    }
}

void Codec::Port::setEncTier(uint32_t tier)
{
    log << "setEncTier( " << tier << " )" << endl;

    bool setTier = false;
    struct v4l2_control control;

    memset(&control, 0, sizeof(control));

    if (io->getFormat() == to4cc("hevc"))
    {
        setTier = true;
        control.id = V4L2_CID_MPEG_VIDEO_HEVC_TIER;
        control.value = tier;
    }

    if (setTier)
    {
        if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
        {
            throw Exception("Failed to set tier=%u for fmt: %u .", tier, io->getFormat());
        }
    }
    else
    {
        log << "Tier cannot be set for this codec" << endl;
    }
}

void Codec::Port::setEncLevel(uint32_t level)
{
    log << "setEncLevel( " << level << " )" << endl;

    bool setLevel = false;
    struct v4l2_control control;

    memset(&control, 0, sizeof(control));

    if (io->getFormat() == to4cc("h264"))
    {
        setLevel = true;
        control.id = V4L2_CID_MPEG_VIDEO_H264_LEVEL;
        control.value = level;
    }
    else if (io->getFormat() == to4cc("hevc"))
    {
        setLevel = true;
        control.id = V4L2_CID_MPEG_VIDEO_HEVC_LEVEL;
        control.value = level;
    }

    if (setLevel)
    {
        if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
        {
            throw Exception("Failed to set level=%u for fmt: %u .", level, io->getFormat());
        }
    }
    else
    {
        log << "Level cannot be set for this codec" << endl;
    }
}

void Codec::Port::setEncConstrainedIntraPred(uint32_t cip)
{
    log << "setEncConstrainedIntraPred( " << cip << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_CONSTR_IPRED;
    control.value = cip;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set encoding cip=%u.", cip);
    }
}

void Codec::Port::setH264EncEntropyMode(uint32_t ecm)
{
    log << "setH264EncEntropyMode( " << ecm << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE;
    control.value = ecm;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set H264 ecm=%u.", ecm);
    }
}

void Codec::Port::setH264EncGOPType(uint32_t gop)
{
    log << "setH264EncGOPType( " << gop << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_GOP_TYPE;
    control.value = gop;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set H264 gop=%u.", gop);
    }
}

void Codec::Port::setEncMinQP(uint32_t minqp)
{
    log << "setH264EncMinQP( " << minqp << " )" << endl;

    struct v4l2_control control;
    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE;
    control.value = 1;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to enable/disable rate control.");
    }

    memset(&control, 0, sizeof(control));
    if (io->getFormat() == V4L2_PIX_FMT_H264) {
        control.id = V4L2_CID_MPEG_VIDEO_H264_MIN_QP;
    } else if (io->getFormat() == V4L2_PIX_FMT_HEVC) {
        control.id = V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP;
    } else if (io->getFormat() == V4L2_PIX_FMT_VP9) {
        control.id = V4L2_CID_MPEG_VIDEO_VPX_MIN_QP;
    } else {
        cerr<<"This format is not supported for min QP"<<endl;
    }
    control.value = minqp;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set H264 minqp=%u.", minqp);
    }
}

void Codec::Port::setEncMaxQP(uint32_t maxqp)
{
    log << "setH264EncMaxQP( " << maxqp << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE;
    control.value = 1;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to enable/disable rate control.");
    }

    memset(&control, 0, sizeof(control));
    if (io->getFormat() == V4L2_PIX_FMT_H264) {
        control.id = V4L2_CID_MPEG_VIDEO_H264_MAX_QP;
    } else if (io->getFormat() == V4L2_PIX_FMT_HEVC) {
        control.id = V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP;
    } else if (io->getFormat() == V4L2_PIX_FMT_VP9) {
        control.id = V4L2_CID_MPEG_VIDEO_VPX_MAX_QP;
    } else {
        cerr<<"This format is not supported for max QP"<<endl;
    }
    control.value = maxqp;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set H264 maxqp=%u.", maxqp);
    }
}

void Codec::Port::setEncFixedQP(uint32_t fqp)
{
    log << "setH264EncFixedQP( " << fqp << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_QP_FIXED;
    control.value = fqp;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set fixed qp=%u.", fqp);
    }
}

void Codec::Port::setEncFixedQPI(uint32_t fqp)
{
    log << "setH264EncFixedQPI( " << fqp << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    if (io->getFormat() == V4L2_PIX_FMT_H264) {
        control.id = V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP;
    } else if (io->getFormat() == V4L2_PIX_FMT_HEVC) {
        control.id = V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP;
    } else if (io->getFormat() == V4L2_PIX_FMT_VP9) {
        control.id = V4L2_CID_MPEG_VIDEO_VPX_I_FRAME_QP;
    } else {
        cerr<<"This format is not supported for I frames fixed QP"<<endl;
    }
    control.value = fqp;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set H264 I frame fqp=%u.", fqp);
    }
}

void Codec::Port::setEncFixedQPP(uint32_t fqp)
{
    log << "setH264EncFixedQPP( " << fqp << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    if (io->getFormat() == V4L2_PIX_FMT_H264) {
        control.id = V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP;
    } else if (io->getFormat() == V4L2_PIX_FMT_HEVC) {
        control.id = V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP;
    } else if (io->getFormat() == V4L2_PIX_FMT_VP9) {
        control.id = V4L2_CID_MPEG_VIDEO_VPX_P_FRAME_QP;
    } else {
        cerr<<"This format is not supported for P frames fixed QP"<<endl;
    }
    control.value = fqp;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set H264 P frame fqp=%u.", fqp);
    }

}

void Codec::Port::setEncFixedQPB(uint32_t fqp)
{
    log << "setH264EncFixedQPB( " << fqp << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    if (io->getFormat() == V4L2_PIX_FMT_H264) {
        control.id = V4L2_CID_MPEG_VIDEO_H264_B_FRAME_QP;
    } else if (io->getFormat() == V4L2_PIX_FMT_HEVC) {
        control.id = V4L2_CID_MPEG_VIDEO_HEVC_B_FRAME_QP;
    } else if (io->getFormat() == V4L2_PIX_FMT_VP9) {
        control.id = V4L2_CID_MVE_VIDEO_VPX_B_FRAME_QP;
    } else {
        cerr<<"This format is not supported for B frames fixed QP"<<endl;
    }
    control.value = fqp;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set H264 B frame fqp=%u.", fqp);
    }

}

void Codec::Port::setEncMinQPI(uint32_t nQpMinI)
{
    log << "setEncMinQPI( " << nQpMinI << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_MIN_QP_I;
    control.value = nQpMinI;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set nQpMinI=%u.", nQpMinI);
    }
}

void Codec::Port::setEncMaxQPI(uint32_t nQpMaxI)
{
    log << "setEncMaxQPI( " << nQpMaxI << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_MAX_QP_I;
    control.value = nQpMaxI;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set nQpMaxI=%u.", nQpMaxI);
    }
}

void Codec::Port::setEncInitQPI(uint32_t init_qpi)
{
    log << "setEncInitQPI( " << init_qpi << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_INIT_QP_I;
    control.value = init_qpi;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set init_qpi=%u.", init_qpi);
    }
}

void Codec::Port::setEncInitQPP(uint32_t init_qpp)
{
    log << "setEncInitQPP( " << init_qpp << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_INIT_QP_P;
    control.value = init_qpp;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set init_qpp=%u.", init_qpp);
    }
}

void Codec::Port::setEncSAOluma(uint32_t sao_luma_dis)
{
    log << "setEncSAOluma( " << sao_luma_dis << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_SAO_LUMA;
    control.value = sao_luma_dis;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set sao_luma_dis=%u.", sao_luma_dis);
    }
}

void Codec::Port::setEncSAOchroma(uint32_t sao_chroma_dis)
{
    log << "setEncSAOchroma( " << sao_chroma_dis << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_SAO_CHROMA;
    control.value = sao_chroma_dis;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set sao_chroma_dis=%u.", sao_chroma_dis);
    }
}

void Codec::Port::setEncQPDeltaIP(uint32_t qp_delta_i_p)
{
    log << "setEncQPDeltaIP( " << qp_delta_i_p << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_QP_DELTA_I_P;
    control.value = qp_delta_i_p;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set qp_delta_i_p=%u.", qp_delta_i_p);
    }
}

void Codec::Port::setEncRefRbEn(uint32_t ref_rb_en)
{
    log << "setEncRefRbEn( " << ref_rb_en << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_QP_REF_RB_EN;
    control.value = ref_rb_en;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set ref_rb_en=%u.", ref_rb_en);
    }
}

void Codec::Port::setEncRCClipTop(uint32_t rc_qp_clip_top)
{
    log << "setEncRCClipTop( " << rc_qp_clip_top << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_RC_CLIP_TOP;
    control.value = rc_qp_clip_top;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set rc_qp_clip_top=%u.", rc_qp_clip_top);
    }
}

void Codec::Port::setEncRCClipBot(uint32_t rc_qp_clip_bottom)
{
    log << "setEncRCClipBot( " << rc_qp_clip_bottom << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_RC_CLIP_BOT;
    control.value = rc_qp_clip_bottom;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set rc_qp_clip_bottom=%u.", rc_qp_clip_bottom);
    }
}

void Codec::Port::setEncQpmapClipTop(uint32_t qpmap_clip_top)
{
    log << "setEncQpmapClipTop( " << qpmap_clip_top << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_QP_MAP_CLIP_TOP;
    control.value = qpmap_clip_top;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set qpmap_clip_top=%u.", qpmap_clip_top);
    }
}

void Codec::Port::setEncQpmapClipBot(uint32_t qpmap_clip_bottom)
{
    log << "setEncQpmapClipBot( " << qpmap_clip_bottom << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_QP_MAP_CLIP_BOT;
    control.value = qpmap_clip_bottom;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set qpmap_clip_bottom=%u.", qpmap_clip_bottom);
    }
}

void Codec::Port::setPortJobFrames(uint32_t frames)
{
    log << "setPortJobFrames( " << frames << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_JOB_FRAMES;
    control.value = frames;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set job frames=%u.", frames);
    }
}

void Codec::Port::setH264EncBandwidth(uint32_t bw)
{
    log << "setH264EncBandwidth( " << bw << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_BANDWIDTH_LIMIT;
    control.value = bw;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set H264 bw=%u.", bw);
    }
}

void Codec::Port::setHEVCEncEntropySync(uint32_t es)
{
    log << "setHEVCEncEntropySync( " << es << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_ENTROPY_SYNC;
    control.value = es;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set HEVC es=%u.", es);
    }
}

void Codec::Port::setHEVCEncTemporalMVP(uint32_t tmvp)
{
    log << "setHEVCEncTemporalMVP( " << tmvp << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_TEMPORAL_MVP;
    control.value = tmvp;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set HEVC tmvp=%u.", tmvp);
    }
}

void Codec::Port::setEncStreamEscaping(uint32_t sesc)
{
    log << "setEncStreamEscaping( " << sesc << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_STREAM_ESCAPING;
    control.value = sesc;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set encoding sesc=%u.", sesc);
    }
}

void Codec::Port::setEncHorizontalMVSearchRange(uint32_t hmvsr)
{
    log << "setEncHorizontalMVSearchRange( " << hmvsr << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MPEG_VIDEO_MV_H_SEARCH_RANGE;
    control.value = hmvsr;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set encoding hmvsr=%u.", hmvsr);
    }
}

void Codec::Port::setEncVerticalMVSearchRange(uint32_t vmvsr)
{
    log << "setEncVerticalMVSearchRange( " << vmvsr << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MPEG_VIDEO_MV_V_SEARCH_RANGE;
    control.value = vmvsr;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set encoding vmvsr=%u.", vmvsr);
    }
}

void Codec::Port::setVP9EncTileCR(uint32_t tcr)
{
    log << "setVP9EncTileCR( " << tcr << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_TILE_COLS;
    control.value = tcr;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set VP9 tile cols=%u.", tcr);
    }

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_TILE_ROWS;
    control.value = tcr;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set VP9 tile rows=%u.", tcr);
    }
}

void Codec::Port::setVP9ProbUpdateMode(uint32_t prob)
{
    log << "setVP9ProbUpdateMode( " << prob << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_VP9_PROB_UPDATE;
    control.value = prob;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set VP9 prob update mode=%u.", prob);
    }
}

void Codec::Port::setJPEGEncRefreshInterval(uint32_t r)
{
    log << "setJPEGEncRefreshInterval( " << r << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_JPEG_RESTART_INTERVAL;
    control.value = r;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set JPEG refresh interval=%u.", r);
    }
}

void Codec::Port::setJPEGHufftable(struct v4l2_mvx_huff_table * table)
{
    log << "setJPEGHufftable type( " << table->type<< ")" << endl;

    struct v4l2_mvx_huff_table huff_table;
    memcpy(&huff_table, table, sizeof(struct v4l2_mvx_huff_table));
    int ret = ioctl(fd, VIDIOC_S_MVX_HUFF_TABLE, &huff_table);
    if (ret != 0)
    {
        throw Exception("Failed to set JPEG huff table.");
    }
}

void Codec::Port::setSeamlessTarget(struct v4l2_mvx_seamless_target * seamless)
{
    log << "setSeamlessTarget seamless_mode( " << seamless->seamless_mode<< ")" << endl;
    log << "setSeamlessTarget target_width( " << seamless->target_width<< ")" << endl;
    log << "setSeamlessTarget target_height( " << seamless->target_height<< ")" << endl;
    log << "setSeamlessTarget target_stride( " << seamless->target_stride[0]<< ")" << endl;
    log << "setSeamlessTarget target_stride( " << seamless->target_stride[1]<< ")" << endl;
    log << "setSeamlessTarget target_stride( " << seamless->target_stride[2]<< ")" << endl;
    log << "setSeamlessTarget target_stride( " << seamless->target_size[0]<< ")" << endl;
    log << "setSeamlessTarget target_stride( " << seamless->target_size[1]<< ")" << endl;
    log << "setSeamlessTarget target_stride( " << seamless->target_size[2]<< ")" << endl;

    memcpy(&this->seamless, seamless, sizeof(struct v4l2_mvx_seamless_target));

    int ret = ioctl(fd, VIDIOC_S_MVX_SEAMLESS_TARGET, &this->seamless);
    if (ret != 0)
    {
        throw Exception("Failed to set Seamless Target.");
    }
}
void Codec::Port::setJPEGEncQuality(uint32_t q)
{
    log << "setJPEGEncQuality( " << q << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
    control.value = q;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set JPEG compression quality=%u.", q);
    }
}

void Codec::Port::setJPEGEncQualityLuma(uint32_t q)
{
    log << "setJPEGEncQualityLuma( " << q << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_JPEG_QUALITY_LUMA;
    control.value = q;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set JPEG compression luma quality=%u.", q);
    }
}

void Codec::Port::setJPEGEncQualityChroma(uint32_t q)
{
    log << "setJPEGEncQualityChroma( " << q << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_JPEG_QUALITY_CHROMA;
    control.value = q;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set JPEG compression chroma quality=%u.", q);
    }
}

void Codec::Port::setRotation(int rotation)
{
    this->rotation = rotation;
    io->setRotation(rotation);
}

void Codec::Port::setMirror(int mirror)
{
    this->mirror = mirror;
}

void Codec::Port::setDownScale(int scale)
{
    this->scale = scale;
}

void Codec::Port::setDSLFrame(int width, int height)
{
    log<<"setDSLFrame( "<<width<<" ,"<<height<<")"<<endl;

    dsl_width = width;
    dsl_height = height;

    return;
}

void Codec::Port::enableDSLFrame()
{
    if (dsl_width > 0 && dsl_height > 0)
    {
        struct v4l2_selection sel;
        sel.type = type;
        sel.target = V4L2_SEL_TGT_COMPOSE;
        sel.flags = 0;
        sel.r.left = 0;
        sel.r.top = 0;
        sel.r.width = dsl_width;
        sel.r.height = dsl_height;
        if (-1 == ioctl(fd, VIDIOC_S_SELECTION, &sel))
        {
            throw Exception("Failed to set DownScale.");
        }
    }

    return;
}

void Codec::Port::setDSLRatio(int hor, int ver)
{
    log<<"setDSLRatio( "<<hor<<" ,"<<ver<<")"<<endl;

    struct v4l2_mvx_dsl_ratio dsl_ratio;
    memset(&dsl_ratio, 0, sizeof(dsl_ratio));
    dsl_ratio.hor = hor;
    dsl_ratio.ver = ver;
    int ret = ioctl(fd, VIDIOC_S_MVX_DSL_RATIO, &dsl_ratio);
    if (ret != 0)
    {
        throw Exception("Failed to set DSL frame hor/ver.");
    }

    return;
}

void Codec::Port::setDSLMode(int mode)
{
    log<<"setDSLMode("<<mode<<")"<<endl;
    int dsl_pos_mode = mode;
    int ret = ioctl(fd, VIDIOC_S_MVX_DSL_MODE, &dsl_pos_mode);
    if (ret != 0) {
        throw Exception("Failed to set dsl mode.");
    }
}

void Codec::Port::setDSLInterpMode(int mode)
{
    log << "setDSLInterpMode (" << mode << ")" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_DSL_INTERP_MODE;
    control.value = mode;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set dsl interpolation mode=%u.", control.value);
    }
}

void Codec::Port::enableDualAfbcDownScale()
{
    if (enable_dual_afbc_downscale)
    {
        struct v4l2_selection sel;
        sel.type = type;
        sel.target = V4L2_SEL_TGT_COMPOSE;
        sel.flags = 0;
        sel.r.left = 0;
        sel.r.top = 0;
        sel.r.width = 16; // just set to a small resolution so driver can detect downscaling request
        sel.r.height = 16;
        if (-1 == ioctl(fd, VIDIOC_S_SELECTION, &sel))
        {
            throw Exception("Failed to set Dual Afbc DownScale.");
        }
    }
}
 
void Codec::Port::setDualAfbcDownScale(int enable)
{
    log << "setDualAfbcDownScale (" << enable << ")" << endl;
    enable_dual_afbc_downscale = enable;
}

void Codec::Port::setDisabledFeatures(int val)
{
    log << "setDisabledFeatures (" << val << ")" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_DISABLED_FEATURES;
    control.value = val;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set disabled features=%u.", control.value);
    }
}

void Codec::Port::setColorConversion(uint32_t mode)
{
    log << "setColorConversion (" << mode << ")" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_COLOR_CONVERSION;
    control.value = mode;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setColorConversion mode =%u.", control.value);
    }
}

void Codec::Port::setRGBToYUVMode(uint32_t mode)
{
    log << "setRGBToYUVMode (" << mode << ")" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_RGB_TO_YUV_MODE;
    control.value = mode;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setRGBToYUVMode mode =%u.", control.value);
    }
}

void Codec::Port::setCustColorConvCoef(struct v4l2_mvx_color_conv_coef * color_conv_coef)
{
    log << "setCustColorConvCoef coef[0][0]( " << color_conv_coef->coef[0][0]<< ")" << endl;
    log << "setCustColorConvCoef coef[0][1]( " << color_conv_coef->coef[0][1]<< ")" << endl;
    log << "setCustColorConvCoef coef[0][2]( " << color_conv_coef->coef[0][2]<< ")" << endl;
    log << "setCustColorConvCoef coef[1][0]( " << color_conv_coef->coef[1][0]<< ")" << endl;
    log << "setCustColorConvCoef coef[1][1]( " << color_conv_coef->coef[1][1]<< ")" << endl;
    log << "setCustColorConvCoef coef[1][2]( " << color_conv_coef->coef[1][2]<< ")" << endl;
    log << "setCustColorConvCoef coef[2][0]( " << color_conv_coef->coef[2][0]<< ")" << endl;
    log << "setCustColorConvCoef coef[2][1]( " << color_conv_coef->coef[2][1]<< ")" << endl;
    log << "setCustColorConvCoef coef[2][2]( " << color_conv_coef->coef[2][2]<< ")" << endl;
    log << "setCustColorConvCoef offset[0] ( " << color_conv_coef->offset[0]<< ")" << endl;
    log << "setCustColorConvCoef offset[1] ( " << color_conv_coef->offset[1]<< ")" << endl;
    log << "setCustColorConvCoef offset[2] ( " << color_conv_coef->offset[2]<< ")" << endl;

    int ret = ioctl(fd, VIDIOC_S_MVX_COLOR_CONV_COEF, color_conv_coef);
    if (ret != 0)
    {
        throw Exception("Failed to set Cust Color Conv Coef.");
    }
}

void Codec::Port::setRGBConvertYUV(struct v4l2_mvx_rgb2yuv_color_conv_coef * color_conv_coef)
{
    int ret = ioctl(fd, VIDIOC_S_MVX_RGB2YUV_COLOR_CONV_COEF, color_conv_coef);
    if (ret != 0)
    {
        throw Exception("Failed to set RGB To YUV Conv Coef.");
    }
}

void Codec::Port::setDecDstCrop(struct v4l2_rect * dst_crop)
{
    log << "setDecDstCrop       x( " << dst_crop->left<< ")" << endl;
    log << "setDecDstCrop       y( " << dst_crop->top<< ")" << endl;
    log << "setDecDstCrop   width( " << dst_crop->width<< ")" << endl;
    log << "setDecDstCrop  height( " << dst_crop->height<< ")" << endl;

    struct v4l2_selection sel;
    sel.type = type;
    sel.target = V4L2_SEL_TGT_CROP;
    sel.flags = 0;
    sel.r = *dst_crop;
    int ret = ioctl(fd, VIDIOC_S_SELECTION, &sel);
    if (ret != 0)
    {
        throw Exception("Failed to setDecDstCrop.");
    }
}


void Codec::Port::setVisibleWidth(uint32_t v_width)
{
    log<<"setVisibleWidth("<<v_width<<")"<<endl;
    struct v4l2_control control;
    if (v_width > io->getWidth()) {
        cerr<<"visible width should not greater than frame width!"<<endl;
        return;
    }
    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_VISIBLE_WIDTH;
    control.value = v_width;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set visible width =%u.", v_width);
    }

}

void Codec::Port::setVisibleHeight(uint32_t v_height)
{
    log<<"setVisibleHeight("<<v_height<<")"<<endl;
    struct v4l2_control control;
    if (v_height > io->getHeight()) {
        cerr<<"visible height should not greater than frame height!"<<endl;
        return;
    }

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_VISIBLE_HEIGHT;
    control.value = v_height;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set visible height =%u.", v_height);
    }
}

void Codec::Port::setLongTermRef(uint32_t mode, uint32_t period)
{
    log<<"setLongTermRef( "<<mode<<" ,"<<period<<")"<<endl;
    struct v4l2_mvx_long_term_ref ltr;
    memset(&ltr, 0, sizeof(ltr));
    ltr.mode = mode;
    ltr.period = period;
    int ret = ioctl(fd, VIDIOC_S_MVX_LONG_TERM_REF, &ltr);
    if (ret != 0)
    {
        throw Exception("Failed to set long term mode/period.");
    }

}

void Codec::Port::setMiniCnt(uint32_t cnt)
{
    log<<"set miniFrameBuffer cnt ( "<<cnt<<" )"<<endl;
    mini_frame_cnt = cnt;
    int ret = ioctl(fd, VIDIOC_S_MVX_MINI_FRAME_CNT, &mini_frame_cnt);
    if (ret != 0) {
        throw Exception("Failed to mini Frame Buf cnt.");
    }
}

void Codec::Port::setFrameCount(int frames)
{
    this->frames_count = frames;
}

void Codec::Port::setStatsMode(int mode, int index){
    int stats_mode = mode;

    struct v4l2_buffer_param_enc_stats stats;
    memset(&stats, 0, sizeof(struct v4l2_buffer_param_enc_stats));
    stats.pic_index_or_mb_size = index;
    stats.mms_buffer_size = stats_mode & 0x1? mms_buffer_size : 0;
    stats.bitcost_buffer_size = stats_mode & 0x2? bitcost_buffer_size : 0;
    stats.qp_buffer_size = stats_mode & 0x4? qp_buffer_size : 0;
    stats.flags |= stats_mode & 0x1? V4L2_BUFFER_ENC_STATS_FLAG_MMS     : 0;
    stats.flags |= stats_mode & 0x2? V4L2_BUFFER_ENC_STATS_FLAG_BITCOST : 0;
    stats.flags |= stats_mode & 0x4? V4L2_BUFFER_ENC_STATS_FLAG_QP      : 0;

    int ret = ioctl(fd, VIDIOC_S_MVX_STATS_MODE, &stats);
    if (ret != 0)
    {
        throw Exception("Failed to set color description.");
    }

    return;
}

void Codec::Port::setOsdCfg(struct v4l2_osd_config osd){
    log << "setOsdCfg, for pic_index " << osd.pic_index << endl;
    struct v4l2_osd_config osd_cfg = osd;
    int ret = ioctl(fd, VIDIOC_S_MVX_OSD_CONFIG, &osd_cfg);
    if (ret != 0)
    {
        throw Exception("Failed to set osd cfg.");
    }
}

void Codec::Port::setEncOSDinfo(struct v4l2_osd_info* info){
    log << "setOsdinfo, osd 1 size " << info->width_osd[0] <<"x"<< info->height_osd[0] << endl;
    log << "setOsdinfo, osd 2 size " << info->width_osd[1] <<"x"<< info->height_osd[1] << endl;
    int ret = ioctl(fd, VIDIOC_S_MVX_OSD_INFO, info);
    if (ret != 0)
    {
        throw Exception("Failed to set osd info.");
    }
}

void Codec::Port::setSeiUserData(struct v4l2_sei_user_data *sei_user_data){
    log << "setSeiUserData( " << sei_user_data->user_data<< ")" << endl;

    int ret = ioctl(fd, VIDIOC_S_MVX_SEI_USERDATA, sei_user_data);
    if (ret != 0)
    {
        throw Exception("Failed to set color description.");
    }

    return;

}

#if KERNEL_VERSION(5, 15, 0) <= LINUX_VERSION_CODE
void Codec::Port::setHDR10Info(struct v4l2_ctrl_hdr10_cll_info *cll,
                        struct v4l2_ctrl_hdr10_mastering_display *mastering){
    uint32_t i = 0;
    struct v4l2_ext_control control[2];
    struct v4l2_ext_controls controls = {
        .which = V4L2_CTRL_WHICH_CUR_VAL,
        .count = 0,
        .controls = control,
    };

    if (cll)
    {
        control[i].id = V4L2_CID_COLORIMETRY_HDR10_CLL_INFO;
        control[i].size = sizeof(*cll);
        control[i].ptr = cll;
        i++;
    }

    if (mastering)
    {
        control[i].id = V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY;
        control[i].size = sizeof(*mastering);
        control[i].ptr = mastering;
        i++;
    }

    controls.count = i;

    int ret = ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls);
    if (ret != 0)
    {
        printf("Failed to set HDR10 info: %d, %d.\n", ret, controls.error_idx);
    }

    return;
}
#endif

void Codec::Port::setColorDescription(uint32_t _colorspace, uint8_t _xfer_func,
                        uint8_t _ycbcr_enc, uint8_t _quantization){
    colorspace = _colorspace;
    xfer_func = _xfer_func;
    ycbcr_enc = _ycbcr_enc;
    quantization = _quantization;
}

void Codec::Port::setHRDBufferSize(int size) {
    log << "setHRDBufferSize( " << size << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_HRD_BUFFER_SIZE;
    control.value = size;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set crop bottom=%u.", size);
    }
}

void Codec::Port::setFrameStride(size_t* stride){
    __stride[0] = stride[0];
    __stride[1] = stride[1];
    __stride[2] = stride[2];
}

void Codec::Port::setRcBitIMode(uint32_t mode)
{
    log << "set mode for I frame of rate control( " << mode << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_RC_I_MODE;
    control.value = mode;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set rc bit i frame mode=%u.", mode);
    }
}

void Codec::Port::setRcBitRationI(uint32_t ratio)
{
    log << "set ratio for I frame of rate control( " << ratio << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_RC_I_RATIO;
    control.value = ratio;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set rc i frame ratio=%u.", ratio);
    }
}

void Codec::Port::setMultiSPSPPS(uint32_t sps_pps)
{
    log << "Support multi SPS PSS for h264 and hevc ( " << sps_pps << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_MULIT_SPS_PPS;
    control.value = sps_pps;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to support multi SPS PSS=%u.", sps_pps);
    }
}

void Codec::Port::setEnableVisual(uint32_t enable)
{
    log << "Enable Visual ( " << enable << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_ENABLE_VISUAL;
    control.value = enable;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to enable visual =%u.", enable);
    }
}

void Codec::Port::setAdaptiveIntraBlock(uint32_t enable)
{
    log << "setAdaptiveIntraBlock (" << enable << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_ENABLE_ADAPTIVE_INTRA_BLOCK;
    control.value = enable;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to adaptive intra block =%u.", enable);
    }
}

void Codec::Port::setEnableSCD(uint32_t scd_enable)
{
    log << "Enable SCD ( " << scd_enable << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_SCD_ENABLE;
    control.value = scd_enable;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to enable SCD =%u.", scd_enable);
    }
}

void Codec::Port::setScdPercent(uint32_t scd_percent)
{
    log << "SCD Percent ( " << scd_percent << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_SCD_PERCENT;
    control.value = scd_percent;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set SCD percent =%u.", scd_percent);
    }
}

void Codec::Port::setScdThreshold(uint32_t scd_threshold)
{
    log << "SCD Threshold ( " << scd_threshold << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_SCD_THRESHOLD;
    control.value = scd_threshold;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set scd threshold =%u.", scd_threshold);
    }
}

void Codec::Port::setEnableAQSsim(uint32_t aq_ssim_en)
{
    log << "Enable AQ SSIM ( " << aq_ssim_en << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_AQ_SSIM_EN;
    control.value = aq_ssim_en;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to enable AQ SSIM =%u.", aq_ssim_en);
    }
}

void Codec::Port::setAQNegRatio(uint32_t aq_neg_ratio)
{
    log << "AQ Negaive Ratio ( " << aq_neg_ratio << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_AQ_NEG_RATIO;
    control.value = aq_neg_ratio;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set AQ negaive ratio =%u.", aq_neg_ratio);
    }
}

void Codec::Port::setAQPosRatio(uint32_t aq_pos_ratio)
{
    log << "AQ Positive Ratio ( " << aq_pos_ratio << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_AQ_POS_RATIO;
    control.value = aq_pos_ratio;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set AQ positive ratio =%u.", aq_pos_ratio);
    }
}

void Codec::Port::setAQQPDeltaLmt(uint32_t aq_qpdelta_lmt)
{
    log << "AQ QPDelta LMT ( " << aq_qpdelta_lmt << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_AQ_QPDELTA_LMT;
    control.value = aq_qpdelta_lmt;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set AQ QPDelta LMT =%u.", aq_qpdelta_lmt);
    }
}

void Codec::Port::setAQInitFrmAvgSvar(uint32_t aq_init_frm_avg_svar)
{
    log << "Initial Frame Variance ( " << aq_init_frm_avg_svar << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_AQ_INIT_FRM_AVG_SVAR;
    control.value = aq_init_frm_avg_svar;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to initial frame variance =%u.", aq_init_frm_avg_svar);
    }
}

void Codec::Port::setIntermediateBufSize(uint32_t size)
{
    log << "setIntermediateBufSize( " << size << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_INTER_MED_BUF_SIZE;
    control.value = size;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setIntermediateBufSize=%u.", size);
    }
}

void Codec::Port::setSvct3Level1Period(uint32_t period)
{
    log << "setSvct3Level1Period( " << period << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_SVCT3_LEVEL1_PERIOD;
    control.value = period;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setSvct3Level1Period=%u.", period);
    }
}

void Codec::Port::setGopResetPframes(int pframes)
{
    log << "setGopResetPframes( " << pframes << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_GOP_RESET_PFRAMES;
    control.value = pframes;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setGopResetPframes=%u.", pframes);
    }

}

void Codec::Port::setLtrResetPeriod(int period)
{
    log << "setLtrResetPeriod( " << period << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_LTR_RESET_PERIOD;
    control.value = period;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setLtrResetPeriod=%u.", period);
    }

}

void Codec::Port::setGDRnumber(uint32_t numbder)
{
    log << "setGDRnumber( " << numbder << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_GDR_NUMBER;
    control.value = numbder;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setGDRnumber=%u.", numbder);
    }
}

void Codec::Port::setGDRperiod(uint32_t period)
{
    log << "setGDRperiod( " << period << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_GDR_PERIOD;
    control.value = period;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setGDRperiod=%u.", period);
    }
}

void Codec::Port::setEncIntraIpenaltyAngular(uint32_t value)
{
    log << "setEncIntraIpenaltyAngular( " << value << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_ENC_INTRA_IPENALTY_ANGULAR;
    control.value = value;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setEncIntraIpenaltyAngular=%u.", value);
    }
}

void Codec::Port::setEncIntraIpenaltyPlanar(uint32_t value)
{
    log << "setEncIntraIpenaltyPlanar( " << value << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_ENC_INTRA_IPENALTY_PLANAR;
    control.value = value;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setEncIntraIpenaltyPlanar=%u.", value);
    }
}

void Codec::Port::setEncIntraIpenaltyDc(uint32_t value)
{
    log << "setEncIntraIpenaltyDc( " << value << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_ENC_INTRA_IPENALTY_DC;
    control.value = value;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setEncIntraIpenaltyDc=%u.", value);
    }
}

void Codec::Port::setEncInterIpenaltyAngular(uint32_t value)
{
    log << "setEncInterIpenaltyAngular( " << value << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_ENC_INTER_IPENALTY_ANGULAR;
    control.value = value;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setEncInterIpenaltyAngular=%u.", value);
    }
}

void Codec::Port::setEncInterIpenaltyPlanar(uint32_t value)
{
    log << "setEncInterIpenaltyPlanar( " << value << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_ENC_INTER_IPENALTY_PLANAR;
    control.value = value;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setEncInterIpenaltyPlanar=%u.", value);
    }
}

void Codec::Port::setEncInterIpenaltyDc(uint32_t value)
{
    log << "setEncInterIpenaltyDc( " << value << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_ENC_INTER_IPENALTY_DC;
    control.value = value;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setEncInterIpenaltyDc=%u.", value);
    }
}

void Codec::Port::setForcedUVvalue(uint32_t uv_value)
{
    log << "setForcedUVvalue( " << uv_value << " )" << endl;

    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_FORCED_UV_VALUE;
    control.value = uv_value;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to setForcedUVvalue=%u.", uv_value);
    }
}

void Codec::Port::setEncCrop(struct v4l2_rect * _crop)
{
    const char *dir = V4L2_TYPE_IS_OUTPUT(type) ? "Src" : "Dst";
    log << "setEncCrop " << dir << endl;
    log << "setEncCrop       x( " << _crop->left<< ")" << endl;
    log << "setEncCrop       y( " << _crop->top<< ")" << endl;
    log << "setEncCrop   width( " << _crop->width<< ")" << endl;
    log << "setEncCrop  height( " << _crop->height<< ")" << endl;

    crop = *_crop;
}

void Codec::Port::enableEncCrop()
{
    if (crop.left >= 0 && crop.top >= 0 && crop.width > 0 && crop.height > 0) {
        struct v4l2_selection sel;
        sel.type = type;
        sel.target = V4L2_TYPE_IS_OUTPUT(type) ?
                    V4L2_SEL_TGT_CROP : V4L2_SEL_TGT_COMPOSE;
        sel.flags = 0;
        sel.r = crop;
        int ret = ioctl(fd, VIDIOC_S_SELECTION, &sel);
        if (ret != 0)
        {
            throw Exception("Failed to setEnc%sCrop.",
                            V4L2_TYPE_IS_OUTPUT(type) ? "Src" : "Dst");
        }
    }
}

void Codec::Port::setPortStatsSize(uint32_t mms, uint32_t bc, uint32_t qp)
{
    mms_buffer_size = mms;
    bitcost_buffer_size = bc;
    qp_buffer_size = qp;
}

void Codec::Port::setFsfMode(int val)
{
    log << "fsf (" << val << ")" << endl;
    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_AV1_FSF;
    control.value = val;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set fsf mode for av1 =%u.", control.value);
    }
}

void Codec::Port::setPriority(int val)
{
    log << "Priority (" << val << ")" << endl;
    struct v4l2_control control;

    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_MVE_VIDEO_PRIORITY;
    control.value = val;

    if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control))
    {
        throw Exception("Failed to set priority to %d.", control.value);
    }
}

void Codec::Port::setEncLambdaScale(struct v4l2_mvx_lambda_scale * lambda_scale)
{
    log << "setEncLambdaScale lambda_scale_i_q8( " << lambda_scale->lambda_scale_i_q8<< ")" << endl;
    log << "setEncLambdaScale lambda_scale_sqrt_i_q8( " << lambda_scale->lambda_scale_sqrt_i_q8<< ")" << endl;
    log << "setEncLambdaScale lambda_scale_p_q8( " << lambda_scale->lambda_scale_p_q8<< ")" << endl;
    log << "setEncLambdaScale lambda_scale_sqrt_p_q8( " << lambda_scale->lambda_scale_sqrt_p_q8<< ")" << endl;
    log << "setEncLambdaScale lambda_scale_b_ref_q8( " << lambda_scale->lambda_scale_b_ref_q8<< ")" << endl;
    log << "setEncLambdaScale lambda_scale_sqrt_b_ref_q8( " << lambda_scale->lambda_scale_sqrt_b_ref_q8<< ")" << endl;
    log << "setEncLambdaScale lambda_scale_b_nonref_q8( " << lambda_scale->lambda_scale_b_nonref_q8<< ")" << endl;
    log << "setEncLambdaScale lambda_scale_sqrt_b_nonref_q8( " << lambda_scale->lambda_scale_sqrt_b_nonref_q8<< ")" << endl;

    int ret = ioctl(fd, VIDIOC_S_MVX_ENC_LAMBDA_SCALE, lambda_scale);
    if (ret != 0)
    {
        throw Exception("Failed to setEncLambdaScale.");
    }
}

void Codec::flush()
{
    streamoff();
    streamon();
}

void Codec::requeue()
{
    resetBuffers();
    queueBuffers();
}

void Codec::runPoll()
{
    bool eos = false;

    while (!eos)
    {
        struct pollfd p = {
            .fd = fd, .events = POLLPRI
        };

        if (input.pending > 0)
        {
            p.events |= POLLOUT;
        }

        if (output.pending > 0)
        {
            p.events |= POLLIN;
        }

        int ret = poll(&p, 1, 600000);
        if (ret < 0)
        {
            if (errno == EAGAIN)
                continue;
            else
                throw Exception("Poll returned error code.");
        }

        if (p.revents & POLLERR)
        {
            throw Exception("Poll returned error event.");
        }

        if (ret == 0)
        {
            throw Exception("Poll timed out.");
        }

        if (p.revents & POLLOUT)
        {
            Input *is = dynamic_cast<Input*>(input.io);
            if (is->frame_bytes.size()) {
                usleep(1 * 1000000 / LOW_LATENCY_FPS);
            }
            input.handleBuffer();
        }
        if (p.revents & POLLIN)
        {
            if (csweo)
            {
                log << "Changing settings while encoding." << endl;
                if (fps_n != 0 && fps_d !=0)
                {
                    output.setEncFramerate(fps_n, fps_d);
                    fps_n = fps_d = 0;
                }
                if (bps != 0)
                {
                    /* output.setEncBitrate(bps); */
                    output.setEncBitrate(0); /* only for coverage */
                    bps = 0;
                }
                /* Set maxQP before minQP, otherwise FW rejects */
                if (maxqp != 0)
                {
                    output.setEncMaxQP(maxqp);
                    maxqp = 0;
                }
                if (minqp != 0)
                {
                    output.setEncMinQP(minqp);
                    minqp = 0;
                }
                if (fixedqp != 0)
                {
                    output.setEncFixedQP(fixedqp);
                    fixedqp = 0;
                }

                csweo = false;
            }

            eos = output.handleBuffer();
            seek();
        }
        else if (p.revents & POLLPRI)
        {
            handleEvent();
        }
    }
}

void Codec::runThreads()
{
    int ret;
    void *retval;

    ret = pthread_create(&input.tid, NULL, runThreadInput, this);
    if (ret != 0)
    {
        throw Exception("Failed to create input thread.");
    }

    ret = pthread_create(&output.tid, NULL, runThreadOutput, this);
    if (ret != 0)
    {
        throw Exception("Failed to create output thread.");
    }

    pthread_join(input.tid, &retval);
    pthread_join(output.tid, &retval);
}

void *Codec::runThreadInput(void *arg)
{
    Codec *_this = static_cast<Codec *>(arg);
    bool eos = false;

    while (!eos && _this->outputThreadDone == false)
        eos = _this->input.handleBuffer();

    return NULL;
}

void *Codec::runThreadOutput(void *arg)
{
    Codec *_this = static_cast<Codec *>(arg);
    bool eos = false;

    while (!eos)
        eos = _this->output.handleBuffer();

    _this->outputThreadDone = true;

    return NULL;
}

bool Codec::Port::handleBuffer()
{
    Buffer &buffer = dequeueBuffer();
    v4l2_buffer &b = buffer.getBuffer();
    if (io->getDir() == 0 && isInputThread) {
        appendInputBufferIdx(&input_producer_mutex, &input_producer_cond, &input_producer_queue, b.index);
    }

    buffer.dmaMemorySync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ, io->isExtInput());
    io->finalize(buffer);
    if (io->eof())
    {
        if (tryDecStop)
        {
            sendDecCommand(V4L2_DEC_CMD_STOP);
        }
        return true;
    }

    /* EOS on capture port. */
    if (!V4L2_TYPE_IS_OUTPUT(b.type) && b.flags & V4L2_BUF_FLAG_LAST)
    {
        log << "Capture EOS." << endl;
        return true;
    }

    /* Resolution change. we should only handle this on decode output:V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE*/
    if (!V4L2_TYPE_IS_OUTPUT(b.type) && V4L2_TYPE_IS_MULTIPLANAR(b.type) &&
            (getBytesUsed(b) == 0 ||
            (b.flags & V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT) != V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT) &&
            (b.flags & V4L2_BUF_FLAG_ERROR) == 0)
    {
        if ((b.flags & V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC) == V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC)
            return false;
    }

    /* Remove vendor custom flags. */
    //decoder specfied frames count to be processed
    if (io->getDir() == 1) {
        if (V4L2_TYPE_IS_MULTIPLANAR(b.type)) {
            if ((b.flags & V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT) == V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT)
                frames_processed++;
        } else {
            frames_processed++;
        }
    }
    buffer.resetVendorFlags();
    if (io->getDir() == 1 && frames_count > 0 && frames_processed >= frames_count) {
        if (io->isDisplay()) {
            if (prev_buffer_index != 0xFFFFFFFF) {
                Buffer &prev_buffer = *(buffers.at(prev_buffer_index));
                prev_buffer.clearBytesUsed();
                prev_buffer.setEndOfStream(true);
                queueBuffer(prev_buffer);
            }
            prev_buffer_index = b.index;
        } else {
            buffer.clearBytesUsed();
            buffer.setEndOfStream(true);
            queueBuffer(buffer);
        }
        return true;
    } else {
        if (io->getDir() == 0 && isInputThread) {
            uint32_t index = getInputBufferIdx(&input_consumer_mutex, &input_consumer_cond, &input_consumer_queue);
            Buffer& buffer_input = *(buffers.at(index));
            buffer_input.setEndOfFrame(io->eof());
            queueBuffer(buffer_input);
            if (io->eof())
            {
                sendEncStopCommand();
            }
            return false;
        } else if (io->getDir() == 0 && io->isSkipRead() == true) {
            io->prepare(buffer);
            buffer.setEndOfStream(io->eof());
            queueBuffer(buffer);
        } else {
            buffer.dmaMemorySync(DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ, io->isExtInput());
            io->prepare(buffer);
            buffer.dmaMemorySync(DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ, io->isExtInput());
            if (io->isDisplay()) {
                if (prev_buffer_index != 0xFFFFFFFF) {
                    Buffer &prev_buffer = *(buffers.at(prev_buffer_index));
                    prev_buffer.setEndOfStream(io->eof());
                    queueBuffer(prev_buffer);
                }
                prev_buffer_index = b.index;
            } else {
                buffer.setEndOfStream(io->eof());
                queueBuffer(buffer);
            }
        }
    }

    if (io->eof())
    {
        sendEncStopCommand();
    }

    return false;
}

void Codec::Port::updateIoResolution()
{
    if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
        io->setWidth(format.fmt.pix_mp.width);
        io->setHeight(format.fmt.pix_mp.height);
    } else {
        io->setWidth(format.fmt.pix.width);
        io->setHeight(format.fmt.pix.height);
    }
}

bool Codec::Port::checkResolutionChange()
{
    v4l2_format fmt = format;
    getFormat();
    printFormat(format);
    bool isResChange = true;
    uint32_t alloc_width_align = 0;
    uint32_t alloc_height_align = 0;
    if (V4L2_TYPE_IS_MULTIPLANAR(fmt.type))
    {
        struct v4l2_pix_format_mplane &f = fmt.fmt.pix_mp;
        if (Codec::isAFBC(format.fmt.pix_mp.pixelformat)) {
            alloc_width_align = crop.left;
            alloc_height_align = crop.top;
        }

        isResChange = ((alloc_width + alloc_width_align) * (alloc_height + alloc_height_align) < format.fmt.pix_mp.width * format.fmt.pix_mp.height);
        isResChange |= (f.pixelformat != format.fmt.pix_mp.pixelformat);
        isResChange |= (alloc_afbc_bytes < format.fmt.pix_mp.plane_fmt[0].sizeimage) && (Codec::isAFBC(format.fmt.pix_mp.pixelformat));
    } else {
        struct v4l2_pix_format &f = fmt.fmt.pix;
        if (Codec::isAFBC(format.fmt.pix.pixelformat)) {
            alloc_width_align = crop.left;
            alloc_height_align = crop.top;
        }

        isResChange = ((alloc_width + alloc_width_align) * (alloc_height + alloc_height_align) < format.fmt.pix.width * format.fmt.pix.height);
        isResChange |= (f.pixelformat != format.fmt.pix.pixelformat);
        isResChange |= (alloc_afbc_bytes < format.fmt.pix.sizeimage) && (Codec::isAFBC(format.fmt.pix.pixelformat));
    }
    isResChange |= (dsl_width > 0 && dsl_height > 0);

    log << "Resolution changed:" << isResChange <<endl;

    return isResChange;
}

void Codec::Port::handleResolutionChange()
{
    crop = getCrop();
    if (!checkResolutionChange()) {
        updateIoResolution();
        allocateBuffers(getRequiredBufferCount(), memory_type_port, true);
        queueBuffers();
        sendDecCommand(V4L2_DEC_CMD_START);
        return;
    }
    allocateBuffers(0, memory_type_port);
    streamoff();
    setPixelFormat();
    updateIoResolution();
    allocateBuffers(getRequiredBufferCount(), memory_type_port);
    queueBuffers();
    //frames_processed = 0;
    streamon();
}

int Codec::Port::seek(int offset)
{
    if (isInputThread) {
        cerr << "WARNING: Flush is not supported in input thread mode. Skipping." << endl;
        return -1;
    }

    if (io->seekToKeyFrame(offset) != 0) {
        cerr << "WARNING: Failed to seek" << endl;
        return -1;
    }

    return 0;
}

bool Codec::handleEvent()
{
    struct v4l2_event event;
    int ret;

    ret = ioctl(fd, VIDIOC_DQEVENT, &event);
    if (ret != 0)
    {
        throw Exception("Failed to dequeue event.");
    }

    log << "Event. type=" << event.type << "." << endl;

    if (event.type == V4L2_EVENT_SOURCE_CHANGE)
    {
#if KERNEL_VERSION(5, 15, 0) <= LINUX_VERSION_CODE
        getHDR10Info();
#endif
        if (event.u.src_change.changes == V4L2_EVENT_SRC_CH_RESOLUTION)
            output.handleResolutionChange();
    }

    if (event.type == V4L2_EVENT_EOS)
    {
        return true;
    }

    return false;
}

void Codec::getStride(uint32_t format, size_t &nplanes, size_t stride[3][2])
{
    switch (format)
    {
        case V4L2_PIX_FMT_YUV420M:
            nplanes = 3;
            stride[0][0] = 2;
            stride[0][1] = 2;
            stride[1][0] = 1;
            stride[1][1] = 1;
            stride[2][0] = 1;
            stride[2][1] = 1;
            break;
        case V4L2_PIX_FMT_NV12M:
        case V4L2_PIX_FMT_NV21M:
            nplanes = 2;
            stride[0][0] = 2;
            stride[0][1] = 2;
            stride[1][0] = 2;
            stride[1][1] = 1;
            stride[2][0] = 0;
            stride[2][1] = 0;
            break;
        case V4L2_PIX_FMT_P010M:
        case V4L2_PIX_FMT_YUV420_2P_10:
            nplanes = 2;
            stride[0][0] = 4;
            stride[0][1] = 2;
            stride[1][0] = 4;
            stride[1][1] = 1;
            stride[2][0] = 0;
            stride[2][1] = 0;
            break;
        case V4L2_PIX_FMT_Y0L2:
            nplanes = 1;
            stride[0][0] = 8;
            stride[0][1] = 1;
            stride[1][0] = 0;
            stride[1][1] = 0;
            stride[2][0] = 0;
            stride[2][1] = 0;
            break;
        case V4L2_PIX_FMT_YUYV:
        case V4L2_PIX_FMT_UYVY:
            nplanes = 1;
            stride[0][0] = 4;
            stride[0][1] = 2;
            stride[1][0] = 0;
            stride[1][1] = 0;
            stride[2][0] = 0;
            stride[2][1] = 0;
            break;
        case V4L2_PIX_FMT_Y210:
        case V4L2_PIX_FMT_YUV422_1P_10:
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_RGBA8888:
        case DRM_FORMAT_BGRA8888:
            nplanes = 1;
            stride[0][0] = 8;
            stride[0][1] = 2;
            stride[1][0] = 0;
            stride[1][1] = 0;
            stride[2][0] = 0;
            stride[2][1] = 0;
            break;
        case V4L2_PIX_FMT_RGB24:
        case V4L2_PIX_FMT_BGR24:
            nplanes = 1;
            stride[0][0] = 6;
            stride[0][1] = 2;
            stride[1][0] = 0;
            stride[1][1] = 0;
            stride[2][0] = 0;
            stride[2][1] = 0;
            break;
        case V4L2_PIX_FMT_RGB_3P:
        case V4L2_PIX_FMT_YUV444M:
            nplanes = 3;
            stride[0][0] = 2;
            stride[0][1] = 2;
            stride[1][0] = 2;
            stride[1][1] = 2;
            stride[2][0] = 2;
            stride[2][1] = 2;
            break;
        case V4L2_PIX_FMT_GREY:
            nplanes = 1;
            stride[0][0] = 2;
            stride[0][1] = 2;
            stride[1][0] = 0;
            stride[1][1] = 0;
            stride[2][0] = 0;
            stride[2][1] = 0;
            break;
        case V4L2_PIX_FMT_Y10_LE:
            nplanes = 1;
            stride[0][0] = 4;
            stride[0][1] = 2;
            stride[1][0] = 0;
            stride[1][1] = 0;
            stride[2][0] = 0;
            stride[2][1] = 0;
            break;
        case V4L2_PIX_FMT_YUV444_10:
            nplanes = 3;
            stride[0][0] = 4;
            stride[0][1] = 2;
            stride[1][0] = 4;
            stride[1][1] = 2;
            stride[2][0] = 4;
            stride[2][1] = 2;
            break;
        case V4L2_PIX_FMT_YUV420_I420_10:
            nplanes = 3;
            stride[0][0] = 4;
            stride[0][1] = 2;
            stride[1][0] = 2;
            stride[1][1] = 1;
            stride[2][0] = 2;
            stride[2][1] = 1;
            break;
        case V4L2_PIX_FMT_ARGB555:
        case V4L2_PIX_FMT_ARGB444:
        case V4L2_PIX_FMT_RGB565:
            nplanes = 1;
            stride[0][0] = 4;
            stride[0][1] = 2;
            stride[1][0] = 0;
            stride[1][1] = 0;
            stride[2][0] = 0;
            stride[2][1] = 0;
            break;
        default:
            throw Exception("Unsupported buffer format.");
    }
}

size_t Codec::getSize(uint32_t format, size_t width, size_t height,
                      size_t strideAlign, size_t &nplanes, size_t stride[3], size_t size[3], size_t heights[3])
{
    size_t s[3][2];
    size_t frameSize = 0;
    size_t tmp_witdh, tmp_height;
    getStride(format, nplanes, s);

    for (int i = 0; i < 3; ++i)
    {
        tmp_witdh = i == 0? width : roundUp(width, 2);
        tmp_height = i == 0? height : roundUp(height, 2);
        stride[i] = max(stride[i], roundUp(divRoundUp(tmp_witdh * s[i][0], 2), divRoundUp(strideAlign * s[i][0], s[0][0])));
        heights[i] = divRoundUp(tmp_height * s[i][1], 2);
        size[i] = stride[i] * heights[i];//divRoundUp(height * stride[i] * s[i][1], 4);
        frameSize += size[i];
    }

    return frameSize;
}

Uevent::Uevent(void *buf, int size, void *data)
{
#ifdef ENABLE_DRM
    secvid_session = static_cast<struct cix_secvid_session *>(data);
#endif
    parseEvent(buf, size);

    return;
}

int Uevent::parseEvent(void *buf, int size)
{
    char *e = (char *)buf;
    int i = 0;

    action = UEVENT_ACTION_UNKNOWN;
    devpath = NULL;
    subsystem = NULL;
    type = UEVENT_TYPE_UNKNOWN;
    memset(&msg, 0, sizeof(msg));

    if (strncmp(e, "add", 3))
        return 0;

    i = strlen(e) + 1;
    while (i < size) {
        char *entry = e+i;
        printf("%s\n", entry);
        if (!strncmp(entry, "ACTION=", 7)) {
            if (!strncmp(entry+7, "add", 3))
                action = UEVENT_ACTION_ADD;
        } else if (!strncmp(entry, "DEVPATH=", 8)) {
            devpath = entry+8;
        } else if (!strncmp(entry, "SUBSYSTEM=", 10)) {
            subsystem = entry+10;
        } else if (!strncmp(entry, "TYPE=", 5)) {
            if (!strncmp(entry+5, "firmware", 8))
                type = UEVENT_TYPE_FIRMWARE;
            else if (!strncmp(entry+5, "memory", 6))
                type = UEVENT_TYPE_MEMORY;
            else if (!strncmp(entry+5, "hardware", 8))
                type = UEVENT_TYPE_HARDWARE;
        } else if (!strncmp(entry, "NUMCORES=", 9)) {
            msg.fw.numcores = atoi(entry+9);
        } else if (!strncmp(entry, "FIRMWARE=", 9)) {
            msg.fw.firmware = entry+9;
        } else if (!strncmp(entry, "SIZE=", 5)) {
            msg.mem.size = atoi(entry+5);
        } else if (!strncmp(entry, "REGION=", 7)) {
            msg.mem.region = atoi(entry+7);
        }
        i += strlen(entry) + 1;
    }

    return 0;
}

int Uevent::processEvent()
{
    if (action != UEVENT_ACTION_ADD)
        return -1;
    if (type == UEVENT_TYPE_FIRMWARE) {
        loadFirmware();
        sendFirmware();
    } else if (type == UEVENT_TYPE_MEMORY) {
        const char *region = DMA_HEAP_PRIVATE;
        if (msg.mem.region == UEVENT_MEMORY_REGION_PROTECTED)
            region = DMA_HEAP_PROTECTED_INTBUF;
        else if (msg.mem.region == UEVENT_MEMORY_REGION_OUTBUF)
            region = DMA_HEAP_OUTBUF;
        printf("UEVENT_TYPE_MEMORY %s\n", region);
        int fd = allocMemory(region, msg.mem.size);
        if (fd >= 0)
            sendMemory(fd);
    } else if (type == UEVENT_TYPE_HARDWARE) {
#ifdef ENABLE_DRM
        printf("UEVENT_TYPE_HARDWARE\n");
        int reslut;
        int ret = cix_secvid_send_command(secvid_session, CIX_CA_CMD_SECVID_INIT_VPU_HW, &reslut);
        if (ret != OK) {
            throw Exception("Failed to load secure vpu firmware.");
        } else {
            int32_t done = reslut == 0 ? 1 : 0;
            ackInitHw(done);
        }
#else
        ackInitHw(1);
#endif
    }
    return 0;
}

int Uevent::loadFirmware()
{
#ifdef ENABLE_DRM
    struct mvx_secure_firmare sec_fw_desc;

    memset(&sec_fw_desc, 0x0, sizeof(sec_fw_desc));
    sec_fw_desc.cores = msg.fw.numcores;
    strncpy(sec_fw_desc.name, msg.fw.firmware, sizeof(sec_fw_desc.name) - 1);
    int ret = cix_secvid_send_command(secvid_session, CIX_CA_CMD_SECVID_LOAD_VPU_FW, &sec_fw_desc);
    if (ret != OK) {
        throw Exception("Failed to load secure vpu firmware.");
    }

    fw_desc.fd = sec_fw_desc.fd;
    fw_desc.l2pages = sec_fw_desc.l2pages;
    fw_desc.protocol.major = sec_fw_desc.protocol.major;
    fw_desc.protocol.minor = sec_fw_desc.protocol.minor;

    return 0;
#else
    /* In secure decoding Phase I, decrypted firmware and its L2 page table
     * are loaded by backdoor, so just need to allocate a 4MB buffer from
     * private region and assume the data is already there */
    fw_desc.fd = allocMemory(DMA_HEAP_PRIVATE, SIZE_4M);
    /* Firmware L2 pages are located in the end of firmware buffer,
     * one 4K page for each core */
    fw_desc.l2pages = SECURE_DECODING_PHASE_I_FW_BASE + SIZE_4M -
                    (msg.fw.numcores * MVE_PAGE_SIZE);

    /* Hardcode protocol version to 3.5 for now, just for secure decoding Phase I.
     * For Phase II, it should be from TA */
    fw_desc.protocol.major = 3;
    fw_desc.protocol.minor = 5;

    /* Driver will call dma_buf_put() in secure_firmware_release()
     * to close memfd. */

    return 0;
#endif
}

int Uevent::allocMemory(const char *region, size_t size)
{
    printf("Allocating %ld bytes from dma heap %s\n", size, region);
    int fd = open(region, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return fd;
    struct dma_heap_allocation_data heap_data {
        .len = size,
        .fd_flags = O_RDWR | O_CLOEXEC,
    };

    int ret = ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &heap_data);
    if (ret < 0) {
        close(fd);
        return ret;
    }

    /* Driver will call dma_buf_put() in mvx_mmu_free_pages()
     * to close heap_data.fd */
    close(fd);
    return heap_data.fd;
}

int Uevent::sendFirmware()
{
    char path[256];
    snprintf(path, sizeof(path), "/sys%s/firmware", devpath);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return fd;
    ssize_t n = write(fd, &fw_desc, sizeof(fw_desc));
    if (n < (ssize_t)sizeof(fw_desc))
        cerr << "Error: Failed to send firmware descriptor" << endl;
    close(fd);

    return 0;
}

int Uevent::sendMemory(int dmafd)
{
    struct MemoryProtocol memory = { .fd = dmafd };
    char path[256];
    snprintf(path, sizeof(path), "/sys%s/memory", devpath);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return fd;
    ssize_t n = write(fd, &memory, sizeof(memory));
    if (n < (ssize_t)sizeof(memory))
        cerr << "Error: Failed to send memory fd" << endl;
    close(fd);
    return 0;
}

int Uevent::ackInitHw(int done)
{
    struct HardwareProtocol hw = { .done = done };
    char path[256];
    snprintf(path, sizeof(path), "/sys%s/hardware", devpath);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return fd;
    ssize_t n = write(fd, &hw, sizeof(hw));
    if (n < (ssize_t)sizeof(hw))
        cerr << "Error: Failed to send vpu initializion result" << endl;
    close(fd);
    return 0;
}

Decoder::Decoder(const char *dev, Input &input, Output &output, bool freerun, bool nonblock, ostream &log) :
    Codec(dev, input, V4L2_BUF_TYPE_VIDEO_OUTPUT, output,
          V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, log, nonblock, freerun),
    tid(0),
    sktfd(-1),
    eos(0)
{}

Decoder::~Decoder()
{
#ifdef ENABLE_DRM
    if (secvid_session)
        cix_secvid_close_session(secvid_session);
#endif
    if (sktfd >= 0) {
        eos = 1;
        pthread_join(tid, NULL);
    }
}

void Decoder::setH264IntBufSize(uint32_t ibs)
{
    output.setH264DecIntBufSize(ibs);
}

void Decoder::setInterlaced(bool interlaced)
{
    output.setInterlaced(interlaced);
}

void Decoder::setFrameReOrdering(uint32_t fro)
{
    output.setDecFrameReOrdering(fro);
}

void Decoder::setIgnoreStreamHeaders(uint32_t ish)
{
    output.setDecIgnoreStreamHeaders(ish);
}

void Decoder::tryStopCmd(bool tryStop)
{
    input.tryDecStopCmd(tryStop);
}

void Decoder::setNaluFormat(int nalu)
{
    input.io->setNaluFormat(nalu);
    naluFmt = nalu;
}

void Decoder::setRotation(int rotation)
{
    output.setRotation(rotation);
}

void Decoder::setDownScale(int scale)
{
    output.setDownScale(scale);
}

void Decoder::setFrameCount(int frames) {
    output.setFrameCount(frames);
}

void Decoder::setDSLFrame(int width, int height){
    output.setDSLFrame(width, height);
}

void Decoder::setDSLRatio(int hor, int ver){
    output.setDSLRatio(hor, ver);
}

void Decoder::setDSLMode(int mode){
    output.setDSLMode(mode);
}

void Decoder::setDSLInterpMode(int mode){
    output.setDSLInterpMode(mode);
}

void Decoder::setDualAfbcDownScale(int enable){
    output.setDualAfbcDownScale(enable);
}

int Decoder::openUeventSocket()
{
    struct sockaddr_nl addr;
    //int sz = 64*1024;
    int sfd;

    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = getpid();
    addr.nl_groups = 0xffffffff;

    sfd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
    if (sfd < 0)
        return -1;

    if (::bind(sfd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(sfd);
        return -1;
    }

    sktfd = sfd;

    return sfd;
}

int Decoder::wait(int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;
    int ret;

    FD_ZERO(&rfds);
    FD_SET(sktfd, &rfds);

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    ret = select(sktfd+1, &rfds, NULL, NULL, &tv);

    return ret;
}

void* Decoder::monitorUevent(void *arg)
{
    Decoder *decoder = (Decoder *)arg;
    int n;
    int sfd = decoder->openUeventSocket();
    void* msg;

    if (sfd < 0)
        throw Exception("Failed to open uevent socket.");

    msg = malloc(UEVENT_MSG_SIZE);
    if (msg == NULL)
        throw Exception("Failed to allocate message buffer.");
    memset(msg, 0, UEVENT_MSG_SIZE);

    sem_post(&m_sem_uevent_monitor);
    while (!decoder->eos) {
        if (decoder->wait(WAIT_TIMEOUT_MS) <= 0)
            continue;
        if ((n = recv(sfd, msg, UEVENT_MSG_SIZE, 0)) > 0) {
#ifdef ENABLE_DRM
            Uevent uevent = Uevent(msg, n, static_cast<void *>(decoder->secvid_session));
#else
            Uevent uevent = Uevent(msg, n, NULL);
#endif
            uevent.processEvent();
        }
    }

    close(sfd);
    decoder->sktfd = -1;
    free(msg);

    return NULL;
}

void Decoder::setSecureVideo()
{
    struct v4l2_control ctrl = {
        .id = V4L2_CID_MVE_VIDEO_SECURE_VIDEO, .value = 1
    };

    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) != 0)
        throw Exception("Failed to enable secure video.");

    sem_init(&m_sem_uevent_monitor, 0, 0);

    int ret = pthread_create(&tid, NULL, monitorUevent, this);
    if (ret != 0)
    {
        throw Exception("Failed to create input thread.");
    }

    /* Secure video supports dma-buf memory only */
    setMemoryType(V4L2_MEMORY_DMABUF);

    input.setSecureVideo();
    output.setSecureVideo();

#ifdef ENABLE_DRM
    secvid_session = cix_secvid_open_session();
    if (!secvid_session)
        throw Exception("Failed to open secure video session.");
#endif

    sem_wait(&m_sem_uevent_monitor);
    sem_destroy(&m_sem_uevent_monitor);
}

void Decoder::setDisabledFeatures(int val){
    output.setDisabledFeatures(val);
}

void Decoder::setColorConversion(uint32_t mode){
    output.setColorConversion(mode);
}

void Decoder::setCustColorConvCoef(struct v4l2_mvx_color_conv_coef *coef){
    output.setCustColorConvCoef(coef);
}

void Decoder::setDecDstCrop(struct v4l2_rect *dst_crop){
    output.setDecDstCrop(dst_crop);
}

void Decoder::setStride(size_t *stride){
    output.setFrameStride(stride);
}

void Decoder::setFsfMode(int val){
    output.setFsfMode(val);
}

void Decoder::setSeamlessTarget(uint32_t format, struct v4l2_mvx_seamless_target *seamless)
{
    size_t nplanes;
    size_t stride[3];
    size_t size[3];
    size_t heights[3];
    unsigned int framesize;

    memset(stride,0,sizeof(stride));
    memset(size,0,sizeof(size));
    memset(heights,0,sizeof(heights));
    framesize = Codec::getSize(format, (size_t)seamless->target_width, (size_t)seamless->target_height,(size_t)16, nplanes, stride, size, heights);
    seamless->target_stride[0] = stride[0];
    seamless->target_stride[1] = stride[1];
    seamless->target_stride[2] = stride[2];

    seamless->target_size[0]  = size[0];
    seamless->target_size[1]  = size[1];
    seamless->target_size[2]  = size[2];

    cout<<"setSeamlessTarget framesize= %u" <<framesize<<endl;
    output.setSeamlessTarget(seamless);
}
void Decoder::setFrameBufCnt(uint32_t count)
{
    output.setBufferCnt(count);
}

void Decoder::setBitBufCnt(uint32_t count)
{
    input.setBufferCnt(count);
}

void Decoder::setZeroOutAfbc()
{
    output.setZeroOutAfbc();
}

void Decoder::addSeekPoint(int from_frame, int to_offset)
{
    struct SeekPoint sp;
    sp.from_frame = from_frame;
    sp.to_offset = to_offset;
    seek_points.push(sp);
}

void Decoder::seek()
{
    if (!seek_points.empty()) {
        struct SeekPoint sp = seek_points.front();
        int frames_processed = output.getFramesProcessed();
        if (frames_processed == sp.from_frame) {
            cout << "Seeking from frame " << sp.from_frame << " to file offset " << sp.to_offset << endl;
            seek_points.pop();
            if (input.seek(sp.to_offset) == 0) {
                flush();
                requeue();
            }
        } else if (frames_processed > sp.from_frame) {
            cerr << "WARNING: Invalid seek point: Try to seek from " << sp.from_frame <<
                    " while " << frames_processed << " frames have been processed. Drop it." << endl;
            seek_points.pop();
        }
    }
}

Encoder::Encoder(const char *dev, Input &input, Output &output, bool freerun, bool nonblock, ostream &log) :
    Codec(dev, input, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, output,
          V4L2_BUF_TYPE_VIDEO_CAPTURE, log, nonblock, freerun)
{
    //this->output.setEncFramerate(30 << 16);
    //this->output.setEncBitrate(input.getWidth() * input.getHeight() * 30 / 2);
}

void Encoder::changeSWEO(uint32_t csweo)
{
    this->csweo = (csweo == 1);
}

void Encoder::setFramerate(unsigned int fps_n, unsigned int fps_d)
{
    if (!csweo)
    {
        output.setEncFramerate(fps_n, fps_d);
    }
    else
    {
        this->fps_n = fps_n;
        this->fps_d = fps_d;
    }
}

void Encoder::setBitrate(uint32_t bps)
{
    if (!csweo)
    {
        output.setEncBitrate(bps);
    }
    else
    {
        output.setEncBitrate(bps);
        this->bps = bps - 500;
    }
}

void Encoder::setGOPSize(uint32_t gopSize)
{
    output.setEncGOPSize(gopSize);
}

void Encoder::setBFrames(uint32_t bframes)
{
    output.setEncBFrames(bframes);
}

void Encoder::setSliceSpacing(uint32_t spacing)
{
    output.setEncSliceSpacing(spacing);
}

void Encoder::setHorizontalMVSearchRange(uint32_t hmvsr)
{
    output.setEncHorizontalMVSearchRange(hmvsr);
}

void Encoder::setVerticalMVSearchRange(uint32_t vmvsr)
{
    output.setEncVerticalMVSearchRange(vmvsr);
}

void Encoder::setEncForceChroma(uint32_t fmt)
{
    input.setEncForceChroma(fmt);
}

void Encoder::setEncBitdepth(uint32_t bd)
{
    input.setEncBitdepth(bd);
}

void Encoder::setH264IntraMBRefresh(uint32_t period)
{
    output.setH264EncIntraMBRefresh(period);
}

void Encoder::setProfile(uint32_t profile)
{
    output.setEncProfile(profile);
}

void Encoder::setTier(uint32_t tier)
{
    output.setEncTier(tier);
}

void Encoder::setLevel(uint32_t level)
{
    output.setEncLevel(level);
}

void Encoder::setConstrainedIntraPred(uint32_t cip)
{
    output.setEncConstrainedIntraPred(cip);
}

void Encoder::setH264EntropyCodingMode(uint32_t ecm)
{
    output.setH264EncEntropyMode(ecm);
}

void Encoder::setH264GOPType(uint32_t gop)
{
    output.setH264EncGOPType(gop);
}

void Encoder::setEncMinQP(uint32_t minqp)
{
    if (!csweo)
    {
        output.setEncMinQP(minqp);
    }
    else
    {
        this->minqp = minqp;
    }
}

void Encoder::setEncMaxQP(uint32_t maxqp)
{
    if (!csweo)
    {
        output.setEncMaxQP(maxqp);
    }
    else
    {
        this->maxqp = maxqp;
    }
}

void Encoder::setEncFixedQP(uint32_t fqp)
{
    if (!csweo)
    {
        output.setEncFixedQP(fqp);
    }
    else
    {
        output.setEncFixedQP(fqp);
        this->fixedqp = fqp + 2;
    }
}

void Encoder::setEncFixedQPI(uint32_t fqp)
{
    output.setEncFixedQPI(fqp);
}

void Encoder::setEncFixedQPP(uint32_t fqp)
{
    output.setEncFixedQPP(fqp);
}

void Encoder::setEncFixedQPB(uint32_t fqp)
{
    output.setEncFixedQPB(fqp);
}

void Encoder::setEncMinQPI(uint32_t nQpMinI)
{
    output.setEncMinQPI(nQpMinI);
}

void Encoder::setEncMaxQPI(uint32_t nQpMaxI)
{
    output.setEncMaxQPI(nQpMaxI);
}

void Encoder::setEncInitQPI(uint32_t init_qpi)
{
    output.setEncInitQPI(init_qpi);
}

void Encoder::setEncInitQPP(uint32_t init_qpp)
{
    output.setEncInitQPP(init_qpp);
}

void Encoder::setEncSAOluma(uint32_t sao_luma_dis)
{
    output.setEncSAOluma(sao_luma_dis);
}

void Encoder::setEncSAOchroma(uint32_t sao_chroma_dis)
{
    output.setEncSAOchroma(sao_chroma_dis);
}

void Encoder::setEncQPDeltaIP(uint32_t qp_delta_i_p)
{
    output.setEncQPDeltaIP(qp_delta_i_p);
}

void Encoder::setEncRefRbEn(uint32_t ref_rb_en)
{
    output.setEncRefRbEn(ref_rb_en);
}

void Encoder::setEncRCClipTop(uint32_t rc_qp_clip_top)
{
    output.setEncRCClipTop(rc_qp_clip_top);
}

void Encoder::setEncRCClipBot(uint32_t rc_qp_clip_bottom)
{
    output.setEncRCClipBot(rc_qp_clip_bottom);
}

void Encoder::setEncQpmapClipTop(uint32_t qpmap_clip_top)
{
    output.setEncQpmapClipTop(qpmap_clip_top);
}

void Encoder::setEncQpmapClipBot(uint32_t qpmap_clip_bottom)
{
    output.setEncQpmapClipBot(qpmap_clip_bottom);
}

void Encoder::setH264Bandwidth(uint32_t bw)
{
    output.setH264EncBandwidth(bw);
}

void Encoder::setVP9TileCR(uint32_t tcr)
{
    output.setVP9EncTileCR(tcr);
}

void Encoder::setVP9ProbUpdateMode(uint32_t prob) {
    output.setVP9ProbUpdateMode(prob);
}

void Encoder::setJPEGRefreshInterval(uint32_t r)
{
    output.setJPEGEncRefreshInterval(r);
}

void Encoder::setJPEGQuality(uint32_t q)
{
    output.setJPEGEncQuality(q);
}

void Encoder::setJPEGQualityLuma(uint32_t q)
{
    output.setJPEGEncQualityLuma(q);
}

void Encoder::setJPEGQualityChroma(uint32_t q)
{
    output.setJPEGEncQualityChroma(q);
}

void Encoder::setJPEGHufftable(struct v4l2_mvx_huff_table *table)
{
    output.setJPEGHufftable(table);
}

void Encoder::setHEVCEntropySync(uint32_t es)
{
    output.setHEVCEncEntropySync(es);
}

void Encoder::setHEVCTemporalMVP(uint32_t tmvp)
{
    output.setHEVCEncTemporalMVP(tmvp);
}

void Encoder::setStreamEscaping(uint32_t sesc)
{
    output.setEncStreamEscaping(sesc);
}

void Encoder::tryStopCmd(bool tryStop)
{
    input.tryEncStopCmd(tryStop);
}

void Encoder::setMirror(int mirror)
{
    input.setMirror(mirror);
}

void Encoder::setRotation(int rotation)
{
    input.setRotation(rotation);
}

void Encoder::setFrameCount(int frames) {
    input.setFrameCount(frames);
}

void Encoder::setMiniCnt(uint32_t mini_cnt)
{
    mini_frame_cnt = mini_cnt;
    input.setMiniCnt(mini_cnt);
}

void Encoder::setStride(size_t *stride){
    input.setFrameStride(stride);
}

void Encoder::setRateControl(const std::string &rc, int target_bitrate, int maximum_bitrate){

    bool rc_enabled = true;
    int rc_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;
    if (rc.compare("standard") == 0) {
        rc_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_STANDARD;
    } else if (rc.compare("variable") == 0) {
        rc_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
    } else if (rc.compare("cvbr") == 0) {
        rc_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_CVBR;
    } else if (rc.compare("off") == 0){
        rc_enabled = false;
    }
    output.setRateControl(rc_enabled, rc_mode, target_bitrate, maximum_bitrate);
}

void Encoder::setStatsMode(int mode){
    input.setStatsMode(mode);
}

void Encoder::setSeiUserData(struct v4l2_sei_user_data *sei_user_data){
    input.setSeiUserData(sei_user_data);
}

#if KERNEL_VERSION(5, 15, 0) <= LINUX_VERSION_CODE
void Encoder::setHDR10Info(struct v4l2_ctrl_hdr10_cll_info *cll,
                        struct v4l2_ctrl_hdr10_mastering_display *mastering){
    input.setHDR10Info(cll, mastering);

    printHDR10Info(cll, mastering);
}
#endif

void Encoder::setColorDescription(uint32_t colorspace, uint8_t xfer_func,
                        uint8_t ycbcr_enc, uint8_t quantization){
    input.setColorDescription(colorspace, xfer_func, ycbcr_enc, quantization);
}

void Encoder::setHRDBufferSize(int size) {
    input.setHRDBufferSize(size);
}

void Encoder::setLongTermRef(uint32_t mode, uint32_t period){
    input.setLongTermRef(mode, period);
}

void Encoder::setVisibleWidth(uint32_t v_width)
{
    input.setVisibleWidth(v_width);
}

void Encoder::setVisibleHeight(uint32_t v_height)
{
    input.setVisibleHeight(v_height);
}

void Encoder::setRcBitIMode(uint32_t mode)
{
    output.setRcBitIMode(mode);
}

void Encoder::setRcBitRationI(uint32_t ratio)
{
    output.setRcBitRationI(ratio);
}

void Encoder::setMultiSPSPPS(uint32_t sps_pps){
    output.setMultiSPSPPS(sps_pps);
}

void Encoder::setEnableSCD(uint32_t scd_enable){
    output.setEnableSCD(scd_enable);
}

void Encoder::setScdPercent(uint32_t scd_percent){
    output.setScdPercent(scd_percent);
}

void Encoder::setScdThreshold(uint32_t scd_threshold){
    output.setScdThreshold(scd_threshold);
}

void Encoder::setEnableAQSsim(uint32_t aq_ssim_en){
    output.setEnableAQSsim(aq_ssim_en);
}

void Encoder::setAQNegRatio(uint32_t aq_neg_ratio){
    output.setAQNegRatio(aq_neg_ratio);
}

void Encoder::setAQPosRatio(uint32_t aq_pos_ratio){
    output.setAQPosRatio(aq_pos_ratio);
}

void Encoder::setAQQPDeltaLmt(uint32_t aq_qpdelta_lmt){
    output.setAQQPDeltaLmt(aq_qpdelta_lmt);
}

void Encoder::setAQInitFrmAvgSvar(uint32_t aq_init_frm_avg_svar){
    output.setAQInitFrmAvgSvar(aq_init_frm_avg_svar);
}

void Encoder::setEnableVisual(uint32_t enable){
    output.setEnableVisual(enable);
}

void Encoder::setAdaptiveIntraBlock(uint32_t enable){
    output.setAdaptiveIntraBlock(enable);
}

void Encoder::setSvct3Level1Period(uint32_t period)
{
    input.setSvct3Level1Period(period);
}

void Encoder::setIntermediateBufSize(uint32_t size)
{
    input.setIntermediateBufSize(size);
}

void Encoder::setStatsSize(uint32_t mms, uint32_t bc, uint32_t qp)
{
    input.setPortStatsSize(mms,bc,qp);
}

void Encoder::setGDRnumber(uint32_t numbder)
{
    input.setGDRnumber(numbder);
}

void Encoder::setGDRperiod(uint32_t period)
{
    input.setGDRperiod(period);
}

void Encoder::setEncIntraIpenaltyAngular(uint32_t value)
{
    output.setEncIntraIpenaltyAngular(value);
}

void Encoder::setEncIntraIpenaltyPlanar(uint32_t value)
{
    output.setEncIntraIpenaltyPlanar(value);
}

void Encoder::setEncIntraIpenaltyDc(uint32_t value)
{
    output.setEncIntraIpenaltyDc(value);
}

void Encoder::setEncInterIpenaltyAngular(uint32_t value)
{
    output.setEncInterIpenaltyAngular(value);
}

void Encoder::setEncInterIpenaltyPlanar(uint32_t value)
{
    output.setEncInterIpenaltyPlanar(value);
}

void Encoder::setEncInterIpenaltyDc(uint32_t value)
{
    output.setEncInterIpenaltyDc(value);
}

void Encoder::setForcedUVvalue(uint32_t uv_value)
{
    input.setForcedUVvalue(uv_value);
}

void Encoder::setEncSrcCrop(struct v4l2_rect * src_crop)
{
    input.setEncCrop(src_crop);
}

void Encoder::setEncCrop(struct v4l2_rect * crop)
{
    output.setEncCrop(crop);
}

void Encoder::setRGBToYUVMode(uint32_t mode){
    input.setRGBToYUVMode(mode);
}

void Encoder::setRGBConvertYUV(struct v4l2_mvx_rgb2yuv_color_conv_coef *coef){
    input.setRGBConvertYUV(coef);
}

void Encoder::setFrameBufCnt(uint32_t count)
{
    input.setBufferCnt(count);
}

void Encoder::setBitBufCnt(uint32_t count)
{
    output.setBufferCnt(count);
}

void Encoder::setEncOSDinfo(struct v4l2_osd_info* info)
{
    input.setEncOSDinfo(info);
}

void Encoder::setEncLambdaScale(struct v4l2_mvx_lambda_scale * lambda_scale)
{
    output.setEncLambdaScale(lambda_scale);
}

#ifdef ENABLE_CPIPE
void Encoder::setExtInput(const char *ext_input)
{
    if (CPIPE_create(&cpipe, ext_input, CPIPE_MODE_CLIENT) != CPIPE_STATUS_OK)
        throw Exception("Failed to create CPIPE.");

    if (CPIPE_connect(cpipe, NULL) != CPIPE_STATUS_OK)
        throw Exception("Failed to connect to server.");

    input.io->setExtInput(cpipe);
}
#endif

Info::Info(const char *dev, ostream &log) :
    Codec(dev,
          V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
          V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
          log,
          true,
          true)
{}

void Info::enumerateFormats()
{
    try
    {
        Codec::enumerateFormats();
    }
    catch (Exception &e)
    {
        cerr << "Error: " << e.what() << endl;
    }
}
