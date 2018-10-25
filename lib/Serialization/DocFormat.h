//===--- DocFormat.h - The internals of swiftdoc files ----------*- C++ -*-===//
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
///
/// \file Contains various constants and helper types to deal with serialized
/// documentation info (swiftdoc files).
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SERIALIZATION_DOCFORMAT_H
#define SWIFT_SERIALIZATION_DOCFORMAT_H

#include "llvm/Bitcode/RecordLayout.h"

namespace swift {
namespace serialization {

using llvm::BCArray;
using llvm::BCBlob;
using llvm::BCFixed;
using llvm::BCGenericRecordLayout;
using llvm::BCRecordLayout;
using llvm::BCVBR;

/// Magic number for serialized documentation files.
const unsigned char SWIFTDOC_SIGNATURE[] = { 0xE2, 0x9C, 0xA8, 0x07 };

/// The record types within the comment block.
///
/// Be very careful when changing this block; it must remain stable. Adding new
/// records is okay---they will be ignored---but modifying existing ones must be
/// done carefully. You may need to update the version when you do so.
///
/// \sa COMMENT_BLOCK_ID
namespace comment_block {
  enum RecordKind {
    DECL_COMMENTS = 1,
    GROUP_NAMES = 2,
  };

  using DeclCommentListLayout = BCRecordLayout<
    DECL_COMMENTS, // record ID
    BCVBR<16>,     // table offset within the blob (an llvm::OnDiskHashTable)
    BCBlob         // map from Decl USRs to comments
  >;

  using GroupNamesLayout = BCRecordLayout<
    GROUP_NAMES,    // record ID
    BCBlob          // actual names
  >;

} // namespace comment_block

} // end namespace serialization
} // end namespace swift

#endif
