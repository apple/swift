//===--- SILOptimizerRequests.h - SILOptimizer Requests ---------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file defines SILOptimizer requests.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_REQUESTS_H
#define SWIFT_SILOPTIMIZER_REQUESTS_H

#include "swift/AST/ASTTypeIDs.h"
#include "swift/AST/EvaluatorDependencies.h"
#include "swift/AST/SILGenRequests.h"
#include "swift/AST/SimpleRequest.h"

namespace swift {

namespace irgen {
class IRGenModule;
}

class SILModule;
class SILPassPipelinePlan;

struct SILPipelineExecutionDescriptor {
  SILModule *SM;

  // Note that we currently store a reference to the pipeline plan on the stack.
  // If ExecuteSILPipelineRequest ever becomes cached, we will need to adjust
  // this.
  const SILPassPipelinePlan &Plan;
  bool IsMandatory;
  irgen::IRGenModule *IRMod;

  bool operator==(const SILPipelineExecutionDescriptor &other) const;
  bool operator!=(const SILPipelineExecutionDescriptor &other) const {
    return !(*this == other);
  }
};

llvm::hash_code hash_value(const SILPipelineExecutionDescriptor &desc);

/// Executes a SIL pipeline plan on a SIL module.
class ExecuteSILPipelineRequest
    : public SimpleRequest<ExecuteSILPipelineRequest,
                           evaluator::SideEffect(SILPipelineExecutionDescriptor),
                           RequestFlags::Uncached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  evaluator::SideEffect
  evaluate(Evaluator &evaluator, SILPipelineExecutionDescriptor desc) const;
};

void simple_display(llvm::raw_ostream &out,
                    const SILPipelineExecutionDescriptor &desc);

SourceLoc extractNearestSourceLoc(const SILPipelineExecutionDescriptor &desc);

/// Produces lowered SIL from a Swift file or module, ready for IRGen. This runs
/// the diagnostic, optimization, and lowering SIL passes.
class LoweredSILRequest
    : public SimpleRequest<LoweredSILRequest,
                           std::unique_ptr<SILModule>(ASTLoweringDescriptor),
                           RequestFlags::Uncached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  std::unique_ptr<SILModule> evaluate(Evaluator &evaluator,
                                      ASTLoweringDescriptor desc) const;
};

class ComputeFieldIndicesRequest :
    public SimpleRequest<ComputeFieldIndicesRequest,
                         std::tuple<>(NominalTypeDecl *),
                         RequestFlags::Uncached> {
public:
  using SimpleRequest::SimpleRequest;

private:
  friend SimpleRequest;

  // Evaluation.
  std::tuple<> evaluate(Evaluator &evaluator, NominalTypeDecl *decl) const;
};

/// Report that a request of the given kind is being evaluated, so it
/// can be recorded by the stats reporter.
template <typename Request>
void reportEvaluatedRequest(UnifiedStatsReporter &stats,
                            const Request &request);

/// The zone number for SILOptimizer.
#define SWIFT_TYPEID_ZONE SILOptimizer
#define SWIFT_TYPEID_HEADER "swift/AST/SILOptimizerTypeIDZone.def"
#include "swift/Basic/DefineTypeIDZone.h"
#undef SWIFT_TYPEID_ZONE
#undef SWIFT_TYPEID_HEADER

// Set up reporting of evaluated requests.
#define SWIFT_REQUEST(Zone, RequestType, Sig, Caching, LocOptions)             \
template<>                                                                     \
inline void reportEvaluatedRequest(UnifiedStatsReporter &stats,                \
                                   const RequestType &request) {               \
  ++stats.getFrontendCounters().RequestType;                                   \
}
#include "swift/AST/SILOptimizerTypeIDZone.def"
#undef SWIFT_REQUEST

} // end namespace swift

#endif // SWIFT_SILOPTIMIZER_REQUESTS_H
