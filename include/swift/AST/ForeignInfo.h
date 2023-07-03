//===--- ForeignInfo.h - Declaration import information ---------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the ForeignInfo structure, which includes
// structural information about how a foreign API's physical type
// maps into the Swift type system.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_FOREIGN_INFO_H
#define SWIFT_FOREIGN_INFO_H

#include "swift/AST/ForeignAsyncConvention.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/Decl.h"

namespace swift {

struct ForeignInfo {
  ImportAsMemberStatus self;
  llvm::Optional<ForeignErrorConvention> error;
  llvm::Optional<ForeignAsyncConvention> async;
};

} // end namespace swift

#endif
