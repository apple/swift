//===--- DefaultArgumentKind.h - Default Argument Kind Enum -----*- C++ -*-===//
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
// This file defines the DefaultArgumentKind enumeration.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_DEFAULTARGUMENTKIND_H
#define SWIFT_DEFAULTARGUMENTKIND_H

#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>

namespace llvm {
class StringRef;
}

namespace swift {

/// Describes the kind of default argument a tuple pattern element has.
enum class DefaultArgumentKind : uint8_t {
  /// No default argument.
  None,
  /// A normal default argument.
  Normal,
  /// The default argument is inherited from the corresponding argument of the
  /// overridden declaration.
  Inherited,
  /// The "nil" literal.
  NilLiteral,
  /// An empty array literal.
  EmptyArray,
  /// An empty dictionary literal.
  EmptyDictionary,
  /// A reference to the stored property. This is a special default argument
  /// kind for the synthesized memberwise constructor to emit a call to the
  /// property's initializer.
  StoredProperty,
  // Magic identifier literals expanded at the call site:
#define MAGIC_IDENTIFIER(NAME, STRING, SYNTAX_KIND) NAME,
#include "swift/AST/MagicIdentifierKinds.def"
};
enum { NumDefaultArgumentKindBits = 4 };

struct ArgumentAttrs {
  DefaultArgumentKind argumentKind;
  bool isUnavailableInSwift = false;
  llvm::StringRef CXXOptionsEnumName = "";

  ArgumentAttrs(DefaultArgumentKind argumentKind,
                bool isUnavailableInSwift = false,
                llvm::StringRef CXXOptionsEnumName = "")
      : argumentKind(argumentKind), isUnavailableInSwift(isUnavailableInSwift),
        CXXOptionsEnumName(CXXOptionsEnumName) {}

  bool operator !=(const DefaultArgumentKind &rhs) const {
    return argumentKind != rhs;
  }

  bool operator==(const DefaultArgumentKind &rhs) const {
    return argumentKind == rhs;
  }

  bool hasDefaultArg() const {
    return argumentKind != DefaultArgumentKind::None;
  }

  bool hasAlternateCXXOptionsEnumName() const {
    return !CXXOptionsEnumName.empty() && isUnavailableInSwift;
  }

  llvm::StringRef getAlternateCXXOptionsEnumName() const {
    assert(hasAlternateCXXOptionsEnumName() &&
           "Expected a C++ Options type for C++-Interop but found none.");
    return CXXOptionsEnumName;
  }
};

} // end namespace swift

#endif // LLVM_SWIFT_DEFAULTARGUMENTKIND_H

