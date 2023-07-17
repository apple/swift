#ifndef SWIFT_PARTITIONUTILS_H
#define SWIFT_PARTITIONUTILS_H

#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILInstruction.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Debug.h"
#include <algorithm>

namespace swift {

// PartitionOpKind represents the different kinds of PartitionOps that
// SILInstructions can be translated to
enum class PartitionOpKind : uint8_t {
  // Assign one value to the region of another, takes two args, second arg
  // must already be tracked with a non-consumed region
  Assign,

  // Assign one value to a fresh region, takes one arg.
  AssignFresh,

  // Consume the region of a value, takes one arg. Region of arg must be
  // non-consumed before the op.
  Consume,

  // Merge the regions of two values, takes two args, both must be from
  // non-consumed regions.
  Merge,

  // Require the region of a value to be non-consumed, takes one arg.
  Require
};

// PartitionOp represents a primitive operation that can be performed on
// Partitions. This is part of the SendNonSendable SIL pass workflow:
// first SILBasicBlocks are compiled to vectors of PartitionOps, then a fixed
// point partition is found over the CFG.
class PartitionOp {
private:
  PartitionOpKind OpKind;
  llvm::SmallVector<unsigned, 2> OpArgs;

  // Record the SILInstruction that this PartitionOp was generated from, if
  // generated during compilation from a SILBasicBlock
  SILInstruction *sourceInst;

  // TODO: can the following declarations be merged?
  PartitionOp(PartitionOpKind OpKind, unsigned arg1,
              SILInstruction *sourceInst = nullptr)
      : OpKind(OpKind), OpArgs({arg1}), sourceInst(sourceInst) {}

  PartitionOp(PartitionOpKind OpKind, unsigned arg1, unsigned arg2,
              SILInstruction *sourceInst = nullptr)
      : OpKind(OpKind), OpArgs({arg1, arg2}), sourceInst(sourceInst) {}

  friend class Partition;

public:
  static PartitionOp Assign(unsigned tgt, unsigned src,
                            SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::Assign, tgt, src, sourceInst);
  }

  static PartitionOp AssignFresh(unsigned tgt,
                                 SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::AssignFresh, tgt, sourceInst);
  }

  static PartitionOp Consume(unsigned tgt,
                             SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::Consume, tgt, sourceInst);
  }

  static PartitionOp Merge(unsigned tgt1, unsigned tgt2,
                           SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::Merge, tgt1, tgt2, sourceInst);
  }

  static PartitionOp Require(unsigned tgt,
                             SILInstruction *sourceInst = nullptr) {
    return PartitionOp(PartitionOpKind::Require, tgt, sourceInst);
  }

  SILInstruction *getSourceInst() const {
    return sourceInst;
  }

  void dump() const LLVM_ATTRIBUTE_USED {
    raw_ostream &os = llvm::errs();
    switch (OpKind) {
    case PartitionOpKind::Assign:
      os.changeColor(llvm::raw_ostream::CYAN, true);
      os << "assign";
      os.resetColor();
      os << " %%" << OpArgs[0] << " = %%" << OpArgs[1] << "\n";
      break;
    case PartitionOpKind::AssignFresh:
      os.changeColor(llvm::raw_ostream::GREEN, true);
      os << "assign_fresh";
      os.resetColor();
      os << " %%" << OpArgs[0] << "\n";
      break;
    case PartitionOpKind::Consume:
      os.changeColor(llvm::raw_ostream::RED, true);
      os << "consume";
      os.resetColor();
      os << " %%" << OpArgs[0] << "\n";
      break;
    case PartitionOpKind::Merge:
      os.changeColor(llvm::raw_ostream::BLUE, true);
      os << "merge";
      os.resetColor();
      os << " %%" << OpArgs[0] << " with %%" << OpArgs[1] << "\n";
      break;
    case PartitionOpKind::Require:
      os.changeColor(llvm::raw_ostream::YELLOW, true);
      os << "require";
      os.resetColor();
      os << " %%" << OpArgs[0] << "\n";
      break;
    }
  }
};

// For the passed `map`, ensure that `key` maps to `val`. If `key` already
// mapped to a different value, ensure that all other keys mapped to that
// value also now map to `val`. This is a relatively expensive (linear time)
// operation that's unfortunately used pervasively throughout PartitionOp
// application. If this is a performance bottleneck, let's consider optimizing
// it to a true union-find or other tree-based data structure.
static void horizontalUpdate(std::map<unsigned, signed> &map, unsigned key,
                             signed val) {
  if (!map.count(key)) {
    map[key] = val;
    return;
  }

  signed oldVal = map[key];
  if (val == oldVal) return;

  for (auto [otherKey, otherVal] : map)
    if (otherVal == oldVal)
      map[otherKey] = val;
}

class Partition {
private:
  // Label each index with a non-negative (unsigned) label if it is associated
  // with a valid region, and with -1 if it is associated with a consumed region
  // in-order traversal relied upon.
  std::map<unsigned, signed> labels;

  // Track a label that is guaranteed to be strictly larger than all in use,
  // and therefore safe for use as a fresh label.
  unsigned fresh_label = 0;

  // In a canonical partition, all regions are labelled with the smallest index
  // of any member. Certain operations like join and equals rely on canonicality
  // so when it's invalidated this boolean tracks that, and it must be
  // reestablished by a call to canonicalize().
  bool canonical;

  // Used only in assertions, check that Partitions promised to be canonical
  // are actually canonical
  bool is_canonical_correct() {
    if (!canonical) return true; // vacuously correct

    auto fail = [&](unsigned i, int type) {
      llvm::dbgs() << "FAIL(i=" << i << "; type=" << type << "): ";
      dump();
      return false;
    };

    for (auto &[i, label] : labels) {
      // correctness vacuous at consumed indices
      if (label < 0) continue;

      // this label should not exceed fresh_label
      if ((unsigned) label >= fresh_label) return fail(i, 0);

      // the label of a region should be at most as large as each index in it
      if ((unsigned) label > i) return fail(i, 1);

      // each region label should refer to an index in that region
      if (labels[label] != label) return fail(i, 2);
    }

    return true;
  }

  // linear time - For each region label that occurs, find the first index
  // at which it occurs and relabel all instances of it to that index.
  // This excludes the -1 label for consumed regions.
  void canonicalize() {
    if (canonical)
      return;
    canonical = true;

    std::map<signed, unsigned> relabel;

    // relies on in-order traversal of labels
    for (auto &[i, label] : labels) {
      // leave -1 (consumed region) as is
      if (label < 0)
        continue;

      if (!relabel.count(label)) {
        // if this is the first time encountering this region label,
        // then this region label should be relabelled to this index,
        // so enter that into the map
        relabel[label] = i;
      }

      // update this label with either its own index, or a prior index that
      // shared a region with it
      label = relabel[label];

      // the maximum index iterated over will be used here to appropriately
      // set fresh_label
      fresh_label = i + 1;
    }

    assert(is_canonical_correct());
  }

  // linear time - merge the regions of two indices, maintaining canonicality
  void merge(unsigned fst, unsigned snd) {
    assert(labels.count(fst) && labels.count(snd));
    if (labels[fst] == labels[snd])
      return;

    // maintain canonicality by renaming the greater-numbered region
    if (labels[fst] < labels[snd])
      horizontalUpdate(labels, snd, labels[fst]);
    else
      horizontalUpdate(labels, fst, labels[snd]);

    assert(is_canonical_correct());
  }

public:
  Partition() : labels({}), canonical(true) {}

  // 1-arg constructor used when canonicality will be immediately invalidated,
  // so set to false to begin with
  Partition(bool canonical) : labels({}), canonical(canonical) {}

  static Partition singleRegion(std::vector<unsigned> indices) {
    Partition p;
    if (!indices.empty()) {
      unsigned min_index = *std::min_element(indices.begin(), indices.end());
      p.fresh_label = min_index + 1;
      for (unsigned index : indices) {
        p.labels[index] = min_index;
      }
    }

    assert(p.is_canonical_correct());
    return p;
  }

  // linear time - Test two partititons for equality by first putting them
  // in canonical form then comparing for exact equality.
  static bool equals(Partition &fst, Partition &snd) {
    fst.canonicalize();
    snd.canonicalize();

    return fst.labels == snd.labels;
  }

  // quadratic time - Construct the partition corresponding to the join of the
  // two passed partitions; the join labels each index labelled by both operands
  // and two indices are in the same region of the join iff they are in the same
  // region in either operand.
  static Partition join(Partition &fst, Partition &snd) {
    //ensure copies are made
    Partition fst_reduced = false;
    Partition snd_reduced = false;

    // make canonical copies of fst and snd, reduced to their intersected domain
    for (auto [i, _] : fst.labels)
      if (snd.labels.count(i)) {
        fst_reduced.labels[i] = fst.labels[i];
        snd_reduced.labels[i] = snd.labels[i];
      }
    fst_reduced.canonicalize();
    snd_reduced.canonicalize();

    // merging each index in fst with its label in snd ensures that all pairs
    // of indices that are in the same region in snd are also in the same region
    // in fst - the desired property
    for (const auto [i, snd_label] : snd_reduced.labels) {
      if (snd_label < 0)
        // if snd says that the region has been consumed, mark it consumed in fst
        horizontalUpdate(fst_reduced.labels, i, -1);
      else
        fst_reduced.merge(i, snd_label);
    }

    assert(fst_reduced.is_canonical_correct());

    // fst_reduced is now the join
    return fst_reduced;
  }

  // Apply the passed PartitionOp to this partition, performing its action.
  // A `handleFailure` closure can optionally be passed in that will be called
  // if a consumed region is required. The closure is given the PartitionOp that
  // failed, and the index of the SIL value that was required but consumed.
  // Additionally, a list of "nonconsumable" indices can be passed in along with
  // a handleConsumeNonConsumable closure. In the event that a region containing
  // one of the nonconsumable indices is consumed, the closure will be called
  // with the offending Consume.
  void apply(
      PartitionOp op,
      llvm::function_ref<void(const PartitionOp&, unsigned)>
      handleFailure = [](const PartitionOp&, unsigned) {},

      std::vector<unsigned>
          nonconsumables = {},

      llvm::function_ref<void(const PartitionOp&, unsigned)>
      handleConsumeNonConsumable = [](const PartitionOp&, unsigned) {},

      bool reviveAfterFailure = false
  ) {
    auto handleFailureAndRevive =
        [&](const PartitionOp& partitionOp, unsigned consumedVal) {
          if (reviveAfterFailure)
            horizontalUpdate(labels, consumedVal, fresh_label++);
          handleFailure(partitionOp, consumedVal);
        };
    switch (op.OpKind) {
    case PartitionOpKind::Assign:
      assert(op.OpArgs.size() == 2 &&
             "Assign PartitionOp should be passed 2 arguments");
      assert(labels.count(op.OpArgs[1]) &&
             "Assign PartitionOp's source argument should be already tracked");
      // if assigning to a missing region, handle the failure
      if (labels[op.OpArgs[1]] < 0)
        handleFailureAndRevive(op, op.OpArgs[1]);

      labels[op.OpArgs[0]] = labels[op.OpArgs[1]];

      // assignment could have invalidated canonicality of either the old region
      // of op.OpArgs[0] or the region of op.OpArgs[1], or both
      canonical = false;
      break;
    case PartitionOpKind::AssignFresh:
      assert(op.OpArgs.size() == 1 &&
             "AssignFresh PartitionOp should be passed 1 argument");

      // map index op.OpArgs[0] to a fresh label
      labels[op.OpArgs[0]] = fresh_label++;
      canonical = false;
      break;
    case PartitionOpKind::Consume:
      assert(op.OpArgs.size() == 1 &&
             "Consume PartitionOp should be passed 1 argument");
      assert(labels.count(op.OpArgs[0]) &&
             "Consume PartitionOp's argument should already be tracked");

      // if attempting to consume a consumed region, handle the failure
      if (labels[op.OpArgs[0]] < 0)
        handleFailureAndRevive(op, op.OpArgs[0]);

      // mark region as consumed
      horizontalUpdate(labels, op.OpArgs[0], -1);

      // check if any nonconsumables were consumed, and handle the failure if so
      for (unsigned nonconsumable : nonconsumables) {
        assert(labels.count(nonconsumable) &&
               "nonconsumables should be function args and self, and therefore"
               "always present in the label map because of initialization at "
               "entry");
        if (labels[nonconsumable] < 0) {
          handleConsumeNonConsumable(op, nonconsumable);
          break;
        }
      }

      break;
    case PartitionOpKind::Merge:
      assert(op.OpArgs.size() == 2 &&
             "Merge PartitionOp should be passed 2 arguments");
      assert(labels.count(op.OpArgs[0]) && labels.count(op.OpArgs[1]) &&
             "Merge PartitionOp's arguments should already be tracked");

      // if attempting to merge a consumed region, handle the failure
      if (labels[op.OpArgs[0]] < 0)
        handleFailureAndRevive(op, op.OpArgs[0]);
      if (labels[op.OpArgs[1]] < 0)
        handleFailureAndRevive(op, op.OpArgs[1]);

      merge(op.OpArgs[0], op.OpArgs[1]);
      break;
    case PartitionOpKind::Require:
      assert(op.OpArgs.size() == 1 &&
             "Require PartitionOp should be passed 1 argument");
      assert(labels.count(op.OpArgs[0]) &&
             "Require PartitionOp's argument should already be tracked");
      if (labels[op.OpArgs[0]] < 0)
        handleFailureAndRevive(op, op.OpArgs[0]);
    }

    assert(is_canonical_correct());
  }

  void dump_labels() const LLVM_ATTRIBUTE_USED {
    llvm::dbgs() << "Partition";
    if (canonical)
      llvm::dbgs() << "(canonical)";
    llvm::dbgs() << "(fresh=" << fresh_label << "){";
    for (const auto &[i, label] : labels)
      llvm::dbgs() << "[" << i << ": " << label << "] ";
    llvm::dbgs() << "}\n";
  }

  void dump() LLVM_ATTRIBUTE_USED {
    std::map<signed, std::vector<unsigned>> buckets;

    for (auto [i, label] : labels) {
      buckets[label].push_back(i);
    }

    llvm::dbgs() << "[";
    for (auto [label, indices] : buckets) {
      llvm::dbgs() << (label < 0 ? "{" : "(");
      int j = 0;
      for (unsigned i : indices) {
        llvm::dbgs() << (j++? " " : "") << i;
      }
      llvm::dbgs() << (label < 0 ? "}" : ")");
    }
    llvm::dbgs() << "]";
  }
};
}

#endif
