//===--- VariableNameUtils.cpp --------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-variable-name-inference"

#include "swift/Basic/Assertions.h"
#include "swift/SILOptimizer/Utils/VariableNameUtils.h"
#include "swift/SIL/AddressWalker.h"
#include "swift/SIL/Test.h"

using namespace swift;

namespace {
struct AddressWalkerState {
  bool foundError = false;
  InstructionSet writes;
  AddressWalkerState(SILFunction *fn) : writes(fn) {}
};
} // namespace

static SILValue
findRootValueForNonTupleTempAllocation(AllocationInst *allocInst,
                                       AddressWalkerState &state) {
  // These are instructions which we are ok with looking through when
  // identifying our allocation. It must always refer to the entire allocation.
  auto isAlloc = [&](SILValue value) -> bool {
    if (auto *ieai = dyn_cast<InitExistentialAddrInst>(value))
      value = ieai->getOperand();
    return value == SILValue(allocInst);
  };

  // Walk from our allocation to one of our writes. Then make sure that the
  // write writes to our entire value.
  for (auto &inst : allocInst->getParent()->getRangeStartingAtInst(allocInst)) {
    // See if we have a full tuple value.

    if (!state.writes.contains(&inst))
      continue;

    if (auto *copyAddr = dyn_cast<CopyAddrInst>(&inst)) {
      if (isAlloc(copyAddr->getDest()) &&
          copyAddr->isInitializationOfDest()) {
        return copyAddr->getSrc();
      }
    }

    if (auto *si = dyn_cast<StoreInst>(&inst)) {
      if (isAlloc(si->getDest()) &&
          si->getOwnershipQualifier() != StoreOwnershipQualifier::Assign) {
        return si->getSrc();
      }
    }

    if (auto *sbi = dyn_cast<StoreBorrowInst>(&inst)) {
      if (isAlloc(sbi->getDest()))
        return sbi->getSrc();
    }

    // If we do not identify the write... return SILValue(). We weren't able
    // to understand the write.
    break;
  }

  return SILValue();
}

static SILValue findRootValueForTupleTempAllocation(AllocationInst *allocInst,
                                                    AddressWalkerState &state) {
  SmallVector<SILValue, 8> tupleValues;

  for (unsigned i : range(allocInst->getType().getNumTupleElements())) {
    (void)i;
    tupleValues.push_back(nullptr);
  }

  unsigned numEltsLeft = tupleValues.size();

  // If we have an empty tuple, just return SILValue() for now.
  //
  // TODO: What does this pattern look like out of SILGen?
  if (!numEltsLeft)
    return SILValue();

  // Walk from our allocation to one of our writes. Then make sure that the
  // write writes to our entire value.
  DestructureTupleInst *foundDestructure = nullptr;
  SILValue foundRootAddress;
  for (auto &inst : allocInst->getParent()->getRangeStartingAtInst(allocInst)) {
    if (!state.writes.contains(&inst))
      continue;

    if (auto *copyAddr = dyn_cast<CopyAddrInst>(&inst)) {
      if (copyAddr->isInitializationOfDest()) {
        if (auto *tei = dyn_cast<TupleElementAddrInst>(copyAddr->getDest())) {
          if (tei->getOperand() == allocInst) {
            unsigned i = tei->getFieldIndex();
            if (auto *otherTei = dyn_cast_or_null<TupleElementAddrInst>(
                    copyAddr->getSrc()->getDefiningInstruction())) {
              // If we already were processing destructures, then we have a mix
              // of struct/destructures... we do not support that, so bail.
              if (foundDestructure)
                return SILValue();

              // Otherwise, update our root address. If we already had a root
              // address and it doesn't match our tuple_element_addr's operand,
              // bail. There is some sort of mix/match of tuple addresses that
              // we do not support. We are looking for a specific SILGen
              // pattern.
              if (!foundRootAddress) {
                foundRootAddress = otherTei->getOperand();
              } else if (foundRootAddress != otherTei->getOperand()) {
                return SILValue();
              }

              if (i != otherTei->getFieldIndex())
                return SILValue();
              if (tupleValues[i])
                return SILValue();
              tupleValues[i] = otherTei;

              // If we have completely covered the tuple, break.
              --numEltsLeft;
              if (!numEltsLeft)
                break;

              // Otherwise, continue so we keep processing.
              continue;
            }
          }
        }
      }
    }

    if (auto *si = dyn_cast<StoreInst>(&inst)) {
      if (si->getOwnershipQualifier() != StoreOwnershipQualifier::Assign) {
        // Check if we are updating the entire tuple value.
        if (si->getDest() == allocInst) {
          // If we already found a root address (meaning we were processing
          // tuple_elt_addr), bail. We have some sort of unhandled mix of
          // copy_addr and store.
          if (foundRootAddress)
            return SILValue();

          // If we already found a destructure, return SILValue(). We are
          // initializing twice.
          if (foundDestructure)
            return SILValue();

          // We are looking for a pattern where we construct a tuple from
          // destructured parts.
          if (auto *ti = dyn_cast<TupleInst>(si->getSrc())) {
            for (auto p : llvm::enumerate(ti->getOperandValues())) {
              SILValue value = lookThroughOwnershipInsts(p.value());
              if (auto *dti = dyn_cast_or_null<DestructureTupleInst>(
                      value->getDefiningInstruction())) {
                // We should always go through the same dti.
                if (foundDestructure && foundDestructure != dti)
                  return SILValue();
                if (!foundDestructure)
                  foundDestructure = dti;

                // If we have a mixmatch of indices, we cannot look through.
                if (p.index() != dti->getIndexOfResult(value))
                  return SILValue();
                if (tupleValues[p.index()])
                  return SILValue();
                tupleValues[p.index()] = value;

                // If we have completely covered the tuple, break.
                --numEltsLeft;
                if (!numEltsLeft)
                  break;
              }
            }

            // If we haven't completely covered the tuple, return SILValue(). We
            // should completely cover the tuple.
            if (numEltsLeft)
              return SILValue();

            // Otherwise, break since we are done.
            break;
          }
        }

        // If we store to a tuple_element_addr, update for a single value.
        if (auto *tei = dyn_cast<TupleElementAddrInst>(si->getDest())) {
          if (tei->getOperand() == allocInst) {
            unsigned i = tei->getFieldIndex();
            if (auto *dti = dyn_cast_or_null<DestructureTupleInst>(
                    si->getSrc()->getDefiningInstruction())) {
              // If we already found a root address (meaning we were processing
              // tuple_elt_addr), bail. We have some sort of unhandled mix of
              // copy_addr and store [init].
              if (foundRootAddress)
                return SILValue();
              if (!foundDestructure) {
                foundDestructure = dti;
              } else if (foundDestructure != dti) {
                return SILValue();
              }

              if (i != dti->getIndexOfResult(si->getSrc()))
                return SILValue();
              if (tupleValues[i])
                return SILValue();
              tupleValues[i] = si->getSrc();

              // If we have completely covered the tuple, break.
              --numEltsLeft;
              if (!numEltsLeft)
                break;

              // Otherwise, continue so we keep processing.
              continue;
            }
          }
        }
      }
    }

    // Found a write that we did not understand... bail.
    break;
  }

  // Now check if we have a complete tuple with all elements coming from the
  // same destructure_tuple. In such a case, we can look through the
  // destructure_tuple.
  if (numEltsLeft)
    return SILValue();

  if (foundDestructure)
    return foundDestructure->getOperand();
  if (foundRootAddress)
    return foundRootAddress;

  return SILValue();
}

SILValue VariableNameInferrer::getRootValueForTemporaryAllocation(
    AllocationInst *allocInst) {
  struct AddressWalker final : public TransitiveAddressWalker<AddressWalker> {
    AddressWalkerState &state;

    AddressWalker(AddressWalkerState &state) : state(state) {}

    bool visitUse(Operand *use) {
      if (use->getUser()->mayWriteToMemory())
        state.writes.insert(use->getUser());
      return true;
    }

    TransitiveUseVisitation visitTransitiveUseAsEndPointUse(Operand *use) {
      if (auto *sbi = dyn_cast<StoreBorrowInst>(use->getUser()))
        return TransitiveUseVisitation::OnlyUser;
      return TransitiveUseVisitation::OnlyUses;
    }

    void onError(Operand *use) { state.foundError = true; }
  };

  AddressWalkerState state(allocInst->getFunction());
  AddressWalker walker(state);
  if (std::move(walker).walk(allocInst) == AddressUseKind::Unknown ||
      state.foundError)
    return SILValue();

  if (allocInst->getType().is<TupleType>())
    return findRootValueForTupleTempAllocation(allocInst, state);
  return findRootValueForNonTupleTempAllocation(allocInst, state);
}

SILValue
VariableNameInferrer::findDebugInfoProvidingValue(SILValue searchValue) {
  if (!searchValue)
    return SILValue();
  LLVM_DEBUG(llvm::dbgs() << "Searching for debug info providing value for: "
                          << searchValue);
  ValueSet valueSet(searchValue->getFunction());
  SILValue result = findDebugInfoProvidingValueHelper(searchValue, valueSet);
  if (result) {
    LLVM_DEBUG(llvm::dbgs() << "Result: " << result);
  } else {
    LLVM_DEBUG(llvm::dbgs() << "Result: None\n");
  }
  return result;
}

SILValue VariableNameInferrer::findDebugInfoProvidingValuePhiArg(
    SILValue incomingValue, ValueSet &visitedValues) {
  // We use pushSnapShot to run recursively and if we fail to find a
  // value, we just pop our list to the last snapshot end of list. If we
  // succeed, we do not pop and just return recusive value. Our user
  // will consume variableNamePath at this point.
  LLVM_DEBUG(llvm::dbgs() << "Before pushing a snap shot!\n";
             variableNamePath.print(llvm::dbgs()));

  unsigned oldSnapShotIndex = variableNamePath.pushSnapShot();
  LLVM_DEBUG(llvm::dbgs() << "After pushing a snap shot!\n";
             variableNamePath.print(llvm::dbgs()));

  if (SILValue recursiveValue =
          findDebugInfoProvidingValueHelper(incomingValue, visitedValues)) {
    LLVM_DEBUG(llvm::dbgs() << "Returned: " << recursiveValue);
    variableNamePath.returnSnapShot(oldSnapShotIndex);
    return recursiveValue;
  }

  variableNamePath.popSnapShot(oldSnapShotIndex);
  LLVM_DEBUG(llvm::dbgs() << "After popping a snap shot!\n";
             variableNamePath.print(llvm::dbgs()));
  return SILValue();
}

static BeginBorrowInst *hasOnlyBorrowingNonDestroyUse(SILValue searchValue) {
  BeginBorrowInst *result = nullptr;
  for (auto *use : searchValue->getUses()) {
    if (isIncidentalUse(use->getUser()))
      continue;
    if (use->isConsuming()) {
      if (!isa<DestroyValueInst>(use->getUser()))
        return nullptr;
      continue;
    }

    auto *bbi = dyn_cast<BeginBorrowInst>(use->getUser());
    if (!bbi || !bbi->isFromVarDecl())
      return nullptr;
    if (result)
      return nullptr;
    result = bbi;
  }
  return result;
}

SILValue VariableNameInferrer::findDebugInfoProvidingValueHelper(
    SILValue searchValue, ValueSet &visitedValues) {
  assert(searchValue);

  while (true) {
    assert(searchValue);

    // If we already visited the value, return SILValue(). This prevents issues
    // caused by looping phis. We treat this as a failure and visit the either
    // phi values.
    if (!visitedValues.insert(searchValue))
      return SILValue();

    LLVM_DEBUG(llvm::dbgs() << "Value: " << *searchValue);

    // Before we do anything, lets see if we have an explicit match due to a
    // debug_value use.
    if (auto *use = getAnyDebugUse(searchValue)) {
      if (auto debugVar = DebugVarCarryingInst(use->getUser())) {
        assert(debugVar.getKind() == DebugVarCarryingInst::Kind::DebugValue);
        variableNamePath.push_back(use->getUser());

        // We return the value, not the debug_info.
        return searchValue;
      }
    }

    // If we are in Ownership SSA, see if we have an owned value that has one
    // use, a move_value [var decl]. In such a case, check the move_value [var
    // decl] for a debug_value.
    //
    // This pattern comes up if we are asked to get a name for an apply that is
    // used to initialize a value. The name will not yet be associated with the
    // value so we have to compensate.
    //
    // NOTE: This is a heuristic. Feel free to tweak accordingly.
    if (auto *singleUse = searchValue->getSingleUse()) {
      if (auto *mvi = dyn_cast<MoveValueInst>(singleUse->getUser())) {
        if (mvi->isFromVarDecl()) {
          if (auto *debugUse = getAnyDebugUse(mvi)) {
            if (auto debugVar = DebugVarCarryingInst(debugUse->getUser())) {
              assert(debugVar.getKind() ==
                     DebugVarCarryingInst::Kind::DebugValue);
              variableNamePath.push_back(debugUse->getUser());

              // We return the value, not the debug_info.
              return searchValue;
            }
          }
        }
      }
    }

    if (auto *bbi = hasOnlyBorrowingNonDestroyUse(searchValue)) {
      if (auto *debugUse = getAnyDebugUse(bbi)) {
        if (auto debugVar = DebugVarCarryingInst(debugUse->getUser())) {
          assert(debugVar.getKind() == DebugVarCarryingInst::Kind::DebugValue);
          variableNamePath.push_back(debugUse->getUser());

          // We return the value, not the debug_info.
          return searchValue;
        }
      }
    }

    if (auto *allocInst = dyn_cast<AllocationInst>(searchValue)) {
      // If the instruction itself doesn't carry any variable info, see
      // whether it's copied from another place that does.
      auto allocInstHasInfo = [](AllocationInst *allocInst) {
        if (allocInst->getDecl())
          return true;
        auto debugVar = DebugVarCarryingInst(allocInst);
        return debugVar && debugVar.maybeGetName().has_value();
      };

      if (!allocInstHasInfo(allocInst)) {
        if (auto value = getRootValueForTemporaryAllocation(allocInst)) {
          searchValue = value;
          continue;
        }

        return SILValue();
      }

      variableNamePath.push_back(allocInst);
      return allocInst;
    }

    // If we have a store_borrow, always look at the dest. We are going to see
    // if we can determine if dest is a temporary alloc_stack.
    if (auto *sbi = dyn_cast<StoreBorrowInst>(searchValue)) {
      searchValue = sbi->getDest();
      continue;
    }

    if (auto *globalAddrInst = dyn_cast<GlobalAddrInst>(searchValue)) {
      variableNamePath.push_back(globalAddrInst);
      return globalAddrInst;
    }

    if (auto *oeInst = dyn_cast<OpenExistentialAddrInst>(searchValue)) {
      searchValue = oeInst->getOperand();
      continue;
    }

    if (auto *rei = dyn_cast<RefElementAddrInst>(searchValue)) {
      variableNamePath.push_back(rei);
      searchValue = rei->getOperand();
      continue;
    }

    if (auto *sei = dyn_cast<StructExtractInst>(searchValue)) {
      variableNamePath.push_back(sei);
      searchValue = sei->getOperand();
      continue;
    }

    if (auto *uedi = dyn_cast<UncheckedEnumDataInst>(searchValue)) {
      variableNamePath.push_back(uedi);
      searchValue = uedi->getOperand();
      continue;
    }

    if (auto *tei = dyn_cast<TupleExtractInst>(searchValue)) {
      variableNamePath.push_back(tei);
      searchValue = tei->getOperand();
      continue;
    }

    if (auto *sei = dyn_cast<StructElementAddrInst>(searchValue)) {
      variableNamePath.push_back(sei);
      searchValue = sei->getOperand();
      continue;
    }

    if (auto *tei = dyn_cast<TupleElementAddrInst>(searchValue)) {
      variableNamePath.push_back(tei);
      searchValue = tei->getOperand();
      continue;
    }

    if (auto *e = dyn_cast<UncheckedTakeEnumDataAddrInst>(searchValue)) {
      variableNamePath.push_back(e);
      searchValue = e->getOperand();
      continue;
    }

    // Enums only have a single possible parent and is used sometimes like a
    // transformation (e.x.: constructing an optional). We want to look through
    // them and add the case to the variableNamePath.
    if (auto *e = dyn_cast<EnumInst>(searchValue)) {
      if (e->hasOperand()) {
        variableNamePath.push_back(e);
        searchValue = e->getOperand();
        continue;
      }
    }

    if (auto *dti = dyn_cast_or_null<DestructureTupleInst>(
            searchValue->getDefiningInstruction())) {
      // Append searchValue, so we can find the specific tuple index.
      variableNamePath.push_back(searchValue);
      searchValue = dti->getOperand();
      continue;
    }

    if (auto *dsi = dyn_cast_or_null<DestructureStructInst>(
            searchValue->getDefiningInstruction())) {
      // Append searchValue, so we can find the specific struct field.
      variableNamePath.push_back(searchValue);
      searchValue = dsi->getOperand();
      continue;
    }

    if (auto *fArg = dyn_cast<SILFunctionArgument>(searchValue)) {
      if (fArg->getDecl()) {
        variableNamePath.push_back({fArg});
        return fArg;
      }
    }

    // If we have a phi argument, visit each of the incoming values and pick the
    // first one that gives us a name.
    if (auto *phiArg = dyn_cast<SILPhiArgument>(searchValue)) {
      if (auto *term = phiArg->getSingleTerminator()) {
        if (auto *swi = dyn_cast<SwitchEnumInst>(term)) {
          if (auto value = findDebugInfoProvidingValuePhiArg(swi->getOperand(),
                                                             visitedValues))
            return value;
        }
      }

      SmallVector<SILValue, 8> incomingValues;
      if (phiArg->getIncomingPhiValues(incomingValues)) {
        for (auto value : incomingValues) {
          if (auto resultValue =
                  findDebugInfoProvidingValuePhiArg(value, visitedValues))
            return resultValue;
        }
      }
    }

    auto getNamePathComponentFromCallee = [&](FullApplySite call) -> SILValue {
      // Use the name of the property being accessed if we can get to it.
      if (isa<FunctionRefBaseInst>(call.getCallee()) ||
          isa<MethodInst>(call.getCallee())) {
        if (call.getSubstCalleeType()->hasSelfParam()) {
          variableNamePath.push_back(
              call.getCallee()->getDefiningInstruction());
          return call.getSelfArgument();
        }

        return SILValue();
      }

      return SILValue();
    };

    // Read or modify accessor.
    if (auto bai = dyn_cast_or_null<BeginApplyInst>(
            searchValue->getDefiningInstruction())) {
      if (auto selfParam = getNamePathComponentFromCallee(bai)) {
        searchValue = selfParam;
        continue;
      }
    }

    if (options.contains(Flag::InferSelfThroughAllAccessors)) {
      if (auto *inst = searchValue->getDefiningInstruction()) {
        if (auto fas = FullApplySite::isa(inst)) {
          if (auto selfParam = getNamePathComponentFromCallee(fas)) {
            searchValue = selfParam;
            continue;
          }
        }
      }
    }

    // Addressor accessor.
    if (auto ptrToAddr =
            dyn_cast<PointerToAddressInst>(stripAccessMarkers(searchValue))) {
      // The addressor can either produce the raw pointer itself or an
      // `UnsafePointer` stdlib type wrapping it.
      ApplyInst *addressorInvocation;
      if (auto structExtract =
              dyn_cast<StructExtractInst>(ptrToAddr->getOperand())) {
        addressorInvocation = dyn_cast<ApplyInst>(structExtract->getOperand());
      } else {
        addressorInvocation = dyn_cast<ApplyInst>(ptrToAddr->getOperand());
      }

      if (addressorInvocation) {
        if (auto selfParam =
                getNamePathComponentFromCallee(addressorInvocation)) {
          searchValue = selfParam;
          continue;
        }
      }
    }

    // Look through a function conversion thunk if we have one.
    if (auto *pai = dyn_cast<PartialApplyInst>(searchValue)) {
      if (auto *fn = pai->getCalleeFunction()) {
        if (fn->isThunk() && ApplySite(pai).getNumArguments() == 1) {
          SILValue value = ApplySite(pai).getArgument(0);
          if (value->getType().isFunction()) {
            searchValue = value;
            continue;
          }
        }
      }
    }

    // Otherwise, try to see if we have a single value instruction we can look
    // through.
    if (isa<BeginBorrowInst>(searchValue) || isa<LoadInst>(searchValue) ||
        isa<LoadBorrowInst>(searchValue) || isa<BeginAccessInst>(searchValue) ||
        isa<MarkUnresolvedNonCopyableValueInst>(searchValue) ||
        isa<ProjectBoxInst>(searchValue) || isa<CopyValueInst>(searchValue) ||
        isa<ConvertFunctionInst>(searchValue) ||
        isa<MarkUninitializedInst>(searchValue) ||
        isa<CopyableToMoveOnlyWrapperAddrInst>(searchValue) ||
        isa<MoveOnlyWrapperToCopyableAddrInst>(searchValue) ||
        isa<MoveOnlyWrapperToCopyableValueInst>(searchValue) ||
        isa<CopyableToMoveOnlyWrapperValueInst>(searchValue) ||
        isa<EndInitLetRefInst>(searchValue)) {
      searchValue = cast<SingleValueInstruction>(searchValue)->getOperand(0);
      continue;
    }

    // Return SILValue() if we ever get to the bottom to signal we failed to
    // find anything.
    return SILValue();
  }
}

static StringRef getNameFromDecl(Decl *d) {
  if (d) {
    if (auto accessor = dyn_cast<AccessorDecl>(d)) {
      return accessor->getStorage()->getBaseName().userFacingName();
    }
    if (auto vd = dyn_cast<ValueDecl>(d)) {
      return vd->getBaseName().userFacingName();
    }
  }

  return "<unknown decl>";
}

void VariableNameInferrer::popSingleVariableName() {
  auto next = variableNamePath.pop_back_val();

  if (auto *inst = next.dyn_cast<SILInstruction *>()) {
    if (auto i = DebugVarCarryingInst(inst)) {
      resultingString += i.getName();
      return;
    }

    if (auto i = VarDeclCarryingInst(inst)) {
      resultingString += i.getName();
      return;
    }

    if (auto f = dyn_cast<FunctionRefBaseInst>(inst)) {
      if (auto dc = f->getInitiallyReferencedFunction()->getDeclContext()) {
        resultingString += getNameFromDecl(dc->getAsDecl());
        return;
      }

      resultingString += "<unknown decl>";
      return;
    }

    if (auto m = dyn_cast<MethodInst>(inst)) {
      resultingString += getNameFromDecl(m->getMember().getDecl());
      return;
    }

    if (auto *sei = dyn_cast<StructExtractInst>(inst)) {
      resultingString += getNameFromDecl(sei->getField());
      return;
    }

    if (auto *tei = dyn_cast<TupleExtractInst>(inst)) {
      llvm::raw_svector_ostream stream(resultingString);
      stream << tei->getFieldIndex();
      return;
    }

    if (auto *uedi = dyn_cast<UncheckedEnumDataInst>(inst)) {
      resultingString += getNameFromDecl(uedi->getElement());
      return;
    }

    if (auto *sei = dyn_cast<StructElementAddrInst>(inst)) {
      resultingString += getNameFromDecl(sei->getField());
      return;
    }

    if (auto *tei = dyn_cast<TupleElementAddrInst>(inst)) {
      llvm::raw_svector_ostream stream(resultingString);
      stream << tei->getFieldIndex();
      return;
    }

    if (auto *uedi = dyn_cast<UncheckedTakeEnumDataAddrInst>(inst)) {
      resultingString += getNameFromDecl(uedi->getElement());
      return;
    }

    if (auto *ei = dyn_cast<EnumInst>(inst)) {
      resultingString += getNameFromDecl(ei->getElement());
      return;
    }

    resultingString += "<unknown decl>";
    return;
  }

  auto value = next.get<SILValue>();
  if (auto *fArg = dyn_cast<SILFunctionArgument>(value)) {
    resultingString += fArg->getDecl()->getBaseName().userFacingName();
    return;
  }

  if (auto *dti = dyn_cast_or_null<DestructureTupleInst>(
          value->getDefiningInstruction())) {
    llvm::raw_svector_ostream stream(resultingString);
    stream << *dti->getIndexOfResult(value);
    return;
  }

  if (auto *dsi = dyn_cast_or_null<DestructureStructInst>(
          value->getDefiningInstruction())) {
    unsigned index = *dsi->getIndexOfResult(value);
    resultingString +=
        getNameFromDecl(dsi->getStructDecl()->getStoredProperties()[index]);
    return;
  }

  resultingString += "<unknown decl>";
}

void VariableNameInferrer::drainVariableNamePath() {
  if (variableNamePath.empty())
    return;

  // Walk backwards, constructing our string.
  while (true) {
    popSingleVariableName();

    if (variableNamePath.empty())
      return;

    resultingString += '.';
  }
}

std::optional<Identifier> VariableNameInferrer::inferName(SILValue value) {
  auto *fn = value->getFunction();
  if (!fn)
    return {};
  VariableNameInferrer::Options options;
  options |= VariableNameInferrer::Flag::InferSelfThroughAllAccessors;
  SmallString<64> resultingName;
  VariableNameInferrer inferrer(fn, options, resultingName);
  if (!inferrer.inferByWalkingUsesToDefsReturningRoot(value))
    return {};
  return fn->getASTContext().getIdentifier(resultingName);
}

std::optional<std::pair<Identifier, SILValue>>
VariableNameInferrer::inferNameAndRoot(SILValue value) {
  auto *fn = value->getFunction();
  if (!fn)
    return {};
  VariableNameInferrer::Options options;
  options |= VariableNameInferrer::Flag::InferSelfThroughAllAccessors;
  SmallString<64> resultingName;
  VariableNameInferrer inferrer(fn, options, resultingName);
  SILValue rootValue = inferrer.inferByWalkingUsesToDefsReturningRoot(value);
  if (!rootValue)
    return {};
  return {{fn->getASTContext().getIdentifier(resultingName), rootValue}};
}

//===----------------------------------------------------------------------===//
//                                MARK: Tests
//===----------------------------------------------------------------------===//

namespace swift::test {

// Arguments:
// - SILValue: value to emit a name for.
// Dumps:
// - The inferred name
// - The inferred value.
static FunctionTest VariableNameInferrerTests(
    "variable-name-inference", [](auto &function, auto &arguments, auto &test) {
      auto value = arguments.takeValue();
      SmallString<64> finalString;
      VariableNameInferrer::Options options;
      options |= VariableNameInferrer::Flag::InferSelfThroughAllAccessors;
      VariableNameInferrer inferrer(&function, options, finalString);
      SILValue rootValue =
          inferrer.inferByWalkingUsesToDefsReturningRoot(value);
      llvm::outs() << "Input Value: " << *value;
      if (!rootValue) {
        llvm::outs() << "Name: 'unknown'\nRoot: 'unknown'\n";
        return;
      }
      llvm::outs() << "Name: '" << finalString << "'\nRoot: " << rootValue;
    });
} // namespace swift::test
