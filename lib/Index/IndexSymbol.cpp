//===--- IndexSymbol.cpp --------------------------------------------------===//
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

#include "swift/Index/IndexSymbol.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Types.h"

using namespace swift;
using namespace swift::index;

static NominalTypeDecl *getNominalParent(const ValueDecl *D) {
  return D->getDeclContext()->getSelfNominalTypeDecl();
}

/// \returns true if \c D is a subclass of 'XCTestCase'.
static bool isUnitTestCase(const ClassDecl *D) {
  if (!D)
    return false;

  return D->walkSuperclasses([D](ClassDecl *SuperD) {
    if (SuperD != D && // Do not treat XCTestCase itself as a test.
        SuperD->getNameStr() == "XCTestCase")
      return TypeWalker::Action::Stop; // Found test; stop and return true.
    return TypeWalker::Action::Continue;
  });
}

static bool isUnitTest(const ValueDecl *D) {
  if (!D->hasName())
    return false;

  // A 'test candidate' is:
  // 1. An instance method...
  auto FD = dyn_cast<FuncDecl>(D);
  if (!FD)
    return false;
  if (!D->isInstanceMember())
    return false;

  // 2. ...on a class or extension (not a struct) subclass of XCTestCase...
  auto parentNTD = getNominalParent(D);
  if (!parentNTD)
    return false;
  if (!isa<ClassDecl>(parentNTD))
    return false;
  if (!isUnitTestCase(cast<ClassDecl>(parentNTD)))
    return false;

  // 3. ...that returns void...
  Type RetTy = FD->getResultInterfaceType();
  if (RetTy && !RetTy->isVoid())
    return false;

  // 4. ...takes no parameters...
  if (FD->getParameters()->size() != 0)
    return false;

  // 5. ...is of at least 'internal' access (unless we can use
  //    Objective-C reflection)...
  if (!D->getASTContext().LangOpts.EnableObjCInterop &&
      (D->getFormalAccess() < AccessLevel::Internal ||
      parentNTD->getFormalAccess() < AccessLevel::Internal))
    return false;

  // 6. ...and starts with "test".
  if (FD->getBaseIdentifier().str().startswith("test"))
    return true;

  return false;
}

static void setFuncSymbolInfo(const FuncDecl *FD, SymbolInfo &sym) {
  sym.Kind = SymbolKind::Function;

  if (FD->getAttrs().hasAttribute<IBActionAttr>() ||
      FD->getAttrs().hasAttribute<IBSegueActionAttr>())
    sym.Properties |= SymbolProperty::IBAnnotated;

  if (isUnitTest(FD))
    sym.Properties |= SymbolProperty::UnitTest;

  if (FD->getDeclContext()->isTypeContext()) {
    if (FD->isStatic()) {
      if (FD->getCorrectStaticSpelling() == StaticSpellingKind::KeywordClass)
        sym.Kind = SymbolKind::ClassMethod;
      else
        sym.Kind = SymbolKind::StaticMethod;
    } else {
      sym.Kind = SymbolKind::InstanceMethod;
    }
  }

  if (auto accessor = dyn_cast<AccessorDecl>(FD)) {
    sym.SubKind = getSubKindForAccessor(accessor->getAccessorKind());
    return;
  }

  if (auto *op = FD->getOperatorDecl()) {
    switch (op->getKind()) {
      case DeclKind::PrefixOperator:
        sym.SubKind = SymbolSubKind::SwiftPrefixOperator;
        return;
      case DeclKind::PostfixOperator:
        sym.SubKind = SymbolSubKind::SwiftPostfixOperator;
        return;
      case DeclKind::InfixOperator:
        sym.SubKind = SymbolSubKind::SwiftInfixOperator;
        return;
      default:
        llvm_unreachable("unexpected operator kind");
    }
  }
}

static SymbolKind getVarSymbolKind(const VarDecl *VD) {
  auto *DC = VD->getDeclContext();
  if (DC->isTypeContext()) {
    if (VD->isStatic()) {
      if (VD->getCorrectStaticSpelling() == StaticSpellingKind::KeywordClass)
        return SymbolKind::ClassProperty;
      return SymbolKind::StaticProperty;
    }
    return SymbolKind::InstanceProperty;
  }
  return SymbolKind::Variable;
}

SymbolInfo index::getSymbolInfoForModule(ModuleEntity Mod) {
  SymbolInfo info;
  info.Kind = SymbolKind::Module;
  info.SubKind = SymbolSubKind::None;
  info.Properties = SymbolPropertySet();
  if (auto *D = Mod.getAsSwiftModule()) {
    if (!D->isNonSwiftModule()) {
      info.Lang = SymbolLanguage::Swift;
    } else {
      info.Lang = SymbolLanguage::C;
    }
  } else if (Mod.getAsClangModule()) {
    info.Lang = SymbolLanguage::C;
  } else {
    llvm_unreachable("unexpected module kind");
  }
  return info;
}

SymbolInfo index::getSymbolInfoForDecl(const Decl *D) {
  SymbolInfo info{ SymbolKind::Unknown, SymbolSubKind::None,
                   SymbolLanguage::Swift, SymbolPropertySet() };
  switch (D->getKind()) {
    case DeclKind::Enum:
      info.Kind = SymbolKind::Enum;
      break;
    case DeclKind::Struct:
      info.Kind = SymbolKind::Struct;
      break;
    case DeclKind::Protocol:
      info.Kind = SymbolKind::Protocol;
      break;
    case DeclKind::Class:
      info.Kind = SymbolKind::Class;
      if (isUnitTestCase(cast<ClassDecl>(D)))
        info.Properties |= SymbolProperty::UnitTest;
      break;
    case DeclKind::Extension: {
      info.Kind = SymbolKind::Extension;
      auto *ED = cast<ExtensionDecl>(D);
      NominalTypeDecl *NTD = ED->getExtendedNominal();
      if (!NTD)
        break;
      if (isa<StructDecl>(NTD))
        info.SubKind = SymbolSubKind::SwiftExtensionOfStruct;
      else if (auto *CD = dyn_cast<ClassDecl>(NTD)) {
        info.SubKind = SymbolSubKind::SwiftExtensionOfClass;
        if (isUnitTestCase(CD))
          info.Properties |= SymbolProperty::UnitTest;
      } else if (isa<EnumDecl>(NTD))
        info.SubKind = SymbolSubKind::SwiftExtensionOfEnum;
      else if (isa<ProtocolDecl>(NTD))
        info.SubKind = SymbolSubKind::SwiftExtensionOfProtocol;
      assert(info.SubKind != SymbolSubKind::None);
      break;
    }
    case DeclKind::TypeAlias:        info.Kind = SymbolKind::TypeAlias; break;
    case DeclKind::AssociatedType:
      info.Kind = SymbolKind::TypeAlias;
      info.SubKind = SymbolSubKind::SwiftAssociatedType;
      break;
    case DeclKind::GenericTypeParam:
      info.Kind = SymbolKind::TypeAlias;
      info.SubKind = SymbolSubKind::SwiftGenericTypeParam;
      break;
    case DeclKind::EnumElement:      info.Kind = SymbolKind::EnumConstant; break;
    case DeclKind::Subscript:
      info.Kind = SymbolKind::InstanceProperty;
      info.SubKind = SymbolSubKind::SwiftSubscript;
      break;
    case DeclKind::Constructor:      info.Kind = SymbolKind::Constructor; break;
    case DeclKind::Destructor:       info.Kind = SymbolKind::Destructor; break;
    case DeclKind::Param:
      info.Kind = SymbolKind::Parameter;
      break;
    case DeclKind::Accessor:
    case DeclKind::Func:
      setFuncSymbolInfo(cast<FuncDecl>(D), info);
      break;
    case DeclKind::Var:
      info.Kind = getVarSymbolKind(cast<VarDecl>(D));
      if (D->getAttrs().hasAttribute<IBOutletAttr>())
        info.Properties |= SymbolProperty::IBAnnotated;
      if (D->getAttrs().hasAttribute<GKInspectableAttr>())
        info.Properties |= SymbolProperty::GKInspectable;
      break;

    // Arguably these should be indexed?
    case DeclKind::PrecedenceGroup:
    case DeclKind::InfixOperator:
    case DeclKind::PrefixOperator:
    case DeclKind::PostfixOperator:
      break;

    case DeclKind::Macro:
      info.Kind = SymbolKind::Macro;
      break;

    // These all reflect some sort of uninteresting syntactic structure
    // and don't merit indexing.
    case DeclKind::Import:
    case DeclKind::PatternBinding:
    case DeclKind::EnumCase:
    case DeclKind::TopLevelCode:
    case DeclKind::IfConfig:
    case DeclKind::PoundDiagnostic:
    case DeclKind::Missing:
    case DeclKind::MissingMember:
    case DeclKind::Module:
    case DeclKind::OpaqueType:
    case DeclKind::BuiltinTuple:
    case DeclKind::MacroExpansion:
      break;
  }

  if (isLocalSymbol(D)) {
    info.Properties |= SymbolProperty::Local;
  }

  if (auto *FD = dyn_cast<AbstractFunctionDecl>(D)) {
    if (FD->hasAsync())
      info.Properties |= SymbolProperty::SwiftAsync;
  }

  return info;
}

SymbolSubKind index::getSubKindForAccessor(AccessorKind AK) {
  switch (AK) {
  case AccessorKind::Get:    return SymbolSubKind::AccessorGetter;
  case AccessorKind::Set:    return SymbolSubKind::AccessorSetter;
  case AccessorKind::WillSet:   return SymbolSubKind::SwiftAccessorWillSet;
  case AccessorKind::DidSet:    return SymbolSubKind::SwiftAccessorDidSet;
  case AccessorKind::Address: return SymbolSubKind::SwiftAccessorAddressor;
  case AccessorKind::MutableAddress:
    return SymbolSubKind::SwiftAccessorMutableAddressor;
  case AccessorKind::Read:      return SymbolSubKind::SwiftAccessorRead;
  case AccessorKind::Modify:    return SymbolSubKind::SwiftAccessorModify;
  case AccessorKind::Init:      return SymbolSubKind::SwiftAccessorInit;
  }

  llvm_unreachable("Unhandled AccessorKind in switch.");
}

bool index::isLocalSymbol(const swift::Decl *D) {
  return D->getDeclContext()->getLocalContext() &&
    (!isa<ParamDecl>(D) || cast<ParamDecl>(D)->getArgumentNameLoc().isValid() ||
     D->getDeclContext()->getContextKind() == DeclContextKind::AbstractClosureExpr);
}
