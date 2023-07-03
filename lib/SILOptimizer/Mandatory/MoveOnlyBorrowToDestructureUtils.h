//===--- MoveOnlyBorrowToDestructure.h ------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_MANDATORY_MOVEONLYBORROWTODESTRUCTURE_H
#define SWIFT_SILOPTIMIZER_MANDATORY_MOVEONLYBORROWTODESTRUCTURE_H

#include "swift/Basic/FrozenMultiMap.h"
#include "swift/SIL/FieldSensitivePrunedLiveness.h"
#include "swift/SIL/StackList.h"
#include "swift/SILOptimizer/Analysis/PostOrderAnalysis.h"

#include "llvm/ADT/IntervalMap.h"

namespace swift {
namespace siloptimizer {

class DiagnosticEmitter;

namespace borrowtodestructure {

class IntervalMapAllocator {
public:
  using Map = llvm::IntervalMap<
      unsigned, SILValue,
      llvm::IntervalMapImpl::NodeSizer<unsigned, SILValue>::LeafSize,
      llvm::IntervalMapHalfOpenInfo<unsigned>>;

  using Allocator = Map::Allocator;

private:
  /// Lazily initialized allocator.
  llvm::Optional<Allocator> allocator;

public:
  Allocator &get() {
    if (!allocator)
      allocator.emplace();
    return *allocator;
  }
};

struct Implementation;

} // namespace borrowtodestructure

class BorrowToDestructureTransform {
  friend borrowtodestructure::Implementation;

  using IntervalMapAllocator = borrowtodestructure::IntervalMapAllocator;

  IntervalMapAllocator &allocator;
  MarkMustCheckInst *mmci;
  SILValue rootValue;
  DiagnosticEmitter &diagnosticEmitter;
  PostOrderAnalysis *poa;
  PostOrderFunctionInfo *pofi = nullptr;
  SmallVector<SILInstruction *, 8> createdDestructures;
  SmallVector<SILPhiArgument *, 8> createdPhiArguments;

public:
  BorrowToDestructureTransform(IntervalMapAllocator &allocator,
                               MarkMustCheckInst *mmci, SILValue rootValue,
                               DiagnosticEmitter &diagnosticEmitter,
                               PostOrderAnalysis *poa)
      : allocator(allocator), mmci(mmci), rootValue(rootValue),
        diagnosticEmitter(diagnosticEmitter), poa(poa) {}

  bool transform();

private:
  PostOrderFunctionInfo *getPostOrderFunctionInfo() {
    if (!pofi)
      pofi = poa->get(mmci->getFunction());
    return pofi;
  }
};

} // namespace siloptimizer
} // namespace swift

#endif
