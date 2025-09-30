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

/******************************************************************************
 * Includes
 ******************************************************************************/

#include "mvx_logd.hpp"
#include "mvx_log_ram.h"

#include <cstring>
#include <cstddef>
#include <iostream>

#include <poll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>

using namespace std;
using namespace MVX;

/******************************************************************************
 * Logd
 ******************************************************************************/

Logd::Logd(const string &inputFile, const string &outputFile, int timezone, bool polling) :
    inputFile(inputFile),
    outputFile(outputFile),
    polling(polling),
    output(0),
    timezone(timezone)
{}

Logd::~Logd()
{}

int Logd::clear()
{
    int fd = open(inputFile.c_str(), 0);
    int ret;

    if (fd < 0)
    {
        cerr << "Error: IOCTL failed to open file descriptor." << endl;
        return 2;
    }

    ret = ioctl(fd, MVX_LOG_IOCTL_CLEAR);
    if (ret < 0)
    {
        cerr << "Error: IOCTL failed to clear log file." << endl;
        close(fd);
        return 2;
    }

    close(fd);

    return 0;
}

int Logd::run()
{
    char *buf;
    int ret = 0;

    int inputfd = open(inputFile.c_str(), O_RDONLY | O_NONBLOCK);
    if (inputfd < 0)
    {
        cerr << "Error: Failed to open input '" << inputFile << "'." << endl;
        return 1;
    }

    /* TODO append. */
    bool closeOutput = false;
    output = stdout;
    if (outputFile.compare("stdout") != 0)
    {
        output = fopen(outputFile.c_str(), "w+");
        if (output == 0)
        {
            cerr << "Error: Failed to open output '" << outputFile << "'." << endl;
            return 1;
        }

        closeOutput = true;
    }

    /* Allocate message buffer. Add one byte to allow unpack to null
     * terminate strings.
     */
    buf = new char[MVX_LOG_MESSAGE_LENGTH_MAX + 1];
    if (buf == NULL)
    {
        cerr << "Failed to allocate message buffer." << endl;
        return -1;
    }

    while (1)
    {
        mvx_log_header *header = (mvx_log_header *)buf;
        size_t n;

        /* Read magic word. */
        ret = readn(inputfd, &header->magic, sizeof(header->magic));
        if (ret != 0)
        {
            break;
        }

        if (header->magic != MVX_LOG_MAGIC)
        {
            continue;
        }

        /* Read rest of header. This assumes that the header always begins with
         * the magic word followed by the length field. */
        ret = readn(inputfd, &header->length, sizeof(*header) - offsetof(typeof(*header), length));
        if (ret != 0)
        {
            break;
        }

        /* Verify that length field is valid. */
        if (header->length > MVX_LOG_MESSAGE_LENGTH_MAX)
        {
            cerr << "Length field is too large. length=" << header->length << ", max=" << MVX_LOG_MESSAGE_LENGTH_MAX << "." << endl;
            continue;
        }

        /* Verify that type field is valid. */
        if (header->type >= MVX_LOG_TYPE_MAX)
        {
            cerr << "Illegal type field. type=" << header->type << "." << endl;
            continue;
        }

        /* Round up to next 32-bit boundary and read buffer into
         * memory.
         */
        n = (header->length + 3) & ~3;

        /* Read message body. */
        ret = readn(inputfd, (header + 1), n);
        if (ret != 0)
        {
            cerr << "Failed to read message body. length=" << header->length << "." << endl;
            ret = 0;
            break;
        }

        /* Unpack format. */
        unpack(header);

        /* Flush output. */
        fflush(output);
    }

    delete[] buf;

    if (closeOutput)
    {
        fclose(output);
    }

    close(inputfd);

    return ret;
}

void Logd::write(void *data, size_t size)
{
    size_t n = fwrite(data, size, 1, output);
    if (n != 1)
    {
        cerr << "Error: Failed to write data." << endl;
    }
}

void Logd::write(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vfprintf(output, format, ap);
    va_end(ap);
}

void Logd::write(const char c)
{
    fputc(c, output);
}

void Logd::write(mvx_log_timeval &tv)
{
    char buf[64];
    getTimestamp(tv, buf, sizeof(buf));
    write("%s", buf);
}

void Logd::getTimestamp(const mvx_log_timeval &tv, char *timestamp, size_t size) const
{
    struct tm tm;

    /* Print date to buffer. */
    time_t sec = tv.sec;
    if (timezone != 0)
    {
        sec += timezone;
        gmtime_r(&sec, &tm);
    }
    else
    {
        localtime_r(&sec, &tm);
    }

    size_t n = strftime(timestamp, size, "%m-%d %H:%M:%S", &tm);
    snprintf(&timestamp[n], size - n, ".%06u", (unsigned int)(tv.nsec / 1000));
}

const char *Logd::getChannelName(const mvx_log_fwif_channel channel)
{
    switch (channel)
    {
        case MVX_LOG_FWIF_CHANNEL_MESSAGE:
            return "MSG";
        case MVX_LOG_FWIF_CHANNEL_INPUT_BUFFER:
            return "INP";
        case MVX_LOG_FWIF_CHANNEL_OUTPUT_BUFFER:
            return "OUT";
        case MVX_LOG_FWIF_CHANNEL_RPC:
            return "RPC";
        default:
            return "UNKNOWN";
    }
}

const char *Logd::getDirection(const mvx_log_fwif_direction direction)
{
    if (direction == MVX_LOG_FWIF_DIRECTION_HOST_TO_FIRMWARE)
    {
        return "->";
    }
    else
    {
        return "<-";
    }
}

void Logd::getFWBinaryData(const mvx_log_fw_binary *binary, char *buf, size_t size)
{
    struct FirmwareHeader
    {
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
    }
    *firmwareHeader = (FirmwareHeader *)(binary + 1);

    /* Print firmware header. */
    scnprintf(buf, size, "protocol_major=%u, protocol_minor=%u, info_string=\"%s\", part_number=\"%s\", version_string=\"%s\"",
              firmwareHeader->protocol_major, firmwareHeader->protocol_minor,
              firmwareHeader->info_string, firmwareHeader->part_number,
              firmwareHeader->version_string);
}

int Logd::readn(int fd, void *buf, size_t size)
{
    ssize_t n;
    char *b = (char *)buf;

    while (size > 0)
    {
        if (polling)
        {
            struct pollfd pfd = {
                .fd     = fd,
                .events = POLLIN | POLLPRI
            };
            int ret = poll(&pfd, 1, -1);

            if (ret < 0 || (pfd.revents & (POLLIN | POLLPRI)) == 0)
            {
                return -1;
            }
        }

        n = read(fd, b, size);
        if (n <= 0)
        {
            return -1;
        }

        b += n;
        size -= n;
    }

    return 0;
}

int Logd::vscnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    int n;

    n = vsnprintf(buf, size, fmt, ap);

    /* Return 0 on error. */
    n = max(0, n);

    /* Return number of bytes written. */
    n = min(n, (int)size - 1);

    return n;
}

int Logd::scnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    int n;

    va_start(args, fmt);
    n = vscnprintf(buf, size, fmt, args);
    va_end(args);

    return n;
}

void Logd::rstrip(char *str, const char *trim)
{
    size_t l = strlen(str);

    while (l-- > 0)
    {
        const char *t;

        for (t = trim; *t != '\0'; t++)
        {
            if (str[l] == *t)
            {
                str[l] = '\0';
                break;
            }
        }

        if (*t == '\0')
        {
            break;
        }
    }
}

/******************************************************************************
 * Logd binary
 ******************************************************************************/

LogdBinary::LogdBinary(const string &inputFile, const string &outputFile, int timezone, bool polling) :
    Logd(inputFile, outputFile, timezone, polling)
{}

void LogdBinary::unpack(mvx_log_header *header)
{
    /* Round up length to next 4 byte boundary. */
    size_t len = (header->length + 3) & ~3;

    /* Write header. */
    write((void *)header, sizeof(*header) + len);
}

/******************************************************************************
 * Logd text
 ******************************************************************************/

LogdText::LogdText(const string &inputFile, const string &outputFile, int timezone, bool polling) :
    Logd(inputFile, outputFile, timezone, polling)
{}

void LogdText::unpack(mvx_log_header *header)
{
    /* Print date to buffer. */
    write(header->timestamp);

    switch (header->type)
    {
        case MVX_LOG_TYPE_TEXT:
            unpack(reinterpret_cast<char *>(header + 1), header->length);
            break;
        case MVX_LOG_TYPE_FWIF:
            unpack(reinterpret_cast<mvx_log_fwif *>(header + 1), header->length);
            break;
        case MVX_LOG_TYPE_FW_BINARY:
            unpack(reinterpret_cast<mvx_log_fw_binary *>(header + 1), header->length);
            break;
        default:
            cerr << "Warning: Unsupported header type '" << header->type << "'." << endl;
    }
}

void LogdText::unpack(char *message, size_t length)
{
    /* Make sure message is NULL terminated. */
    message[length] = '\0';

    write(" %s\n", message);
}

void LogdText::unpack(mvx_log_fwif *fwif, size_t length)
{
    /* Print session, channel name and direction. */
    write(" %" PRIx64 " %s %s ", fwif->session,
          getChannelName(mvx_log_fwif_channel(fwif->channel)),
          getDirection(mvx_log_fwif_direction(fwif->direction)));

    switch (fwif->version_major)
    {
        case 2:
        case 3:
            unpackV2(fwif, length);
            break;
        default:
            cerr << "Warning: Unsupported FW IF version. version_major=" << (int)fwif->version_major << "." << endl;
            return;
    }

    write("\n");
}

void LogdText::unpack(mvx_log_fw_binary *binary, size_t length)
{
    char data[500];
    getFWBinaryData(binary, data, sizeof(data));
    write(" %" PRIx64 " fw_binary={%s}\n", binary->session, data);
}

/******************************************************************************
 * Logd JSON
 ******************************************************************************/

LogdJSON::LogdJSON(const string &inputFile, const string &outputFile, int timezone, bool polling) :
    Logd(inputFile, outputFile, timezone, polling)
{}

void LogdJSON::unpack(mvx_log_header *header)
{
    char timestamp[64];
    getTimestamp(header->timestamp, timestamp, sizeof(timestamp));
    write("{\"timestamp\": \"%s\"", timestamp);

    switch (header->type)
    {
        case MVX_LOG_TYPE_TEXT:
            unpack(reinterpret_cast<char *>(header + 1), header->length);
            break;
        case MVX_LOG_TYPE_FWIF:
            unpack(reinterpret_cast<mvx_log_fwif *>(header + 1), header->length);
            break;
        case MVX_LOG_TYPE_FW_BINARY:
            unpack(reinterpret_cast<mvx_log_fw_binary *>(header + 1), header->length);
            break;
        default:
            cerr << "Warning: Unsupported header type '" << header->type << "'." << endl;
    }

    write("}\n");
}

void LogdJSON::unpack(char *message, size_t length)
{
    /* Make sure message is NULL terminated. */
    message[length] = '\0';

    writeJSON("text", message);
}

void LogdJSON::unpack(mvx_log_fwif *fwif, size_t length)
{
    /* Print session, channel name and direction. */
    write(", \"fwif\": { \"session\": \"%" PRIx64 "\"", fwif->session);
    writeJSON("channel", getChannelName(mvx_log_fwif_channel(fwif->channel)));
    writeJSON("direction", getDirection(mvx_log_fwif_direction(fwif->direction)));

    switch (fwif->version_major)
    {
        case 2:
        case 3:
            unpackV2(fwif, length);
            break;
        default:
            cerr << "Warning: Unsupported FW IF version. version_major=" << (int)fwif->version_major << "." << endl;
            return;
    }

    write("}");
}

void LogdJSON::unpack(mvx_log_fw_binary *binary, size_t length)
{
    char data[500];
    getFWBinaryData(binary, data, sizeof(data));
    write(" \"fw_binary\": { \"session\": \"%" PRIx64 "\"", binary->session);
    writeJSON("data", data);
    write("}");
}

void LogdJSON::writeJSON(const char *type, const char *value)
{
    write(", \"%s\": \"", type);

    /* Loop over value and escape any special characters. */
    while (*value != '\0')
    {
        const char *escape = "'\"\\";

        /* Escape special characters. */
        while (*escape != '\0')
        {
            if (*value == *escape)
            {
                write('\\');
                break;
            }

            ++escape;
        }

        /* Print character. */
        write(*value);

        ++value;
    }

    write('"');
}

/******************************************************************************
 * main
 ******************************************************************************/

static const char DEFAULT_INPUT_FILE[] = "/sys/kernel/debug/amvx/log/drain/ram0/msg";

static void help(const char *prog)
{
    printf("Usage: %s [OPTIONS] [DST]\n", prog);
    printf("\n");
    printf("Positional arguments:\n");
    printf("  DST       Output file (default stdout).\n");
    printf("\n");
    printf("Optional arguments:\n");
    printf("  -h --help This help message.\n");
    printf("  -C        Clear ring buffer.\n");
    printf(
        "  -c        Clear ring buffer after first printing its contents.\n");
    printf(
        "  -d        Daemonize process. DST file is expected in this case.\n");
    printf("  --follow  Keep on reading.\n");
    printf("  -f        Format.\n");
    printf("              text  Text format (default).\n");
    printf("              bin   Binary format.\n");
    printf("              json  Java Script Object Notation.\n");
    printf("  -i        Input file (default %s).\n", DEFAULT_INPUT_FILE);
    printf("  -t        Adjust for timezone differences.\n");
    printf("\n");
    printf("Example:\n");
    printf("  Print and clear log.\n");
    printf("  # mvx_logd -c\n");
    printf("\n");
    printf("  Run in daemon mode.\n");
    printf("  # mvx_logd -d -f bin fw.log\n");
    printf("\n");
    printf("  Unpack binary log and adjust for timezone differences.\n");
    printf("  # mvx_logd -t -1 fw.log\n");
}

int main(int argc, char **argv)
{
    string input = DEFAULT_INPUT_FILE;
    string output = "stdout";
    unsigned int posCount = 0;
    bool clearBefore = false;
    bool clearAfter = false;
    bool daemonize = false;
    bool polling = false;
    string format = "text";

    /* Parse command line options. */
    for (int i = 1; i < argc; ++i)
    {
        string arg(argv[i]);

        if (arg.compare("-h") == 0 || arg.compare("--help") == 0)
        {
            help(argv[0]);
            return 1;
        }
        else if (arg.compare("-C") == 0)
        {
            clearBefore = true;
        }
        else if (arg.compare("-c") == 0)
        {
            clearAfter = true;
        }
        else if (arg.compare("-d") == 0)
        {
            daemonize = true;
            polling = true;
        }
        else if (arg.compare("--follow") == 0)
        {
            polling = true;
        }
        else if (arg.compare("-f") == 0)
        {
            if (++i >= argc)
            {
                cerr << "Error: Missing argument to '" << arg << "'." << endl;
                return 2;
            }

            format = argv[i];
        }
        else if (arg.compare("-i") == 0)
        {
            if (++i >= argc)
            {
                cerr << "Error: Missing argument to '" << arg << "'." << endl;
                return 2;
            }

            input = argv[i];
        }
        else if (arg.compare("-t") == 0)
        {
            if (++i >= argc)
            {
                cerr << "Error: Missing argument to '" << arg << "'." << endl;
                return 2;
            }

            timezone = atof(argv[i]) * 3600;
        }
        else if (arg.compare(0, 1, "-") == 0)
        {
            cerr << "Error: Unexpected optional argument '" << arg << "'." << endl;
            return 2;
        }
        else
        {
            if (posCount++ > 0)
            {
                cerr << "Error: Unexpected positional argument '" << arg << "'." << endl;
                return 2;
            }

            output = argv[i];
        }
    }

    if (daemonize != false && posCount < 1)
    {
        cerr << "Error: Output file should be specified in this case." << endl;
        return 2;
    }

    /* Construct logd object. */
    Logd *logd;
    if (format.compare("bin") == 0)
    {
        logd = new LogdBinary(input, output, timezone, polling);
    }
    else if (format.compare("text") == 0)
    {
        logd = new LogdText(input, output, timezone, polling);
    }
    else if (format.compare("json") == 0)
    {
        logd = new LogdJSON(input, output, timezone, polling);
    }
    else
    {
        cerr << "Error Unsupport format '" << format << "'." << endl;
        return 2;
    }

    /* Clear log before. */
    if (clearBefore)
    {
        logd->clear();
    }

    /* Daemonize the process. */
    if (daemonize)
    {
        daemon(0, 0);
    }

    logd->run();

    if (clearAfter)
    {
        logd->clear();
    }

    return 0;
}
