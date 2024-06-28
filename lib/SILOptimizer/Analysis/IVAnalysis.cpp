//===--- IVAnalysis.cpp - SIL IV Analysis ---------------------------------===//
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

#include "swift/SILOptimizer/Analysis/IVAnalysis.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/PatternMatch.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILValue.h"

using namespace swift;
using namespace swift::PatternMatch;

#if !defined(NDEBUG)
static bool inSCC(ValueBase *value, IVInfo::SCCType &SCC) {
  for (SILNode *node : SCC) {
    if (node == value)
      return true;
  }
  return false;
}
#endif

// For now, we'll consider only the simplest induction variables:
// - Exactly one element in the cycle must be a SILArgument.
// - Only a single increment by a literal.
//
// In other words many valid things that could be considered induction
// variables are disallowed at this point.
SILArgument *IVInfo::isInductionSequence(SCCType &SCC) {
  // Ignore SCCs of size 1 for now. Some of these are derived IVs
  // like i+1 or i*4, which we will eventually want to handle.
  if (SCC.size() == 1)
    return nullptr;

  BuiltinInst *FoundBuiltin = nullptr;
  SILArgument *FoundArgument = nullptr;
  IntegerLiteralInst *IncValue = nullptr;
  for (unsigned long i = 0, e = SCC.size(); i != e; ++i) {
    if (auto IV = dyn_cast<SILArgument>(SCC[i])) {
      if (FoundArgument)
        return nullptr;

      FoundArgument = IV;
      continue;
    }

    // TODO: MultiValueInstruction

    auto *I = cast<SILInstruction>(SCC[i]);
    switch (I->getKind()) {
    case SILInstructionKind::BuiltinInst: {
      if (FoundBuiltin)
        return nullptr;

      FoundBuiltin = cast<BuiltinInst>(I);

      SILValue L, R;
      if (!match(FoundBuiltin, m_ApplyInst(BuiltinValueKind::SAddOver,
                                           m_SILValue(L), m_SILValue(R))))
        return nullptr;

      if (match(L, m_IntegerLiteralInst(IncValue)))
        std::swap(L, R);

      if (!match(R, m_IntegerLiteralInst(IncValue)))
        return nullptr;
      break;
    }

    case SILInstructionKind::TupleExtractInst: {
      assert(inSCC(cast<TupleExtractInst>(I)->getOperand(), SCC) &&
             "TupleExtract operand not an induction var");
      break;
    }

    default:
      return nullptr;
    }
  }
  if (!FoundBuiltin || !FoundArgument || !IncValue)
    return nullptr;

  InductionInfoMap[FoundArgument] = IVDesc(FoundBuiltin, IncValue);
  return FoundArgument;
}

void IVInfo::visit(SCCType &SCC) {
  assert(SCC.size() && "SCCs should have an element!!");

  SILArgument *IV;
  if (!(IV = isInductionSequence(SCC)))
    return;

  for (auto node : SCC) {
    if (auto value = dyn_cast<ValueBase>(node))
      InductionVariableMap[value] = IV;
  }
}
