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

#ifndef __MVX_LOGD_HPP__
#define __MVX_LOGD_HPP__

/******************************************************************************
 * Includes
 ******************************************************************************/

#include <fstream>
#include "mvx_log_ram.h"

struct mve_rpc_communication_area;
struct mve_msg_header;

namespace MVX
{
/******************************************************************************
 * Logd
 ******************************************************************************/

class Logd
{
public:
    Logd(const std::string &inputFile, const std::string &outputFile, int timezone, bool polling);
    virtual ~Logd();

    int run();
    int clear();

protected:
    void write(void *data, size_t size);
    void write(const char *format, ...);
    void write(const char c);
    void write(mvx_log_timeval &tv);
    static void rstrip(char *str, const char *trim);

    void getTimestamp(const mvx_log_timeval &tv, char *timestamp, size_t size) const;
    static const char *getChannelName(const mvx_log_fwif_channel channel);
    static const char *getDirection(const mvx_log_fwif_direction direction);
    static const char *getSignalName(const uint32_t code);
    static const char *getSetOptionName(const uint32_t index);
    static const char *getBufferParamName(const uint32_t type);
    static const char *getEventName(const uint32_t event);
    static const char *getNaluName(const uint32_t nalu);
    static void getSignalData(mve_msg_header *header, char *buf, size_t size);
    static void getRpcData(mve_rpc_communication_area *rpc, char *buf, size_t size);
    static const char *getRpcState(const uint32_t state);
    static const char *getRpcCallId(const uint32_t id);
    static void getFWBinaryData(const mvx_log_fw_binary *binary, char *buf, size_t size);

private:
    int readn(int fd, void *buf, size_t size);
    static int vscnprintf(char *buf, size_t size, const char *fmt, va_list ap);
    static int scnprintf(char *buf, size_t size, const char *fmt, ...);

    virtual void unpack(mvx_log_header *header) = 0;

    std::string inputFile;
    std::string outputFile;
    bool polling;
    FILE *output;
    int timezone;
};

/******************************************************************************
 * Logd binary
 ******************************************************************************/

class LogdBinary :
    public Logd
{
public:
    LogdBinary(const std::string &inputFile, const std::string &outputFile, int timezone, bool polling);

private:
    virtual void unpack(mvx_log_header *header);
};

/******************************************************************************
 * Logd text
 ******************************************************************************/

class LogdText :
    public Logd
{
public:
    LogdText(const std::string &inputFile, const std::string &outputFile, int timezone, bool polling);

private:
    virtual void unpack(mvx_log_header *header);
    void unpack(char *message, size_t length);
    void unpack(mvx_log_fwif *fwif, size_t length);
    void unpack(mvx_log_fw_binary *binary, size_t length);

    void unpackV2(mvx_log_fwif *fwif, size_t length);
    void unpackV2(mve_msg_header *msg, size_t length);
    void unpackV2(mve_rpc_communication_area *rpc, size_t length);
};

/******************************************************************************
 * Logd JSON
 ******************************************************************************/

class LogdJSON :
    public Logd
{
public:
    LogdJSON(const std::string &inputFile, const std::string &outputFile, int timezone, bool polling);

private:
    virtual void unpack(mvx_log_header *header);
    void unpack(char *message, size_t length);
    void unpack(mvx_log_fwif *fwif, size_t length);
    void unpack(mvx_log_fw_binary *binary, size_t length);

    void unpackV2(mvx_log_fwif *fwif, size_t length);
    void unpackV2(mve_msg_header *msg, size_t length);
    void unpackV2(mve_rpc_communication_area *rpc, size_t length);

    void writeJSON(const char *type, const char *value);
};
}

#endif /* __MVX_LOGD_HPP__ */
