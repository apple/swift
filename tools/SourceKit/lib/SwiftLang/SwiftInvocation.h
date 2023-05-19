//===--- SwiftInvocation.h - ------------------------------------*- C++ -*-===//
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

#ifndef LLVM_SOURCEKIT_LIB_SWIFTLANG_SWIFTINVOCATION_H
#define LLVM_SOURCEKIT_LIB_SWIFTLANG_SWIFTINVOCATION_H

#include "swift/Basic/ThreadSafeRefCounted.h"
#include <string>
#include <vector>

namespace swift {
  class CompilerInvocation;
}

namespace SourceKit {
  class SwiftASTManager;

/// Encompasses an invocation for getting an AST. This is used to control AST
/// sharing among different requests.
class SwiftInvocation : public llvm::ThreadSafeRefCountedBase<SwiftInvocation> {
public:
  ~SwiftInvocation();

  struct Implementation;
  Implementation &Impl;

  ArrayRef<std::string> getArgs() const;
  void applyTo(swift::CompilerInvocation &CompInvok) const;
  void raw(std::vector<std::string> &Args, std::string &PrimaryFile) const;

private:
  SwiftInvocation(Implementation &Impl) : Impl(Impl) { }
  friend class SwiftASTManager;
};

} // namespace SourceKit

#endif
