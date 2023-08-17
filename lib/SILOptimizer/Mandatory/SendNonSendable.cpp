#include "../../Sema/TypeCheckConcurrency.h"
#include "../../Sema/TypeChecker.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Type.h"
#include "swift/SIL/BasicBlockData.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/PartitionUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "send-non-sendable"

using namespace swift;

namespace {

static const char *SEP_STR = "╾──────────────────────────────╼\n";

// SILApplyCrossesIsolation determines if a SIL instruction is an isolation
// crossing apply expression. This is done by checking its correspondence
// to an ApplyExpr AST node, and then checking the internal flags of that
// AST node to see if the ActorIsolationChecker determined it crossed isolation.
// It's possible this is brittle and a more nuanced check is needed, but this
// suffices for all cases tested so far.
static bool SILApplyCrossesIsolation(const SILInstruction *inst) {
  if (ApplyExpr *apply = inst->getLoc().getAsASTNode<ApplyExpr>())
    return apply->getIsolationCrossing().has_value();

  // if the instruction doesn't correspond to an ApplyExpr, then it can't
  // cross an isolation domain
  return false;
}

inline bool isAddress(SILValue val) {
  return val->getType() && val->getType().isAddress();
}

inline bool isApplyInst(const SILInstruction &inst) {
  return isa<ApplyInst,
             BeginApplyInst,
             BuiltinInst,
             PartialApplyInst,
             TryApplyInst>(inst);
}

using TrackableValueID = PartitionPrimitives::Element;
using Region = PartitionPrimitives::Region;

// PartitionOpTranslator is responsible for performing the translation from
// SILInstructions to PartitionOps. Not all SILInstructions have an effect on
// the region partition, and some have multiple effects - such as an application
// pairwise merging its arguments - so the core functions like
// translateSILBasicBlock map SILInstructions to std::vectors of PartitionOps.
// No more than a single instance of PartitionOpTranslator should be used for
// each SILFunction, as SILValues are assigned unique IDs through the nodeIDMap.
// Some special correspondences between SIL values are also tracked statefully
// by instances of this class, such as the "projection" relationship:
// instructions like begin_borrow and begin_access create effectively temporary
// values used for alternative access to base "projected" values. These are
// tracked to implement "write-through" semantics for assignments to projections
// when they're addresses.
//
// TODO: when translating basic blocks, optimizations might be possible
//       that reduce lists of PartitionOps to smaller, equivalent lists
class PartitionOpTranslator {
  SILFunction *function;
  ProtocolDecl *sendableProtocol;

  class TrackableSILValue {
    bool aliased = true;
    bool sendable = false;
    SILValue value;

    AccessStorage getAccessStorageFromAddr(SILValue val) {
      assert(isAddress(val));
      auto accessStorage = AccessStorage::compute(val);
      if (accessStorage && accessStorage.getRoot()) {
        auto definingInst = accessStorage.getRoot().getDefiningInstruction();
        if (definingInst &&
            isa<InitExistentialAddrInst, CopyValueInst>(definingInst))
          // look through these because AccessStorage does not
          return getAccessStorageFromAddr(definingInst->getOperand(0));
      }
      return accessStorage;
    }

    // simplifyVal reduces an address-typed SILValue to the root SILValue
    // that it was derived from, reducing the set of values that must be
    // reasoned about by rendering two values that are projections/aliases the
    // same.
    SILValue simplifyVal(SILValue val) {
      if (!isAddress(val))
        //TODO: consider adding more reduction for non-address values
        return getUnderlyingObject(val);
      if (auto accessStorage = getAccessStorageFromAddr(val)) {
        if (accessStorage.getKind() == AccessRepresentation::Kind::Global)
          // globals don't reduce
          return val;
        assert(accessStorage.getRoot());
        return accessStorage.getRoot();
      }
      return val;
    }

  public:
    TrackableSILValue(const PartitionOpTranslator *translator,
                      SILValue value_in) :
        value(simplifyVal(value_in)) {

      // set `aliased` appropriately
      if (isAddress(value))
        if (auto accessStorage = AccessStorage::compute(value))
          aliased = !accessStorage.isUniquelyIdentified() ||
                    translator->capturedUIValues.count(*this);

      // set `sendable` appropriately
      SILInstruction *defInst = value.getDefiningInstruction();
      if (defInst && isa<ClassMethodInst, FunctionRefInst>(defInst)) {
        // though these values are technically non-Sendable, we can safely
        // and consistently treat them as Sendable
        sendable = true;
      } else {
        sendable = !translator->isNonSendableType(value->getType());
      }
    }
    inline bool isAliased() const {
      return aliased;
    }

    inline bool isUnaliased() const {
      return !aliased;
    }

    inline bool isSendable() const {
      return sendable;
    }

    inline bool isNonSendable() const {
      return !sendable;
    }

    SILValue getValue() const {
      return value;
    }

    // comparison operates lookthrough to the underlying SILValue
    bool operator==(const TrackableSILValue &other) const {
      return value == other.value;
    }
    bool operator<(const TrackableSILValue &other) const {
        return value < other.value;
    }

    void dump() const {
        llvm::dbgs() << "TrackableSILValue[aliased="
                     << aliased << "][sendable=" << sendable << "]: ";
        value->dump();
    }
  };

  llvm::Optional<TrackableSILValue> trackIfNonSendable(SILValue value) const {
    auto trackedVal = TrackableSILValue(this, value);
    if (trackedVal.isNonSendable()) {
        return trackedVal;
    }
    return {};
  }

  // nodeIDMap stores unique IDs for all SILNodes corresponding to
  // non-Sendable values. Implicit conversion from SILValue used pervasively.
  // ensure simplifyVal is called on SILValues before entering into this map
  std::map<TrackableSILValue, TrackableValueID> trackableValueIDs;
  unsigned nextID = 0;

  // some values that AccessStorage claims are uniquely identified are still
  // captured (e.g. in a closure). This set is initialized upon
  // PartitionOpTranslator construction to store those values.
  // ensure simplifyVal is called on SILValues before entering into this map
  //
  // TODO: we could remember not just which values fit this description,
  //       but at what points in function flow they do, this would be more
  //       permissive, but I'm avoiding implementing it in case existing
  //       utilities would make it easier than handrolling
  std::set<TrackableSILValue> capturedUIValues;

  void initCapturedUIValues() {
    for (const SILBasicBlock &block: *function) {
      for (const SILInstruction &inst: block) {
        if (isApplyInst(inst)) {
          // add all nonsendable, uniquely identified arguments to applications
          // to capturedUIValues, because applications capture them
          for (SILValue val : inst.getOperandValues()) {
            auto trackVal = TrackableSILValue(this, val);
            if (trackVal.isNonSendable() && trackVal.isUnaliased())
              capturedUIValues.insert(trackVal);
          }
        }
      }
    }
  }

public:
  // create a new PartitionOpTranslator, all that's needed is the underlying
  // SIL function
  PartitionOpTranslator(SILFunction *function) :
      function(function),
      sendableProtocol(function->getASTContext()
                           .getProtocol(KnownProtocolKind::Sendable)) {
    assert(sendableProtocol && "PartitionOpTranslators should only be created "
                               "in contexts in which the availability of the "
                               "Sendable protocol has already been checked.");
    initCapturedUIValues();
    LLVM_DEBUG(
      llvm::dbgs() << "Captured Uniquely Identified addresses for "
                     << function->getName() << ":\n";
      for (TrackableSILValue val : capturedUIValues)
            val.getValue()->dump();
    );
  }

private:
  bool valueHasID(TrackableSILValue value,
                  bool dumpIfHasNoID = false) {
    assert(value.isNonSendable() &&
           "only non-Sendable values should be entered in the map");
    bool hasID = trackableValueIDs.count(value);
    if (!hasID && dumpIfHasNoID) {
      llvm::dbgs() << "FAILURE: valueHasID of ";
      value.dump();
    }
    return hasID;
  }

  // lookup the internally assigned unique ID of a SILValue, or create one
  TrackableValueID lookupValueID(TrackableSILValue value) {
    assert(value.isNonSendable() &&
           "only non-Sendable values should be entered in the map");
    if (trackableValueIDs.count(value)) {
      return trackableValueIDs.at(value);
    }
    trackableValueIDs.insert({value, TrackableValueID(nextID)});
    return TrackableValueID(nextID++);
  }

  // check the passed type for sendability, special casing the type used for
  // raw pointers to ensure it is treated as non-Sendable and strict checking
  // is applied to it
  bool isNonSendableType(SILType type) const {
    switch (type.getASTType()->getKind()) {
    case TypeKind::BuiltinNativeObject:
    case TypeKind::BuiltinRawPointer:
      // these are very unsafe... definitely not Sendable
      return true;
    default:
      // consider caching this if it's a performance bottleneck
      return TypeChecker::conformsToProtocol(
                 type.getASTType(), sendableProtocol, function->getParentModule())
          .hasMissingConformance(function->getParentModule());
    }
  }

  // used to statefully track the instruction currently being translated,
  // for insertion into generated PartitionOps
  SILInstruction *currentInstruction;

  // ===========================================================================
  // The following section of functions create fresh PartitionOps referencing
  // the current value of currentInstruction for ease of programming.

  std::vector<PartitionOp> AssignFresh(TrackableSILValue value) {
    return {PartitionOp::AssignFresh(
        lookupValueID(value),
        currentInstruction)};
  }

  std::vector<PartitionOp> Assign(TrackableSILValue tgt, TrackableSILValue src) {
    assert(valueHasID(src, /*dumpIfHasNoID=*/true) &&
           "source value of assignment should already have been encountered");

    if (lookupValueID(tgt) == lookupValueID(src))
      return {}; //noop

    return {PartitionOp::Assign(
        lookupValueID(tgt),
        lookupValueID(src),
        currentInstruction)};
  }

  std::vector<PartitionOp> Consume(
      TrackableSILValue value,
      Expr *sourceExpr = nullptr) {
    assert(valueHasID(value) &&
           "consumed value should already have been encountered");

    return {PartitionOp::Consume(
        lookupValueID(value),
        currentInstruction,
        sourceExpr)};
  }

  std::vector<PartitionOp> Merge(TrackableSILValue fst, TrackableSILValue snd) {
    assert(valueHasID(fst, /*dumpIfHasNoID=*/true) && valueHasID(snd, /*dumpIfHasNoID=*/true) &&
           "merged values should already have been encountered");

    if (lookupValueID(fst) == lookupValueID(snd))
      return {}; //noop

    return {PartitionOp::Merge(lookupValueID(fst), lookupValueID(snd),
                              currentInstruction)};
  }

  std::vector<PartitionOp> Require(TrackableSILValue value) {
    assert(valueHasID(value, /*dumpIfHasNoID=*/true) &&
           "required value should already have been encountered");
    return {PartitionOp::Require(
        lookupValueID(value),
        currentInstruction)};
  }
  // ===========================================================================

  // Get the vector of IDs corresponding to the arguments to the underlying
  // function, and the self parameter if there is one.
  std::vector<TrackableValueID> getArgIDs() {
    std::vector<TrackableValueID> argIDs;

    for (SILArgument *arg : function->getArguments())
      if (auto trackArg = trackIfNonSendable(arg))
        argIDs.push_back(lookupValueID(trackArg.value()));

    if (function->hasSelfParam())
      if (auto trackSelf = trackIfNonSendable(function->getSelfArgument()))
          argIDs.push_back(lookupValueID(trackSelf.value()));

    return argIDs;
  }

public:
  // Create a partition that places all arguments from this function,
  // including self if available, into the same region, ensuring those
  // arguments get IDs in doing so. This Partition will be used as the
  // entry point for the full partition analysis.
  Partition getEntryPartition() {
    return Partition::singleRegion(getArgIDs());
  }

  // Get the vector of IDs that cannot be legally consumed at any point in
  // this function. Since we place all args and self in a single region right
  // now, it is only necessary to choose a single representative of the set.
  std::vector<TrackableValueID> getNonConsumables() {
    if (const auto &argIDs = getArgIDs(); !argIDs.empty()) {
      return {argIDs.front()};
    }
    return {};
  }

  // get the results of an apply instruction. This is the single result value
  // for most apply instructions, but for try apply it is the two arguments
  // to each succ block
  std::vector<SILValue> getApplyResults(const SILInstruction *inst) {
    if (isa<ApplyInst, BeginApplyInst, BuiltinInst, PartialApplyInst>(inst))
      return {inst->getResults().begin(), inst->getResults().end()};
    if (auto tryApplyInst = dyn_cast<TryApplyInst>(inst))
      return {tryApplyInst->getNormalBB()->getArgument(0),
               tryApplyInst->getErrorBB()->getArgument(0)};
    llvm_unreachable("all apply instructions should be covered");
    return {};
  }

  // ===========================================================================
  // The following section of functions wrap the more primitive Assign, Require,
  // Merge, etc functions that generate PartitionOps with more logic common to
  // the translations from source-level SILInstructions.

  // require all non-sendable sources, merge their regions, and assign the
  // resulting region to all non-sendable targets, or assign non-sendable
  // targets to a fresh region if there are no non-sendable sources
  std::vector<PartitionOp> translateSILMultiAssign(
      std::vector<SILValue> tgts, std::vector<SILValue> srcs) {

    std::vector<TrackableSILValue> nonSendableSrcs;
    std::vector<TrackableSILValue> nonSendableTgts;

    for (SILValue src : srcs)
      if (auto trackSrc = trackIfNonSendable(src))
          nonSendableSrcs.push_back(trackSrc.value());

    for (SILValue tgt : tgts)
      if (auto trackTgt = trackIfNonSendable(tgt))
          nonSendableTgts.push_back(trackTgt.value());

    std::vector<PartitionOp> translated;
    auto add_to_translation = [&](std::vector<PartitionOp> ops) {
      for (auto op : ops) translated.push_back(op);
    };

    // require all srcs
    for (auto src : nonSendableSrcs)
      add_to_translation(Require(src));

    // merge all srcs
    for (unsigned i = 1; i < nonSendableSrcs.size(); i++) {
      add_to_translation(Merge(nonSendableSrcs.at(i-1),
                               nonSendableSrcs.at(i)));
    }

    // if no non-sendable tgts, return at this point
    if (nonSendableTgts.empty()) return translated;

    if (nonSendableSrcs.empty()) {
      // if no non-sendable srcs, non-sendable tgts get a fresh region
      add_to_translation(AssignFresh(nonSendableTgts.front()));
    } else {
      add_to_translation(Assign(nonSendableTgts.front(),
                                nonSendableSrcs.front()));
    }

    // assign all targets to the target region
    for (unsigned i = 1; i < nonSendableTgts.size(); i++) {
      add_to_translation(Assign(nonSendableTgts.at(i),
                                nonSendableTgts.front()));
    }

    return translated;
  }

  std::vector<PartitionOp> translateSILApply(SILInstruction *applyInst) {
    // if this apply does not cross isolation domains, it has normal,
    // non-consuming multi-assignment semantics
    if (!SILApplyCrossesIsolation(applyInst))
      return translateSILMultiAssign(
          getApplyResults(applyInst),
          {applyInst->getOperandValues().begin(),
           applyInst->getOperandValues().end()}
      );

    if (auto cast = dyn_cast<ApplyInst>(applyInst))
      return translateIsolationCrossingSILApply(cast);
    if (auto cast = dyn_cast<BeginApplyInst>(applyInst))
      return translateIsolationCrossingSILApply(cast);
    if (auto cast = dyn_cast<TryApplyInst>(applyInst))
      return translateIsolationCrossingSILApply(cast);

    llvm_unreachable("Only ApplyInst, BeginApplyInst, and TryApplyInst should cross isolation domains");
  }

  // handles the semantics for SIL applies that cross isolation
  // in particular, all arguments are consumed
  std::vector<PartitionOp> translateIsolationCrossingSILApply(
     ApplySite applySite) {
    ApplyExpr *sourceApply = applySite.getLoc().getAsASTNode<ApplyExpr>();
    assert(sourceApply && "only ApplyExpr's should cross isolation domains");

    std::vector<PartitionOp> translated;

    // require all operands
    for (auto op : applySite->getOperandValues())
      if (auto trackOp = trackIfNonSendable(op))
        translated.push_back(Require(trackOp.value()).front());

    auto getSourceArg = [&](unsigned i) {
      if (i < sourceApply->getArgs()->size())
        return sourceApply->getArgs()->getExpr(i);
      return (Expr *)nullptr;
    };

    auto getSourceSelf = [&]() {
      if (auto callExpr = dyn_cast<CallExpr>(sourceApply))
        if (auto calledExpr = dyn_cast<DotSyntaxCallExpr>(callExpr->getDirectCallee()))
          return calledExpr->getBase();
      return (Expr *)nullptr;
    };

    auto handleSILOperands = [&](OperandValueArrayRef ops) {
      int argNum = 0;
      for (auto arg : ops) {
        if (auto trackArg = trackIfNonSendable(arg))
          translated.push_back(
              Consume(trackArg.value(), getSourceArg(argNum)).front());
        argNum++;
      }
    };

    auto handleSILSelf = [&](SILValue self) {
      if (auto trackSelf = trackIfNonSendable(self))
        translated.push_back(
            Consume(trackSelf.value(), getSourceSelf()).front());
    };

    if (applySite.hasSelfArgument()) {
      handleSILOperands(applySite.getArgumentsWithoutSelf());
      handleSILSelf(applySite.getSelfArgument());
    } else {
      handleSILOperands(applySite.getArguments());
    }

    // non-sendable results can't be returned from cross-isolation calls without
    // a diagnostic emitted elsewhere. Here, give them a fresh value for better
    // diagnostics hereafter
    for (auto result : getApplyResults(*applySite))
      if (auto trackResult = trackIfNonSendable(result))
        translated.push_back(AssignFresh(trackResult.value()).front());

    return translated;
  }

  std::vector<PartitionOp> translateSILAssign(SILValue tgt, SILValue src) {
    return translateSILMultiAssign({tgt}, {src});
  }

  // If the passed SILValue is NonSendable, then create a fresh region for it,
  // otherwise do nothing.
  std::vector<PartitionOp> translateSILAssignFresh(SILValue val) {
    return translateSILMultiAssign({val}, {});
  }

  std::vector<PartitionOp> translateSILMerge(SILValue fst, SILValue snd) {
    auto nonSendableFst = trackIfNonSendable(fst);
    auto nonSendableSnd = trackIfNonSendable(snd);
    if (nonSendableFst && nonSendableSnd)
      return Merge(nonSendableFst.value(), nonSendableSnd.value());
    return {};
  }

  // if the tgt is known to be unaliased (computed thropugh a combination
  // of AccessStorage's inUniquelyIdenfitied check and a custom search for
  // captures by applications), then these can be treated as assignments
  // of tgt to src. If the tgt could be aliased, then we must instead treat
  // them as merges, to ensure any aliases of tgt are also updated.
  std::vector<PartitionOp> translateSILStore(SILValue tgt, SILValue src) {
    if (auto nonSendableTgt = trackIfNonSendable(tgt)) {

      // stores to unaliased storage can be treated as assignments, not merges
      if (nonSendableTgt.value().isUnaliased())
        return translateSILAssign(tgt, src);

      // stores to possibly aliased storage must be treated as merges
      return translateSILMerge(tgt, src);
    }

    // stores to storage of non-Sendable type can be ignored
    return {};
  }

  std::vector<PartitionOp> translateSILRequire(SILValue val) {
    if (auto nonSendableVal = trackIfNonSendable(val))
      return Require(nonSendableVal.value());
    return {};
  }

  // an enum select is just a multi assign
  std::vector<PartitionOp> translateSILSelectEnum(
      SelectEnumOperation selectEnumInst) {
    std::vector<SILValue> enumOperands;
    for (unsigned i = 0; i < selectEnumInst.getNumCases(); i ++)
      enumOperands.push_back(selectEnumInst.getCase(i).second);
    if (selectEnumInst.hasDefault())
      enumOperands.push_back(selectEnumInst.getDefaultResult());
    return translateSILMultiAssign({selectEnumInst->getResult(0)}, enumOperands);
  }

  std::vector<PartitionOp> translateSILSwitchEnum(
      SwitchEnumInst *switchEnumInst) {
    std::vector<std::pair<std::vector<SILValue>, SILBasicBlock*>> branches;

    // accumulate each switch case that branches to a basic block with an arg
    for (unsigned i = 0; i < switchEnumInst->getNumCases(); i++) {
      SILBasicBlock *dest = switchEnumInst->getCase(i).second;
      if (dest->getNumArguments() > 0) {
        assert(dest->getNumArguments() == 1
               && "expected at most one bb arg in dest of enum switch");
        branches.push_back({{switchEnumInst->getOperand()}, dest});
      }
    }
    return translateSILPhi(branches);
  }

  // translate a SIL instruction corresponding to possible branches with args
  // to one or more basic blocks. This is the SIL equivalent of SSA Phi nodes.
  // each element of `branches` corresponds to the arguments passed to a bb,
  // and a pointer to the bb being branches to itself.
  // this is handled as assigning to each possible arg being branched to the
  // merge of all values that could be passed to it from this basic block.
  std::vector<PartitionOp> translateSILPhi(
      std::vector<std::pair<std::vector<SILValue>, SILBasicBlock*>> branches) {
    std::map<SILValue, std::vector<SILValue>> argSources;
    for (const auto &[args, dest] : branches) {
      assert(args.size() >= dest->getNumArguments());
      for (unsigned i = 0; i < dest->getNumArguments(); i++)
        argSources[dest->getArgument(i)].push_back(args[i]);
    }
    std::vector<PartitionOp> translated;
    for (const auto &[arg, srcs] : argSources)
      for (auto op : translateSILMultiAssign({arg}, srcs))
        translated.push_back(op);
    return translated;
  }

  // ===========================================================================

  // used to index the translations of SILInstructions performed
  // for refrence and debugging
  static inline int translationIndex = 0;

  // Some SILInstructions contribute to the partition of non-Sendable values
  // being analyzed. translateSILInstruction translate a SILInstruction
  // to its effect on the non-Sendable partition, if it has one.
  //
  // The current pattern of
  std::vector<PartitionOp> translateSILInstruction(SILInstruction *instruction) {
    LLVM_DEBUG(translationIndex++;);
    currentInstruction = instruction;

    // The following instructions are treated as assigning their result to a
    // fresh region.
    if (isa<AllocBoxInst,
            AllocPackInst,
            AllocRefDynamicInst,
            AllocRefInst,
            AllocStackInst,
            EnumInst,
            KeyPathInst,
            LiteralInst,
            ObjCProtocolInst,
            WitnessMethodInst>(instruction))
      return translateSILAssignFresh(instruction->getResult(0));

    if (isa<SelectEnumAddrInst, SelectEnumInst>(instruction))
      return translateSILSelectEnum(instruction);

    // The following instructions are treated as assignments of their result (tgt)
    // to their operand (src). This could yield a variety of behaviors depending
    // on the sendability of both src and tgt.
    // TODO: we could reduce the size of PartitionOp sequences generated by
    //       treating some of these as lookthroughs in simplifyVal instead of
    //       as assignments
    if (isa<AddressToPointerInst,
            BeginAccessInst,
            BeginBorrowInst,
            CopyValueInst,
            ConvertEscapeToNoEscapeInst,
            ConvertFunctionInst,
            CopyBlockInst,
            CopyBlockWithoutEscapingInst,
            IndexAddrInst,
            InitBlockStorageHeaderInst,
            InitEnumDataAddrInst,
            InitExistentialAddrInst,
            InitExistentialRefInst,
            LoadInst,
            LoadBorrowInst,
            LoadWeakInst,
            OpenExistentialAddrInst,
            OpenExistentialBoxInst,
            OpenExistentialRefInst,
            PointerToAddressInst,
            ProjectBlockStorageInst,
            RefElementAddrInst,
            RefToUnmanagedInst,
            StrongCopyUnownedValueInst,
            StructElementAddrInst,
            StructExtractInst,
            TailAddrInst,
            ThickToObjCMetatypeInst,
            ThinToThickFunctionInst,
            TupleElementAddrInst,
            UncheckedAddrCastInst,
            UncheckedEnumDataInst,
            UncheckedOwnershipConversionInst,
            UncheckedRefCastInst,
            UncheckedTakeEnumDataAddrInst,
            UnmanagedToRefInst,
            UpcastInst,

            //dynamic dispatch:
            ClassMethodInst,
            ObjCMethodInst,
            SuperMethodInst,
            ObjCSuperMethodInst
            >(instruction))
      return translateSILAssign(
          instruction->getResult(0),
          instruction->getOperand(0));

    // these are treated as stores - meaning that they could write values into
    // memory. The beahvior of this depends on whether the tgt addr is aliased,
    // but conservative behavior is to treat these as merges of the regions
    // of the src value and tgt addr
    if (isa<CopyAddrInst,
            ExplicitCopyAddrInst,
            StoreInst,
            StoreBorrowInst,
            StoreWeakInst>(instruction))
      return translateSILStore(
          instruction->getOperand(1),
          instruction->getOperand(0));

    // handle applications
    if (isApplyInst(*instruction))
      return translateSILApply(instruction);

    // handle tuple destruction
    if (auto destructTupleInst = dyn_cast<DestructureTupleInst>(instruction))
      return translateSILMultiAssign(
          {destructTupleInst->getResults().begin(),
           destructTupleInst->getResults().end()},
          {destructTupleInst->getOperand()});

    // handle instructions that aggregate their operands into a single structure
    // - treated as a multi assign
    if (isa<ObjectInst,
            StructInst,
            TupleInst>(instruction))
      return translateSILMultiAssign(
          {instruction->getResult(0)},
          {instruction->getOperandValues().begin(),
           instruction->getOperandValues().end()});

    // Handle returns and throws - require the operand to be non-consumed
    if (isa<ReturnInst, ThrowInst>(instruction))
      return translateSILRequire(instruction->getOperand(0));

    // handle branching terminators
    // in particular, need to handle phi-node-like argument passing

    if (auto branchInst = dyn_cast<BranchInst>(instruction)) {
      assert(branchInst->getNumArgs() == branchInst->getDestBB()->getNumArguments());
      return translateSILPhi(
          {{{branchInst->getArgs().begin(), branchInst->getArgs().end()},
            branchInst->getDestBB()}});
    }

    if (auto condBranchInst = dyn_cast<CondBranchInst>(instruction)) {
      assert(condBranchInst->getNumTrueArgs() ==
             condBranchInst->getTrueBB()->getNumArguments());
      assert(condBranchInst->getNumFalseArgs() ==
             condBranchInst->getFalseBB()->getNumArguments());
      return translateSILPhi({{// true branch
                               {condBranchInst->getTrueArgs().begin(),
                                condBranchInst->getTrueArgs().end()},
                               condBranchInst->getTrueBB()},
                              {// false branch
                               {condBranchInst->getFalseArgs().begin(),
                                condBranchInst->getFalseArgs().end()},
                               condBranchInst->getFalseBB()}});
    }

    if (auto switchEnumInst = dyn_cast<SwitchEnumInst>(instruction))
      return translateSILSwitchEnum(switchEnumInst);

    if (auto dmBranchInst = dyn_cast<DynamicMethodBranchInst>(instruction)) {
      assert(dmBranchInst->getHasMethodBB()->getNumArguments() <= 1);
      return translateSILPhi(
          {{{dmBranchInst->getOperand()}, dmBranchInst->getHasMethodBB()}});
    }

    if (auto ccBranchInst = dyn_cast<CheckedCastBranchInst>(instruction)) {
      assert(ccBranchInst->getSuccessBB()->getNumArguments() <= 1);
      return translateSILPhi(
          {{{ccBranchInst->getOperand()}, ccBranchInst->getSuccessBB()}});
    }

    if (auto ccAddrBranchInst = dyn_cast<CheckedCastAddrBranchInst>(instruction)) {
      assert(ccAddrBranchInst->getSuccessBB()->getNumArguments() <= 1);
      return translateSILPhi({{{ccAddrBranchInst->getOperand(0)},
                               ccAddrBranchInst->getSuccessBB()}});
    }

    // these instructions are ignored because they cannot affect the partition
    // state - they do not manipulate what region non-sendable values lie in
    if (isa<AllocGlobalInst,
            DeallocBoxInst,
            DeallocStackInst,
            DebugValueInst,
            DestroyAddrInst,
            DestroyValueInst,
            EndAccessInst,
            EndBorrowInst,
            EndLifetimeInst,
            HopToExecutorInst,
            InjectEnumAddrInst,
            IsEscapingClosureInst, // ignored because result is always int
            MarkDependenceInst,
            MetatypeInst,

            EndApplyInst,
            AbortApplyInst,

            //ignored terminators
            CondFailInst,
            SwitchEnumAddrInst, // ignored as long as destinations can take no args
            SwitchValueInst, // ignored as long as destinations can take no args
            UnreachableInst,
            UnwindInst,
            YieldInst //TODO: yield should be handled
    >(instruction))
      //ignored instructions
      return {};

    LLVM_DEBUG(
        llvm::errs().changeColor(llvm::raw_ostream::MAGENTA, true);
        llvm::errs() << "warning: ";
        llvm::errs().resetColor();
        llvm::errs() << "unhandled instruction kind "
                     << getSILInstructionName(instruction->getKind())
                     << "\n";
    );

    return {};
  }

  // translateSILBasicBlock reduces a SIL basic block to the vector of
  // transformations to the non-Sendable partition that it induces.
  // it accomplished this by sequentially calling translateSILInstruction
  std::vector<PartitionOp> translateSILBasicBlock(SILBasicBlock *basicBlock) {
    LLVM_DEBUG(
        llvm::dbgs() << SEP_STR
                     << "Compiling basic block for function "
                     << basicBlock->getFunction()->getName()
                     << ": ";
        basicBlock->dumpID();
        llvm::dbgs() << SEP_STR;
        basicBlock->dump();
        llvm::dbgs() << SEP_STR << "Results:\n";
    );

    //translate each SIL instruction to a PartitionOp, if necessary
    std::vector<PartitionOp> partitionOps;
    int lastTranslationIndex = -1;
    for (SILInstruction &instruction : *basicBlock) {
      auto ops = translateSILInstruction(&instruction);
      for (PartitionOp &op : ops) {
        partitionOps.push_back(op);

        LLVM_DEBUG(
            if (translationIndex != lastTranslationIndex) {
              llvm::dbgs() << " ┌─┬─╼";
              instruction.dump();
              llvm::dbgs() << " │ └─╼  ";
              instruction.getLoc().getSourceLoc().printLineAndColumn(
                  llvm::dbgs(), function->getASTContext().SourceMgr);
              llvm::dbgs() << " │ translation #" << translationIndex;
              llvm::dbgs() << "\n └─────╼ ";
            } else {
              llvm::dbgs() << "      └╼ ";
            }
            op.dump();
            lastTranslationIndex = translationIndex;
        );
      }
    }

    return partitionOps;
  }
};

// Instances of BlockPartitionState record all relevant state about a
// SILBasicBlock for the region-based Sendable checking fixpoint analysis.
// In particular, it records flags such as whether the block has been
// reached by the analysis, whether the prior round indicated that this block
// needs to be updated; it records aux data such as the underlying basic block
// and associated PartitionOpTranslator; and most importantly of all it includes
// region partitions at entry and exit to this block - these are the stateful
// component of the fixpoint analysis.
class BlockPartitionState {
  friend class PartitionAnalysis;

  bool needsUpdate = false;
  bool reached = false;

  Partition entryPartition;
  Partition exitPartition;

  SILBasicBlock *basicBlock;
  PartitionOpTranslator &translator;

  bool blockPartitionOpsPopulated = false;
  std::vector<PartitionOp> blockPartitionOps = {};

  BlockPartitionState(SILBasicBlock *basicBlock,
                      PartitionOpTranslator &translator)
      : basicBlock(basicBlock), translator(translator) {}

  void ensureBlockPartitionOpsPopulated() {
    if (blockPartitionOpsPopulated) return;
    blockPartitionOpsPopulated = true;
    blockPartitionOps = translator.translateSILBasicBlock(basicBlock);
  }

  // recomputes the exit partition from the entry partition,
  // and returns whether this changed the exit partition.
  // Note that this method ignored errors that arise.
  bool recomputeExitFromEntry() {
    ensureBlockPartitionOpsPopulated();

    Partition workingPartition = entryPartition;
    for (auto partitionOp : blockPartitionOps) {
      // by calling apply without providing a `handleFailure` closure,
      // errors will be suppressed
      workingPartition.apply(partitionOp);
    }
    bool exitUpdated = !Partition::equals(exitPartition, workingPartition);
    exitPartition = workingPartition;
    return exitUpdated;
  }

  // apply each PartitionOP in this block to the entry partition,
  // but this time pass in a handleFailure closure that can be used
  // to diagnose any failures
  void diagnoseFailures(
      llvm::function_ref<void(const PartitionOp&, TrackableValueID)>
          handleFailure,
      llvm::function_ref<void(const PartitionOp&, TrackableValueID)>
          handleConsumeNonConsumable) {
    Partition workingPartition = entryPartition;
    for (auto &partitionOp : blockPartitionOps) {
      workingPartition.apply(partitionOp, handleFailure,
                             translator.getNonConsumables(),
                             handleConsumeNonConsumable);
    }
  }

public:
  // run the passed action on each partitionOp in this block. Action should
  // return true iff iteration should continue
  void forEachPartitionOp(llvm::function_ref<bool (const PartitionOp&)> action) const {
    for (const PartitionOp &partitionOp : blockPartitionOps)
      if (!action(partitionOp)) break;
  }

  const Partition& getEntryPartition() const {
    return entryPartition;
  }

  const Partition& getExitPartition() const {
    return exitPartition;
  }

  void dump() LLVM_ATTRIBUTE_USED {
    LLVM_DEBUG(
        llvm::dbgs() << SEP_STR
                     << "BlockPartitionState[reached="
                     << reached
                     << ", needsUpdate="
                     << needsUpdate
                     << "]\nid: ";
        basicBlock->dumpID();
        llvm::dbgs() << "entry partition: ";
        entryPartition.dump();
        llvm::dbgs() << "exit partition: ";
        exitPartition.dump();
        llvm::dbgs() << "instructions:\n┌──────────╼\n";
        for (PartitionOp op : blockPartitionOps) {
          llvm::dbgs() << "│ ";
          op.dump();
        }
        llvm::dbgs() << "└──────────╼\nSuccs:\n";
        for (auto succ : basicBlock->getSuccessorBlocks()) {
          llvm::dbgs() << "→";
          succ->dumpID();
        }
        llvm::dbgs() << "Preds:\n";
        for (auto pred : basicBlock->getPredecessorBlocks()) {
          llvm::dbgs() << "←";
          pred->dumpID();
        }
        llvm::dbgs()<< SEP_STR;
    );
  }
};

enum class LocalConsumedReasonKind {
  LocalConsumeInst,
  LocalNonConsumeInst,
  NonLocal
};

// Why was a value consumed, without looking across blocks?
// kind == LocalConsumeInst: a consume instruction in this block
// kind == LocalNonConsumeInst: an instruction besides a consume instruction
//                              in this block
// kind == NonLocal: an instruction outside this block
struct LocalConsumedReason {
  LocalConsumedReasonKind kind;
  llvm::Optional<PartitionOp> localInst;

  static LocalConsumedReason ConsumeInst(PartitionOp localInst) {
    assert(localInst.getKind() == PartitionOpKind::Consume);
    return LocalConsumedReason(LocalConsumedReasonKind::LocalConsumeInst, localInst);
  }

  static LocalConsumedReason NonConsumeInst() {
    return LocalConsumedReason(LocalConsumedReasonKind::LocalNonConsumeInst);
  }

  static LocalConsumedReason NonLocal() {
    return LocalConsumedReason(LocalConsumedReasonKind::NonLocal);
  }

  // 0-ary constructor only used in maps, where it's immediately overridden
  LocalConsumedReason() : kind(LocalConsumedReasonKind::NonLocal) {}

private:
  LocalConsumedReason(LocalConsumedReasonKind kind,
                 llvm::Optional<PartitionOp> localInst = {})
      : kind(kind), localInst(localInst) {}
};

// This class captures all available information about why a value's region was
// consumed. In particular, it contains a map `consumeOps` whose keys are
// "distances" and whose values are Consume PartitionOps that cause the target
// region to be consumed. Distances are (roughly) the number of times
// two different predecessor blocks had to have their exit partitions joined
// together to actually cause the target region to be consumed. If a Consume
// op only causes a target access to be invalid because of merging/joining
// that spans many different blocks worth of control flow, it is less likely
// to be informative, so distance is used as a heuristic to choose which
// access sites to display in diagnostics given a racy consumption.
class ConsumedReason {
  std::map<unsigned, std::vector<PartitionOp>> consumeOps;

  friend class ConsumeRequireAccumulator;

  bool containsOp(const PartitionOp& op) {
    for (auto [_, vec] : consumeOps)
      for (auto vecOp : vec)
        if (op == vecOp)
          return true;
    return false;
  }

public:
  // a ConsumedReason is valid if it contains at least one consume instruction
  bool isValid() {
    for (auto [_, vec] : consumeOps)
      if (!vec.empty())
        return true;
    return false;
  }

  ConsumedReason() {}

  ConsumedReason(LocalConsumedReason localReason) {
    assert(localReason.kind == LocalConsumedReasonKind::LocalConsumeInst);
    consumeOps[0] = {localReason.localInst.value()};
  }

  void addConsumeOp(PartitionOp consumeOp, unsigned distance) {
    assert(consumeOp.getKind() == PartitionOpKind::Consume);
    // duplicates should not arise
    if (!containsOp(consumeOp))
      consumeOps[distance].push_back(consumeOp);
  }

  // merge in another consumedReason, adding the specified distane to all its ops
  void addOtherReasonAtDistance(const ConsumedReason &otherReason, unsigned distance) {
    for (auto &[otherDistance, otherConsumeOpsAtDistance] : otherReason.consumeOps)
      for (auto otherConsumeOp : otherConsumeOpsAtDistance)
        addConsumeOp(otherConsumeOp, distance + otherDistance);
  }
};

// This class is the "inverse" of a ConsumedReason: instead of associating
// accessing PartitionOps with their consumption sites, it associates
// consumption site Consume PartitionOps with the corresponding accesses.
// It is built up by repeatedly calling accumulateConsumedReason on
// ConsumedReasons, which "inverts" the contents of that reason and adds it to
// this class's tracking. Instead of a two-level map, we store a set that
// join together distances and access partitionOps so that we can use the
// ordering by lowest diagnostics for prioritized output
class ConsumeRequireAccumulator {
  struct PartitionOpAtDistance {
    PartitionOp partitionOp;
    unsigned distance;

    PartitionOpAtDistance(PartitionOp partitionOp, unsigned distance)
        : partitionOp(partitionOp), distance(distance) {}

    bool operator<(const PartitionOpAtDistance& other) const {
      if (distance != other.distance)
        return distance < other.distance;
      return partitionOp < other.partitionOp;
    }
  };

  // map consumptions to sets of requirements for that consumption, ordered so
  // that requirements at a smaller distance from the consumption come first
  std::map<PartitionOp, std::set<PartitionOpAtDistance>>
      requirementsForConsumptions;

public:
  ConsumeRequireAccumulator() {}

  void accumulateConsumedReason(PartitionOp requireOp, const ConsumedReason &consumedReason) {
    for (auto [distance, consumeOps] : consumedReason.consumeOps)
      for (auto consumeOp : consumeOps)
        requirementsForConsumptions[consumeOp].insert({requireOp, distance});
  }

  // for each consumption in this ConsumeRequireAccumulator, call the passed
  // processConsumeOp closure on it, followed immediately by calling the passed
  // processRequireOp closure on the top `numRequiresPerConsume` operations
  // that access ("require") the region consumed. Sorting is by lowest distance
  // first, then arbitrarily. This is used for final diagnostic output.
  void forEachConsumeRequire(
      llvm::function_ref<void(const PartitionOp& consumeOp, unsigned numProcessed, unsigned numSkipped)>
          processConsumeOp,
      llvm::function_ref<void(const PartitionOp& requireOp)>
          processRequireOp,
      unsigned numRequiresPerConsume = UINT_MAX) const {
    for (auto [consumeOp, requireOps] : requirementsForConsumptions) {
      unsigned numProcessed = std::min({(unsigned) requireOps.size(),
                                        (unsigned) numRequiresPerConsume});
      processConsumeOp(consumeOp, numProcessed, requireOps.size() - numProcessed);
      unsigned numRequiresToProcess = numRequiresPerConsume;
      for (auto [requireOp, _] : requireOps) {
        // ensures at most numRequiresPerConsume requires are processed per consume
        if (numRequiresToProcess-- == 0) break;
        processRequireOp(requireOp);
      }
    }
  }

  void dump() const {
    forEachConsumeRequire(
        [](const PartitionOp& consumeOp, unsigned numProcessed, unsigned numSkipped) {
          llvm::dbgs() << " ┌──╼ CONSUME: ";
          consumeOp.dump();
        },
        [](const PartitionOp& requireOp) {
          llvm::dbgs() << " ├╼ REQUIRE: ";
          requireOp.dump();
        }
    );
  }
};

// A RaceTracer is used to accumulate the facts that the main phase of
// PartitionAnalysis generates - that certain values were required at certain
// points but were in consumed regions and thus should yield diagnostics -
// and traces those facts to the Consume operations that could have been
// responsible.
class RaceTracer {
  const BasicBlockData<BlockPartitionState>& blockStates;

  std::map<std::pair<SILBasicBlock *, TrackableValueID>, ConsumedReason>
      consumedAtEntryReasons;

  // caches the reasons why consumedVals were consumed at the exit to basic blocks
  std::map<std::pair<SILBasicBlock *, TrackableValueID>, LocalConsumedReason>
      consumedAtExitReasons;

  ConsumeRequireAccumulator accumulator;

  ConsumedReason findConsumedAtOpReason(TrackableValueID consumedVal, PartitionOp op) {
    ConsumedReason consumedReason;
    findAndAddConsumedReasons(op.getSourceInst(true)->getParent(), consumedVal,
                              consumedReason, 0, op);
    return consumedReason;
  }

  void findAndAddConsumedReasons(
      SILBasicBlock * SILBlock, TrackableValueID consumedVal,
      ConsumedReason &consumedReason, unsigned distance,
      llvm::Optional<PartitionOp> targetOp = {}) {
    assert(blockStates[SILBlock].getExitPartition().isConsumed(consumedVal));
    LocalConsumedReason localReason
        = findLocalConsumedReason(SILBlock, consumedVal, targetOp);
    switch (localReason.kind) {
    case LocalConsumedReasonKind::LocalConsumeInst:
      // there is a local consume in the pred block
      consumedReason.addConsumeOp(localReason.localInst.value(), distance);
      break;
    case LocalConsumedReasonKind::LocalNonConsumeInst:
      // ignore this case, that instruction will initiate its own search
      // for a consume op
      break;
    case LocalConsumedReasonKind::NonLocal:
      consumedReason.addOtherReasonAtDistance(
          // recursive call
          findConsumedAtEntryReason(SILBlock, consumedVal), distance);
    }
  }

  // find the reason why a value was consumed at entry to a block
  const ConsumedReason &findConsumedAtEntryReason(
      SILBasicBlock *SILBlock, TrackableValueID consumedVal) {
    const BlockPartitionState &block = blockStates[SILBlock];
    assert(block.getEntryPartition().isConsumed(consumedVal));

    // check the cache
    if (consumedAtEntryReasons.count({SILBlock, consumedVal}))
      return consumedAtEntryReasons.at({SILBlock, consumedVal});

    // enter a dummy value in the cache to prevent circular call dependencies
    consumedAtEntryReasons[{SILBlock, consumedVal}] = ConsumedReason();

    auto entryTracks = [&](TrackableValueID val) {
      return block.getEntryPartition().isTracked(val);
    };

    // this gets populated with all the tracked values at entry to this block
    // that are consumed at the exit to some predecessor block, associated
    // with the blocks that consume them
    std::map<TrackableValueID, std::vector<SILBasicBlock *>> consumedInSomePred;
    for (SILBasicBlock *pred : SILBlock->getPredecessorBlocks())
      for (TrackableValueID consumedVal
          : blockStates[pred].getExitPartition().getConsumedVals())
        if (entryTracks(consumedVal))
          consumedInSomePred[consumedVal].push_back(pred);

    // this gets populated with all the multi-edges between values tracked
    // at entry to this block that will be merged because of common regionality
    // in the exit partition of some predecessor. It is not transitively closed
    // because we want to count how many steps transitive merges require
    std::map<TrackableValueID, std::set<TrackableValueID>> singleStepJoins;
    for (SILBasicBlock *pred : SILBlock->getPredecessorBlocks())
      for (std::vector<TrackableValueID> region
          : blockStates[pred].getExitPartition().getNonConsumedRegions()) {
        for (TrackableValueID fst : region) for (TrackableValueID snd : region)
            if (fst != snd && entryTracks(fst) && entryTracks(snd))
              singleStepJoins[fst].insert(snd);
      }

    // this gets populated with the distance, in terms of single step joins,
    // from the target consumedVal to other values that will get merged with it
    // because of the join at entry to this basic block
    std::map<TrackableValueID, unsigned> distancesFromTarget;

    // perform BFS
    // an entry of `{val, dist}` in the `processValues` deque indicates that
    // `val` is known to be merged with `consumedVal` (the target of this find)
    // at a distance of `dist` single-step joins
    std::deque<std::pair<TrackableValueID, unsigned>> processValues;
    processValues.push_back({consumedVal, 0});
    while (!processValues.empty()) {
      auto [currentTarget, currentDistance] = processValues.front();
      processValues.pop_front();
      distancesFromTarget[currentTarget] = currentDistance;
      for (TrackableValueID nextTarget : singleStepJoins[currentTarget])
        if (!distancesFromTarget.count(nextTarget))
          processValues.push_back({nextTarget, currentDistance + 1});
    }

    ConsumedReason consumedReason;

    for (auto [predVal, distanceFromTarget] : distancesFromTarget) {
      for (SILBasicBlock *predBlock : consumedInSomePred[predVal]) {
        // one reason that our target consumedVal is consumed is that
        // predConsumedVal was consumed at exit of predBlock, and
        // distanceFromTarget merges had to be performed to make that
        // be a reason. Use this to build a ConsumedReason for consumedVal.
        findAndAddConsumedReasons(predBlock, predVal, consumedReason, distanceFromTarget);
      }
    }

    consumedAtEntryReasons[{SILBlock, consumedVal}] = std::move(consumedReason);

    return consumedAtEntryReasons[{SILBlock, consumedVal}];
  }

  // assuming that consumedVal is consumed at the point of targetOp within
  // SILBlock (or block exit if targetOp = {}), find the reason why it was
  // consumed, possibly local or nonlocal. Return the reason.
  LocalConsumedReason findLocalConsumedReason(
      SILBasicBlock * SILBlock, TrackableValueID consumedVal,
      llvm::Optional<PartitionOp> targetOp = {}) {
    // if this is a query for consumption reason at block exit, check the cache
    if (!targetOp && consumedAtExitReasons.count({SILBlock, consumedVal}))
      return consumedAtExitReasons.at({SILBlock, consumedVal});

    const BlockPartitionState &block = blockStates[SILBlock];

    // if targetOp is null, we're checking why the value is consumed at exit,
    // so assert that it's actually consumed at exit
    assert(targetOp || block.getExitPartition().isConsumed(consumedVal));

    llvm::Optional<LocalConsumedReason> consumedReason;

    Partition workingPartition = block.getEntryPartition();

    // we're looking for a local reason, so if the value is consumed at entry,
    // revive it for the sake of this search
    if (workingPartition.isConsumed(consumedVal))
      workingPartition.apply(PartitionOp::AssignFresh(consumedVal));

    int i = 0;
    block.forEachPartitionOp([&](const PartitionOp& partitionOp) {
      if (targetOp == partitionOp)
        return false; //break
      workingPartition.apply(partitionOp);
      if (workingPartition.isConsumed(consumedVal) && !consumedReason) {
        // this partitionOp consumes the target value
        if (partitionOp.getKind() == PartitionOpKind::Consume)
          consumedReason = LocalConsumedReason::ConsumeInst(partitionOp);
        else
          // a merge or assignment invalidated this, but that will be a separate
          // failure to diagnose, so we don't worry about it here
          consumedReason = LocalConsumedReason::NonConsumeInst();
      }
      if (!workingPartition.isConsumed(consumedVal) && consumedReason)
        // value is no longer consumed - e.g. reassigned or assigned fresh
        consumedReason = llvm::None;

      // continue walking block
      i++;
      return true;
    });

    // if we failed to find a local consume reason, but the value was consumed
    // at entry to the block, then the reason is "NonLocal"
    if (!consumedReason && block.getEntryPartition().isConsumed(consumedVal))
      consumedReason = LocalConsumedReason::NonLocal();

    // if consumedReason is none, then consumedVal was not actually consumed
    assert(consumedReason
           || dumpBlockSearch(SILBlock, consumedVal)
           && " no consumption was found"
    );

    // if this is a query for consumption reason at block exit, update the cache
    if (!targetOp)
      return consumedAtExitReasons[std::pair{SILBlock, consumedVal}]
                 = consumedReason.value();

    return consumedReason.value();
  }

  bool dumpBlockSearch(SILBasicBlock * SILBlock, TrackableValueID consumedVal) {
    LLVM_DEBUG(
        unsigned i = 0;
        const BlockPartitionState &block = blockStates[SILBlock];
        Partition working = block.getEntryPartition();
        llvm::dbgs() << "┌──────────╼\n│ ";
        working.dump();
        block.forEachPartitionOp([&](const PartitionOp &op) {
          llvm::dbgs() << "├[" << i++ << "] ";
          op.dump();
          working.apply(op);
          llvm::dbgs() << "│ ";
          if (working.isConsumed(consumedVal)) {
            llvm::errs().changeColor(llvm::raw_ostream::RED, true);
            llvm::errs() << "(" << consumedVal << " CONSUMED) ";
            llvm::errs().resetColor();
          }
          working.dump();
          return true;
        });
        llvm::dbgs() << "└──────────╼\n";
    );
    return false;
  }

public:
  RaceTracer(const BasicBlockData<BlockPartitionState>& blockStates)
      : blockStates(blockStates) {}

  void traceUseOfConsumedValue(PartitionOp use, TrackableValueID consumedVal) {
    accumulator.accumulateConsumedReason(
        use, findConsumedAtOpReason(consumedVal, use));
  }

  const ConsumeRequireAccumulator &getAccumulator() {
    return accumulator;
  }
};

// Instances of PartitionAnalysis perform the region-based Sendable checking.
// Internally, a PartitionOpTranslator is stored to perform the translation from
// SILInstructions to PartitionOps, then a fixed point iteration is run to
// determine the set of exit and entry partitions to each point satisfying
// the flow equations.
class PartitionAnalysis {
  PartitionOpTranslator translator;

  BasicBlockData<BlockPartitionState> blockStates;

  RaceTracer raceTracer;

  SILFunction *function;

  bool solved;

  // TODO: make this configurable in a better way
  const static int NUM_REQUIREMENTS_TO_DIAGNOSE = 50;

  // The constructor initializes each block in the function by compiling it
  // to PartitionOps, then seeds the solve method by setting `needsUpdate` to
  // true for the entry block
  PartitionAnalysis(SILFunction *fn)
      : translator(fn),
        blockStates(fn,
                    [this](SILBasicBlock *block) {
                      return BlockPartitionState(block, translator);
                    }),
        raceTracer(blockStates),
        function(fn),
        solved(false) {
    // initialize the entry block as needing an update, and having a partition
    // that places all its non-sendable args in a single region
    blockStates[fn->getEntryBlock()].needsUpdate = true;
    blockStates[fn->getEntryBlock()].entryPartition =
        translator.getEntryPartition();
  }

  void solve() {
    assert(!solved && "solve should only be called once");
    solved = true;

    bool anyNeedUpdate = true;
    while (anyNeedUpdate) {
      anyNeedUpdate = false;

      for (auto [block, blockState] : blockStates) {
        if (!blockState.needsUpdate) continue;

        // mark this block as no longer needing an update
        blockState.needsUpdate = false;

        // mark this block as reached by the analysis
        blockState.reached = true;

        // compute the new entry partition to this block
        Partition newEntryPartition;
        bool firstPred = true;

        // this loop computes the join of the exit partitions of all
        // predecessors of this block
        for (SILBasicBlock *predBlock : block.getPredecessorBlocks()) {
          BlockPartitionState &predState = blockStates[predBlock];
          // ignore predecessors that haven't been reached by the analysis yet
          if (!predState.reached) continue;

          if (firstPred) {
            firstPred = false;
            newEntryPartition = predState.exitPartition;
            continue;
          }

          newEntryPartition = Partition::join(
              newEntryPartition, predState.exitPartition);
        }

        // if we found predecessor blocks, then attempt to use them to
        // update the entry partition for this block, and abort this block's
        // update if the entry partition was not updated
        if (!firstPred) {
          // if the recomputed entry partition is the same as the current one,
          // perform no update
          if (Partition::equals(newEntryPartition, blockState.entryPartition))
            continue;

          // otherwise update the entry partition
          blockState.entryPartition = newEntryPartition;
        }

        // recompute this block's exit partition from its (updated) entry
        // partition, and if this changed the exit partition notify all
        // successor blocks that they need to update as well
        if (blockState.recomputeExitFromEntry()) {
          for (SILBasicBlock *succBlock : block.getSuccessorBlocks()) {
            anyNeedUpdate = true;
            blockStates[succBlock].needsUpdate = true;
          }
        }
      }
    }
  }

  // track the AST exprs that have already had diagnostics emitted about
  llvm::DenseSet<Expr *> emittedExprs;

  // check if a diagnostic has already been emitted about expr, only
  // returns true false for each expr
  bool hasBeenEmitted(Expr *expr) {
    if (auto castExpr = dyn_cast<ImplicitConversionExpr>(expr))
      return hasBeenEmitted(castExpr->getSubExpr());

    if (emittedExprs.contains(expr)) return true;
    emittedExprs.insert(expr);
    return false;
  }

  // used for generating informative diagnostics
  Expr *getExprForPartitionOp(const PartitionOp& op) {
    SILInstruction *sourceInstr = op.getSourceInst(/*assertNonNull=*/true);
    Expr *expr = sourceInstr->getLoc().getAsASTNode<Expr>();
    assert(expr && "PartitionOp's source location should correspond to"
                   "an AST node");
    return expr;
  }

  // once the fixpoint has been solved for, run one more pass over each basic
  // block, reporting any failures due to requiring consumed regions in the
  // fixpoint state
  void diagnose() {
    assert(solved && "diagnose should not be called before solve");

    LLVM_DEBUG(
        llvm::dbgs() << "Emitting diagnostics for function "
                     << function->getName() << "\n");
    RaceTracer tracer = blockStates;

    for (auto [_, blockState] : blockStates) {
      // populate the raceTracer with all requires of consumed valued found
      // throughout the CFG
      blockState.diagnoseFailures(
          /*handleFailure=*/
          [&](const PartitionOp& partitionOp, TrackableValueID consumedVal) {
            auto expr = getExprForPartitionOp(partitionOp);

            // ensure that multiple consumptions at the same AST node are only
            // entered once into the race tracer
            if (hasBeenEmitted(expr)) return;

            raceTracer.traceUseOfConsumedValue(partitionOp, consumedVal);
          },

          /*handleConsumeNonConsumable=*/
          [&](const PartitionOp& partitionOp, TrackableValueID consumedVal) {
            auto expr = getExprForPartitionOp(partitionOp);
            function->getASTContext().Diags.diagnose(
                expr->getLoc(), diag::arg_region_consumed);
          });
    }

    LLVM_DEBUG(
        llvm::dbgs() << "Accumulator Complete:\n";
        raceTracer.getAccumulator().dump();
    );

    // ask the raceTracer to report diagnostics at the consumption sites
    // for all the racy requirement sites entered into it above
    raceTracer.getAccumulator().forEachConsumeRequire(
        /*diagnoseConsume=*/
        [&](const PartitionOp& consumeOp,
            unsigned numDisplayed, unsigned numHidden) {

          if (tryDiagnoseAsCallSite(consumeOp, numDisplayed, numHidden))
            return;

          assert(false && "no consumptions besides callsites implemented yet");

          // default to more generic diagnostic
          auto expr = getExprForPartitionOp(consumeOp);
          auto diag = function->getASTContext().Diags.diagnose(
              expr->getLoc(), diag::consumption_yields_race,
              numDisplayed, numDisplayed != 1, numHidden > 0, numHidden);
          if (auto sourceExpr = consumeOp.getSourceExpr())
            diag.highlight(sourceExpr->getSourceRange());
        },

        /*diagnoseRequire=*/
        [&](const PartitionOp& requireOp) {
          auto expr = getExprForPartitionOp(requireOp);
          function->getASTContext().Diags.diagnose(
              expr->getLoc(), diag::possible_racy_access_site)
              .highlight(expr->getSourceRange());
        },
        NUM_REQUIREMENTS_TO_DIAGNOSE);
  }

  // try to interpret this consumeOp as a source-level callsite (ApplyExpr),
  // and report a diagnostic including actor isolation crossing information
  // returns true iff one was succesfully formed and emitted
  bool tryDiagnoseAsCallSite(
      const PartitionOp& consumeOp, unsigned numDisplayed, unsigned numHidden) {
    SILInstruction *sourceInst = consumeOp.getSourceInst(/*assertNonNull=*/true);
    ApplyExpr *apply = sourceInst->getLoc().getAsASTNode<ApplyExpr>();
    if (!apply)
      // consumption does not correspond to an apply expression
      return false;
    auto isolationCrossing = apply->getIsolationCrossing();
    if (!isolationCrossing) {
      assert(false && "ApplyExprs should be consuming only if"
                      " they are isolation crossing");
      return false;
    }
    auto argExpr = consumeOp.getSourceExpr();
    if (!argExpr)
      assert(false && "sourceExpr should be populated for ApplyExpr consumptions");

    function->getASTContext().Diags.diagnose(
        argExpr->getLoc(), diag::call_site_consumption_yields_race,
        findOriginalValueType(argExpr),
        isolationCrossing.value().getCallerIsolation(),
        isolationCrossing.value().getCalleeIsolation(),
        numDisplayed, numDisplayed != 1, numHidden > 0, numHidden)
        .highlight(argExpr->getSourceRange());
    return true;
  }

public:

  void dump() LLVM_ATTRIBUTE_USED {
    llvm::dbgs() << "\nPartitionAnalysis[fname=" << function->getName() << "]\n";

    for (auto [_, blockState] : blockStates) {
      blockState.dump();
    }
  }

  static void performForFunction(SILFunction *function) {
    auto analysis = PartitionAnalysis(function);
    analysis.solve();
    LLVM_DEBUG(
        llvm::dbgs() << "SOLVED: ";
        analysis.dump();
    );
    analysis.diagnose();
  }
};

// this class is the entry point to the region-based Sendable analysis,
// after certain checks are performed to ensure the analysis can be completed
// a PartitionAnalysis object is created and used to run the analysis.
class SendNonSendable : public SILFunctionTransform {

  // find any ApplyExprs in this function, and check if any of them make an
  // unsatisfied isolation jump, emitting appropriate diagnostics if so
  void run() override {
    SILFunction *function = getFunction();
    DeclContext *declContext = function->getDeclContext();

    // if this function does not correspond to a syntactic declContext, don't
    // check it
    // TODO: revisit this assumption, in particular perhaps verify no isolation
    // crossing applies occurs within these
    if (!declContext)
      return;

    // if the experimental feature SendNonSendable is not provided,
    // do not perform this pass
    if (!function->getASTContext().LangOpts.hasFeature(
            Feature::SendNonSendable))
      return;

    // if the Sendable protocol is not available, don't perform this checking
    // because we'd have to conservatively treat every value as non-Sendable
    // which would be very expensive
    if (!function->getASTContext().getProtocol(KnownProtocolKind::Sendable))
      return;

    PartitionAnalysis::performForFunction(function);
  }
};
}

/// This pass is known to depend on the following passes having run before it:
/// none so far
SILTransform *swift::createSendNonSendable() {
  return new SendNonSendable();
}
