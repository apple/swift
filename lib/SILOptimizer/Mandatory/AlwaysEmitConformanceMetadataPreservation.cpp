//===--- AlwaysEmitConformanceMetadataPreservation.cpp -------------------===//
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
///
/// Some frameworks may rely on conformances to protocols they provide
/// to be present in binary product they are compiled into even if
/// such conformances are not otherwise referenced in user code.
/// Such conformances may then, for example, be queried and used by the
/// runtime.
///
/// The developer may not ever explicitly reference or instantiate this type in
/// their code, as it is effectively defining an XPC endpoint. However, when
/// optimizations are enabled, the type may be stripped from the binary as it is
/// never referenced. `@_alwaysEmitConformanceMetadata` can be used to mark
/// a protocol to ensure that its conformances are always marked as externally
/// visible even if not `public` to ensure they do not get optimized away.
/// This mandatory pass makes it so.
///
//===----------------------------------------------------------------------===//

#include "swift/AST/Attr.h"
#define DEBUG_TYPE "always-emit-conformance-metadata-preservation"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SourceFile.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"

using namespace swift;

namespace {

/// A helper class to collect all nominal type declarations that conform to
/// `@_alwaysEmitConformanceMetadata` protocols.
class AlwaysEmitMetadataConformanceCollector : public ASTWalker {
  std::vector<NominalTypeDecl *> &AlwaysEmitMetadataConformanceDecls;

public:
  AlwaysEmitMetadataConformanceCollector(
      std::vector<NominalTypeDecl *> &AlwaysEmitMetadataConformanceDecls)
      : AlwaysEmitMetadataConformanceDecls(AlwaysEmitMetadataConformanceDecls) {
  }

  PreWalkAction walkToDeclPre(Decl *D) override {
    auto hasAlwaysEmitMetadataConformance =
        [&](llvm::PointerUnion<const TypeDecl *, const ExtensionDecl *> Decl) {
          bool anyObject = false;
          for (const auto &found :
               getDirectlyInheritedNominalTypeDecls(Decl, anyObject))
            if (auto Protocol = dyn_cast<ProtocolDecl>(found.Item))
              if (Protocol->getAttrs()
                      .hasAttribute<AlwaysEmitConformanceMetadataAttr>())
                return true;
          return false;
        };

    if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
      if (hasAlwaysEmitMetadataConformance(NTD))
        AlwaysEmitMetadataConformanceDecls.push_back(NTD);
    } else if (auto *ETD = dyn_cast<ExtensionDecl>(D)) {
      if (hasAlwaysEmitMetadataConformance(ETD))
        AlwaysEmitMetadataConformanceDecls.push_back(ETD->getExtendedNominal());
    }

    return Action::Continue();
  }
};

class AlwaysEmitConformanceMetadataPreservation : public SILModuleTransform {
  void run() override {
    auto &M = *getModule();
    std::vector<NominalTypeDecl *> AlwaysEmitMetadataConformanceDecls;
    AlwaysEmitMetadataConformanceCollector Walker(
        AlwaysEmitMetadataConformanceDecls);

    SmallVector<Decl *> TopLevelDecls;
    if (M.getSwiftModule()->isMainModule()) {
      if (M.isWholeModule()) {
        for (const auto File : M.getSwiftModule()->getFiles())
          File->getTopLevelDecls(TopLevelDecls);
      } else {
        for (const auto Primary : M.getSwiftModule()->getPrimarySourceFiles())
          Primary->getTopLevelDecls(TopLevelDecls);
      }
    }
    for (auto *TLD : TopLevelDecls)
      TLD->walk(Walker);

    for (auto &NTD : AlwaysEmitMetadataConformanceDecls)
      M.addExternallyVisibleDecl(NTD);
  }
};
} // end anonymous namespace

SILTransform *swift::createAlwaysEmitConformanceMetadataPreservation() {
  return new AlwaysEmitConformanceMetadataPreservation();
}
