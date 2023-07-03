//===--- JSON.cpp - Symbol Graph JSON Helpers -----------------------------===//
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
// Adds Symbol Graph JSON serialization to other types.
//===----------------------------------------------------------------------===//

#include "JSON.h"
#include "Symbol.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/GenericParamList.h"
#include "swift/AST/Module.h"
#include "swift/AST/Type.h"
#include "swift/AST/USRGeneration.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Serialization/SerializedModuleLoader.h"

void swift::symbolgraphgen::serialize(const llvm::VersionTuple &VT,
                                      llvm::json::OStream &OS) {
  OS.object([&](){
    OS.attribute("major", VT.getMajor());
    if (VT.getMinor()) {
      OS.attribute("minor", *VT.getMinor());
    }
    if (VT.getSubminor()) {
      OS.attribute("patch", *VT.getSubminor());
    }
    // Despite the name,
    // this is not Semantic Versioning "build metadata"
    if (VT.getBuild()) {
      OS.attribute("prerelease", *VT.getBuild());
    }
  });
}

void swift::symbolgraphgen::serialize(const llvm::Triple &T,
                                      llvm::json::OStream &OS) {
  OS.object([&](){
    OS.attribute("architecture", T.getArchName());
    if (!T.getEnvironmentName().empty()) {
      OS.attribute("environment", T.getEnvironmentName());
    }
    OS.attribute("vendor", T.getVendorName());
    OS.attributeObject("operatingSystem", [&](){
      OS.attribute("name", T.getOSTypeName(T.getOS()));

      llvm::VersionTuple OSVersion = T.getOSVersion();
      if (!OSVersion.empty()) {
        OS.attributeBegin("minimumVersion");
        serialize(OSVersion, OS);
        OS.attributeEnd();
      }
    });
  });
}

void swift::symbolgraphgen::serialize(const ExtensionDecl *Extension,
                                      llvm::json::OStream &OS) {
  OS.attributeObject("swiftExtension", [&](){
    if (const auto *ExtendedNominal = Extension->getExtendedNominal()) {
      if (const auto *ExtendedModule = ExtendedNominal->getModuleContext()) {
        OS.attribute("extendedModule", ExtendedModule->getNameStr());
      }

      OS.attribute("typeKind", Symbol::getKind(ExtendedNominal).first);
    }

    SmallVector<Requirement, 4> FilteredRequirements;

    filterGenericRequirements(Extension,
                              FilteredRequirements);

    if (!FilteredRequirements.empty()) {
      OS.attributeArray("constraints", [&](){
        for (const auto &Requirement : FilteredRequirements) {
          serialize(Requirement, OS);
        }
      }); // end constraints:
    }
  }); // end swiftExtension:
}

void swift::symbolgraphgen::serialize(const Requirement &Req,
                                      llvm::json::OStream &OS) {
  StringRef Kind;
  switch (Req.getKind()) {
    case RequirementKind::SameShape:
      Kind = "sameShape";
      break;
    case RequirementKind::Conformance:
      Kind = "conformance";
      break;
    case RequirementKind::Superclass:
      Kind = "superclass";
      break;
    case RequirementKind::SameType:
      Kind = "sameType";
      break;
    case RequirementKind::Layout:
      return;
  }

  OS.object([&](){
    OS.attribute("kind", Kind);
    OS.attribute("lhs", Req.getFirstType()->getString());
    OS.attribute("rhs", Req.getSecondType()->getString());
    
    // If the RHS type has a USR we can link to, add it to the output
    if (auto *TyDecl = Req.getSecondType()->getAnyNominal()) {
      SmallString<256> USR;
      {
        llvm::raw_svector_ostream SOS(USR);
        ide::printDeclUSR(TyDecl, SOS);
      }
      OS.attribute("rhsPrecise", USR.str());
    }
  });
}

void swift::symbolgraphgen::serialize(const swift::GenericTypeParamType *Param,
                                      llvm::json::OStream &OS) {
  OS.object([&](){
    OS.attribute("name", Param->getName().str());
    OS.attribute("index", Param->getIndex());
    OS.attribute("depth", Param->getDepth());
  });
}

void swift::symbolgraphgen::serialize(const ModuleDecl &Module,
                                      llvm::json::OStream &OS,
                                      llvm::Triple Target) {
  auto *MainFile = Module.getFiles().front();
  switch (MainFile->getKind()) {
  case FileUnitKind::Builtin:
    llvm_unreachable("Unexpected module kind: Builtin");
  case FileUnitKind::DWARFModule:
    llvm_unreachable("Unexpected module kind: DWARFModule");
  case FileUnitKind::Synthesized:
    llvm_unreachable("Unexpected module kind: Synthesized");
    break;
  case FileUnitKind::Source:
    serialize(Module.getASTContext().LangOpts.Target, OS);
    break;
  case FileUnitKind::SerializedAST: {
    auto SerializedAST = cast<SerializedASTFile>(MainFile);
    auto Target = llvm::Triple(SerializedAST->getTargetTriple());
    serialize(Target, OS);
    break;
  }
  case FileUnitKind::ClangModule: {
    auto ClangModule = cast<ClangModuleUnit>(MainFile);
    if (const auto *Overlay = ClangModule->getOverlayModule()) {
      serialize(*Overlay, OS, Target);
    } else {
      serialize(Target, OS);
    }
    break;
  }
  }
}

void
swift::symbolgraphgen::filterGenericParams(
    ArrayRef<GenericTypeParamType *> GenericParams,
    SmallVectorImpl<const GenericTypeParamType*> &FilteredParams,
    SubstitutionMap SubMap) {

  for (auto Param : GenericParams) {
    if (const auto *GPD = Param->getDecl()) {

      // Ignore the implicit Self param
      if (GPD->isImplicit()) {
        if (!isa<ExtensionDecl>(GPD->getDeclContext()))
          continue;

        // Extension decls (and their children) refer to implicit copies of the
        // explicit params of the nominal they extend. Don't filter those out.
        auto *ED = cast<ExtensionDecl>(GPD->getDeclContext());
        if (auto *NTD = ED->getExtendedNominal()) {
          if (auto *GPL = NTD->getGenericParams()) {
            auto ImplicitAndSameName = [&](GenericTypeParamDecl *NominalGPD) {
              return NominalGPD->isImplicit() &&
                  GPD->getName() == NominalGPD->getName();
            };
            if (llvm::any_of(GPL->getParams(), ImplicitAndSameName))
              continue;
          }
        }
      }

      // Ignore parameters that have been substituted.
      if (!SubMap.empty()) {
        Type SubTy = Type(Param).subst(SubMap);
        if (!SubTy->hasError() && SubTy.getPointer() != Param) {
          if (!SubTy->is<ArchetypeType>()) {
            continue;
          }
          auto AT = SubTy->castTo<ArchetypeType>();
          if (!AT->getInterfaceType()->isEqual(Param)) {
            continue;
          }
        }
      }

      FilteredParams.push_back(Param);
    }
  }
}

static bool containsParams(swift::Type Ty, llvm::ArrayRef<const swift::GenericTypeParamType*> Others) {
  return Ty.findIf([&](swift::Type T) -> bool {
    if (auto AT = T->getAs<swift::ArchetypeType>()) {
      T = AT->getInterfaceType();
    }

    for (auto *Param: Others) {
      if (T->isEqual(const_cast<swift::GenericTypeParamType*>(Param)))
        return true;
    }
    return false;
  });
}

void swift::symbolgraphgen::filterGenericRequirements(
    ArrayRef<Requirement> Requirements,
    const NominalTypeDecl *Self,
    SmallVectorImpl<Requirement> &FilteredRequirements,
    SubstitutionMap SubMap,
    ArrayRef<const GenericTypeParamType *> FilteredParams) {

  for (const auto &Req : Requirements) {
    if (Req.getKind() == RequirementKind::Layout) {
      continue;
    }
    // extension /* protocol */ Q {
    // func foo() {}
    // }
    // ignore Self : Q, obvious
    if (Req.getSecondType()->getAnyNominal() == Self) {
      continue;
    }

    // Ignore requirements that don't involve the filtered set of generic
    // parameters after substitution.
    if (!SubMap.empty()) {
      Type SubFirst = Req.getFirstType().subst(SubMap);
      if (SubFirst->hasError())
        SubFirst = Req.getFirstType();
      Type SubSecond = Req.getSecondType().subst(SubMap);
      if (SubSecond->hasError())
        SubSecond = Req.getSecondType();

      if (!containsParams(SubFirst, FilteredParams) &&
          !containsParams(SubSecond, FilteredParams))
        continue;

      // Use the same requirement kind with the substituted types.
      FilteredRequirements.emplace_back(Req.getKind(), SubFirst, SubSecond);
    } else {
      // Use the original requirement.
      FilteredRequirements.push_back(Req);
    }
  }
}
void
swift::symbolgraphgen::filterGenericRequirements(const ExtensionDecl *Extension,
    SmallVectorImpl<Requirement> &FilteredRequirements) {
  for (const auto &Req : Extension->getGenericRequirements()) {
    if (Req.getKind() == RequirementKind::Layout) {
      continue;
    }

    if (!isa<ProtocolDecl>(Extension->getExtendedNominal()) &&
        Req.getFirstType()->isEqual(Extension->getExtendedType())) {
      continue;
    }

    // extension /* protocol */ Q
    // ignore Self : Q, obvious
    if (Req.getSecondType()->isEqual(Extension->getExtendedType())) {
      continue;
    }
    FilteredRequirements.push_back(Req);
  }
}
