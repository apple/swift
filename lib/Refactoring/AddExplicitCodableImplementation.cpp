//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "RefactoringActions.h"
#include "Utils.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/Basic/Assertions.h"

using namespace swift::refactoring;

namespace {
class AddCodableContext {

  /// Declaration context
  IterableDeclContext *IDC;

  AddCodableContext(NominalTypeDecl *nominal) : IDC(nominal){};
  AddCodableContext(ExtensionDecl *extension) : IDC(extension){};
  AddCodableContext(std::nullptr_t) : IDC(nullptr){};

  const NominalTypeDecl *getNominal() const {
    switch (IDC->getIterableContextKind()) {
    case IterableDeclContextKind::NominalTypeDecl:
      return cast<NominalTypeDecl>(IDC);
    case IterableDeclContextKind::ExtensionDecl:
      return cast<ExtensionDecl>(IDC)->getExtendedNominal();
    }
    assert(false && "unhandled IterableDeclContextKind");
  }

  /// Get the left brace location of the type-or-extension decl.
  SourceLoc getLeftBraceLoc() const {
    switch (IDC->getIterableContextKind()) {
    case IterableDeclContextKind::NominalTypeDecl:
      return cast<NominalTypeDecl>(IDC)->getBraces().Start;
    case IterableDeclContextKind::ExtensionDecl:
      return cast<ExtensionDecl>(IDC)->getBraces().Start;
    }
    assert(false && "unhandled IterableDeclContextKind");
  }

  /// Get the token location where the text should be inserted after.
  SourceLoc getInsertStartLoc() const {
    // Prefer the end of elements.
    for (auto *member : llvm::reverse(IDC->getParsedMembers())) {
      if (isa<AccessorDecl>(member) || isa<VarDecl>(member)) {
        // These are part of 'PatternBindingDecl' but are hoisted in AST.
        continue;
      }
      return member->getEndLoc();
    }

    // After the starting brace if empty.
    return getLeftBraceLoc();
  }

  std::string getBaseIndent() const {
    SourceManager &SM = IDC->getDecl()->getASTContext().SourceMgr;
    SourceLoc startLoc = getInsertStartLoc();
    StringRef extraIndent;
    StringRef currentIndent =
        Lexer::getIndentationForLine(SM, startLoc, &extraIndent);
    if (startLoc == getLeftBraceLoc()) {
      return (currentIndent + extraIndent).str();
    } else {
      return currentIndent.str();
    }
  }

  void printInsertText(llvm::raw_ostream &OS) const {
    auto &ctx = IDC->getDecl()->getASTContext();

    PrintOptions Options = PrintOptions::printDeclarations();
    Options.SynthesizeSugarOnTypes = true;
    Options.FunctionDefinitions = true;
    Options.VarInitializers = true;
    Options.PrintExprs = true;
    Options.TypeDefinitions = false;
    Options.PrintSpaceBeforeInheritance = false;
    Options.ExcludeAttrList.push_back(DeclAttrKind::HasInitialValue);
    Options.PrintInternalAccessKeyword = false;

    std::string baseIndent = getBaseIndent();
    ExtraIndentStreamPrinter Printer(OS, baseIndent);

    // The insertion starts at the end of the last token.
    Printer.printNewline();

    // Synthesized 'CodingKeys' are placed in the main nominal decl.
    // Iterate members and look for synthesized enums that conforms to
    // 'CodingKey' protocol.
    auto *codingKeyProto = ctx.getProtocol(KnownProtocolKind::CodingKey);
    for (auto *member : getNominal()->getMembers()) {
      auto *enumD = dyn_cast<EnumDecl>(member);
      if (!enumD || !enumD->isSynthesized())
        continue;
      llvm::SmallVector<ProtocolConformance *, 1> codingKeyConformance;
      if (!enumD->lookupConformance(codingKeyProto, codingKeyConformance))
        continue;

      // Print the decl, but without the body.
      Printer.printNewline();
      enumD->print(Printer, Options);

      // Manually print elements because CodingKey enums have some synthesized
      // members for the protocol conformance e.g 'init(intValue:)'.
      // We don't want to print them here.
      Printer << " {";
      Printer.printNewline();
      Printer.setIndent(2);
      for (auto *elementD : enumD->getAllElements()) {
        elementD->print(Printer, Options);
        Printer.printNewline();
      }
      Printer.setIndent(0);
      Printer << "}";
      Printer.printNewline();
    }

    // Look for synthesized witness decls and print them.
    for (auto *conformance : IDC->getLocalConformances()) {
      auto protocol = conformance->getProtocol();
      auto kind = protocol->getKnownProtocolKind();
      if (kind == KnownProtocolKind::Encodable ||
          kind == KnownProtocolKind::Decodable) {
        for (auto requirement : protocol->getProtocolRequirements()) {
          auto witness = conformance->getWitnessDecl(requirement);
          if (witness && witness->isSynthesized()) {
            Printer.printNewline();
            witness->print(Printer, Options);
            Printer.printNewline();
          }
        }
      }
    }
  }

public:
  static AddCodableContext getFromCursorInfo(ResolvedCursorInfoPtr Info);

  bool isApplicable() const {
    if (!IDC || !getNominal())
      return false;

    // Check if 'IDC' conforms to 'Encodable' and/or 'Decodable' and any of the
    // requirements are synthesized.
    for (auto *conformance : IDC->getLocalConformances()) {
      auto protocol = conformance->getProtocol();
      auto kind = protocol->getKnownProtocolKind();
      if (kind == KnownProtocolKind::Encodable ||
          kind == KnownProtocolKind::Decodable) {
        // Check if any of the protocol requirements are synthesized.
        for (auto requirement : protocol->getProtocolRequirements()) {
          auto witness = conformance->getWitnessDecl(requirement);
          if (!witness || witness->isSynthesized())
            return true;
        }
      }
    }
    return false;
  }

  void getInsertion(SourceLoc &insertLoc, std::string &insertText) const {
    insertLoc = getInsertStartLoc();
    llvm::raw_string_ostream OS(insertText);
    printInsertText(OS);
  }
};

AddCodableContext
AddCodableContext::getFromCursorInfo(ResolvedCursorInfoPtr Info) {
  auto ValueRefInfo = dyn_cast<ResolvedValueRefCursorInfo>(Info);
  if (!ValueRefInfo) {
    return nullptr;
  }

  if (auto *ext = ValueRefInfo->getExtTyRef()) {
    // For 'extension Outer.Inner: Codable {}', only 'Inner' part is valid.
    if (ext->getExtendedNominal() == ValueRefInfo->getValueD()) {
      return AddCodableContext(ext);
    } else {
      return nullptr;
    }
  }

  if (!ValueRefInfo->isRef()) {
    if (auto *nominal = dyn_cast<NominalTypeDecl>(ValueRefInfo->getValueD())) {
      return AddCodableContext(nominal);
    }
  }

  return nullptr;
}
} // namespace

bool RefactoringActionAddExplicitCodableImplementation::isApplicable(
    ResolvedCursorInfoPtr Tok, DiagnosticEngine &Diag) {
  return AddCodableContext::getFromCursorInfo(Tok).isApplicable();
}

bool RefactoringActionAddExplicitCodableImplementation::performChange() {
  auto Context = AddCodableContext::getFromCursorInfo(CursorInfo);
  assert(Context.isApplicable() &&
         "Should not run performChange when refactoring is not applicable");

  SourceLoc insertLoc;
  std::string insertText;
  Context.getInsertion(insertLoc, insertText);

  EditConsumer.insertAfter(SM, insertLoc, insertText);
  return false;
}
