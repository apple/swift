//===--- FunctionOrder.cpp - Utility for function ordering ----------------===//
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

#include "swift/SILOptimizer/Analysis/FunctionOrder.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/TinyPtrVector.h"
#include <algorithm>

using namespace swift;

/// Use Tarjan's strongly connected components (SCC) algorithm to find
/// the SCCs in the call graph.
void BottomUpFunctionOrder::DFS(SILFunction *Start) {
  // Set the DFSNum for this node if we haven't already, and if we
  // have, which indicates it's already been visited, return.
  if (!DFSNum.insert(std::make_pair(Start, NextDFSNum)).second)
    return;

  assert(MinDFSNum.find(Start) == MinDFSNum.end() &&
         "Function should not already have a minimum DFS number!");

  MinDFSNum[Start] = NextDFSNum;
  ++NextDFSNum;

  DFSStack.insert(Start);

  // Visit all the instructions, looking for apply sites.
  for (auto &B : *Start) {
    for (auto &I : B) {
      CalleeList callees;
      if (auto FAS = FullApplySite::isa(&I)) {
        callees = BCA->getCalleeList(FAS);
      } else if (isa<StrongReleaseInst>(&I) || isa<ReleaseValueInst>(&I) ||
                 isa<DestroyValueInst>(&I)) {
        callees = BCA->getDestructors(I.getOperand(0)->getType(), /*isExactType*/ false);
      } else if (auto *bi = dyn_cast<BuiltinInst>(&I)) {
        switch (bi->getBuiltinInfo().ID) {
          case BuiltinValueKind::Once:
          case BuiltinValueKind::OnceWithContext:
            callees = BCA->getCalleeListOfValue(bi->getArguments()[1]);
            break;
          default:
            continue;
        }
      } else {
        continue;
      }

      for (auto *CalleeFn : callees) {
        // If not yet visited, visit the callee.
        if (DFSNum.find(CalleeFn) == DFSNum.end()) {
          DFS(CalleeFn);
          MinDFSNum[Start] = std::min(MinDFSNum[Start], MinDFSNum[CalleeFn]);
        } else if (DFSStack.count(CalleeFn)) {
          // If the callee is on the stack, it update our minimum DFS
          // number based on it's DFS number.
          MinDFSNum[Start] = std::min(MinDFSNum[Start], DFSNum[CalleeFn]);
        }
      }
    }
  }

  // If our DFS number is the minimum found, we've found a
  // (potentially singleton) SCC, so pop the nodes off the stack and
  // push the new SCC on our stack of SCCs.
  if (DFSNum[Start] == MinDFSNum[Start]) {
    SCC CurrentSCC;

    SILFunction *Popped;
    do {
      Popped = DFSStack.pop_back_val();
      CurrentSCC.push_back(Popped);
    } while (Popped != Start);

    TheSCCs.push_back(CurrentSCC);
  }
}

void BottomUpFunctionOrder::FindSCCs(SILModule &M) {
  for (auto &F : M)
    DFS(&F);
}
