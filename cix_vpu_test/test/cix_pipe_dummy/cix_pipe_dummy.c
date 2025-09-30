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

#include "cix_pipe.h"

CpipeStatus
CPIPE_getVersion(CpipeVersion *version)
{
    return CPIPE_STATUS_OK;
}

CpipeStatus
CPIPE_create(CpipeHandle *cpipe, const char *name, CpipeMode mode)
{
    return CPIPE_STATUS_OK;
}

CpipeStatus
CPIPE_connect(CpipeHandle cpipe, CpipeChannelID *channel)
{
    return CPIPE_STATUS_OK;
}

CpipeStatus
CPIPE_send(CpipeHandle cpipe, CpipeChannelID channel, CpipePacket *pkt)
{
    return CPIPE_STATUS_OK;
}

CpipeStatus
CPIPE_recv(CpipeHandle cpipe, CpipeChannelID channel, CpipePacket *pkt)
{
    return CPIPE_STATUS_OK;
}

CpipeStatus
CPIPE_wait(CpipeHandle cpipe, CpipeChannelID channel, int timeout_ms)
{
    return CPIPE_STATUS_OK;
}

CpipeStatus
CPIPE_close(CpipeHandle cpipe, CpipeChannelID channel)
{
    return CPIPE_STATUS_OK;
}

CpipeStatus
CPIPE_connect_ex(CpipeHandle cpipe, CpipeChannelID *channel, int timeout_ms)
{
    return CPIPE_STATUS_OK;
}

CpipeStatus
CPIPE_recv_ex(CpipeHandle cpipe, CpipeChannelID channel, CpipePacket *pkt, int timeout_ms)
{
    return CPIPE_STATUS_OK;
}
