//===--- DeclNameLoc.cpp - Declaration Name Location Info -----------------===//
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
//
//  This file implements the DeclNameLoc class.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/DeclNameLoc.h"
#include "swift/AST/ASTContext.h"

using namespace swift;

DeclNameLoc::DeclNameLoc(ASTContext &ctx, SourceLoc moduleSelectorLoc,
                         SourceLoc baseNameLoc,
                         SourceLoc lParenLoc,
                         ArrayRef<SourceLoc> argumentLabelLocs,
                         SourceLoc rParenLoc)
    : NumArgumentLabels(argumentLabelLocs.size()),
      HasModuleSelectorLoc(moduleSelectorLoc.isValid())
{
  if (NumArgumentLabels == 0 && !HasModuleSelectorLoc) {
    LocationInfo = baseNameLoc.getOpaquePointerValue();
    return;
  }

  // Copy the location information into permanent storage.
  auto storedLocs =
      ctx.Allocate<SourceLoc>(FirstArgumentLabelIndex + NumArgumentLabels);
  storedLocs[BaseNameIndex] = baseNameLoc;
  storedLocs[ModuleSelectorIndex] = moduleSelectorLoc;
  storedLocs[LParenIndex] = lParenLoc;
  storedLocs[RParenIndex] = rParenLoc;
  std::memcpy(storedLocs.data() + FirstArgumentLabelIndex,
              argumentLabelLocs.data(),
              argumentLabelLocs.size() * sizeof(SourceLoc));

  LocationInfo = storedLocs.data();
}
