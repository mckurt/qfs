//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2011/12/07
// Author: Mike Ovsiannikov
//
// Copyright 2011,2016 Quantcast Corporation. All rights reserved.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
//
//----------------------------------------------------------------------------

#include "RequestParser.h"

namespace KFS {

const unsigned char HexIntParser::sChar2Hex[256] = {
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0x00 /* 0 */, 0x01 /* 1 */,
0x02 /* 2 */, 0x03 /* 3 */, 0x04 /* 4 */, 0x05 /* 5 */, 0x06 /* 6 */,
0x07 /* 7 */, 0x08 /* 8 */, 0x09 /* 9 */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0x0a /* A */, 0x0b /* B */, 0x0c /* C */, 0x0d /* D */, 0x0e /* E */,
0x0f /* F */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0x0a /* a */, 0x0b /* b */, 0x0c /* c */,
0x0d /* d */, 0x0e /* e */, 0x0f /* f */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */, 0xff /*   */,
0xff /*   */
};
}
