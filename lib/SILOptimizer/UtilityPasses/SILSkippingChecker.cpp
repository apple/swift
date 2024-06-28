//===---------------------- SILSkippingChecker.cpp ------------------------===//
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

#include "swift/AST/Module.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

/// Determines whether we should have skipped SILGenning a function that
/// appears in a SILModule. This only applies when
/// \c -experimental-skip-non-inlinable-function-bodies is enabled.
static bool shouldHaveSkippedFunction(const SILFunction &F) {
  assert(F.getModule().getOptions().SkipFunctionBodies ==
             FunctionBodySkipping::NonInlinable &&
         "this check only makes sense if we're skipping function bodies");

  // First, we only care about functions that haven't been marked serialized.
  // If they've been marked serialized, they will end up in the final module
  // and we needed to SILGen them.
  if (F.isAnySerialized())
    return false;

  // Next, we're looking for functions that shouldn't have a body, but do. If
  // the function doesn't have a body (i.e. it's an external declaration), we
  // skipped it successfully.
  if (F.isExternalDeclaration())
    return false;

  // Thunks and specializations are automatically synthesized, so it's fine that
  // they end up in the module.
  if (F.isThunk() || F.isSpecialization())
    return false;

  // FIXME: We can probably skip property initializers, too.
  auto func = F.getLocation().getAsASTNode<AbstractFunctionDecl>();
  if (!func)
    return false;

  // If a body is synthesized/implicit, it shouldn't be skipped.
  if (func->isImplicit())
    return false;

  // Local function bodies in inlinable code are okay to show up in the module.
  if (func->getDeclContext()->isLocalContext())
    return false;

  // FIXME: Identify __ivar_destroyer, __allocating_init, and
  //        __deallocating_deinit, which have no special marking, are always
  //        emitted, and do have a source location with the original decl
  //        attached.
  if (isa<DestructorDecl>(func) || isa<ConstructorDecl>(func))
    return false;

  // Some AccessorDecls can't be skipped (see IsFunctionBodySkippedRequest).
  if (auto *AD = dyn_cast<AccessorDecl>(func)) {
    if (AD->getAccessorKind() == AccessorKind::DidSet)
      return false;

    if (AD->hasForcedStaticDispatch())
      return false;
  }

  // Functions with @backDeployed may be copied into the client, so they
  // shouldn't be skipped. The SILFunction that may be copied into the client
  // should be serialized and therefore is already handled above. However, a
  // second resilient SILFunction is also emitted for back deployed functions.
  // Since the body of the function as written was not skipped, it's expected
  // that we see the SILFunction for the resilient copy here.
  if (func->hasBackDeployedAttr())
    return false;

  // If none of those conditions trip, then this is something that _should_
  // be serialized in the module even when we're skipping non-inlinable
  // function bodies.
  return true;
}

namespace {

/// This is a verification utility pass that's meant to be used with
/// \c -experimental-skip-non-inlinable-function-bodies or
/// \c -experimental-skip-all-function-bodies. It checks that SIL is only
/// generated for what's actually serialized.all functions
/// that aren't serialized also have no generated SIL.
class SILSkippingChecker : public SILModuleTransform {
  void run() override {
    auto &M = *getModule();

    // Skip this verification for SwiftOnoneSupport
    if (M.getSwiftModule()->isOnoneSupportModule())
      return;

    if (M.getOptions().SkipFunctionBodies == FunctionBodySkipping::All) {
      // Should only have an empty SIL module
      bool notEmpty = false;
      if (!M.getFunctions().empty())
        notEmpty = true;
      if (!M.getVTables().empty())
        notEmpty = true;
      if (!M.getWitnessTables().empty())
        notEmpty = true;
      if (!M.getSILGlobals().empty())
        notEmpty = true;

      if (notEmpty) {
        llvm::dbgs() << "Non-empty SIL module when all function bodies should "
                        "have been skipped!\n";
        abort();
      }
    }

    if (M.getOptions().SkipFunctionBodies ==
        FunctionBodySkipping::NonInlinable) {
      for (auto &F : *getModule()) {
        if (!shouldHaveSkippedFunction(F))
          continue;

        llvm::dbgs() << "Function serialized that should have been skipped!\n";
        F.getLocation().print(llvm::dbgs(), F.getModule().getSourceManager());
        llvm::dbgs() << "\n";
        F.dump();
        abort();
      }
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createSILSkippingChecker() {
  return new SILSkippingChecker();
}
