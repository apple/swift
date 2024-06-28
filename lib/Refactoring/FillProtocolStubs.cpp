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
#include "swift/AST/ProtocolConformance.h"
#include "swift/Basic/Assertions.h"

using namespace swift::refactoring;

namespace {
/// The helper class analyzes a given nominal decl or an extension decl to
/// decide whether stubs are required to filled in and the context in which
/// these stubs should be filled.
class FillProtocolStubContext {

  std::vector<ValueDecl *>
  getUnsatisfiedRequirements(const IterableDeclContext *IDC);

  /// Context in which the content should be filled; this could be either a
  /// nominal type declaraion or an extension declaration.
  DeclContext *DC;

  /// The type that adopts the required protocol stubs. For nominal type decl,
  /// this should be the declared type itself; for extension decl, this should
  /// be the extended type at hand.
  Type Adopter;

  /// The start location of the decl, either nominal type or extension, for the
  /// printer to figure out the right indentation.
  SourceLoc StartLoc;

  /// The location of '{' for the decl, thus we know where to insert the filling
  /// stubs.
  SourceLoc BraceStartLoc;

  /// The value decls that should be satisfied; this could be either function
  /// decls, property decls, or required type alias.
  std::vector<ValueDecl *> FillingContents;

public:
  FillProtocolStubContext(ExtensionDecl *ED)
      : DC(ED), Adopter(ED->getExtendedType()), StartLoc(ED->getStartLoc()),
        BraceStartLoc(ED->getBraces().Start),
        FillingContents(getUnsatisfiedRequirements(ED)){};

  FillProtocolStubContext(NominalTypeDecl *ND)
      : DC(ND), Adopter(ND->getDeclaredType()), StartLoc(ND->getStartLoc()),
        BraceStartLoc(ND->getBraces().Start),
        FillingContents(getUnsatisfiedRequirements(ND)){};

  FillProtocolStubContext() : DC(nullptr), Adopter(), FillingContents({}){};

  static FillProtocolStubContext
  getContextFromCursorInfo(ResolvedCursorInfoPtr Tok);

  ArrayRef<ValueDecl *> getFillingContents() const {
    return llvm::ArrayRef(FillingContents);
  }

  DeclContext *getFillingContext() const { return DC; }

  bool canProceed() const {
    return StartLoc.isValid() && BraceStartLoc.isValid() &&
           !getFillingContents().empty();
  }

  Type getAdopter() const { return Adopter; }
  SourceLoc getContextStartLoc() const { return StartLoc; }
  SourceLoc getBraceStartLoc() const { return BraceStartLoc; }
};

FillProtocolStubContext FillProtocolStubContext::getContextFromCursorInfo(
    ResolvedCursorInfoPtr CursorInfo) {
  if (!CursorInfo->isValid())
    return FillProtocolStubContext();
  auto ValueRefInfo = dyn_cast<ResolvedValueRefCursorInfo>(CursorInfo);
  if (!ValueRefInfo) {
    return FillProtocolStubContext();
  }
  if (!ValueRefInfo->isRef()) {
    // If the type name is on the declared nominal, e.g. "class A {}"
    if (auto ND = dyn_cast<NominalTypeDecl>(ValueRefInfo->getValueD())) {
      return FillProtocolStubContext(ND);
    }
  } else if (auto *ED = ValueRefInfo->getExtTyRef()) {
    // If the type ref is on a declared extension, e.g. "extension A {}"
    return FillProtocolStubContext(ED);
  }
  return FillProtocolStubContext();
}
} // namespace

std::vector<ValueDecl *> FillProtocolStubContext::getUnsatisfiedRequirements(
    const IterableDeclContext *IDC) {
  // The results to return.
  std::vector<ValueDecl *> NonWitnessedReqs;

  // For each conformance of the extended nominal.
  for (ProtocolConformance *Con : IDC->getLocalConformances()) {

    // Collect non-witnessed requirements.
    Con->forEachNonWitnessedRequirement(
        [&](ValueDecl *VD) { NonWitnessedReqs.push_back(VD); });
  }

  return NonWitnessedReqs;
}

bool RefactoringActionFillProtocolStub::isApplicable(ResolvedCursorInfoPtr Tok,
                                                     DiagnosticEngine &Diag) {
  return FillProtocolStubContext::getContextFromCursorInfo(Tok).canProceed();
}

bool RefactoringActionFillProtocolStub::performChange() {
  // Get the filling protocol context from the input token.
  FillProtocolStubContext Context =
      FillProtocolStubContext::getContextFromCursorInfo(CursorInfo);

  assert(Context.canProceed());
  assert(!Context.getFillingContents().empty());
  assert(Context.getFillingContext());
  SmallString<128> Text;
  {
    llvm::raw_svector_ostream SS(Text);
    Type Adopter = Context.getAdopter();
    SourceLoc Loc = Context.getContextStartLoc();
    auto Contents = Context.getFillingContents();

    // For each unsatisfied requirement, print the stub to the buffer.
    std::for_each(Contents.begin(), Contents.end(), [&](ValueDecl *VD) {
      printRequirementStub(VD, Context.getFillingContext(), Adopter, Loc, SS);
    });
  }

  // Insert all stubs after '{' in the extension/nominal type decl.
  EditConsumer.insertAfter(SM, Context.getBraceStartLoc(), Text);
  return false;
}
