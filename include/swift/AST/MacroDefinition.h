//===--- MacroDefinition.h - Swift Macro Definition -------------*- C++ -*-===//
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
// This provides the definition of a macro, which gives access to its
// implementation.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_MACRO_DEFINITION_H
#define SWIFT_AST_MACRO_DEFINITION_H

#include "swift/Basic/StringExtras.h"
#include "llvm/ADT/PointerUnion.h"

namespace swift {

class ASTContext;

/// A reference to an external macro definition that is understood by ASTGen.
class ExternalMacroDefinition {
  enum class Status : int8_t {
    Success = 0,
    Error,
  };
  Status status;
  /// ASTGen's notion of an macro definition, which is opaque to the C++ part
  /// of the compiler. If 'kind' is 'PluginKind::Error', this is a C-string to
  /// the error message
  const void *opaqueHandle;

  ExternalMacroDefinition(Status status, const void *opaqueHandle)
      : status(status), opaqueHandle(opaqueHandle) {}

public:
  static ExternalMacroDefinition success(const void *opaqueHandle) {
    return ExternalMacroDefinition{Status::Success, opaqueHandle};
  }

  static ExternalMacroDefinition error(NullTerminatedStringRef message) {
    return ExternalMacroDefinition{Status::Error,
                                   static_cast<const void *>(message.data())};
  }

  const void *get() {
    if (status != Status::Success)
      return nullptr;
    return opaqueHandle;
  }
  bool isError() const { return status == Status::Error; }
  NullTerminatedStringRef getErrorMessage() const {
    assert(isError());
    return static_cast<const char *>(opaqueHandle);
  }
};

/// A reference to an external macro.
struct ExternalMacroReference {
  Identifier moduleName;
  Identifier macroTypeName;
};

/// Describes the known kinds of builtin macros.
enum class BuiltinMacroKind: uint8_t {
  /// #externalMacro, which references an external macro.
  ExternalMacro,
  /// #isolation, which produces the isolation of the current context
  IsolationMacro,
};

/// A single replacement
struct ExpandedMacroReplacement {
  unsigned startOffset, endOffset;
  unsigned parameterIndex;
};

/// An expansion of another macro.
class ExpandedMacroDefinition {
  friend class MacroDefinition;

  /// The expansion text, ASTContext-allocated.
  StringRef expansionText;

  /// The macro replacements, ASTContext-allocated.
  ArrayRef<ExpandedMacroReplacement> replacements;

  /// Same as above but for generic argument replacements
  ArrayRef<ExpandedMacroReplacement> genericReplacements;

  ExpandedMacroDefinition(
    StringRef expansionText,
    ArrayRef<ExpandedMacroReplacement> replacements,
    ArrayRef<ExpandedMacroReplacement> genericReplacements
  ) : expansionText(expansionText),
          replacements(replacements),
          genericReplacements(genericReplacements) { }

public:
  StringRef getExpansionText() const { return expansionText; }

  ArrayRef<ExpandedMacroReplacement> getReplacements() const {
    return replacements;
  }
  ArrayRef<ExpandedMacroReplacement> getGenericReplacements() const {
    return genericReplacements;
  }
};

/// Provides the definition of a macro.
class MacroDefinition {
public:
    /// Describes how the macro is implemented.
  enum class Kind: uint8_t {
    /// The macro has a definition, but it is invalid, so the macro cannot be
    /// expanded.
    Invalid,

    /// The macro has no definition.
    Undefined,

    /// An externally-provided macro definition.
    External,

    /// A builtin macro definition, which has a separate builtin kind.
    Builtin,

    /// A macro that is defined as an expansion of another macro.
    Expanded,
  };

  Kind kind;

private:
  union Data {
    ExternalMacroReference external;
    BuiltinMacroKind builtin;
    ExpandedMacroDefinition expanded;

    Data() : builtin(BuiltinMacroKind::ExternalMacro) { }
  } data;

  MacroDefinition(Kind kind) : kind(kind) { }

  MacroDefinition(ExternalMacroReference external) : kind(Kind::External) {
    data.external = external;
  }

  MacroDefinition(BuiltinMacroKind builtinKind) : kind(Kind::Builtin) {
    data.builtin = builtinKind;
  }

  MacroDefinition(ExpandedMacroDefinition expanded) : kind(Kind::Expanded) {
    data.expanded = expanded;
  }

public:
  static MacroDefinition forInvalid() {
    return MacroDefinition(Kind::Invalid);
  }

  static MacroDefinition forUndefined() {
    return MacroDefinition(Kind::Undefined);
  }

  static MacroDefinition forExternal(
      Identifier moduleName,
      Identifier macroTypeName
   ) {
    return MacroDefinition(ExternalMacroReference{moduleName, macroTypeName});
  }

  static MacroDefinition forBuiltin(BuiltinMacroKind builtinKind) {
    return MacroDefinition(builtinKind);
  }

  /// Create a representation of an expanded macro definition.
  static MacroDefinition forExpanded(
      ASTContext &ctx,
      StringRef expansionText,
      ArrayRef<ExpandedMacroReplacement> replacements,
      ArrayRef<ExpandedMacroReplacement> genericReplacements
  );

  /// Retrieve the external macro being referenced.
  ExternalMacroReference getExternalMacro() const {
    assert(kind == Kind::External);
    return data.external;
  }

  /// Retrieve the builtin kind.
  BuiltinMacroKind getBuiltinKind() const {
    assert(kind == Kind::Builtin);
    return data.builtin;
  }

  ExpandedMacroDefinition getExpanded() const {
    assert(kind == Kind::Expanded);
    return data.expanded;
  }

  operator Kind() const { return kind; }
};

}

#endif // SWIFT_AST_MACRO_DEFINITION_H
