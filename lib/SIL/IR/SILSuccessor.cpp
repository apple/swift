//===--- SILSuccessor.cpp - Implementation of SILSuccessor.h --------------===//
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

#include "swift/Basic/Assertions.h"
#include "swift/SIL/SILSuccessor.h"
#include "swift/SIL/SILBasicBlock.h"
using namespace swift;

void SILSuccessor::operator=(SILBasicBlock *BB) {
  // If we're not changing anything, we're done.
  if (SuccessorBlock == BB) return;
  
  assert(ContainingInst &&"init method not called after default construction?");
  
  // If we were already pointing to a basic block, remove ourself from its
  // predecessor list.
  if (SuccessorBlock) {
    *Prev = Next;
    if (Next) Next->Prev = Prev;
  }
  
  // If we have a successor, add ourself to its prev list.
  if (BB) {
    Prev = &BB->PredList;
    Next = BB->PredList;
    if (Next) Next->Prev = &Next;
    BB->PredList = this;
  }
  
  SuccessorBlock = BB;
}
