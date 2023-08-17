//===--- USRGeneration.cpp - Routines for USR generation ------------------===//
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

#include "swift/AST/ASTContext.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/Module.h"
#include "swift/AST/USRGeneration.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/SwiftNameTranslation.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/USRGeneration.h"
#include "swift/Demangling/Demangler.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Lex/PreprocessingRecord.h"
#include "clang/Lex/Preprocessor.h"

using namespace swift;
using namespace ide;

static inline StringRef getUSRSpacePrefix() {
  return "s:";
}

bool ide::printTypeUSR(Type Ty, raw_ostream &OS) {
  assert(!Ty->hasArchetype() && "cannot have contextless archetypes mangled.");
  Mangle::ASTMangler Mangler;
  OS << Mangler.mangleTypeAsUSR(Ty->getRValueType());
  return false;
}

bool ide::printDeclTypeUSR(const ValueDecl *D, raw_ostream &OS) {
  Mangle::ASTMangler Mangler;
  std::string MangledName = Mangler.mangleDeclType(D);
  OS << MangledName;
  return false;
}

static bool printObjCUSRFragment(const ValueDecl *D, StringRef ObjCName,
                                 const ExtensionDecl *ExtContextD,
                                 raw_ostream &OS) {
  if (!D)
    return true;

  // The Swift module name that the decl originated from. If the decl is
  // originating from ObjC code (ObjC module or the bridging header) then this
  // will be empty.
  StringRef ModuleName;
  if (!D->hasClangNode())
    ModuleName = D->getModuleContext()->getNameStr();

  if (isa<ClassDecl>(D)) {
    StringRef extContextName;
    if (ExtContextD) {
      extContextName = ExtContextD->getModuleContext()->getNameStr();
    }
    clang::index::generateUSRForObjCClass(ObjCName, OS,
                                          ModuleName, extContextName);
  } else if (isa<ProtocolDecl>(D)) {
    clang::index::generateUSRForObjCProtocol(ObjCName, OS, ModuleName);
  } else if (isa<VarDecl>(D)) {
    clang::index::generateUSRForObjCProperty(ObjCName, D->isStatic(), OS);
  } else if (isa<ConstructorDecl>(D)) {
    // init() is a class member in Swift, but an instance method in ObjC.
    clang::index::generateUSRForObjCMethod(ObjCName, /*IsInstanceMethod=*/true,
                                           OS);
  } else if (isa<AbstractFunctionDecl>(D)) {
    clang::index::generateUSRForObjCMethod(ObjCName, D->isInstanceMember(), OS);
  } else if (isa<EnumDecl>(D)) {
    clang::index::generateUSRForGlobalEnum(ObjCName, OS, ModuleName);
  } else if (isa<EnumElementDecl>(D)) {
    clang::index::generateUSRForEnumConstant(ObjCName, OS);
  } else {
    llvm_unreachable("Unexpected value decl");
  }
  return false;
}

static bool printObjCUSRContext(const Decl *D, raw_ostream &OS) {
  OS << clang::index::getUSRSpacePrefix();
  auto *DC = D->getDeclContext();
  if (auto *Parent = DC->getSelfNominalTypeDecl()) {
    auto *extContextD = dyn_cast<ExtensionDecl>(DC);
    auto ObjCName = objc_translation::getObjCNameForSwiftDecl(Parent);
    if (printObjCUSRFragment(Parent, ObjCName.first.str(), extContextD, OS))
      return true;
  }
  return false;
}

static bool printObjCUSRForAccessor(const AbstractStorageDecl *ASD,
                                    AccessorKind Kind,
                                    raw_ostream &OS) {
  if (printObjCUSRContext(ASD, OS))
    return true;

  ObjCSelector Selector;
  switch (Kind) {
    case swift::AccessorKind::Get:
      Selector = ASD->getObjCGetterSelector();
      break;
    case swift::AccessorKind::Set:
      Selector = ASD->getObjCSetterSelector();
      break;
    default:
      llvm_unreachable("invalid accessor kind");
  }
  assert(Selector);
  llvm::SmallString<128> Buf;
  clang::index::generateUSRForObjCMethod(Selector.getString(Buf),
                                         ASD->isInstanceMember(), OS);
  return false;
}

static bool printObjCUSR(const ValueDecl *D, raw_ostream &OS) {
  if (printObjCUSRContext(D, OS))
    return true;
  auto *extContextD = dyn_cast<ExtensionDecl>(D->getDeclContext());

  auto ObjCName = objc_translation::getObjCNameForSwiftDecl(D);

  if (!ObjCName.first.empty())
    return printObjCUSRFragment(D, ObjCName.first.str(), extContextD, OS);

  assert(ObjCName.second);
  llvm::SmallString<128> Buf;
  return printObjCUSRFragment(D, ObjCName.second.getString(Buf),
                              extContextD, OS);
}

static bool shouldUseObjCUSR(const Decl *D) {
  // Only the subscript getter/setter are visible to ObjC rather than the
  // subscript itself
  if (isa<SubscriptDecl>(D))
    return false;

  auto Parent = D->getDeclContext()->getInnermostDeclarationDeclContext();
  if (Parent && (!shouldUseObjCUSR(Parent) || // parent should be visible too
                 !D->getDeclContext()->isTypeContext() || // no local decls
                 isa<TypeDecl>(D))) // nested types aren't supported
    return false;

  if (const auto *VD = dyn_cast<ValueDecl>(D)) {
    if (isa<EnumElementDecl>(VD))
      return true;
    return objc_translation::isVisibleToObjC(VD, AccessLevel::Internal);
  }

  if (const auto *ED = dyn_cast<ExtensionDecl>(D)) {
    if (auto baseClass = ED->getSelfClassDecl()) {
      return shouldUseObjCUSR(baseClass) && !baseClass->isForeign();
    }
  }
  return false;
}

std::string
swift::USRGenerationRequest::evaluate(Evaluator &evaluator,
                                      const ValueDecl *D) const {
  if (auto *VD = dyn_cast<VarDecl>(D))
    D = VD->getCanonicalVarDecl();

  if (!D->hasName() && !isa<ParamDecl>(D) && !isa<AccessorDecl>(D))
    return std::string(); // Ignore.
  if (D->getModuleContext()->isBuiltinModule() &&
      !isa<BuiltinTupleDecl>(D))
    return std::string(); // Ignore.
  if (isa<ModuleDecl>(D))
    return std::string(); // Ignore.

  auto interpretAsClangNode = [](const ValueDecl *D)->ClangNode {
    ClangNode ClangN = D->getClangNode();
    if (auto ClangD = ClangN.getAsDecl()) {
      // NSErrorDomain causes the clang enum to be imported like this:
      //
      // struct MyError {
      //     enum Code : Int32 {
      //         case errFirst
      //         case errSecond
      //     }
      //     static var errFirst: MyError.Code { get }
      //     static var errSecond: MyError.Code { get }
      // }
      //
      // The clang enum constants are associated with both the static vars and
      // the enum cases.
      // But we want unique USRs for the above symbols, so use the clang USR
      // for the enum cases, and the Swift USR for the vars.
      //
      if (auto *ClangEnumConst = dyn_cast<clang::EnumConstantDecl>(ClangD)) {
        if (auto *ClangEnum = dyn_cast<clang::EnumDecl>(ClangEnumConst->getDeclContext())) {
          if (ClangEnum->hasAttr<clang::NSErrorDomainAttr>() && isa<VarDecl>(D))
            return ClangNode();
        }
      }
    }
    return ClangN;
  };

  llvm::SmallString<128> Buffer;
  llvm::raw_svector_ostream OS(Buffer);

  if (ClangNode ClangN = interpretAsClangNode(D)) {
    if (auto ClangD = ClangN.getAsDecl()) {
      bool Ignore = clang::index::generateUSRForDecl(ClangD, Buffer);
      if (!Ignore) {
        return std::string(Buffer.str());
      } else {
        return std::string();
      }
    }

    auto &Importer = *D->getASTContext().getClangModuleLoader();

    auto ClangMacroInfo = ClangN.getAsMacro();
    bool Ignore = clang::index::generateUSRForMacro(
        D->getBaseIdentifier().str(),
        ClangMacroInfo->getDefinitionLoc(),
        Importer.getClangASTContext().getSourceManager(), Buffer);
    if (!Ignore)
      return std::string(Buffer.str());
    else
      return std::string();
  }

  if (shouldUseObjCUSR(D)) {
    if (printObjCUSR(D, OS)) {
      return std::string();
    } else {
      return std::string(OS.str());
    }
  }

  auto declIFaceTy = D->getInterfaceType();

  // Invalid code.
  if (declIFaceTy.findIf([](Type t) -> bool {
        return t->is<ModuleType>();
      }))
    return std::string();

  Mangle::ASTMangler NewMangler;
  return NewMangler.mangleDeclAsUSR(D, getUSRSpacePrefix());
}

std::string ide::demangleUSR(StringRef mangled) {
  if (mangled.startswith(getUSRSpacePrefix())) {
    mangled = mangled.substr(getUSRSpacePrefix().size());
  }
  SmallString<128> buffer;
  buffer += "$s";
  buffer += mangled;
  mangled = buffer.str();
  Demangler Dem;
  return nodeToString(Dem.demangleSymbol(mangled));
}

std::string
swift::MangleLocalTypeDeclRequest::evaluate(Evaluator &evaluator,
                                            const TypeDecl *D) const {
  if (isa<ModuleDecl>(D))
    return std::string(); // Ignore.

  Mangle::ASTMangler NewMangler;
  return NewMangler.mangleLocalTypeDecl(D);
}

bool ide::printModuleUSR(ModuleEntity Mod, raw_ostream &OS) {
  if (auto *D = Mod.getAsSwiftModule()) {
    StringRef moduleName = D->getRealName().str();
    return clang::index::generateFullUSRForTopLevelModuleName(moduleName, OS);
  } else if (auto ClangM = Mod.getAsClangModule()) {
    return clang::index::generateFullUSRForModule(ClangM, OS);
  } else {
    return true;
  }
}

bool ide::printValueDeclUSR(const ValueDecl *D, raw_ostream &OS) {
  auto result = evaluateOrDefault(D->getASTContext().evaluator,
                                  USRGenerationRequest { D },
                                  std::string());
  if (result.empty())
    return true;
  OS << result;
  return false;
}

bool ide::printAccessorUSR(const AbstractStorageDecl *D, AccessorKind AccKind,
                           llvm::raw_ostream &OS) {
  // AccKind should always be either IsGetter or IsSetter here, based
  // on whether a reference is a mutating or non-mutating use.  USRs
  // aren't supposed to reflect implementation differences like stored
  // vs. addressed vs. observing.
  //
  // On the other side, the implementation indexer should be
  // registering the getter/setter USRs independently of how they're
  // actually implemented.  So a stored variable should still have
  // getter/setter USRs (pointing to the variable declaration), and an
  // addressed variable should have its "getter" point at the
  // addressor.

  AbstractStorageDecl *SD = const_cast<AbstractStorageDecl*>(D);
  if (shouldUseObjCUSR(SD)) {
    return printObjCUSRForAccessor(SD, AccKind, OS);
  }

  Mangle::ASTMangler NewMangler;
  std::string Mangled = NewMangler.mangleAccessorEntityAsUSR(AccKind,
                          SD, getUSRSpacePrefix(), SD->isStatic());

  OS << Mangled;

  return false;
}

bool ide::printExtensionUSR(const ExtensionDecl *ED, raw_ostream &OS) {
  auto nominal = ED->getExtendedNominal();
  if (!nominal)
    return true;

  // We make up a unique usr for each extension by combining a prefix
  // and the USR of the first value member of the extension.
  for (auto D : ED->getMembers()) {
    if (auto VD = dyn_cast<ValueDecl>(D)) {
      OS << getUSRSpacePrefix() << "e:";
      return printValueDeclUSR(VD, OS);
    }
  }
  OS << getUSRSpacePrefix() << "e:";
  printValueDeclUSR(nominal, OS);
  for (auto Inherit : ED->getInherited()) {
    if (auto T = Inherit.getType()) {
      if (T->getAnyNominal())
        return printValueDeclUSR(T->getAnyNominal(), OS);
    }
  }
  return true;
}

bool ide::printDeclUSR(const Decl *D, raw_ostream &OS) {
  if (auto *VD = dyn_cast<ValueDecl>(D)) {
    if (ide::printValueDeclUSR(VD, OS)) {
      return true;
    }
  } else if (auto *ED = dyn_cast<ExtensionDecl>(D)) {
    if (ide::printExtensionUSR(ED, OS)) {
      return true;
    }
  } else {
    return true;
  }
  return false;
}
