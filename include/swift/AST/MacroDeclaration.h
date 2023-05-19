//===--- MacroDeclaration.h - Swift Macro Declaration -----------*- C++ -*-===//
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
// Data structures that configure the declaration of a macro.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_MACRO_DECLARATION_H
#define SWIFT_AST_MACRO_DECLARATION_H

#include "swift/AST/Identifier.h"

namespace swift {

/// Describes the syntax that is used for a macro, which affects its role.
enum class MacroSyntax: uint8_t {
  /// Freestanding macro syntax starting with an explicit '#' followed by the
  /// macro name and optional arguments.
  Freestanding,

  /// Attached macro syntax written as an attribute with a leading `@` followed
  /// by the macro name an optional arguments.
  Attached,
};

/// The context in which a macro can be used, which determines the syntax it
/// uses.
enum class MacroRole: uint32_t {
  /// An expression macro, referenced explicitly via "#stringify" or similar
  /// in the source code.
  Expression = 0x01,
  /// A freestanding declaration macro.
  Declaration = 0x02,
  /// An attached macro that declares accessors for a variable or subscript
  /// declaration.
  Accessor = 0x04,
  /// An attached macro that generates attributes for the
  /// members inside the declaration.
  MemberAttribute = 0x08,
  /// An attached macro that generates synthesized members
  /// inside the declaration.
  Member = 0x10,
  /// An attached macro that generates declarations that are peers
  /// of the declaration the macro is attached to.
  Peer = 0x20,
  /// An attached macro that adds conformances to the declaration the
  /// macro is attached to.
  Conformance = 0x40,
  /// A freestanding macro that expands to expressions, statements and
  /// declarations in a code block.
  CodeItem = 0x80,

  // NOTE: When adding a new macro role, also add it to `getAllMacroRoles`.
};

/// Returns an enumeratable list of all macro roles.
std::vector<MacroRole> getAllMacroRoles();

/// The contexts in which a particular macro declaration can be used.
using MacroRoles = OptionSet<MacroRole>;

void simple_display(llvm::raw_ostream &out, MacroRoles roles);
bool operator==(MacroRoles lhs, MacroRoles rhs);
llvm::hash_code hash_value(MacroRoles roles);

/// Retrieve the string form of the given macro role, as written on the
/// corresponding attribute.
StringRef getMacroRoleString(MacroRole role);

/// Whether a macro with the given set of macro contexts is freestanding, i.e.,
/// written in the source code with the `#` syntax.
bool isFreestandingMacro(MacroRoles contexts);

MacroRoles getFreestandingMacroRoles();

/// Whether a macro with the given set of macro contexts is attached, i.e.,
/// written in the source code as an attribute with the `@` syntax.
bool isAttachedMacro(MacroRoles contexts);

MacroRoles getAttachedMacroRoles();

/// Checks if the macro is supported or guarded behind an experimental flag.
bool isMacroSupported(MacroRole role, ASTContext &ctx);

enum class MacroIntroducedDeclNameKind {
  Named,
  Overloaded,
  Prefixed,
  Suffixed,
  Arbitrary,

  // NOTE: When adding a new name kind, also add it to
  // `getAllMacroIntroducedDeclNameKinds`.
};

/// Returns an enumeratable list of all macro introduced decl name kinds.
std::vector<MacroIntroducedDeclNameKind> getAllMacroIntroducedDeclNameKinds();

/// Whether a macro-introduced name of this kind requires an argument.
bool macroIntroducedNameRequiresArgument(MacroIntroducedDeclNameKind kind);

StringRef getMacroIntroducedDeclNameString(
    MacroIntroducedDeclNameKind kind);

class CustomAttr;

/// Perform lookup to determine whether the given custom attribute refers to
/// a macro declaration, and populate the \c macros vector with the lookup
/// results that are attached macros.
void findMacroForCustomAttr(CustomAttr *attr, DeclContext *dc,
                            llvm::TinyPtrVector<ValueDecl *> &macros);

class MacroIntroducedDeclName {
public:
  using Kind = MacroIntroducedDeclNameKind;

private:
  Kind kind;
  DeclName name;

public:
  MacroIntroducedDeclName(Kind kind, DeclName name = DeclName())
      : kind(kind), name(name) {};

  static MacroIntroducedDeclName getNamed(DeclName name) {
    return MacroIntroducedDeclName(Kind::Named, name);
  }

  static MacroIntroducedDeclName getOverloaded() {
    return MacroIntroducedDeclName(Kind::Overloaded);
  }

  static MacroIntroducedDeclName getPrefixed(Identifier prefix) {
    return MacroIntroducedDeclName(Kind::Prefixed, prefix);
  }

  static MacroIntroducedDeclName getSuffixed(Identifier suffix) {
    return MacroIntroducedDeclName(Kind::Suffixed, suffix);
  }

  static MacroIntroducedDeclName getArbitrary() {
    return MacroIntroducedDeclName(Kind::Arbitrary);
  }

  Kind getKind() const { return kind; }
  DeclName getName() const { return name; }
};

}

#endif // SWIFT_AST_MACRO_DECLARATION_H
