//===--- DeclarationFragmentPrinter.cpp - Declaration Fragment Printer ----===//
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

#include "swift/AST/USRGeneration.h"
#include "swift/Basic/Assertions.h"
#include "swift/Parse/Token.h"
#include "DeclarationFragmentPrinter.h"
#include "SymbolGraphASTWalker.h"

using namespace swift;
using namespace symbolgraphgen;

void DeclarationFragmentPrinter::openFragment(FragmentKind Kind) {
  assert(Kind != FragmentKind::None);
  if (this->Kind != Kind) {
    closeFragment();
    this->Kind = Kind,
    Spelling.clear();
    USR.clear();
  }
}

StringRef
DeclarationFragmentPrinter::getKindSpelling(FragmentKind Kind) const {
  switch (Kind) {
    case FragmentKind::Keyword:
      return "keyword";
    case FragmentKind::Attribute:
      return "attribute";
    case FragmentKind::NumberLiteral:
      return "number";
    case FragmentKind::StringLiteral:
      return "string";
    case FragmentKind::Identifier:
      return "identifier";
    case FragmentKind::TypeIdentifier:
      return "typeIdentifier";
    case FragmentKind::GenericParameter:
      return "genericParameter";
    case FragmentKind::InternalParam:
      return "internalParam";
    case FragmentKind::ExternalParam:
      return "externalParam";
    case FragmentKind::Text:
      return "text";
    case FragmentKind::None:
      llvm_unreachable("Fragment kind of 'None' has no spelling");
  }
  llvm_unreachable("invalid fragment kind");
}

void DeclarationFragmentPrinter::closeFragment() {
  if (Kind == FragmentKind::None) {
    return;
  }

  ++NumFragments;

  if (!Spelling.empty()) {
    OS.object([&](){
      OS.attribute("kind", getKindSpelling(Kind));
      OS.attribute("spelling", Spelling.str());
      if (!USR.empty()) {
        OS.attribute("preciseIdentifier", USR.str());
      }
    });
  }

  Spelling.clear();
  USR.clear();
  Kind = FragmentKind::None;
}

void DeclarationFragmentPrinter::printDeclLoc(const Decl *D) {
  switch (D->getKind()) {
    case DeclKind::Constructor:
    case DeclKind::Destructor:
    case DeclKind::Subscript:
      openFragment(FragmentKind::Keyword);
      break;
    default:
      openFragment(FragmentKind::Identifier);
      break;
  }
}

void
DeclarationFragmentPrinter::printNamePre(PrintNameContext Context) {
  switch (Context) {
  case PrintNameContext::Keyword:
  case PrintNameContext::IntroducerKeyword:
    openFragment(FragmentKind::Keyword);
    break;
  case PrintNameContext::GenericParameter:
    openFragment(FragmentKind::GenericParameter);
    break;
  case PrintNameContext::Attribute:
    openFragment(FragmentKind::Attribute);
    break;
  case PrintNameContext::ClassDynamicSelf:
  case PrintNameContext::FunctionParameterExternal:
    openFragment(FragmentKind::ExternalParam);
    break;
  case PrintNameContext::FunctionParameterLocal:
    openFragment(FragmentKind::InternalParam);
    break;
  case PrintNameContext::TupleElement:
  case PrintNameContext::TypeMember:
  case PrintNameContext::Normal:
    break;
  }
}

void DeclarationFragmentPrinter::printStructurePre(PrintStructureKind Kind,
                                                   const Decl *D) {
  switch (Kind) {
    case PrintStructureKind::NumberLiteral:
      openFragment(FragmentKind::NumberLiteral);
      break;
    case PrintStructureKind::StringLiteral:
      openFragment(FragmentKind::StringLiteral);
      break;
    default:
      break;
  }
}

void DeclarationFragmentPrinter::printTypeRef(Type T, const TypeDecl *RefTo,
    Identifier Name,
    PrintNameContext NameContext) {
  if (Kind != FragmentKind::Attribute) {
    openFragment(FragmentKind::TypeIdentifier);
  } else {
    // create a separate fragment so that only the attribute name is linkable
    closeFragment();
    openFragment(FragmentKind::Attribute);
  }
  printText(Name.str());
  USR.clear();

  auto ShouldLink = Name.str() != "Self";
  if (T) {
    if (const auto *TD = T->getAnyNominal()) {
      if (SG->isImplicitlyPrivate(TD)) {
        ShouldLink = false;
      }
    }

    if (T->isTypeParameter() && T->getKind() != TypeKind::DependentMember) {
      ShouldLink = false;
    }
  }

  if (ShouldLink) {
    llvm::raw_svector_ostream OS(USR);
    ide::printDeclUSR(RefTo, OS);
    if (ReferencedDecls)
      ReferencedDecls->insert(RefTo);
  }
  closeFragment();
}

void DeclarationFragmentPrinter::printText(StringRef Text) {
  if (Kind == FragmentKind::None) {
    openFragment(FragmentKind::Text);
  }
  Spelling.append(Text);
}

void DeclarationFragmentPrinter::printAbridgedType(const GenericTypeDecl *TD,
                                                   bool PrintKeyword) {
  if (PrintKeyword) {
    openFragment(DeclarationFragmentPrinter::FragmentKind::Keyword);
    switch (TD->getKind()) {
      case DeclKind::Struct:
        printText(getTokenText(tok::kw_struct));
        break;
      case DeclKind::Enum:
        printText(getTokenText(tok::kw_enum));
        break;
      case DeclKind::Protocol:
        printText(getTokenText(tok::kw_protocol));
        break;
      case DeclKind::Class: {
        auto *refDecl = cast<ClassDecl>(TD);
        if (refDecl->isExplicitDistributedActor()) {
          printText("distributed actor");
        } else if (refDecl->isExplicitActor()) {
          printText("actor");
        } else {
          printText(getTokenText(tok::kw_class));
        }
      break;
      }
      case DeclKind::TypeAlias:
        printText(getTokenText(tok::kw_typealias));
        break;
      case DeclKind::OpaqueType:
        llvm_unreachable("OpaqueType should not be in symbol graphs!");
      default:
        llvm_unreachable("GenericTypeDecl kind not handled in DeclarationFragmentPrinter!");
    }

    openFragment(DeclarationFragmentPrinter::FragmentKind::Text);
    printText(" ");
  }

  openFragment(DeclarationFragmentPrinter::FragmentKind::Identifier);
  printText(TD->getNameStr());
}
