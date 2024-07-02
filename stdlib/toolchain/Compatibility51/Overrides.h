//===--- Overrides.h - Compat overrides for Swift 5.0 runtime ------s------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file provides compatibility override hooks for Swift 5.1 runtimes.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Metadata.h"
#include "llvm/ADT/StringRef.h"

namespace swift {

using ConformsToSwiftProtocol_t =
  const ProtocolConformanceDescriptor *(const Metadata * const type,
                                        const ProtocolDescriptor *protocol,
                                        llvm::StringRef moduleName);
// We must ensure visibility become global non-hidden
// because auto-linking from dep dylibs need this symbol
__attribute__((visibility("default")))
const ProtocolConformanceDescriptor *
swift51override_conformsToSwiftProtocol(const Metadata * const type,
                                        const ProtocolDescriptor *protocol,
                                        llvm::StringRef moduleName,
                                        ConformsToSwiftProtocol_t *original);

} // end namespace swift
