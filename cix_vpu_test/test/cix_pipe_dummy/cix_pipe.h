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

#ifndef _CIX_PIPE_H_
#define _CIX_PIPE_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define CPIPE_VERSION_MAJOR     0
#define CPIPE_VERSION_MINOR     1
#define CPIPE_VERSION_REVISION  1

/* Maximum data descriptors in one packet */
#define CPIPE_MAX_DATA_DESCS 4

#define CPIPE_MAX_PLANES 4

/* Close all the connections from client. */
/* Can be used as the second parameter of CPIPE_close when the instance is created as SERVER */
#define CPIPE_CLOSE_ALL -1

/* invalid file descriptor */
#define CPIPE_INVALID_FD -1

#define DATA_HEADER_SIZE (sizeof(CpipeDescHdr))
#define DATA_SIZE(type) (sizeof(type) - DATA_HEADER_SIZE)

typedef void *CpipeHandle;
typedef void CpipeDescriptor;
typedef int32_t CpipeChannelID;

typedef enum _CpipeStatus
{
    CPIPE_STATUS_OK = 0,
    CPIPE_STATUS_MORE_DATA = 1,     /* have more data to parse in the receive buffer */
    CPIPE_STATUS_TIMEOUT = 2,       /* wait timeout, which means no data received within the time */
    CPIPE_STATUS_NO_DATA = 3,       /* no data recieved */
    CPIPE_STATUS_ERROR = -1,        /* general error */
    CPIPE_STATUS_WRONG_PARAMS = -2, /* wrong parameters */
    CPIPE_STATUS_NO_MEMORY = -3,    /* not enough memory */
} CpipeStatus;

/* cpipe mode */
typedef enum _CpipeMode
{
    CPIPE_MODE_SERVER = 0,      /* run cpipe as server; server should be launched before client */
    CPIPE_MODE_CLIENT,          /* run cpipe as client which can connect to server */
    CPIPE_MODE_MAX
} CpipeMode;

#pragma pack(push, 1)
typedef union _CpipeVersion
{
    struct cix_pipe
    {
        uint8_t major;
        uint8_t minor;
        uint8_t revision;
        uint8_t step;
    } v;
    uint32_t version;
} CpipeVersion;

typedef struct _CpipeDescHdr
{
    uint32_t size;      /* data size, excluding this header */
    int32_t type;       /* type of this data descriptor */
    int32_t id;         /* buffer id if it's buffer descriptor. Only values greater than 0 are valid */
} CpipeDescHdr;

/* data packet can be sent/received in one send/recv call */
typedef struct _CpipePacket
{
    int32_t fds[CPIPE_MAX_PLANES];                  /* file descriptors. If CPIPE_INVALID_FD, it's invalid */
    uint64_t timestamp;                             /* timestamp of this packet, the time for rendering */
    int32_t num;                                    /* number of valid data descriptor entries in list[] */
    CpipeDescriptor *list[CPIPE_MAX_DATA_DESCS];    /* list of pointers which point to data descriptor. */
} CpipePacket;
#pragma pack(pop)

/**
 * \brief Create a cpipe instance.
 *
 * Create a cpipe object with the specified name as server or client.
 * When mode is CPIPE_MODE_SERVER, a file of the specified name will be created.
 *
 * \param cpipe Point to the pointer of cpipe instance.
 * \param name Socket file name.
 * \param mode cpipe mode, server or client.
 * \return CPIPE_STATUS_OK successful,
 * \return other unsuccessful.
 */
CpipeStatus CPIPE_getVersion(CpipeVersion *version);

/**
 * \brief Create a cpipe instance.
 *
 * Create a cpipe object with the specified name as server or client.
 * When mode is CPIPE_MODE_SERVER, a file of the specified name will be created.
 *
 * \param cpipe Point to the pointer of cpipe instance.
 * \param name Socket file name.
 * \param mode cpipe mode, server or client.
 * \return CPIPE_STATUS_OK successful,
 * \return other unsuccessful.
 */
CpipeStatus CPIPE_create(CpipeHandle *cpipe, const char *name, CpipeMode mode);

/**
 * \brief Create a connection between server and client
 *
 * Server calls this function to wait for connection and accept.
 * Client calls this function to connect to server.
 *
 * \param cpipe Pointer of cpipe instance.
 * \param channel id of the new connection. Valid for server only. Should be 0 for client
 * \return CPIPE_STATUS_OK successful,
 * \return other unsuccessful.
 */
CpipeStatus CPIPE_connect(CpipeHandle cpipe, CpipeChannelID *channel);

/**
 * \brief Create a connection between server and client with timeout
 *
 * Server calls this function to wait for connection and accept with timeout.
 * Client calls this function to connect to server with timeout.
 *
 * \param cpipe Pointer of cpipe instance.
 * \param channel id of the new connection. Valid for server only. Should be 0 for client
 * \param timeout_ms timeout in unit of milliseconds.
 * \return CPIPE_STATUS_OK successful,
 * \return CPIPE_STATUS_TIMEOUT if no data received within timeout_ms,
 * \return other unsuccessful.
 */
CpipeStatus CPIPE_connect_ex(CpipeHandle cpipe, CpipeChannelID *channel, int32_t timeout_ms);

/**
 * \brief Send a packet
 *
 * Send a packet through the channel.
 *
 * \param cpipe Pointer of cpipe instance.
 * \param channel The channel to send this packet. Valid for server only. Should be 0 for client
 * \param pkt Point to the packet to be sent.
 * \return CPIPE_STATUS_OK successful,
 * \return other unsuccessful.
 */
CpipeStatus CPIPE_send(CpipeHandle cpipe, CpipeChannelID channel, CpipePacket *pkt);

/**
 * \brief Wait for data from channel
 *
 * This function returns once there is data received from the channel.
 *
 * \param cpipe Pointer of cpipe instance.
 * \param channel The channel to receive data. Valid for server only. Should be 0 for client
 * \param timeout_ms timeout in unit of milliseconds.
 * \return CPIPE_STATUS_OK successful,
 * \return CPIPE_STATUS_TIMEOUT if no data received within timeout_ms,
 * \return other unsuccessful.
 */
CpipeStatus CPIPE_wait(CpipeHandle cpipe, CpipeChannelID channel, int32_t timeout_ms);

/**
 * \brief Receive data
 *
 * Receive data from the channel.
 * Data descriptors of the packet are stored in the internal buffer,
 * and may be overwritten in the next CPIPE_recv call.
 *
 * \param cpipe Pointer of cpipe instance.
 * \param channel The channel to receive data. Valid for server only. Should be 0 for client
 * \param pkt Point to the received packet.
 * \return CPIPE_STATUS_OK successful,
 * \return other unsuccessful.
 */
CpipeStatus CPIPE_recv(CpipeHandle cpipe, CpipeChannelID channel, CpipePacket *pkt);

/**
 * \brief Receive data with timeout
 *
 * Receive data from the channel with timeout.
 * Data descriptors of the packet are stored in the internal buffer,
 * and may be overwritten in the next CPIPE_recv call.
 *
 * \param cpipe Pointer of cpipe instance.
 * \param channel The channel to receive data. Valid for server only. Should be 0 for client
 * \param pkt Point to the received packet.
 * \param timeout_ms timeout in unit of milliseconds.
 * \return CPIPE_STATUS_OK successful,
 * \return CPIPE_STATUS_TIMEOUT if no data received within timeout_ms,
 * \return other unsuccessful.
 */
CpipeStatus CPIPE_recv_ex(CpipeHandle cpipe, CpipeChannelID channel, CpipePacket *pkt, int32_t timeout_ms);

/**
 * \brief Close connections
 *
 * Close connections. Server can set channel to CPIPE_CLOSE_ALL to close all connections.
 *
 * \param cpipe Pointer of cpipe instance.
 * \param channel The connection to be closed. Valid for server only. Should be 0 for client
 * \return CPIPE_STATUS_OK successful,
 * \return other unsuccessful.
 */
CpipeStatus CPIPE_close(CpipeHandle cpipe, CpipeChannelID channel);

#ifdef __cplusplus
}
#endif

#endif /* !_CIX_PIPE_H_ */
