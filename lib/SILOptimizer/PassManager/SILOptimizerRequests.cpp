//===--- SILOptimizerRequests.cpp - Requests for SIL Optimization  --------===//
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

#include "swift/AST/SILOptimizerRequests.h"
#include "swift/AST/Evaluator.h"
#include "swift/SILOptimizer/PassManager/PassPipeline.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/Hashing.h"

using namespace swift;

namespace swift {
// Implement the SILOptimizer type zone (zone 13).
#define SWIFT_TYPEID_ZONE SILOptimizer
#define SWIFT_TYPEID_HEADER "swift/AST/SILOptimizerTypeIDZone.def"
#include "swift/Basic/ImplementTypeIDZone.h"
#undef SWIFT_TYPEID_ZONE
#undef SWIFT_TYPEID_HEADER
} // end namespace swift

//----------------------------------------------------------------------------//
// ExecuteSILPipelineRequest computation.
//----------------------------------------------------------------------------//

bool SILPipelineExecutionDescriptor::
operator==(const SILPipelineExecutionDescriptor &other) const {
  return SM == other.SM && Plan == other.Plan &&
         IsMandatory == other.IsMandatory && IRMod == other.IRMod;
}

llvm::hash_code swift::hash_value(const SILPipelineExecutionDescriptor &desc) {
  return llvm::hash_combine(desc.SM, desc.Plan, desc.IsMandatory, desc.IRMod);
}

void swift::simple_display(llvm::raw_ostream &out,
                           const SILPipelineExecutionDescriptor &desc) {
  out << "Run pipelines { ";
  interleave(
      desc.Plan.getPipelines(),
      [&](SILPassPipeline stage) { out << stage.Name; },
      [&]() { out << ", "; });
  out << " } on ";
  simple_display(out, desc.SM);
}

SourceLoc
swift::extractNearestSourceLoc(const SILPipelineExecutionDescriptor &desc) {
  return extractNearestSourceLoc(desc.SM);
}

//----------------------------------------------------------------------------//
// LoweredSILRequest computation.
//----------------------------------------------------------------------------//

std::unique_ptr<SILModule>
LoweredSILRequest::evaluate(Evaluator &evaluator,
                            ASTLoweringDescriptor desc) const {
  auto silMod = llvm::cantFail(evaluator(ASTLoweringRequest{desc}));
  silMod->installSILRemarkStreamer();
  silMod->setSerializeSILAction([]() {});

  runSILDiagnosticPasses(*silMod);

  {
    FrontendStatsTracer tracer(silMod->getASTContext().Stats,
                               "SIL verification, pre-optimization");
    silMod->verify();
  }

  runSILOptimizationPasses(*silMod);

  {
    FrontendStatsTracer tracer(silMod->getASTContext().Stats,
                               "SIL verification, post-optimization");
    silMod->verify();
  }

  runSILLoweringPasses(*silMod);
  return silMod;
}

std::tuple<>
ComputeFieldIndicesRequest::evaluate(Evaluator &evaluator, NominalTypeDecl *decl) const {
  unsigned index = 0;
  if (auto *classDecl = dyn_cast<ClassDecl>(decl)) {
    for (auto *superDecl = classDecl->getSuperclassDecl(); superDecl != nullptr;
         superDecl = superDecl->getSuperclassDecl()) {
      index += superDecl->getStoredProperties().size();
    }
  }
  for (VarDecl *property : decl->getStoredProperties()) {
    assert(property->fieldIndex < 0);
    property->fieldIndex = index++;
  }
  return {};
}

// Define request evaluation functions for each of the SILGen requests.
static AbstractRequestFunction *silOptimizerRequestFunctions[] = {
#define SWIFT_REQUEST(Zone, Name, Sig, Caching, LocOptions)                    \
  reinterpret_cast<AbstractRequestFunction *>(&Name::evaluateRequest),
#include "swift/AST/SILOptimizerTypeIDZone.def"
#undef SWIFT_REQUEST
};

void swift::registerSILOptimizerRequestFunctions(Evaluator &evaluator) {
  evaluator.registerRequestFunctions(Zone::SILOptimizer,
                                     silOptimizerRequestFunctions);
}
