//===--- DeclarationFragmentPrinter.h - Declaration Fragment Printer ------===//
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

#ifndef SWIFT_SYMBOLGRAPHGEN_DECLARATIONFRAGMENTPRINTER_H
#define SWIFT_SYMBOLGRAPHGEN_DECLARATIONFRAGMENTPRINTER_H

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/JSON.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/Basic/LLVM.h"

namespace swift {

class Decl;
class Type;
class TypeDecl;
class GenericTypeDecl;

namespace symbolgraphgen {

struct SymbolGraph;

/// Prints AST nodes as a stream of tagged fragments for syntax highlighting.
///
/// These fragments are meant to display a somewhat abbreviated part of the
/// declaration for display in documentation, ignoring things like member and
/// function bodies.
///
/// For example, a function:
///
/// ```swift
/// func foo() {
///   print("Hello, world!")
/// }
/// ```
///
/// Will have fragments representing the `func foo()` part.
class DeclarationFragmentPrinter : public ASTPrinter {
public:
  enum class FragmentKind {
    None,
    Keyword,
    Attribute,
    NumberLiteral,
    StringLiteral,
    Identifier,
    TypeIdentifier,
    GenericParameter,
    ExternalParam,
    InternalParam,
    Text,
  };
private:
  /// The symbol graph for which a declaration is being printed.
  const SymbolGraph *SG;

  /// The output stream to print fragment objects to.
  llvm::json::OStream &OS;

  /// The current fragment being considered.
  FragmentKind Kind;

  /// The actual source text of the fragment.
  SmallString<256> Spelling;

  SmallString<256> USR;

  /// Stores the set of decls referenced in the fragment if non-null.
  SmallPtrSet<const Decl*, 8> *ReferencedDecls;

  StringRef getKindSpelling(FragmentKind Kind) const;

  /// Open a new kind of fragment without committing its spelling.
  void openFragment(FragmentKind Kind);

  /// Close the current fragment if there is one, and commit it for display.
  void closeFragment();

  unsigned NumFragments;

public:
  DeclarationFragmentPrinter(
      const SymbolGraph *SG, llvm::json::OStream &OS,
      llvm::Optional<StringRef> Key = llvm::None,
      SmallPtrSet<const Decl *, 8> *ReferencedDecls = nullptr)
      : SG(SG), OS(OS), Kind(FragmentKind::None),
        ReferencedDecls(ReferencedDecls), NumFragments(0) {
    if (Key) {
      OS.attributeBegin(*Key);
      OS.arrayBegin();
    } else {
      OS.arrayBegin();
    }
  }

  /// Print an abridged form of a nominal type declaration, as:
  /// keyword text(" ") typeIdentifier.
  ///
  /// Subheadings for types don't include the complete declaration line
  /// including generics and inheritance.
  ///
  /// \param TD The type declaration to print.
  /// \param PrintKeyword Print the corresponding keyword introducer if `true`.
  void printAbridgedType(const GenericTypeDecl *TD, bool PrintKeyword);

  void printDeclLoc(const Decl *D) override;

  void printDeclNameEndLoc(const Decl *D) override {
    closeFragment();
  }

  void printNamePre(PrintNameContext Context) override;

  void printStructurePre(PrintStructureKind Kind, const Decl *D) override;

  void printNamePost(PrintNameContext Context) override {
    closeFragment();
  }

  void printTypeRef(Type T, const TypeDecl *RefTo, Identifier Name,
                    PrintNameContext NameContext) override;

  /// Print plain text to the current fragment, opening a new text fragment
  /// if there isn't an open fragment.
  void printText(StringRef Text) override;

  ~DeclarationFragmentPrinter() {
    closeFragment();
    OS.arrayEnd();
    OS.attributeEnd();
    assert(NumFragments);
  }
};

} // end namespace symbolgraphgen
} // end namespace swift

#endif // SWIFT_SYMBOLGRAPHGEN_DECLARATIONFRAGMENTPRINTER_H
