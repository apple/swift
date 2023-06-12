//===--- eh_frame_hdr.h - DWARF EH frame header definitions ---------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Defines the format of the .eh_frame_hdr section, which isn't part of the
// DWARF specification (it's a GNU extension).
//
// https://refspecs.linuxfoundation.org/LSB_5.0.0/LSB-Core-generic/LSB-Core-generic/ehframechpt.html
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_EH_FRAME_HDR_H
#define SWIFT_EH_FRAME_HDR_H

#include <inttypes.h>

/* .. Useful macros ......................................................... */

#define EH_FRAME_OPTIONS(t,n) \
  t n; \
  enum __attribute__((flag_enum,enum_extensibility(open))): t

/* .. .eh_frame_hdr header .................................................. */

struct EHFrameHdr {
  uint8_t version;
  uint8_t eh_frame_ptr_enc;
  uint8_t fde_count_enc;
  uint8_t table_enc;
  // encoded eh_frame_ptr;
  // encoded fde_count;
  // encoded binary_search_table[fde_count];
};

/* .. Constants ............................................................. */

typedef EH_FRAME_OPTIONS(uint8_t, EHFrameEncoding) {
  DW_EH_PE_omit		= 0xff,	// No value is present
  DW_EH_PE_uleb128	= 0x01, // Unsigned value encoded using LEB128
  DW_EH_PE_udata2	= 0x02, // A 2-byte unsigned value
  DW_EH_PE_udata4	= 0x03, // A 4-byte unsigned value
  DW_EH_PE_udata8	= 0x04, // An 8-byte unsigned value
  DW_EH_PE_sleb128	= 0x09, // Signed value encoded using LEB128
  DW_EH_PE_sdata2	= 0x0a, // A 2-byte signed value
  DW_EH_PE_sdata4	= 0x0b, // A 4-byte signed value
  DW_EH_PE_sdata8	= 0x0c, // An 8-byte signed value

  DW_EH_PE_absptr	= 0x00, // Absolute, used with no modification
  DW_EH_PE_pcrel	= 0x10, // Relative to the current program counter
  DW_EH_PE_datarel	= 0x30, // Relative to the beginning of the .eh_frame_hdr
};

#endif // SWIFT_EH_FRAME_HDR_H
