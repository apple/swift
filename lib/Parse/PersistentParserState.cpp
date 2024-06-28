//===--- PersistentParserState.cpp - Parser State Implementation ----------===//
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
//  This file implements parser state persistent across multiple parses.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Parse/PersistentParserState.h"

using namespace swift;

PersistentParserState::PersistentParserState() { }

PersistentParserState::~PersistentParserState() { }

void PersistentParserState::setIDEInspectionDelayedDeclState(
    SourceManager &SM, unsigned BufferID, IDEInspectionDelayedDeclKind Kind,
    DeclContext *ParentContext, SourceRange BodyRange, SourceLoc PreviousLoc) {
  assert(!IDEInspectionDelayedDeclStat.get() &&
         "only one decl can be delayed for code completion");
  unsigned startOffset = SM.getLocOffsetInBuffer(BodyRange.Start, BufferID);
  unsigned endOffset = SM.getLocOffsetInBuffer(BodyRange.End, BufferID);
  unsigned prevOffset = ~0U;
  if (PreviousLoc.isValid())
    prevOffset = SM.getLocOffsetInBuffer(PreviousLoc, BufferID);

  IDEInspectionDelayedDeclStat.reset(new IDEInspectionDelayedDeclState(
      Kind, ParentContext, startOffset, endOffset, prevOffset));
}

void PersistentParserState::restoreIDEInspectionDelayedDeclState(
    const IDEInspectionDelayedDeclState &other) {
  IDEInspectionDelayedDeclStat.reset(new IDEInspectionDelayedDeclState(
      other.Kind, other.ParentContext, other.StartOffset, other.EndOffset,
      other.PrevOffset));
}
