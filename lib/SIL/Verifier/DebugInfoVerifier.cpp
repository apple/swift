//===--- DebugInfoVerifier.cpp --------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
///
/// Utility verifier code for validating debug info.
///
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILInstruction.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                   MARK: Verify SILInstruction Debug Info
//===----------------------------------------------------------------------===//

void SILInstruction::verifyDebugInfo() const {
  auto require = [&](bool reqt, StringRef message) {
    if (!reqt) {
      llvm::errs() << message << "\n";
      assert(false && "invoking standard assertion failure");
    }
  };

  // Check the location kind.
  SILLocation loc = getLoc();
  SILLocation::LocationKind locKind = loc.getKind();
  SILInstructionKind instKind = getKind();

  // Regular locations are allowed on all instructions.
  if (locKind == SILLocation::RegularKind)
    return;

  if (locKind == SILLocation::ReturnKind ||
      locKind == SILLocation::ImplicitReturnKind)
    require(
        instKind == SILInstructionKind::BranchInst ||
            instKind == SILInstructionKind::ReturnInst ||
            instKind == SILInstructionKind::UnreachableInst,
        "return locations are only allowed on branch and return instructions");

  if (locKind == SILLocation::ArtificialUnreachableKind)
    require(
        instKind == SILInstructionKind::UnreachableInst,
        "artificial locations are only allowed on Unreachable instructions");
}
