/*
 *   Copyright 2024 M Hightower
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
////////////////////////////////////////////////////////////////////////////////
// SFDP - JEDEC SPI Flash Data Parameters
//
// SFDP info may be useful, when dealing with two different SPI Flash chips,
// with identical `spi_flash_get_id()` results. Limited info returns version
// info and Parameter Table pointer and size.
#include <Arduino.h>
#include "SpiFlashUtils.h"
#include "SfdpRevInfo.h"

namespace experimental {

SfdpRevInfo get_sfdp_revision() {
  SfdpHdr sfdp_hdr;
  SfdpParam sfdp_param;
  SfdpRevInfo rev;
  memset(&rev.u32[0], 0, sizeof(SfdpRevInfo));

  size_t sz = sizeof(sfdp_hdr);
  size_t addr = 0u;
  SpiOpResult ok0 = spi0_flash_read_sfdp(addr, &sfdp_hdr.u32[0], sz);
  if (SPI_RESULT_OK == ok0 && 0x50444653 == sfdp_hdr.signature) {
    rev.hdr_major = sfdp_hdr.rev_major;
    rev.hdr_minor = sfdp_hdr.rev_minor;
    rev.num_parm_hdrs = sfdp_hdr.num_parm_hdrs;

    addr += sz;
    sz = sizeof(sfdp_param);
    ok0 = spi0_flash_read_sfdp(addr, &sfdp_param.u32[0], sz);
    if (SPI_RESULT_OK == ok0) {
      // return version of 1st parameter block
      rev.parm_major = sfdp_param.rev_major;
      rev.parm_minor = sfdp_param.rev_minor;
      rev.sz_dw = sfdp_param.sz_dw;
      rev.tbl_ptr = sfdp_param.tbl_ptr;
    }
  } else {
    memset(&rev.u32[0], 0, sizeof(SfdpRevInfo));
  }
  return rev;
}

uint32_t* get_sfdp_basic(SfdpRevInfo *rev) {
  if (nullptr == rev) return nullptr;

  *rev = get_sfdp_revision();
  if (0u == rev->tbl_ptr) return nullptr;

  size_t addr = rev->tbl_ptr;
  size_t sz = rev->sz_dw * 4u;
  uint32_t* dw = (uint32_t*)malloc(sz);
  if (nullptr == dw) return nullptr;

  memset(dw, 0, sz);
  if (SPI_RESULT_OK == spi0_flash_read_sfdp(addr, dw, sz)) {
    return dw;
  }

  free(dw);
  return nullptr;
}

};
