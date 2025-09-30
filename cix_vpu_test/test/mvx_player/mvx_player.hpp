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

#ifndef __MVX_PLAYER_H__
#define __MVX_PLAYER_H__

/****************************************************************************
 * Includes
 ****************************************************************************/

#include <cmath>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <memory>
#include <vector>
#include <list>

#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <mutex>
#include <stdio.h>
#include <fstream>
#include <semaphore.h>

#include <linux/version.h>
#include <linux/videodev2.h>
#include "mvx-v4l2-controls.h"
#ifdef ENABLE_DISPLAY
#include "cix_drm.h"
#endif
#ifdef ENABLE_CPIPE
#include "cix_pipe_types.h"
#endif

#include "reader/read_util.h"
#include "reader/parser.h"
#include "reader/start_code_reader.h"

#define MVX_MAX_DARG_NUM    20

#ifndef MVE_PAGE_SIZE
#define MVE_PAGE_SHIFT          12
#define MVE_PAGE_SIZE           (1 << MVE_PAGE_SHIFT)
#define MVE_PAGE_MASK           (MVE_PAGE_SIZE - 1)
#endif

#define MVX_MAX_PATH_LEN    64
#define LOW_LATENCY_FPS     60
#define SUPPORT_BUF_CNT_MAX 32

struct epr_config
{
    unsigned int pic_index;

    struct v4l2_buffer_general_block_configs   block_configs;
    struct v4l2_buffer_param_qp                qp;
    bool block_configs_present;
    bool qp_present;

    size_t bc_row_body_size;
    union
    {
        char* _bc_row_body_data;
        struct v4l2_buffer_general_rows_uncomp_body* uncomp;
    } bc_row_body;

    epr_config(const size_t size = 0)
    {
        pic_index = 0;
        qp.qp = 0;
        clear();
        allocate_bprf(size);
    };
    epr_config(const epr_config& other)
        : pic_index                       (other.pic_index),
          block_configs                   (other.block_configs),
          qp                              (other.qp),
          block_configs_present           (other.block_configs_present),
          qp_present                      (other.qp_present)
    {
        allocate_bprf(other.bc_row_body_size);

        if (other.bc_row_body_size > 0)
        {
            std::copy(other.bc_row_body._bc_row_body_data,
                      other.bc_row_body._bc_row_body_data + other.bc_row_body_size,
                      bc_row_body._bc_row_body_data);
        }
    };
    ~epr_config()
    {
        if (bc_row_body_size > 0)
        {
            delete [] bc_row_body._bc_row_body_data;
        }
    };
    epr_config& operator= (epr_config other)
    {
        swap(*this, other);
        return *this;
    };
    friend void swap(epr_config& a, epr_config& b)
    {
        using std::swap;

        swap(a.pic_index, b.pic_index);
        swap(a.block_configs, b.block_configs);
        swap(a.qp, b.qp);
        swap(a.block_configs_present, b.block_configs_present);
        swap(a.qp_present, b.qp_present);

        swap(a.bc_row_body_size, b.bc_row_body_size);
        swap(a.bc_row_body._bc_row_body_data, b.bc_row_body._bc_row_body_data);
    };
    void clear(void)
    {
        block_configs_present = false;
        qp_present = false;
    }

private:
    void allocate_bprf(size_t size)
    {
        bc_row_body_size = size;
        if (size > 0)
        {
            bc_row_body._bc_row_body_data = new char[size];
        }
        else
        {
            bc_row_body._bc_row_body_data = NULL;
        }
    };
};

struct v4l2_gop_config
{
    uint32_t gop_pic;
    uint32_t gop_pframes;
};

struct v4l2_reset_ltr_period_config
{
    unsigned int reset_trigger_pic;
    unsigned int reset_ltr_period;
};

struct v4l2_enc_stats_cfg
{
    unsigned int reset_pic;
    unsigned int reset_cfg;
};

struct video_format
{
    uint32_t pix_fmt;
    uint32_t drm_fmt;
    uint32_t enable;
};

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(array)	(sizeof(array) / sizeof((array)[0]))
#endif

typedef std::list<epr_config> v4l2_epr_list_t;
typedef std::list<v4l2_mvx_roi_regions> v4l2_roi_list_t;
typedef std::list<v4l2_mvx_chr_config> v4l2_chr_list_t;
typedef std::queue<v4l2_gop_config> gop_list_t;
typedef std::queue<v4l2_reset_ltr_period_config> ltr_list_t;
typedef std::queue<v4l2_enc_stats_cfg> enc_stats_list_t;
typedef std::list<v4l2_osd_config> v4l2_osd_list_t;

/****************************************************************************
 * Exception
 ****************************************************************************/
class Exception :
    public std::exception
{
public:
    Exception(const char *fmt, ...);
    Exception(const std::string &str);
    virtual ~Exception() throw();

    virtual const char *what() const throw();

private:
    char msg[100];
};

/****************************************************************************
 * Buffer
 ****************************************************************************/

class Buffer
{
public:
    Buffer(const v4l2_format &format);
    Buffer(v4l2_buffer &buf, int fd, const v4l2_format &format, enum v4l2_memory memory_type);
    virtual ~Buffer();

    v4l2_buffer &getBuffer();
    const v4l2_format &getFormat() const;
    void setCrop(const v4l2_rect &crop);
    const v4l2_rect &getCrop() const;
    void setSeamless(const struct v4l2_mvx_seamless_target &seamless);
    const v4l2_mvx_seamless_target &getSeamless()const;
    std::vector<iovec> getImageSize() const;
    std::vector<iovec> getBytesUsed() const;
    void getPackedBuffer(char* img, size_t nplanes, size_t dst_heights[], size_t dst_stride[], size_t src_stride[]);
    void setBytesUsed(std::vector<iovec> &iov);
    void clearBytesUsed();
    void resetVendorFlags();
    void setCodecConfig(bool codecConfig);
    void setTimeStamp(unsigned int timeUs);
    void setEndOfFrame(bool eof);
    void setEndOfStream(bool eos);
    void update(v4l2_buffer &buf);
    void setInterlaced(bool interlaced);
    void setTiled(bool tiled);
    void setRotation(int rotation);
    void setEncRotation(int rotation);
    void setMirror(int mirror);
    void setDownScale(int scale);
    void setEndOfSubFrame(bool eos);
    std::vector<iovec> convert10Bit();
    std::vector<iovec> _convert10Bit(unsigned short* ptr_y, unsigned short* ptr_uv, size_t size_y, size_t size_uv);
    void setRoiCfg(struct v4l2_mvx_roi_regions roi);
    void setChrCfg(struct v4l2_mvx_chr_config chr);
    void setGopResetCfg(struct v4l2_gop_config cfg);
    void setLtrResetCfg(struct v4l2_reset_ltr_period_config cfg);
    bool getRoiCfgflag() {return isRoiCfg;}
    bool getChrCfgflag() {return isChrCfg;}
    bool getGopResetCfgflag() {return isGopCfg;}
    bool getLtrResetCfgflag() {return isLtrCfg;}
    void setResetStatsMode(uint32_t pic_index, uint32_t cfg_mode){stats_pic_index = pic_index;stats_cfg_mode = cfg_mode;}
    uint32_t &getResetStatsMode(){return stats_cfg_mode;}
    uint32_t &getResetStatsPicIndex(){return stats_pic_index;}
    struct v4l2_mvx_roi_regions getRoiCfg() {return roi_cfg;};
    struct v4l2_mvx_chr_config getChrCfg() {return chr_cfg;}
    struct v4l2_gop_config getGopResetCfg() {return gop_cfg;}
    struct v4l2_reset_ltr_period_config getLtrResetCfg() {return ltr_cfg;}
    void setSuperblock(bool superblock);
    void setROIflag(bool roi_valid);
    void setChrflag(bool chr_valid);
    void setOsdCfgEnable(bool enable);
    bool getOsdCfgEnable(){return isOsdCfg;}
    void setOsdCfg(struct v4l2_osd_config osd);
    struct v4l2_osd_config getOsdCfg(){return osd_cfg;}
    void setOsdBufferflag(uint32_t index);
    void setEPRflag();
    void set_force_idr_flag(bool idr);
    void set_reset_rc_flag(bool reset);
    void setQPofEPR(struct v4l2_buffer_param_qp data) {epr_qp = data;};
    struct v4l2_buffer_param_qp getQPofEPR(){return epr_qp;}
    bool isGeneralBuffer(){return (buf.flags & V4L2_BUF_FLAG_MVX_BUFFER_EPR) == V4L2_BUF_FLAG_MVX_BUFFER_EPR;};
    bool isOsdBuffer(){return buf.reserved2 & V4L2_BUF_FLAG_MVX_OSD_MASK;}
    void memoryMap(int fd);
    void memoryUnmap();
    size_t getLength(unsigned int plane);
    void dmaMemoryMap(int dma_fd, const unsigned int plane);
    void dmaMemoryunMap(void *p, const unsigned int plane);
    int dmaMemorySync(unsigned int flags, bool ext_input);
    void setLength(const unsigned int length, const unsigned int plane);
    void setDmaFd(int fd, const unsigned int plane, const unsigned offset = 0);
    int getDmaFd(const unsigned int plane) {return fds[plane];}
    void createBoFromDmaFds(int fd, size_t width, size_t height, uint32_t pixfmt);
    unsigned int getNumPlanes() const;
    void *getPlaneptr(unsigned i) {return ptr[i];}
    void allocMemory(const unsigned int plane);
    void freeMemory(const unsigned int plane);
    void createBo(int fd, size_t width, size_t height);
    void destroyBo();
    struct cix_bo *getBo() {return bo;}
    void zeroBuffer();
    void setInQueue(bool flag) {in_queue = flag;};
    bool IsInQueue() {return in_queue;};
    void setExtInputId(int32_t id) {ext_input_id = id;}
    int32_t getExtInputId() {return ext_input_id;}
private:

    void *ptr[VIDEO_MAX_PLANES];
    int fds[VIDEO_MAX_PLANES];
    unsigned int lengths[VIDEO_MAX_PLANES];
    v4l2_buffer buf;
    v4l2_plane planes[VIDEO_MAX_PLANES];
    const v4l2_format &format;
    v4l2_rect crop;
    struct v4l2_mvx_seamless_target  seamless;
    bool isRoiCfg;
    bool isChrCfg;
    bool isGopCfg;
    bool isLtrCfg;
    bool isOsdCfg;
    struct v4l2_mvx_roi_regions roi_cfg;
    struct v4l2_mvx_chr_config chr_cfg;
    struct v4l2_gop_config gop_cfg;
    struct v4l2_reset_ltr_period_config ltr_cfg;
    struct v4l2_buffer_param_qp epr_qp;
    struct v4l2_osd_config osd_cfg;
    enum v4l2_memory memory_type;
    uint32_t stats_pic_index;
    uint32_t stats_cfg_mode;
    struct cix_bo *bo;
    bool in_queue;
    int32_t ext_input_id;
};

/****************************************************************************
 * Input and output
 ****************************************************************************/

#pragma pack(push, 1)
class IVFHeader
{
public:
    IVFHeader();
    IVFHeader(uint32_t codec, uint16_t width, uint16_t height, uint32_t frameRate, uint32_t frameCount);

    uint32_t signature;
    uint16_t version;
    uint16_t length;
    uint32_t codec;
    uint16_t width;
    uint16_t height;
    uint32_t frameRate;
    uint32_t timeScale;
    uint32_t frameCount;
    uint32_t padding;

    static const uint32_t signatureDKIF;
};

class IVFFrame
{
public:
    IVFFrame();
    IVFFrame(uint32_t size, uint64_t timestamp);

    uint32_t size;
    uint64_t timestamp;
};

/* STRUCT_C (for details see specification SMPTE-421M) */
struct HeaderC
{
    uint32_t reserved : 28;
    uint32_t profile : 4;
};

/* Sequence Layer Data (for details see specification SMPTE-421M) */
class VC1SequenceLayerData
{
public:
    VC1SequenceLayerData();

    uint32_t numFrames : 24;
    uint8_t signature1;
    uint32_t signature2;
    uint32_t headerC;
    uint32_t restOfSLD[6];

    static const uint8_t magic1;
    static const uint32_t magic2;
};

/* Frame Layer Data (for details see specification SMPTE-421M) */
class VC1FrameLayerData
{
public:
    VC1FrameLayerData();

    uint32_t frameSize : 24;
    uint32_t reserved : 7;
    uint32_t key : 1;
    uint32_t timestamp;
    uint8_t data[];
};

class AFBCHeader
{
public:
    AFBCHeader();
    AFBCHeader(const v4l2_format &format, size_t frameSize, const v4l2_rect &crop, bool tiled, const int field = FIELD_NONE);
    void setSize(size_t width, size_t height);

    uint32_t magic;
    uint16_t headerSize;
    uint16_t version;
    uint32_t frameSize;
    uint8_t numComponents;
    uint8_t subsampling;
    uint8_t yuvTransform;
    uint8_t blockSplit;
    uint8_t yBits;
    uint8_t cbBits;
    uint8_t crBits;
    uint8_t alphaBits;
    uint16_t mbWidth;
    uint16_t mbHeight;
    uint16_t width;
    uint16_t height;
    uint8_t cropLeft;
    uint8_t cropTop;
    uint8_t param;
    uint8_t fileMessage;

    static const uint32_t MAGIC = 0x43424641;
    static const uint16_t VERSION = 5;//5;
    static const uint8_t PARAM_TILED_BODY = 0x00000001;
    static const uint8_t PARAM_TILED_HEADER = 0x00000002;
    static const uint8_t PARAM_32X8_SUPERBLOCK = 0x00000004;
    static const int FIELD_NONE = 0;
    static const int FIELD_TOP = 1;
    static const int FIELD_BOTTOM = 2;
};
#pragma pack(pop)

class IO
{
public:
    IO(uint32_t format, size_t width = 0, size_t height = 0, size_t strideAlign = 0);
    virtual ~IO() {}

    virtual void prepare(Buffer &buf) {}
    virtual void finalize(Buffer &buf) {}
    virtual bool eof() { return false; }
    virtual void setNaluFormat(int nalu){}
    virtual int getNaluFormat(){return 0;}
    virtual bool needDoubleCount(){return false;};
    virtual bool isDisplay() {return false;}
    virtual int getFd() {return -1;}
    virtual int seekToKeyFrame(int offset) {return 0;}
#ifdef ENABLE_CPIPE
    virtual void setExtInput(CpipeHandle cpipe) {}
#endif
    virtual bool isExtInput() {return false;}
    virtual bool isSkipRead() {return false;}

    uint32_t getFormat() const { return format; }
    uint8_t getProfile() const { return profile; }
    size_t getWidth() const { return width; }
    void setWidth(size_t w) {width = w; };
    size_t getHeight() const { return height; }
    void setHeight(size_t h) {height = h; };
    size_t getStrideAlign() const { return strideAlign; }
    int getDir(){return dir;}
    void setReadHeight(size_t h) {readHeight = h;}
    size_t getReadHeight() const { return readHeight;}
    void setRotation(int rot) {rotation = rot;}
    int getRotation() {return rotation;}
    void setSecureVideo() {securevideo = true;}
    void setConvert10bit() {convert10bit = true;}

protected:
    uint32_t format;
    uint8_t profile;
    size_t width;
    size_t height;
    size_t strideAlign;
    size_t readHeight;
    int dir;//0 for input; 1 for output
    int rotation;
    bool securevideo;
    bool convert10bit;
};

struct RateControlParams
{
    int32_t frame_index;
    uint32_t target_bit_rate;
    uint32_t frame_rate;
    uint32_t minqp;
    uint32_t maxqp;
};

class Input :
    public IO
{
public:
    Input(uint32_t format, size_t width = 0, size_t height = 0, size_t strideAlign = 0);

    virtual void prepare(Buffer &buf) {}
    virtual void finalize(Buffer &buf) {}
    virtual void setNaluFormat(int nalu){}
    virtual int getNaluFormat(){return 0;}
#ifdef ENABLE_CPIPE
    virtual void setExtInput(CpipeHandle cpipe) {}
#endif
    std::queue<int> idr_list;
    std::queue<size_t> frame_bytes;
};

class InputFile :
    public Input
{
public:
    InputFile(std::istream &input, uint32_t format);
    virtual ~InputFile();

    virtual void prepare(Buffer &buf);
    virtual void finalize(Buffer &buf);
    virtual bool eof();
    virtual void setNaluFormat(int nalu){naluFmt = nalu;}
    virtual int getNaluFormat(){return naluFmt;}
    virtual void setRewind(bool flag){rewind = flag;}
    virtual void setCachedFrame(uint32_t count){cached_frames = count;}
    virtual size_t getFileFrameNum(){return fileFrameNum;}
    virtual int seekToKeyFrame(int offset);
#ifdef ENABLE_CPIPE
    virtual void setExtInput(CpipeHandle _cpipe) {cpipe = _cpipe;}
    virtual bool isExtInput() {return cpipe != NULL;}
#endif
    virtual bool isSkipRead() {return skipRead;}
protected:
    InputFile(std::istream &input, uint32_t format, size_t width, size_t height, size_t strideAlign);
    void getExtBuffer(Buffer &buf, std::vector<iovec> &iov);
    void returnExtBuffer(Buffer &buf);
    std::istream &input;
    char* inputBuf;
    uint32_t offset;
    int state;
    int curlen;
    bool iseof;
    int naluFmt;
    uint32_t remaining_bytes;
    start_code_reader* reader;
    bool send_end_of_frame_flag;
    bool send_end_of_subframe_flag;
    bool rewind;
    uint32_t cached_frames;
    bool skipRead;
    size_t fileSize;
    size_t fileFrameNum;
#ifdef ENABLE_CPIPE
    CpipeHandle cpipe;
#endif
};

class InputIVF :
    public InputFile
{
public:
    InputIVF(std::istream &input, uint32_t informat);

    virtual void prepare(Buffer &buf);
    virtual bool eof();
protected:
    uint32_t left_bytes;
    uint64_t timestamp;
};

class InputRCV :
    public InputFile
{
public:
    InputRCV(std::istream &input);

    virtual void prepare(Buffer &buf);
    virtual bool eof();
private:
    bool codecConfigSent;
    VC1SequenceLayerData sld;
    uint32_t left_bytes;
    bool isRcv;
};

class InputAFBC :
    public InputFile
{
public:
    InputAFBC(std::istream &input, uint32_t format, size_t width, size_t height);

    virtual void prepare(Buffer &buf);
    virtual bool eof();
protected:
    unsigned int prepared_frames;
};

class InputFileFrame :
    public InputFile
{
public:
    InputFileFrame(std::istream &input, uint32_t format, size_t width, size_t height, size_t strideAlign, size_t stride[] = {});

    virtual void prepare(Buffer &buf);
    unsigned int get_prepared_frames(){return prepared_frames;}
    virtual bool eof();
    v4l2_chr_list_t *chr_list;
    gop_list_t *gop_list;
    ltr_list_t *ltr_list;
    enc_stats_list_t *enc_stats_list;
protected:
    size_t nplanes;
    size_t stride[3];
    size_t size[3];
    size_t heights[3];
    size_t framesize;
    v4l2_chr_list_t::iterator chr_cur;
    unsigned int prepared_frames;
};

class InputFileMiniFrame :
    public InputFileFrame

{
public:
    InputFileMiniFrame(std::istream &input, uint32_t format, size_t width, size_t height, size_t strideAlign, uint32_t cnt, size_t stride[]);
    virtual void prepare(Buffer &buf);
    virtual bool eof();
protected:
    size_t offset[3];
    bool is_done[3];//one whoel frame is read done of each plane.
    int count;//nbr of frame
    uint32_t cnt_of_miniframe;
    uint32_t miniframe_height;
};

class InputFileFrameWithROI :
    public InputFileFrame
{
public:
    InputFileFrameWithROI(std::istream &input, uint32_t format,
                        size_t width, size_t height, size_t strideAlign, std::istream &roi, size_t stride[]);
    virtual void prepare(Buffer &buf);
    virtual ~InputFileFrameWithROI();
private:
    void load_roi_cfg();
    std::istream &roi_is;
    v4l2_roi_list_t *roi_list;
    v4l2_roi_list_t::iterator cur;
};

class InputFileFrameWithEPR :
    public InputFileFrame
{
public:
    InputFileFrameWithEPR(std::istream &input, uint32_t format,
                        size_t width, size_t height, size_t strideAlign, std::istream &epr, uint32_t oformat, size_t stride[]);
    virtual ~InputFileFrameWithEPR();
    virtual void prepare(Buffer &buf);
    void prepareEPR(Buffer &buf);
    virtual bool needDoubleCount(){return true;};
private:
    std::istream &epr_is;
    v4l2_epr_list_t *epr_list;
    v4l2_epr_list_t::iterator cur;
    uint32_t outformat;
    void load_epr_cfg();
    void read_efp_cfg(char *buf, int num_epr, struct epr_config *config);
    void read_row_cfg(char *buf, int row, int len, struct epr_config &config);
    void erp_adjust_bpr_to_64_64(
                                    struct v4l2_buffer_general_rows_uncomp_body* uncomp_body,
                                    int qp_delta,
                                    uint32_t bpr_base_idx,
                                    uint32_t row_off,
                                    uint8_t force,
                                    uint8_t quad_skip,
                                    int local_base);
};

class InputFileOsd
{
public:
    InputFileOsd(const char  * filename, uint32_t format, size_t width, size_t height, size_t strideAlign, size_t stride[] = {});
    virtual ~InputFileOsd();
    virtual void prepare(Buffer &buf);
    virtual bool eof();
protected:
    size_t nplanes;
    size_t stride[3];
    size_t size[3];
    size_t heights[3];
    size_t framesize;
    std::ifstream osd_is;
};

class InputFileFrameOSD :
    public InputFileFrame
{
public:
    InputFileFrameOSD(std::istream &input, uint32_t format, size_t width, size_t height, size_t strideAlign, size_t stride[] = {});
    virtual void prepare(Buffer &buf);
    v4l2_osd_list_t *osd_list;
    v4l2_osd_list_t::iterator osd_cur;
    InputFileOsd* osd_file_1;
    InputFileOsd* osd_file_2;
private:
    uint32_t refresh_index;
};

class InputFrame :
    public Input
{
public:
    InputFrame(uint32_t format, size_t width, size_t height,
               size_t strideAlign, size_t nframes);

    virtual void prepare(Buffer &buf);
    virtual bool eof();

private:
    void rgb2yuv(unsigned int yuv[3], const unsigned int rgb[3]);

    size_t nplanes;
    size_t stride[3];
    size_t size[3];
    size_t nframes;
    size_t count;
    size_t heights[3];
};

class Output :
    public IO
{
public:
    Output(uint32_t format);
    Output(uint32_t format, bool packed);
    virtual ~Output();

    virtual void prepare(Buffer &buf);
    virtual void finalize(Buffer &buf);
    virtual void write(void *ptr, size_t nbytes) {}
    void setFrameRate(unsigned int fps_num, unsigned int fps_den);
    void setSkipOutput();
    std::queue<struct RateControlParams> drc_list;

protected:
    void controlFrameRate();
    unsigned int timestamp;
    size_t totalSize;
    bool packed;
    struct timeval base;
    unsigned int count;
    unsigned int fps_n;
    unsigned int fps_d;
    bool skipOutput = false;
};

class InputFileLayer
{
public:
    InputFileLayer(const char  * filename, uint32_t format, size_t width, size_t height, size_t strideAlign = 1, size_t stride[] = {});
    virtual ~InputFileLayer();
    void prepare(char *buf, size_t length);
    size_t getWidth() {return width;}
    size_t getHeight() {return height;}
    size_t getFormat() {return format;}
    size_t getFrameSize() {return framesize;}
private:
    size_t width;
    size_t height;
    uint32_t format;
    size_t nplanes;
    size_t stride[3];
    size_t size[3];
    size_t heights[3];
    size_t framesize;
    std::ifstream layer_is;
};

#ifdef ENABLE_DISPLAY
class OutputDisplay :
    public Output
{
public:
    OutputDisplay(const char *arg, uint32_t format);
    virtual ~OutputDisplay();
    virtual void finalize(Buffer &buf);
    virtual bool isDisplay() {return true;}
    virtual int getFd() {return fd;}
    virtual void addBackground(InputFileLayer *bg_layer);
private:
    void show_single(int planeid, struct cix_bo *bo);
    int parse_dargs(const char *arg);
    void remove_background();
    struct cix_device *dev;
    char *dargs;
    int dargc;
    char *dargv[MVX_MAX_DARG_NUM];
    int fd;
    int nplanes;
    uint32_t *planes;
    struct cix_bo *bg_bo;
};

class OutputDisplayAFBC :
    public OutputDisplay
{
public:
    OutputDisplayAFBC(const char *arg, uint32_t format, bool tiled);
    virtual void prepare(Buffer &buf);
private:
    bool tiled;
};
#endif

class OutputFile :
    public Output
{
public:
    OutputFile(std::ostream &output, uint32_t format);
    OutputFile(std::ostream &output, uint32_t format, bool packed);

    virtual void write(void *ptr, size_t nbytes);

protected:
    std::ostream &output;
};

class OutputIVF :
    public OutputFile
{
public:
    OutputIVF(std::ofstream &output, uint32_t format, uint16_t width, uint16_t height, uint32_t frameRate, uint32_t frameCount);

    virtual void finalize(Buffer &buf);

private:
    std::vector<char> temp;
};

class OutputAFBC :
    public OutputFile
{
public:
    OutputAFBC(std::ofstream &output, uint32_t format, bool tiled);
    virtual void prepare(Buffer &buf);
    virtual void finalize(Buffer &buf);
protected:
    bool tiled;
};

class OutputAFBCInterlaced :
    public OutputAFBC
{
public:
    OutputAFBCInterlaced(std::ofstream &output, uint32_t format, bool tiled);
    virtual void finalize(Buffer &buf);
};

class OutputFileFrameStats :
    public OutputFile
{
public:
    OutputFileFrameStats(std::ostream &output, uint32_t format, uint32_t stats_mode,
                        const std::string& filename, uint32_t width, uint32_t height);
    virtual void finalize(Buffer &buf);
    virtual ~OutputFileFrameStats();
private:
    std::ofstream file_mms;
    std::ofstream file_bitcost;
    std::ofstream file_qp;
    unsigned int queued_buffer;
};

/****************************************************************************
 * Codec, Decoder, Encoder
 ****************************************************************************/

class Codec
{
public:
    typedef std::map<uint32_t, Buffer *> BufferMap;

    Codec(const char *dev,
          enum v4l2_buf_type inputType,
          enum v4l2_buf_type outputType,
          std::ostream &log,
          bool nonblock);
    Codec(const char *dev,
          Input &input,
          enum v4l2_buf_type inputType,
          Output &output,
          enum v4l2_buf_type outputType,
          std::ostream &log,
          bool nonblock);
    virtual ~Codec();

    int stream();
    void setMemoryType(enum v4l2_memory mem_type);
    void setInputThread(int input_thread);
    static uint32_t to4cc(const std::string &str);
    static bool isVPx(uint32_t format);
    static bool isYUV422(uint32_t format);
    static bool isAFBC(uint32_t format);
    static uint32_t calcAfbcSize(uint32_t format, unsigned int width, unsigned int height, bool tiled_headers,
                                 bool tiled_body, bool superblock, bool interlaced);
    static void getStride(uint32_t format, size_t & nplanes, size_t stride[3][2]);
    static size_t getSize(uint32_t format, size_t width, size_t height,
                          size_t strideAlign, size_t & nplanes, size_t stride[3], size_t size[3], size_t heights[3]);
    void setJobFrames(uint32_t frames);
    void setPriority(int val);
    void flush();
    void requeue();
#if KERNEL_VERSION(5, 15, 0) <= LINUX_VERSION_CODE
    void getHDR10Info();
    void printHDR10Info(struct v4l2_ctrl_hdr10_cll_info *cll,
                        struct v4l2_ctrl_hdr10_mastering_display *mastering);
#endif
    static bool findAvailableDevice(uint32_t out_fmt, uint32_t cap_fmt, char dev_name[]);
    static bool isDeviceSupportFmt(int fd, uint32_t type, uint32_t format);
protected:
    enum NaluFormat
    {
        NALU_FORMAT_UNDEFINED,
        NALU_FORMAT_START_CODES,
        NALU_FORMAT_ONE_NALU_PER_BUFFER,
        NALU_FORMAT_ONE_BYTE_LENGTH_FIELD,
        NALU_FORMAT_TWO_BYTE_LENGTH_FIELD,
        NALU_FORMAT_FOUR_BYTE_LENGTH_FIELD
    };

    class Port
    {
    public:
        Port(int &fd, enum v4l2_buf_type type, std::ostream &log) :
            fd(fd),
            type(type),
            log(log),
            interlaced(false),
            tryEncStop(false),
            tryDecStop(false),
            mirror(0),
            scale(1),
            frames_processed(0),
            buffers_sent(0),
            frames_count(0),
            memory_type_port(V4L2_MEMORY_MMAP),
            isInputThread(false),
            securevideo(false),
            zeroafbc(false),
            enable_dual_afbc_downscale(0),
            colorspace(0),
            xfer_func(0),
            ycbcr_enc(0),
            quantization(0),
            dsl_width(0),
            dsl_height(0)
        {memset(__stride, 0, sizeof(size_t) * VIDEO_MAX_PLANES);
        memset(&seamless,0,sizeof(seamless));
        memset(&crop,0,sizeof(crop));}
        Port(int &fd, IO &io, v4l2_buf_type type, std::ostream &log) :
            fd(fd),
            io(&io),
            type(type),
            log(log),
            pending(0),
            tid(0),
            interlaced(false),
            tryEncStop(false),
            tryDecStop(false),
            mirror(0),
            scale(1),
            frames_processed(0),
            buffers_sent(0),
            frames_count(0),
            memory_type_port(V4L2_MEMORY_MMAP),
            isInputThread(false),
            securevideo(false),
            zeroafbc(false),
            enable_dual_afbc_downscale(0),
            colorspace(0),
            xfer_func(0),
            ycbcr_enc(0),
            quantization(0),
            dsl_width(0),
            dsl_height(0)
        {memset(__stride, 0, sizeof(size_t) * VIDEO_MAX_PLANES);
        memset(&seamless,0,sizeof(seamless));
        memset(&crop,0,sizeof(crop));}

        void enumerateFormats();
        const v4l2_format &getFormat();
        void tryFormat(v4l2_format &format);
        void setFormat(v4l2_format &format);
        void getTrySetFormat();
        void setPixelFormat();
        void printFormat(const struct v4l2_format &format);
        const v4l2_rect getCrop();

        void allocateBuffers(size_t count, enum v4l2_memory mempry_type, bool append = false);
        void freeBuffers();
        unsigned int getRequiredBufferCount();
        void resetBuffers();
        void queueBuffers();
        void queueBuffer(Buffer &buf);
        Buffer &dequeueBuffer();
        void printBuffer(const v4l2_buffer &buf, const char *prefix);

        bool handleBuffer();
        bool checkResolutionChange();
        void handleResolutionChange();
        void updateIoResolution();
        int seek(int offset);

        void streamon();
        void streamoff();

        void sendEncStopCommand();
        void sendDecCommand(uint32_t cmd);

        void setH264DecIntBufSize(uint32_t ibs);
        void setNALU(NaluFormat nalu);
        size_t getCaptureSize();
        void setEncFramerate(uint32_t fps);
        void setEncBitrate(uint32_t bps);
        void setEncGOPSize(uint32_t gopSize);
        void setEncBFrames(uint32_t bframes);
        void setEncSliceSpacing(uint32_t spacing);
        void setEncForceChroma(uint32_t fmt);
        void setEncBitdepth(uint32_t bd);
        void setH264EncIntraMBRefresh(uint32_t period);
        void setEncProfile(uint32_t profile);
        void setEncTier(uint32_t tier);
        void setEncLevel(uint32_t level);
        void setEncConstrainedIntraPred(uint32_t cip);
        void setH264EncEntropyMode(uint32_t ecm);
        void setH264EncGOPType(uint32_t gop);
        void setEncMinQP(uint32_t minqp);
        void setEncMaxQP(uint32_t maxqp);
        void setEncFixedQP(uint32_t fqp);
        void setEncFixedQPI(uint32_t fqp);
        void setEncFixedQPP(uint32_t fqp);
        void setEncFixedQPB(uint32_t fqp);
        void setEncMinQPI(uint32_t nQpMinI);
        void setEncMaxQPI(uint32_t nQpMaxI);
        void setEncInitQPI(uint32_t init_qpi);
        void setEncInitQPP(uint32_t init_qpp);
        void setEncSAOluma(uint32_t sao_luma_dis);
        void setEncSAOchroma(uint32_t sao_chroma_dis);
        void setEncQPDeltaIP(uint32_t qp_delta_i_p);
        void setEncRefRbEn(uint32_t ref_rb_en);
        void setEncRCClipTop(uint32_t rc_qp_clip_top);
        void setEncRCClipBot(uint32_t rc_qp_clip_bottom);
        void setEncQpmapClipTop(uint32_t qpmap_clip_top);
        void setEncQpmapClipBot(uint32_t qpmap_clip_bottom);
        void setPortJobFrames(uint32_t frames);
        void setH264EncBandwidth(uint32_t bw);
        void setHEVCEncEntropySync(uint32_t es);
        void setHEVCEncTemporalMVP(uint32_t tmvp);
        void setEncStreamEscaping(uint32_t sesc);
        void setEncHorizontalMVSearchRange(uint32_t hmvsr);
        void setEncVerticalMVSearchRange(uint32_t vmvsr);
        void setVP9EncTileCR(uint32_t tcr);
        void setVP9ProbUpdateMode(uint32_t prob);
        void setJPEGEncQuality(uint32_t q);
        void setJPEGEncQualityLuma(uint32_t q);
        void setJPEGEncQualityChroma(uint32_t q);
        void setJPEGEncRefreshInterval(uint32_t r);
        void setJPEGHufftable(struct v4l2_mvx_huff_table *table);
        void setSeamlessTarget(struct v4l2_mvx_seamless_target * seamless);
        void setInterlaced(bool interlaced);
        void setRotation(int rotation);
        void setMirror(int mirror);
        void setDownScale(int scale);
        void tryEncStopCmd(bool tryStop);
        void tryDecStopCmd(bool tryStop);
        void setDecFrameReOrdering(uint32_t fro);
        void setDecIgnoreStreamHeaders(uint32_t ish);
        bool isEncoder();
        void setFrameCount(int frames);
        void setRateControl(bool rc_enabled, int rc_mode, int target_bitrate, int maximum_bitrate);
        void setColorConversion(uint32_t mode);
        void setCustColorConvCoef(struct v4l2_mvx_color_conv_coef *coef);
        void setStatsMode(int mode, int index = 0);
        void setSeiUserData(struct v4l2_sei_user_data *sei_user_data);
#if KERNEL_VERSION(5, 15, 0) <= LINUX_VERSION_CODE
        void setHDR10Info(struct v4l2_ctrl_hdr10_cll_info *cll,
                        struct v4l2_ctrl_hdr10_mastering_display *mastering);
#endif
        void setColorDescription(uint32_t colorspace, uint8_t xfer_func,
                        uint8_t ycbcr_enc, uint8_t quantization);
        void setHRDBufferSize(int size);
        void setDSLFrame(int width, int height);
        void setDSLRatio(int hor, int ver);
        void setLongTermRef(uint32_t mode, uint32_t period);
        void setDSLMode(int mode);
        void setDSLInterpMode(int mode);
        void setDualAfbcDownScale(int enable);
        void setDisabledFeatures(int val);
        void setRGBToYUVMode(uint32_t mode);
        void setRGBConvertYUV(struct v4l2_mvx_rgb2yuv_color_conv_coef *coef);
        void setDecDstCrop(struct v4l2_rect *dst_crop);
        void setVisibleWidth(uint32_t v_width);
        void setVisibleHeight(uint32_t v_height);
        void setMiniCnt(uint32_t mini_cnt);
        int allocateDMABuf(size_t size);
        void setPortMemoryType(enum v4l2_memory mem_type);
        void startInputThread();
        static void* fillInputThread(void *arg);
        void _fillInputThread();
        uint32_t getInputBufferIdx(pthread_mutex_t *mutex, pthread_cond_t *cond, std::queue<uint32_t> *input_queue);
        void appendInputBufferIdx(pthread_mutex_t *mutex, pthread_cond_t *cond, std::queue<uint32_t> *input_queue, uint32_t index);
        void setFrameStride(size_t *stride);
        void setRcBitIMode(uint32_t mode);
        void setRcBitRationI(uint32_t ratio);
        void setMultiSPSPPS(uint32_t sps_pps);
        void setEnableVisual(uint32_t enable);
        void setEnableSCD(uint32_t scd_enable);
        void setScdPercent(uint32_t scd_percent);
        void setScdThreshold(uint32_t scd_threshold);
        void setEnableAQSsim(uint32_t aq_ssim_en);
        void setAQNegRatio(uint32_t aq_neg_ratio);
        void setAQPosRatio(uint32_t aq_pos_ratio);
        void setAQQPDeltaLmt(uint32_t aq_qpdelta_lmt);
        void setAQInitFrmAvgSvar(uint32_t aq_init_frm_avg_svar);
        void setAdaptiveIntraBlock(uint32_t enable);
        void setIntermediateBufSize(uint32_t size);
        void setSvct3Level1Period(uint32_t period);
        void setGopResetPframes(int pframes);
        void setLtrResetPeriod(int period);
        void setPortStatsSize(uint32_t mms, uint32_t bc, uint32_t qp);
        void setGDRnumber(uint32_t numbder);
        void setGDRperiod(uint32_t period);
        void setEncIntraIpenaltyAngular(uint32_t value);
        void setEncIntraIpenaltyPlanar(uint32_t value);
        void setEncIntraIpenaltyDc(uint32_t value);
        void setEncInterIpenaltyAngular(uint32_t value);
        void setEncInterIpenaltyPlanar(uint32_t value);
        void setEncInterIpenaltyDc(uint32_t value);
        void setForcedUVvalue(uint32_t uv_value);
        void setEncCrop(struct v4l2_rect * crop);
        void setBufferCnt(uint32_t count){buf_cnt = count;}
        uint32_t &getBufferCnt(){return buf_cnt;}//this buf_cnt comes from setting value, default 0
        void setOsdCfg(struct v4l2_osd_config osd);
        void setEncOSDinfo(struct v4l2_osd_info* info);
        void setFsfMode(int val);
        void setPriority(int val);
        void setSecureVideo(){securevideo = true;}
        void setZeroOutAfbc(){zeroafbc = true;}
        int getFramesProcessed(){return frames_processed;};
        void enableDualAfbcDownScale();
        void enableDSLFrame();
        void enableEncCrop();
        void setEncLambdaScale(struct v4l2_mvx_lambda_scale *lambda_scale);
        int &fd;
        IO *io;
        v4l2_buf_type type;
        v4l2_format format;
        std::ostream &log;
        BufferMap buffers;
        size_t pending;
        pthread_t tid;
        FILE *roi_cfg;
        v4l2_rect crop;
        uint32_t alloc_width;
        uint32_t alloc_height;
        uint32_t alloc_afbc_bytes;

    private:
        int rotation;
        bool interlaced;
        bool tryEncStop;
        bool tryDecStop;
        int mirror;
        int scale;
        int frames_processed;
        int buffers_sent;
        int frames_count;
        int rc_type;
        uint32_t mini_frame_cnt;
        enum v4l2_memory memory_type_port;
        bool isInputThread;
        std::queue<uint32_t> input_producer_queue;
        std::queue<uint32_t> input_consumer_queue;
        pthread_mutex_t input_producer_mutex;
        pthread_mutex_t input_consumer_mutex;
        pthread_cond_t input_producer_cond;
        pthread_cond_t input_consumer_cond;
        size_t __stride[VIDEO_MAX_PLANES];
        uint32_t mms_buffer_size;
        uint32_t bitcost_buffer_size;
        uint32_t qp_buffer_size;
        uint32_t buf_cnt;//buffer number to be allocated for this port
        struct v4l2_mvx_seamless_target seamless;
        bool securevideo;
        bool zeroafbc;
        bool enable_dual_afbc_downscale;
        uint32_t prev_buffer_index = 0xFFFFFFFF;
        uint32_t colorspace;
        uint8_t xfer_func;
        uint8_t ycbcr_enc;
        uint8_t quantization;
        uint32_t dsl_width;
        uint32_t dsl_height;
    };

    static size_t getBytesUsed(v4l2_buffer &buf);
    void enumerateFormats();

    Port input;
    Port output;
    int fd;
    std::ostream &log;
    bool csweo;
    uint32_t fps;
    uint32_t bps;
    uint32_t minqp;
    uint32_t maxqp;
    uint32_t fixedqp;
    uint32_t mini_frame_cnt;
    enum v4l2_memory memory_type;
#ifdef ENABLE_CPIPE
    CpipeHandle cpipe;
#endif

private:
    void openDev(const char *dev);
    void closeDev();

    void queryCapabilities();
    void enumerateFramesizes(uint32_t format);
    void setFormats();

    void subscribeEvents();
    void subscribeEvents(uint32_t event);
    void unsubscribeEvents();
    void unsubscribeEvents(uint32_t event);

    void allocateBuffers(enum v4l2_memory m_type = V4L2_MEMORY_MMAP);
    void freeBuffers();
    void queueBuffers();
    void resetBuffers();

    void streamon();
    void streamoff();

    void runPoll();
    void runThreads();
    static void *runThreadInput(void *arg);
    static void *runThreadOutput(void *arg);
    bool handleEvent();
    virtual void seek() {};

    bool nonblock;
};

class Uevent
{
public:
    Uevent(void *buf, int size);
    int getAction() {return action;}
    char *getDevPath() {return devpath;}
    char *getSubsystem() {return subsystem;}
    int getType() {return type;}
    int processEvent();
    int loadFirmware();
    int allocMemory(const char *region, size_t size);
    int sendFirmware(int fd);
    int sendMemory(int fd);

private:
    int parseEvent(void *buf, int size);
#define UEVENT_ACTION_UNKNOWN 0
#define UEVENT_ACTION_ADD 1
    int action;
    char *devpath;
    char *subsystem;
#define UEVENT_TYPE_UNKNOWN 0
#define UEVENT_TYPE_FIRMWARE 1
#define UEVENT_TYPE_MEMORY 2
    int type;
    union uevent_msg
    {
        struct uevent_fw
        {
            int numcores;
            char *firmware;
        } fw;
        struct uevent_mem
        {
            int size;
            int region;
#define UEVENT_MEMORY_REGION_PROTECTED 0
#define UEVENT_MEMORY_REGION_OUTBUF 1
#define UEVENT_MEMORY_REGION_PRIVATE 2
        } mem;
    } msg;
    struct mvx_fw_header {
        uint32_t rasc_jmp;
        uint8_t protocol_minor;
        uint8_t protocol_major;
        uint8_t reserved[2];
        uint8_t info_string[56];
        uint8_t part_number[8];
        uint8_t svn_revision[8];
        uint8_t version_string[16];
        uint32_t text_length;
        uint32_t bss_start_address;
        uint32_t bss_bitmap_size;
        uint32_t bss_bitmap[16];
        uint32_t master_rw_start_address;
        uint32_t master_rw_size;
    } fw_hdr;

#pragma pack(push, 1)
    struct FirmwareProtocol
    {
        int32_t fd;
        uint64_t l2pages;
        struct
        {
            uint32_t major;
            uint32_t minor;
        } protocol;
    };

    struct MemoryProtocol
    {
        int32_t fd;
    };
#pragma pack(pop)

};

class Decoder :
    public Codec
{
    struct SeekPoint
    {
        int from_frame;
        int to_offset;
    };
    typedef std::queue<struct SeekPoint> SeekQueue;
public:
    Decoder(const char *dev, Input &input, Output &output, bool nonblock = true, std::ostream &log = std::cout);
    virtual ~Decoder();
    void setH264IntBufSize(uint32_t ibs);
    void setInterlaced(bool interlaced);
    void setFrameReOrdering(uint32_t fro);
    void setIgnoreStreamHeaders(uint32_t ish);
    void tryStopCmd(bool tryStop);
    void setNaluFormat(int nalu);
    void setRotation(int rotation);
    void setDownScale(int scale);
    void setFrameCount(int frames);
    void setDSLFrame(int width, int height);
    void setDSLRatio(int hor, int ver);
    void setDSLMode(int mode);
    void setDSLInterpMode(int mode);
    void setDualAfbcDownScale(int enable);
    void setSecureVideo();
    void setDisabledFeatures(int val);
    void setColorConversion(uint32_t mode);
    void setCustColorConvCoef(struct v4l2_mvx_color_conv_coef *coef);
    void setDecDstCrop(struct v4l2_rect *dst_crop);
    void setStride(size_t *stride);
    void setSeamlessTarget(uint32_t format, struct v4l2_mvx_seamless_target *seamless);
    void setFrameBufCnt(uint32_t count);
    void setBitBufCnt(uint32_t count);
    void setZeroOutAfbc();
    void setFsfMode(int val);
    static void *monitorUevent(void *arg);
    int openUeventSocket();
    void addSeekPoint(int from_frame, int to_offset);
    virtual void seek();
private:
    int naluFmt;
    pthread_t tid;
    int sktfd;
    int eos;
    SeekQueue seek_points;
#define WAIT_TIMEOUT_MS 500
    int wait(int timeout);
};

class Encoder :
    public Codec
{
public:
    Encoder(const char *dev, Input &input, Output &output, bool nonblock = true, std::ostream &log = std::cout);
    void changeSWEO(uint32_t csweo);
    void setFramerate(uint32_t fps);
    void setBitrate(uint32_t bps);
    void setGOPSize(uint32_t gopSize);
    void setBFrames(uint32_t bframes);
    void setSliceSpacing(uint32_t spacing);
    void setConstrainedIntraPred(uint32_t cip);
    void setEncForceChroma(uint32_t fmt);
    void setEncBitdepth(uint32_t bd);
    void setH264IntraMBRefresh(uint32_t period);
    void setProfile(uint32_t profile);
    void setTier(uint32_t tier);
    void setLevel(uint32_t level);
    void setH264EntropyCodingMode(uint32_t ecm);
    void setH264GOPType(uint32_t gop);
    void setEncMinQP(uint32_t minqp);
    void setEncMaxQP(uint32_t maxqp);
    void setEncFixedQP(uint32_t fqp);
    void setEncFixedQPI(uint32_t fqp);
    void setEncFixedQPP(uint32_t fqp);
    void setEncFixedQPB(uint32_t fqp);
    void setH264Bandwidth(uint32_t bw);
    void setVP9TileCR(uint32_t tcr);
    void setVP9ProbUpdateMode(uint32_t prob);
    void setJPEGRefreshInterval(uint32_t ri);
    void setJPEGQuality(uint32_t q);
    void setJPEGQualityLuma(uint32_t q);
    void setJPEGQualityChroma(uint32_t q);
    void setJPEGHufftable(struct v4l2_mvx_huff_table *table);
    void setHEVCEntropySync(uint32_t es);
    void setHEVCTemporalMVP(uint32_t tmvp);
    void setStreamEscaping(uint32_t sesc);
    void setHorizontalMVSearchRange(uint32_t hmvsr);
    void setVerticalMVSearchRange(uint32_t vmvsr);
    void tryStopCmd(bool tryStop);
    void setMirror(int mirror);
    void setRotation(int rotation);
    void setFrameCount(int frames);
    void setRateControl(const std::string &rc, int target_bitrate, int maximum_bitrate);
    void setStatsMode(int mode);
    void setSeiUserData(struct v4l2_sei_user_data *sei_user_data);
#if KERNEL_VERSION(5, 15, 0) <= LINUX_VERSION_CODE
    void setHDR10Info(struct v4l2_ctrl_hdr10_cll_info *cll,
                    struct v4l2_ctrl_hdr10_mastering_display *mastering);
#endif
    void setColorDescription(uint32_t colorspace, uint8_t xfer_func,
                    uint8_t ycbcr_enc, uint8_t quantization);
    void setHRDBufferSize(int size);
    void setLongTermRef(uint32_t mode, uint32_t period);
    void setMiniCnt(uint32_t mini_cnt);
    void setEncMinQPI(uint32_t nQpMinI);
    void setEncMaxQPI(uint32_t nQpMaxI);
    void setEncInitQPI(uint32_t init_qpi);
    void setEncInitQPP(uint32_t init_qpp);
    void setEncSAOluma(uint32_t sao_luma_dis);
    void setEncSAOchroma(uint32_t sao_chroma_dis);
    void setEncQPDeltaIP(uint32_t qp_delta_i_p);
    void setEncRefRbEn(uint32_t ref_rb_en);
    void setEncRCClipTop(uint32_t rc_qp_clip_top);
    void setEncRCClipBot(uint32_t rc_qp_clip_bottom);
    void setEncQpmapClipTop(uint32_t qpmap_clip_top);
    void setEncQpmapClipBot(uint32_t qpmap_clip_bottom);
    void setVisibleWidth(uint32_t v_width);
    void setVisibleHeight(uint32_t v_height);
    void setStride(size_t *stride);
    void setRcBitIMode(uint32_t mode);
    void setRGBToYUVMode(uint32_t mode);
    void setRGBConvertYUV(struct v4l2_mvx_rgb2yuv_color_conv_coef *coef);
    void setRcBitRationI(uint32_t ratio);
    void setMultiSPSPPS(uint32_t sps_pps);
    void setEnableSCD(uint32_t scd_enable);
    void setScdPercent(uint32_t scd_percent);
    void setScdThreshold(uint32_t scd_threshold);
    void setEnableAQSsim(uint32_t aq_ssim_en);
    void setAQNegRatio(uint32_t aq_neg_ratio);
    void setAQPosRatio(uint32_t aq_pos_ratio);
    void setAQQPDeltaLmt(uint32_t aq_qpdelta_lmt);
    void setAQInitFrmAvgSvar(uint32_t aq_init_frm_avg_svar);
    void setEnableVisual(uint32_t enable);
    void setAdaptiveIntraBlock(uint32_t enable);
    void setIntermediateBufSize(uint32_t size);
    void setSvct3Level1Period(uint32_t period);
    void setStatsSize(uint32_t mms, uint32_t bc, uint32_t qp);
    void setGDRnumber(uint32_t numbder);
    void setGDRperiod(uint32_t period);
    void setEncIntraIpenaltyAngular(uint32_t value);
    void setEncIntraIpenaltyPlanar(uint32_t value);
    void setEncIntraIpenaltyDc(uint32_t value);
    void setEncInterIpenaltyAngular(uint32_t value);
    void setEncInterIpenaltyPlanar(uint32_t value);
    void setEncInterIpenaltyDc(uint32_t value);
    void setForcedUVvalue(uint32_t uv_value);
    void setEncSrcCrop(struct v4l2_rect * src_crop);
    void setEncCrop(struct v4l2_rect * crop);
    void setFrameBufCnt(uint32_t count);
    void setBitBufCnt(uint32_t count);
    void setEncOSDinfo(struct v4l2_osd_info* info);
    void setEncLambdaScale(struct v4l2_mvx_lambda_scale *lambda_scale);
#ifdef ENABLE_CPIPE
    void setExtInput(const char *ext_input);
#endif
};

class Info :
    public Codec
{
public:
    Info(const char *dev, std::ostream &log = std::cout);
    void enumerateFormats();
};

#endif /* __MVX_PLAYER_H__ */
