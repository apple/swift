//===--- ImportDepth.h ----------------------------------------------------===//
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

#ifndef SWIFT_IDE_IMPORTDEPTH_H
#define SWIFT_IDE_IMPORTDEPTH_H

#include "swift/AST/ASTContext.h"
#include "swift/Basic/LLVM.h"
#include "swift/Frontend/FrontendOptions.h"
#include "llvm/ADT/StringMap.h"

namespace swift {
namespace ide {

/// A utility for calculating the import depth of a given module. Direct imports
/// have depth 1, imports of those modules have depth 2, etc.
///
/// Special modules such as Playground auxiliary sources are considered depth
/// 0.
class ImportDepth {
  llvm::StringMap<uint8_t> depths;

public:
  ImportDepth() = default;
  ImportDepth(ASTContext &context, const FrontendOptions &frontendOptions);

  llvm::Optional<uint8_t> lookup(StringRef module) {
    auto I = depths.find(module);
    if (I == depths.end())
      return llvm::None;
    return I->getValue();
  }
};

} // end namespace ide
} // end namespace swift

#endif // SWIFT_IDE_IMPORTDEPTH_H
