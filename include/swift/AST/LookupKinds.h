//===--- LookupKinds.h - Swift name-lookup enums ----------------*- C++ -*-===//
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
// This file defines enums relating to name lookup.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_LOOKUPKINDS_H
#define SWIFT_LOOKUPKINDS_H

namespace swift {

/// NLKind - This is a specifier for the kind of name lookup being performed
/// by various query methods.
enum class NLKind {
  UnqualifiedLookup,
  QualifiedLookup
};

void simple_display(llvm::raw_ostream &out, NLKind kind);

/// Constants used to customize name lookup.
enum NLOptions : unsigned {
  /// Consider declarations within protocols to which the context type conforms.
  NL_ProtocolMembers = 1 << 0,

  /// Remove non-visible declarations from the set of results.
  NL_RemoveNonVisible = 1 << 1,

  /// Remove overridden declarations from the set of results.
  NL_RemoveOverridden = 1 << 2,

  /// Don't check access when doing lookup into a type.
  ///
  /// This option is not valid when performing lookup into a module.
  NL_IgnoreAccessControl = 1 << 3,

  /// This lookup should only return type declarations.
  NL_OnlyTypes = 1 << 4,

  /// Include synonyms declared with @_implements()
  NL_IncludeAttributeImplements = 1 << 5,

  // Include @usableFromInline and @inlinable
  NL_IncludeUsableFromInline = 1 << 6,

  /// Exclude names introduced by macro expansions in the top-level module.
  NL_ExcludeMacroExpansions = 1 << 7,

  /// This lookup should only return macro declarations.
  NL_OnlyMacros = 1 << 8,

  /// The default set of options used for qualified name lookup.
  ///
  /// FIXME: Eventually, add NL_ProtocolMembers to this, once all of the
  /// callers can handle it.
  NL_QualifiedDefault = NL_RemoveNonVisible | NL_RemoveOverridden,

  /// The default set of options used for unqualified name lookup.
  NL_UnqualifiedDefault = NL_RemoveNonVisible | NL_RemoveOverridden
};

static inline NLOptions operator|(NLOptions lhs, NLOptions rhs) {
  return NLOptions(unsigned(lhs) | unsigned(rhs));
}
static inline NLOptions &operator|=(NLOptions &lhs, NLOptions rhs) {
  return (lhs = lhs | rhs);
}
static inline NLOptions operator&(NLOptions lhs, NLOptions rhs) {
  return NLOptions(unsigned(lhs) & unsigned(rhs));
}
static inline NLOptions &operator&=(NLOptions &lhs, NLOptions rhs) {
  return (lhs = lhs & rhs);
}
static inline NLOptions operator~(NLOptions value) {
  return NLOptions(~(unsigned)value);
}

void simple_display(llvm::raw_ostream &out, NLOptions options);


/// Flags affecting module-level lookup.
enum class ModuleLookupFlags : unsigned {
  /// Exclude names introduced by macro expansions in the top-level module.
  ExcludeMacroExpansions = 1 << 0,
};

} // end namespace swift

#endif
