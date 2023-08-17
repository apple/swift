//===--- ASTSectionImporter.h - Import AST Section Modules ------*- C++ -*-===//
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
// This file implements support for loading modules serialized into a
// Mach-O AST section into Swift.
//
//===----------------------------------------------------------------------===//
#ifndef SWIFT_ASTSECTION_IMPORTER_H
#define SWIFT_ASTSECTION_IMPORTER_H

#include "swift/Basic/LLVM.h"
#include "swift/Serialization/Validation.h"
#include "llvm/Support/Error.h"
#include <string>

namespace llvm {
class Triple;
}
namespace swift {
  class MemoryBufferSerializedModuleLoader;

  class ASTSectionParseError : public llvm::ErrorInfo<ASTSectionParseError> {
  public:
    static char ID;

    serialization::Status Error;
    std::string ErrorMessage;

    ASTSectionParseError(serialization::Status Error,
                         StringRef ErrorMessage = {})
        : Error(Error), ErrorMessage(ErrorMessage) {
      assert(Error != serialization::Status::Valid);
    }
    ASTSectionParseError(const ASTSectionParseError &Other)
        : ASTSectionParseError(Other.Error, Other.ErrorMessage) {}
    ASTSectionParseError &operator=(const ASTSectionParseError &Other) {
      Error = Other.Error;
      ErrorMessage = Other.ErrorMessage;
      return *this;
    }

    std::string toString() const;
    void log(llvm::raw_ostream &OS) const override;
    std::error_code convertToErrorCode() const override;
  };

  /// Provided a memory buffer with an entire Mach-O __swift_ast section, this
  /// function makes memory buffer copies of all swift modules found in it and
  /// registers them using registerMemoryBuffer() so they can be found by
  /// loadModule().
  /// \param filter  If fully specified, only matching modules are registered.
  /// \return a vector of the access path of all modules found in the
  /// section if successful.
  llvm::Expected<SmallVector<std::string, 4>>
  parseASTSection(MemoryBufferSerializedModuleLoader &Loader,
                  StringRef Data, const llvm::Triple &filter);

  // An old version temporarily left for remaining call site.
  // TODO: remove this once the other version is committed and used.
  bool parseASTSection(MemoryBufferSerializedModuleLoader &Loader,
                       StringRef Data, const llvm::Triple &filter,
                       SmallVectorImpl<std::string> &foundModules);

}
#endif
