//===--- DiagnoseUnnecessaryPreconcurrencyImports.cpp ---------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// This is run after TransferNonSendable and uses Sema infrastructure to
/// determine if in Sema or TransferNonSendable any of the preconcurrency import
/// statements were not used.
///
/// This only runs when RegionIsolation is enabled. If RegionIsolation is
/// disabled, we emit the unnecessary preconcurrency imports earlier during Sema
/// since no later diagnostics will be emitted.
///
/// NOTE: This needs to be a module pass and run after TransferNonSendable so we
/// can guarantee that we have run TransferNonSendable on all functions in our
/// module before this runs.
///
//===----------------------------------------------------------------------===//

#include "swift/AST/SourceFile.h"
#include "swift/Basic/Assertions.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/Sema/Concurrency.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                         MARK: Top Level Entrypoint
//===----------------------------------------------------------------------===//

namespace {

class DiagnoseUnnecessaryPreconcurrencyImports : public SILModuleTransform {
  void run() override {
    // If region isolation is not enabled... return early.
    if (!getModule()->getASTContext().LangOpts.hasFeature(
            Feature::RegionBasedIsolation))
      return;

    std::vector<SourceFile *> data;
    for (auto &fn : *getModule()) {
      auto *sf = fn.getSourceFile();
      if (!sf) {
        continue;
      }

      data.push_back(sf);
      assert(sf->getBufferID() != -1 && "Must have a buffer id");
    }

    // Sort unique by filename so our diagnostics are deterministic.
    //
    // TODO: If we cannot rely upon this, just sort by pointer address. Non
    // determinism emission of diagnostics isn't great but it isn't fatal.
    sortUnique(data, [](SourceFile *lhs, SourceFile *rhs) -> bool {
      return lhs->getBufferID() < rhs->getBufferID();
    });

    // At this point, we know that we have our sorted unique list of source
    // files.
    for (auto *sf : data) {
      diagnoseUnnecessaryPreconcurrencyImports(*sf);
    }
  }
};

} // namespace

SILTransform *swift::createDiagnoseUnnecessaryPreconcurrencyImports() {
  return new DiagnoseUnnecessaryPreconcurrencyImports();
}
