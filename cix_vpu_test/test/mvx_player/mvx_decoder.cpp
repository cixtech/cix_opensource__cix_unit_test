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

#include "mvx_argparse.h"
#include "mvx_player.hpp"
#include <fstream>

using namespace std;


static void tokenize_values(const std::string &s, const char delim, std::vector<int32_t>&out)
{
    std::string::size_type beg = 0;
    for (std::string::size_type end = 0; (end = s.find(delim, end)) != std::string::npos; ++end)
    {
        out.push_back(atoi(s.substr(beg, end - beg).c_str()));
        beg = end + 1;
    }
    out.push_back(atoi(s.substr(beg).c_str()));
}


bool color_conversion_parse_coef(const char * conv_ceof_str,struct v4l2_mvx_color_conv_coef * conv_coef)
{
    std::vector<int32_t> ceof_list;

    tokenize_values(conv_ceof_str, ':', ceof_list);
    if (ceof_list.size() == 12)
    {
        conv_coef->coef[0][0] = ceof_list[0];
        conv_coef->coef[0][1] = ceof_list[1];
        conv_coef->coef[0][2] = ceof_list[2];

        conv_coef->coef[1][0] = ceof_list[3];
        conv_coef->coef[1][1] = ceof_list[4];
        conv_coef->coef[1][2] = ceof_list[5];

        conv_coef->coef[2][0] = ceof_list[6];
        conv_coef->coef[2][1] = ceof_list[7];
        conv_coef->coef[2][2] = ceof_list[8];


        conv_coef->offset[0] = ceof_list[9];
        conv_coef->offset[1] = ceof_list[10];
        conv_coef->offset[2] = ceof_list[11];

   }
   cout << "color_conversion_parse_coef coef[0][0]( " << conv_coef->coef[0][0]<< ")" << endl;
   cout << "color_conversion_parse_coef coef[0][1]( " << conv_coef->coef[0][1]<< ")" << endl;
   cout << "color_conversion_parse_coef coef[0][2]( " << conv_coef->coef[0][2]<< ")" << endl;
   cout << "color_conversion_parse_coef coef[1][0]( " << conv_coef->coef[1][0]<< ")" << endl;
   cout << "color_conversion_parse_coef coef[1][1]( " << conv_coef->coef[1][1]<< ")" << endl;
   cout << "color_conversion_parse_coef coef[1][2]( " << conv_coef->coef[1][2]<< ")" << endl;
   cout << "color_conversion_parse_coef coef[2][0]( " << conv_coef->coef[2][0]<< ")" << endl;
   cout << "color_conversion_parse_coef coef[2][1]( " << conv_coef->coef[2][1]<< ")" << endl;
   cout << "color_conversion_parse_coef coef[2][2]( " << conv_coef->coef[2][2]<< ")" << endl;
   cout << "color_conversion_parse_coef offset[0] ( " << conv_coef->offset[0]<< ")" << endl;
   cout << "color_conversion_parse_coef offset[1] ( " << conv_coef->offset[1]<< ")" << endl;
   cout << "color_conversion_parse_coef offset[2] ( " << conv_coef->offset[2]<< ")" << endl;

   if (    ( conv_coef->offset[0]<0 || conv_coef->offset[0]>255)
        || ( conv_coef->offset[1]<0 || conv_coef->offset[1]>255)
        || ( conv_coef->offset[2]<0 || conv_coef->offset[2]>255))
   {
        return false;
   }
    return (ceof_list.size() == 12) ? true: false;
}
int main(int argc, const char *argv[])
{
    int ret;
    mvx_argparse argp;
    uint32_t inputFormat;
    uint32_t outputFormat;

    mvx_argp_construct(&argp);
    mvx_argp_add_opt(&argp, '\0', "dev", true, 1, "/dev/video0", "Device.");
    mvx_argp_add_opt(&argp, 'i', "inputformat", true, 1, "h264", "Pixel format.");
    mvx_argp_add_opt(&argp, 'o', "outputformat", true, 1, "yuv420", "Output pixel format.");
    mvx_argp_add_opt(&argp, 'f', "format", true, 1, "ivf", "Input container format. [ivf, rcv, raw]\n\t\tFor ivf input format will be taken from IVF header.");
    mvx_argp_add_opt(&argp, 's', "strideAlign", true, 1, "1", "Stride alignment.");
    mvx_argp_add_opt(&argp, 0, "stride0", true, 1, "1", "Number of bytes of stride for the first plane.");
    mvx_argp_add_opt(&argp, 0, "stride1", true, 1, "1", "Number of bytes of stride for the second plane if have.");
    mvx_argp_add_opt(&argp, 0, "stride2", true, 1, "1", "Number of bytes of stride for the third plane if have.");
    mvx_argp_add_opt(&argp, 'y', "intbuf", true, 1, "1000000", "Limit of intermediate buffer size");
    mvx_argp_add_opt(&argp, 'u', "nalu", true, 1, "1", "Nalu format, DEFAULT(0), START_CODES (1), ONE_NALU_PER_BUFFER (2), ONE_BYTE_LENGTH_FIELD (3), TWO_BYTE_LENGTH_FIELD (4), FOUR_BYTE_LENGTH_FIELD (4).");
    mvx_argp_add_opt(&argp, 'r', "rotate", true, 1, "0", "Rotation, 0 | 90 | 180 | 270");
    mvx_argp_add_opt(&argp, 'd', "downscale", true, 1, "1", "Down Scale, 1 | 2 | 4");
    mvx_argp_add_opt(&argp, 0, "memory", true, 1, "mmap", "support mmap, userptr and dma.");
    mvx_argp_add_opt(&argp, 0, "input_thread", true, 1, "0", "read input buffer in other thread.");
    mvx_argp_add_opt(&argp, 0, "dsl_ratio_hor", true, 1, "0", "Horizontal downscale ratio, [1, 256]");
    mvx_argp_add_opt(&argp, 0, "dsl_ratio_ver", true, 1, "0", "Vertical downscale ratio, [1, 128]");
    mvx_argp_add_opt(&argp, 0, "dsl_frame_width", true, 1, "0", "Downscaled frame width in pixels");
    mvx_argp_add_opt(&argp, 0, "dsl_frame_height", true, 1, "0", "Downscaled frame height in pixels");
    mvx_argp_add_opt(&argp, 0, "dsl_pos_mode", true, 1, "0", "Flexible Downscaled original position mode [0, 2], only availble in high precision mode."
                                                                "\t\tValue: 0 [default:x_original=(x_resized + 0.5)/scale - 0.5]"
                                                                "\t\tValue: 1 [x_original=x_reized/scale]"
                                                                "\t\tValue: 2 [x_original=(x_resized+0.5)/scale]");
    mvx_argp_add_opt(&argp, 0, "dsl_nearest_mode", true, 1, "0", "Downscaling Interpolation mode: 0: Bilinear(default), 1: Nearest");
    mvx_argp_add_opt(&argp, 0, "frames", true, 1, "0", "nr of frames to process");
    mvx_argp_add_opt(&argp, 0, "fro", true, 1, "1", "Frame reordering 1 is on (default), 0 is off");
    mvx_argp_add_opt(&argp, 0, "ish", true, 1, "0", "Ignore Stream Headers 1 is on, 0 is off (default)");
    mvx_argp_add_opt(&argp, 0, "trystop", true, 0, "0", "Try if Decoding Stop Command exixts");
    mvx_argp_add_opt(&argp, 0, "one_frame_per_packet", true, 0, "0", "Each input buffer contains one frame.");
    mvx_argp_add_opt(&argp, 0, "framebuffer_cnt", true, 1, NULL, "Number of buffers to use for yuv data");
    mvx_argp_add_opt(&argp, 0, "bitbuffer_cnt", true, 1, NULL, "Number of buffers to use for bitstream data");
    mvx_argp_add_opt(&argp, 'n', "interlaced", true, 0, "0", "Frames are interlaced");
    mvx_argp_add_opt(&argp, '0', "color_conversion", true, 1, "0", "decoder color conversion for ycbcr2rgb."
                                                                "\t\tValue: 0 [default:predefined standards bt601]"
                                                                "\t\tValue: 1 [predefined standards bt601f]"
                                                                "\t\tValue: 2 [predefined standards bt709]"
                                                                "\t\tValue: 3 [predefined standards bt709f]"
                                                                "\t\tValue: 4 [predefined standards bt2020]"
                                                                "\t\tValue: 5 [predefined standards bt2020f]");
    mvx_argp_add_opt(&argp, 0, "cust_yuv2rgb_coef", true, 1, "", "customized integer coeffiecents for decoder ycbcr2rgb y2r:u2r:v2r:y2g:u2g:v2g:y2b:u2b:v2b:yoffset:cboffste:croffset");
    mvx_argp_add_opt(&argp, 0, "disable_features", true, 1, 0, "Disable features bitmask:"
                                                                "\t\tb0=AFBC compression, b1=REF caching, b2=Deblock, b3=SAO,b5=Picture Output Removal, "
                                                                "\t\tb6=Pipe, b7=Sleep b8=LegacyAFBC, b9=FilmGrain b12=REFSZ limit");

    mvx_argp_add_opt(&argp, 0, "dst_crop_x", true, 1, 0, "left start x of luma in output image");
    mvx_argp_add_opt(&argp, 0, "dst_crop_y", true, 1, 0, "top start y of luma in output image");
    mvx_argp_add_opt(&argp, 0, "dst_crop_width", true, 1, 0, "cropped width of luma in output image");
    mvx_argp_add_opt(&argp, 0, "dst_crop_height", true, 1, 0, "cropped height of luma in output image");

#ifdef ENABLE_DISPLAY
    mvx_argp_add_opt(&argp, 'D', "display", true, 1, "komeda -P 0@0:640x480 -s 0", "display configuration");
    mvx_argp_add_opt(&argp, 0, "bg_infile", true, 1, NULL, "background image file, must be in ARGB format");
    mvx_argp_add_opt(&argp, 0, "bg_width", true, 1, NULL, "Width of background image in pixels");
    mvx_argp_add_opt(&argp, 0, "bg_height", true, 1, NULL, "Height of background image in pixels");
#endif

    mvx_argp_add_opt(&argp, '\0', "tiled", true, 0, "disabled", "Use tiles for AFBC formats.");
    mvx_argp_add_opt(&argp, '\0', "packed", true, 0, "disabled", "Abandon extra stride area for dump.");
    mvx_argp_add_opt(&argp, 0, "dual_afbc_downscaled", true, 1, "0", "For AFBC output only: Also output witdh/2 height/2 downscaled AFBC frame together with original afbc frame. 0:disable 1:enable");
    mvx_argp_add_opt(&argp, 'S', "secure", true, 1, "0", "Enable secure decoding.");
    mvx_argp_add_opt(&argp, 0, "zero_out", true, 1, "0", "Zero out output buffers before buffer queueing");
    mvx_argp_add_opt(&argp, 0, "convert_p010", true, 1, "0", "Convert P010 to yuv420p10le as post-processing");
    mvx_argp_add_opt(&argp, 0, "job_frames", true, 1, "1", "Number of frames to process for one job");
    mvx_argp_add_opt(&argp, 0, "fps_n", true, 1, "0", "Numerator of output frame rate");
    mvx_argp_add_opt(&argp, 0, "fps_d", true, 1, "0", "Denominator of output frame rate");
    mvx_argp_add_opt(&argp, 0, "seek_points", true, 1, "", "Seek points. from_frame0,to_offset0;from_frame1,to_offset1;...");
    mvx_argp_add_opt(&argp, 0, "rewind", true, 1, "0", "rewind the input when reaches eof");

    mvx_argp_add_pos(&argp, "input", false, 1, "", "Input file.");
    mvx_argp_add_pos(&argp, "output", false, 1, "", "Output file.");
    mvx_argp_add_opt(&argp, 0, "seamless", true, 1, "", "seamless mode for mjepg. 0:disable 1:fixed buffer w/h fhd,1:fixed buffer uhd");
    mvx_argp_add_opt(&argp, 0, "read_bytes_cfg", true, 1, NULL, "Per frame bitstream bytes config for low-latency decoding input stream file");
    mvx_argp_add_opt(&argp, 0, "fsf", true, 1, "0", "Enable (1) or disable (0) fast show frame for AV1 decoder");
    mvx_argp_add_opt(&argp, 'P', "priority", true, 1, "2", "Scheduling priority: 0: preemption, 1: high, 2: normal, 3: low");

    ret = mvx_argp_parse(&argp, argc - 1, &argv[1]);
    if (ret != 0)
    {
        mvx_argp_help(&argp, argv[0]);
        return 1;
    }

    inputFormat = Codec::to4cc(mvx_argp_get(&argp, "inputformat", 0));
    if (inputFormat == 0)
    {
        fprintf(stderr, "Error: Illegal bitstream format. format=%s.\n",
                mvx_argp_get(&argp, "inputformat", 0));
        return 1;
    }

    ifstream is(mvx_argp_get(&argp, "input", 0));
    if (is.fail())
    {
        fprintf(stderr, "Error: Open input file failed.\n");
        return 1;
    }
    InputFile *inputFile;
    if (string(mvx_argp_get(&argp, "format", 0)).compare("ivf") == 0)
    {
        inputFile = new InputIVF(is, inputFormat);
    }
    else if (string(mvx_argp_get(&argp, "format", 0)).compare("rcv") == 0)
    {
        inputFile = new InputRCV(is);
    }
    else if (string(mvx_argp_get(&argp, "format", 0)).compare("raw") == 0)
    {
        inputFile = new InputFile(is, inputFormat);
    }
    else
    {
        cerr << "Error: Unsupported container format. format=" <<
        mvx_argp_get(&argp, "format", 0) << "." << endl;
        return 1;
    }
    inputFile->setRewind(mvx_argp_get_int(&argp, "rewind", 0));

    if (mvx_argp_is_set(&argp, "read_bytes_cfg")) {
        const char* read_bytes_cfg = mvx_argp_get(&argp, "read_bytes_cfg", 0);
        std::ifstream fin(read_bytes_cfg);
        std::string line;
        while (getline(fin, line)) {
            inputFile->frame_bytes.push(atoi(line.c_str()));
        }
        fin.close();
    }
    int nalu_format = mvx_argp_get_int(&argp,"nalu",0);
    int rotation = mvx_argp_get_int(&argp,"rotate",0);
    //int scale = mvx_argp_get_int(&argp,"downscale",0);
    int frames = mvx_argp_get_int(&argp,"frames",0);
    if (rotation % 90 != 0){
        cerr << "Unsupported rotation:"<<rotation <<endl;
        rotation = 0;
    }
    outputFormat = Codec::to4cc(mvx_argp_get(&argp, "outputformat", 0));
    if (outputFormat == 0)
    {
        fprintf(stderr, "Error: Illegal frame format. format=%s.\n",
                mvx_argp_get(&argp, "outputformat", 0));
        return 1;
    }

    bool interlaced = mvx_argp_is_set(&argp, "interlaced");
    bool tiled = mvx_argp_is_set(&argp, "tiled");
    bool packed = mvx_argp_is_set(&argp, "packed");
    ofstream os(mvx_argp_get(&argp, "output", 0), ios::binary);
    Output *output;
#ifdef ENABLE_DISPLAY
    if(mvx_argp_is_set(&argp, "display")) {
        if (Codec::isAFBC(outputFormat))
            output = new OutputDisplayAFBC(mvx_argp_get(&argp, "display", 0), outputFormat, tiled);
        else
            output = new OutputDisplay(mvx_argp_get(&argp, "display", 0), outputFormat);
        if (mvx_argp_is_set(&argp, "bg_infile") &&
            mvx_argp_is_set(&argp, "bg_width") && mvx_argp_is_set(&argp, "bg_height")) {
            OutputDisplay *display = dynamic_cast<OutputDisplay*>(output);
            InputFileLayer *bg_layer = new InputFileLayer(mvx_argp_get(&argp, "bg_infile", 0),
                                            Codec::to4cc("bgra"),
                                            mvx_argp_get_int(&argp, "bg_width", 0),
                                            mvx_argp_get_int(&argp, "bg_height", 0));
            display->addBackground(bg_layer);
            delete bg_layer;
        }
    } else {
#endif
        if (Codec::isAFBC(outputFormat)) {
            output =new OutputAFBC(os, outputFormat, tiled);
        } else {
            output = new OutputFile(os, outputFormat, packed);
        }
#ifdef ENABLE_DISPLAY
    }
#endif
    if (strstr(mvx_argp_get(&argp, "output", 0), "/dev/null") != NULL)
        output->setSkipOutput();

    char devName[MVX_MAX_PATH_LEN];
    if (mvx_argp_is_set(&argp, "dev"))
    {
        snprintf(devName, MVX_MAX_PATH_LEN, "%s", mvx_argp_get(&argp, "dev", 0));
    }
    else
    {
        if (!Codec::findAvailableDevice(inputFormat, outputFormat, devName))
        {
            cerr << "Could not find available video device" <<endl;
            return 1;
        }
    }
    Decoder decoder(devName, *inputFile, *output);
    if (mvx_argp_is_set(&argp, "intbuf"))
    {
        decoder.setH264IntBufSize(mvx_argp_get_int(&argp, "intbuf", 0));
    }
    if (mvx_argp_is_set(&argp, "fro"))
    {
        decoder.setFrameReOrdering(mvx_argp_get_int(&argp, "fro", 0));
    }
    if (mvx_argp_is_set(&argp, "ish"))
    {
        decoder.setIgnoreStreamHeaders(mvx_argp_get_int(&argp, "ish", 0));
    }
    if (mvx_argp_is_set(&argp, "trystop"))
    {
        decoder.tryStopCmd(true);
    }
    if (mvx_argp_is_set(&argp, "one_frame_per_packet"))
    {
        decoder.setNaluFormat(V4L2_OPT_NALU_FORMAT_ONE_FRAME_PER_BUFFER);
    } else {
        decoder.setNaluFormat(nalu_format);
    }
    if (mvx_argp_is_set(&argp, "input_thread"))
    {
        decoder.setInputThread(mvx_argp_get_int(&argp, "input_thread", 0));
    }
    if (mvx_argp_is_set(&argp, "memory"))
    {
        const char *memory_type = mvx_argp_get(&argp, "memory", 0);
        if (strcmp(memory_type, "mmap") == 0) {
            decoder.setMemoryType(V4L2_MEMORY_MMAP);
        } else if (strcmp(memory_type, "dma") == 0) {
            decoder.setMemoryType(V4L2_MEMORY_DMABUF);
        } else if (strcmp(memory_type, "userptr") == 0) {
            decoder.setMemoryType(V4L2_MEMORY_USERPTR);
        } else {
            cerr<<"didnot support this memory type!!!"<<endl;
        }
    }

    if (mvx_argp_is_set(&argp, "dsl_frame_width") && mvx_argp_is_set(&argp, "dsl_frame_height")){
        assert(!Codec::isAFBC(outputFormat));
        assert(!mvx_argp_is_set(&argp, "dsl_ratio_hor") && !mvx_argp_is_set(&argp, "dsl_ratio_ver"));
        int width = mvx_argp_get_int(&argp, "dsl_frame_width", 0);
        int height = mvx_argp_get_int(&argp, "dsl_frame_height", 0);
        assert(2 <= width && 2 <= height);
        decoder.setDSLFrame(width,height);
    } else if (mvx_argp_is_set(&argp, "dsl_frame_width") || mvx_argp_is_set(&argp, "dsl_frame_height")){
        cerr << "Downscale frame width and height shoule be set together!"<<endl;
    }

    if (mvx_argp_is_set(&argp, "dsl_ratio_hor") || mvx_argp_is_set(&argp, "dsl_ratio_ver")){
        assert(!Codec::isAFBC(outputFormat));
        assert(!mvx_argp_is_set(&argp, "dsl_frame_width") && !mvx_argp_is_set(&argp, "dsl_frame_height"));
        int hor = mvx_argp_is_set(&argp, "dsl_ratio_hor")? mvx_argp_get_int(&argp, "dsl_ratio_hor", 0): 1;
        int ver = mvx_argp_is_set(&argp, "dsl_ratio_hor")? mvx_argp_get_int(&argp, "dsl_ratio_ver", 0): 1;
        decoder.setDSLRatio(hor,ver);
    }

    if (mvx_argp_is_set(&argp, "dsl_pos_mode")) {
        assert(mvx_argp_is_set(&argp, "dsl_ratio_hor") || mvx_argp_is_set(&argp, "dsl_ratio_ver") ||
                mvx_argp_is_set(&argp, "dsl_frame_width") || mvx_argp_is_set(&argp, "dsl_frame_height"));
        int mode = mvx_argp_get_int(&argp, "dsl_pos_mode", 0);
        if (mode < 0 || mode > 2) {
            mode = 0;
        }
        decoder.setDSLMode(mode);
    }

    if (mvx_argp_is_set(&argp, "dsl_nearest_mode")) {
      assert(mvx_argp_is_set(&argp, "dsl_ratio_hor") || mvx_argp_is_set(&argp, "dsl_ratio_ver") ||
                mvx_argp_is_set(&argp, "dsl_frame_width") || mvx_argp_is_set(&argp, "dsl_frame_height"));
           int mode = mvx_argp_get_int(&argp, "dsl_nearest_mode", 0);
           if (mode < 0 || mode > 1) {
               mode = 0;
           }
           decoder.setDSLInterpMode(mode);
    }

    uint32_t disable_features = mvx_argp_get_int(&argp, "disable_features", 0);
    if (Codec::isAFBC(outputFormat) && tiled && V4L2_PIX_FMT_AV1 == inputFormat)
    {
        //set DISABLE_FEATURE_SUPPORT_NONIBC_TILE
        decoder.setDisabledFeatures(disable_features | 0x10000);
    }
    else
    {
        if (mvx_argp_is_set(&argp, "disable_features"))
        {
            decoder.setDisabledFeatures(mvx_argp_get_int(&argp, "disable_features", 0));
        }
    }

    if (mvx_argp_is_set(&argp, "color_conversion"))
    {
        int conv_mode = mvx_argp_get_int(&argp, "color_conversion", 0);
           if (conv_mode < 0 || conv_mode > 5) {
            conv_mode = 0;
        }
        decoder.setColorConversion(conv_mode);
    }

    if (mvx_argp_is_set(&argp, "cust_yuv2rgb_coef"))
    {
        assert(!mvx_argp_is_set(&argp, "color_conversion"));
        struct v4l2_mvx_color_conv_coef  conv_coef;
        if(true==color_conversion_parse_coef(mvx_argp_get(&argp, "cust_yuv2rgb_coef", 0),&conv_coef))
        decoder.setCustColorConvCoef(&conv_coef);
        else
        cerr << "invalid  yuv2rgb csd coef params,pls check " << endl;
    }

    if (mvx_argp_is_set(&argp, "seek_points"))
    {
        int i;
        std::vector<int32_t> params;
        tokenize_values(mvx_argp_get(&argp, "seek_points", 0), ':', params);
        for (i = 0; i < (int)params.size(); i += 2) {
            decoder.addSeekPoint(params[i], params[i+1]);
            cout << "Seek point " << i/2 << ": " << params[i] << ", " << params[i+1] << endl;
        }
    }

    if (mvx_argp_is_set(&argp, "dst_crop_x") && mvx_argp_is_set(&argp, "dst_crop_y")
    && mvx_argp_is_set(&argp, "dst_crop_width") && mvx_argp_is_set(&argp, "dst_crop_height"))
    {
        assert(!Codec::isAFBC(outputFormat));
        assert(!mvx_argp_is_set(&argp, "dsl_ratio_hor") && !mvx_argp_is_set(&argp, "dsl_ratio_ver"));
        assert(!mvx_argp_is_set(&argp, "dsl_frame_width") && !mvx_argp_is_set(&argp, "dsl_frame_height"));
        assert(rotation == 0);
        assert(mvx_argp_get_int(&argp, "dst_crop_x", 0) %4==0);
        assert(mvx_argp_get_int(&argp, "dst_crop_y", 0) %4==0);
        assert(mvx_argp_get_int(&argp, "dst_crop_width", 0) %4==0);
        assert(mvx_argp_get_int(&argp, "dst_crop_height", 0) %4==0);
        assert(mvx_argp_get_int(&argp, "dst_crop_width", 0) >0);
        assert(mvx_argp_get_int(&argp, "dst_crop_height", 0) >0);

        struct v4l2_rect dst_crop;
        dst_crop.left = mvx_argp_get_int(&argp, "dst_crop_x", 0);
        dst_crop.top = mvx_argp_get_int(&argp, "dst_crop_y", 0);
        dst_crop.width = mvx_argp_get_int(&argp, "dst_crop_width", 0);
        dst_crop.height = mvx_argp_get_int(&argp, "dst_crop_height", 0);
        decoder.setDecDstCrop(&dst_crop);
    }

    if (mvx_argp_is_set(&argp, "stride0") || mvx_argp_is_set(&argp, "stride1") ||
        mvx_argp_is_set(&argp, "stride2")) {
        size_t stride[VIDEO_MAX_PLANES] = {0};
        stride[0] = mvx_argp_get_int(&argp, "stride0", 0);
        stride[1] = mvx_argp_get_int(&argp, "stride1", 0);
        stride[2] = mvx_argp_get_int(&argp, "stride2", 0);
        decoder.setStride(stride);
    }
    if(mvx_argp_is_set(&argp, "seamless") && mvx_argp_get_int(&argp, "seamless", 0) !=0)
    {
        assert(!Codec::isAFBC(outputFormat));
        unsigned int target_width = 1920;
        unsigned int target_height =1088;
        struct v4l2_mvx_seamless_target seamless;
        seamless.seamless_mode = mvx_argp_get_int(&argp, "seamless", 0);

        if(mvx_argp_get_int(&argp, "seamless", 0) == 1)
        {
            target_width = 1920;
            target_height =1088;
        }
        else if(mvx_argp_get_int(&argp, "seamless", 0) == 2)
        {
            target_width = 3840;
            target_height =2160;
        }

         seamless.target_width     = target_width;
         seamless.target_height    = target_height;
         decoder.setSeamlessTarget(outputFormat,&seamless);
    }
    if (mvx_argp_is_set(&argp, "dual_afbc_downscaled") && mvx_argp_get_int(&argp, "dual_afbc_downscaled", 0) !=0) {
        assert(Codec::isAFBC(outputFormat));
        assert(interlaced ==0);
        decoder.setDualAfbcDownScale(mvx_argp_get_int(&argp,"dual_afbc_downscaled",0));
    }
    if (mvx_argp_is_set(&argp, "downscale")
        &&  (mvx_argp_get_int(&argp, "downscale", 0) ==2 || mvx_argp_get_int(&argp, "downscale", 0) ==4))
    {
        assert(!Codec::isAFBC(outputFormat));
        decoder.setDownScale(mvx_argp_get_int(&argp,"downscale",0));
    }
    if (mvx_argp_is_set(&argp, "secure") && mvx_argp_get_int(&argp, "secure", 0) != 0) {
        /* Limit supported input format to H.264 and HEVC to simplify secure decoding case for now. */
        assert(inputFormat == Codec::to4cc("h264") || inputFormat == Codec::to4cc("hevc"));
        decoder.setSecureVideo();
        decoder.setNaluFormat(V4L2_OPT_NALU_FORMAT_START_CODES);
        inputFile->setSecureVideo();
        output->setSecureVideo();
    }
    if (mvx_argp_is_set(&argp, "zero_out") && mvx_argp_get_int(&argp, "zero_out", 0) != 0) {
        decoder.setZeroOutAfbc();
    }
    if (mvx_argp_is_set(&argp, "convert_p010") && mvx_argp_get_int(&argp, "convert_p010", 0) != 0) {
        output->setConvert10bit();
    }
    if (mvx_argp_is_set(&argp, "job_frames")) {
        decoder.setJobFrames(mvx_argp_get_int(&argp, "job_frames", 0));
    }
    if (mvx_argp_is_set(&argp, "fps_n") && mvx_argp_is_set(&argp, "fps_d")) {
        unsigned int numerator = mvx_argp_get_int(&argp, "fps_n", 0);
        unsigned int denominator = mvx_argp_get_int(&argp, "fps_d", 0);
        output->setFrameRate(numerator, denominator);
    }
    if (mvx_argp_is_set(&argp, "fsf"))
    {
        assert("this option is only enabled for AV1 codec" && V4L2_PIX_FMT_AV1 == inputFormat);
        decoder.setFsfMode(mvx_argp_get_int(&argp,"fsf", 0));
    }
    if (mvx_argp_is_set(&argp, "priority"))
    {
        int priority = mvx_argp_get_int(&argp, "priority", 0);
        if (priority < 0 || priority > 3) {
            priority = V4L2_SESSION_PRIORITY_NORMAL; // default to normal
        }
        decoder.setPriority(priority);
    }
    decoder.setInterlaced(interlaced);
    decoder.setRotation(rotation);
    decoder.setFrameCount(frames);
    decoder.setFrameBufCnt(mvx_argp_get_int(&argp, "framebuffer_cnt", 0));
    decoder.setBitBufCnt(mvx_argp_get_int(&argp, "bitbuffer_cnt", 0));
    ret = decoder.stream();

    delete inputFile;
    delete output;

    return ret;
}
