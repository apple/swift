//===--- ModuleSummaryFormat.h - Read and write module summary files ------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SERIALIZATION_MODULE_SUMMARY_FILE_H
#define SWIFT_SERIALIZATION_MODULE_SUMMARY_FILE_H

#include "llvm/Bitcode/BitcodeConvenience.h"
#include <memory>

namespace swift {

namespace modulesummary {

using llvm::BCBlob;
using llvm::BCFixed;
using llvm::BCRecordLayout;
using llvm::BCVBR;

const unsigned char MODULE_SUMMARY_SIGNATURE[] = {'M', 'O', 'D', 'S'};
const unsigned RECORD_BLOCK_ID = llvm::bitc::FIRST_APPLICATION_BLOCKID;

namespace record_block {
enum {
  MODULE_METADATA,
  FUNC_METADATA,
  CALL_GRAPH_EDGE,
  TYPE_REF,
  VFUNC_METADATA,
  VFUNC_IMPL,
  USED_TYPE,
};

using BCGUID = llvm::BCVBR<16>;

using ModuleMetadataLayout = BCRecordLayout<MODULE_METADATA,
                                            BCBlob // module name
                                            >;

using FunctionMetadataLayout = BCRecordLayout<FUNC_METADATA,
                                              BCGUID,     // function guid
                                              BCFixed<1>, // live
                                              BCFixed<1>, // preserved
                                              BCVBR<16>,  // instruction size
                                              BCBlob // name (debug purpose)
                                              >;
using CallGraphEdgeLayout =
    BCRecordLayout<CALL_GRAPH_EDGE,
                   BCFixed<2>, // call kind (direct, vtable or witness)
                   BCGUID,     // callee func guid
                   BCBlob      // name (debug purpose)
                   >;

using TypeRefLayout = BCRecordLayout<TYPE_REF,
                                     BCGUID, // type guid
                                     BCBlob  // type name (debug purpose)
                                     >;

using VFuncMetadataLayout =
    BCRecordLayout<VFUNC_METADATA,
                   BCFixed<1>, // vfunc kind (vtable or witness)
                   BCGUID      // vfunc guid
                   >;

using VFuncImplLayout = BCRecordLayout<VFUNC_IMPL,
                                       BCGUID, // impl func guid
                                       BCGUID // impl type guid
                                       >;
using UsedTypeLayout = BCRecordLayout<USED_TYPE,
                                      BCGUID // type guid
                                      >;
} // namespace record_block
} // namespace modulesummary
} // namespace swift

#endif
