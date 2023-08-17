//===--- PassPipeline.h -----------------------------------------*- C++ -*-===//
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
///
/// \file
///
/// This file defines the SILPassPipelinePlan and SILPassPipeline
/// classes. These are higher level representations of sequences of SILPasses
/// and the run behavior of these sequences (i.e. run one, until fixed point,
/// etc). This makes it easy to serialize and deserialize pipelines without work
/// on the part of the user. Eventually this will be paired with a gyb based
/// representation of Passes.def that will able to be used to generate a python
/// based script builder.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_PASSMANAGER_PASSPIPELINE_H
#define SWIFT_SILOPTIMIZER_PASSMANAGER_PASSPIPELINE_H

#include "swift/Basic/LLVM.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "llvm/ADT/Hashing.h"
#include <vector>

namespace swift {

class SILPassPipelinePlan;

struct SILPassPipeline final {
  unsigned ID;
  StringRef Name;
  unsigned KindOffset;
  bool isFunctionPassPipeline;

  friend bool operator==(const SILPassPipeline &lhs,
                         const SILPassPipeline &rhs) {
    return lhs.ID == rhs.ID && lhs.Name.equals(rhs.Name) &&
           lhs.KindOffset == rhs.KindOffset;
  }

  friend bool operator!=(const SILPassPipeline &lhs,
                         const SILPassPipeline &rhs) {
    return !(lhs == rhs);
  }

  friend llvm::hash_code hash_value(const SILPassPipeline &pipeline) {
    return llvm::hash_combine(pipeline.ID, pipeline.Name, pipeline.KindOffset);
  }
};

enum class PassPipelineKind {
#define PASSPIPELINE(NAME, DESCRIPTION) NAME,
#include "swift/SILOptimizer/PassManager/PassPipeline.def"
};

class SILPassPipelinePlan final {
  const SILOptions &Options;
  std::vector<PassKind> Kinds;
  std::vector<SILPassPipeline> PipelineStages;

public:
  SILPassPipelinePlan(const SILOptions &Options)
      : Options(Options), Kinds(), PipelineStages() {}

  ~SILPassPipelinePlan() = default;
  SILPassPipelinePlan(const SILPassPipelinePlan &) = default;

  const SILOptions &getOptions() const { return Options; }

// Each pass gets its own add-function.
#define PASS(ID, TAG, NAME)                                                    \
  void add##ID() {                                                             \
    assert(!PipelineStages.empty() && "startPipeline before adding passes.");  \
    Kinds.push_back(PassKind::ID);                                             \
  }
#include "swift/SILOptimizer/PassManager/Passes.def"

  void addPasses(ArrayRef<PassKind> PassKinds);

#define PASSPIPELINE(NAME, DESCRIPTION)                                        \
  static SILPassPipelinePlan get##NAME##PassPipeline(const SILOptions &Options);
#include "swift/SILOptimizer/PassManager/PassPipeline.def"

  static SILPassPipelinePlan getPassPipelineForKinds(const SILOptions &Options,
                                                     ArrayRef<PassKind> Kinds);
  static SILPassPipelinePlan getPassPipelineFromFile(const SILOptions &Options,
                                                     StringRef Filename);

  /// Our general format is as follows:
  ///
  ///   [
  ///     [
  ///       "PASS_MANAGER_ID",
  ///       "one_iteration"|"until_fix_point",
  ///       "PASS1", "PASS2", ...
  ///     ],
  ///   ...
  ///   ]
  void dump();

  void print(llvm::raw_ostream &os);

  void startPipeline(StringRef Name = "", bool isFunctionPassPipeline = false);
  using PipelineKindIterator = decltype(Kinds)::const_iterator;
  using PipelineKindRange = iterator_range<PipelineKindIterator>;
  iterator_range<PipelineKindIterator>
  getPipelinePasses(const SILPassPipeline &P) const;

  using PipelineIterator = decltype(PipelineStages)::const_iterator;
  using PipelineRange = iterator_range<PipelineIterator>;
  PipelineRange getPipelines() const {
    return {PipelineStages.begin(), PipelineStages.end()};
  }

  friend bool operator==(const SILPassPipelinePlan &lhs,
                         const SILPassPipelinePlan &rhs) {
    // FIXME: Comparing the SILOptions by identity should work in practice, but
    // we don't currently prevent them from being copied. We should consider
    // either making them move-only, or properly implementing == for them.
    return &lhs.Options == &rhs.Options && lhs.Kinds == rhs.Kinds &&
           lhs.PipelineStages == rhs.PipelineStages;
  }

  friend bool operator!=(const SILPassPipelinePlan &lhs,
                         const SILPassPipelinePlan &rhs) {
    return !(lhs == rhs);
  }

  friend llvm::hash_code hash_value(const SILPassPipelinePlan &plan) {
    using namespace llvm;
    auto &kinds = plan.Kinds;
    auto &stages = plan.PipelineStages;
    return hash_combine(&plan.Options,
                        hash_combine_range(kinds.begin(), kinds.end()),
                        hash_combine_range(stages.begin(), stages.end()));
  }
};

inline void SILPassPipelinePlan::
startPipeline(StringRef Name, bool isFunctionPassPipeline) {
  PipelineStages.push_back(SILPassPipeline{
      unsigned(PipelineStages.size()), Name, unsigned(Kinds.size()),
      isFunctionPassPipeline});
}

inline SILPassPipelinePlan::PipelineKindRange
SILPassPipelinePlan::getPipelinePasses(const SILPassPipeline &P) const {
  unsigned ID = P.ID;
  assert(PipelineStages.size() > ID && "Pipeline with ID greater than the "
                                       "size of its container?!");

  // In this case, we are the last pipeline. Return end and the kind offset.
  if ((PipelineStages.size() - 1) == ID) {
    return {std::next(Kinds.begin(), P.KindOffset), Kinds.end()};
  }

  // Otherwise, end is the beginning of the next PipelineStage.
  return {std::next(Kinds.begin(), P.KindOffset),
          std::next(Kinds.begin(), PipelineStages[ID + 1].KindOffset)};
}

} // end namespace swift

#endif
