/*
 * Copyright 2026 Cix Technology Group Co., Ltd.
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

#ifndef __TEST_FISHEYE_REMAP_H
#define __TEST_FISHEYE_REMAP_H

/* Test cme_2d_fisheye_remap. Args: -i <input> -w -h -f <format> [-o output] [-W dst_w] [-H dst_h]. Dst is RGB888. */
int test_fisheye_remap(int argc, char** argv);

#endif
