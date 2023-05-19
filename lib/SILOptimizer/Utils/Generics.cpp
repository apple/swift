//===--- Generics.cpp ---- Utilities for transforming generics ------------===//
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

#define DEBUG_TYPE "generic-specializer"

#include "swift/SILOptimizer/Utils/Generics.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/TypeMatcher.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Statistic.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/OptimizationRemark.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SILOptimizer/Utils/GenericCloner.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/SpecializationMangler.h"
#include "swift/Serialization/SerializedSILLoader.h"
#include "swift/Strings.h"

using namespace swift;

STATISTIC(NumPreventedGenericSpecializationLoops,
          "# of prevented infinite generic specializations loops");

STATISTIC(NumPreventedTooComplexGenericSpecializations,
          "# of prevented generic specializations with too complex "
          "generic type parameters");

/// Set to true to enable the support for partial specialization.
llvm::cl::opt<bool> EnablePartialSpecialization(
    "sil-partial-specialization", llvm::cl::init(false),
    llvm::cl::desc("Enable partial specialization of generics"));

/// If set, then generic specialization tries to specialize using
/// all substitutions, even if they the replacement types are generic.
llvm::cl::opt<bool> SupportGenericSubstitutions(
    "sil-partial-specialization-with-generic-substitutions",
    llvm::cl::init(false),
    llvm::cl::desc("Enable partial specialization with generic substitutions"));

/// Set to true to print detected infinite generic specialization loops that
/// were prevented.
llvm::cl::opt<bool> PrintGenericSpecializationLoops(
    "sil-print-generic-specialization-loops", llvm::cl::init(false),
    llvm::cl::desc("Print detected infinite generic specialization loops that "
                   "were prevented"));

llvm::cl::opt<bool> VerifyFunctionsAfterSpecialization(
    "sil-generic-verify-after-specialization", llvm::cl::init(false),
    llvm::cl::desc(
        "Verify functions after they are specialized "
        "'PrettyStackTraceFunction'-ing the original function if we fail."));

llvm::cl::opt<bool> DumpFunctionsAfterSpecialization(
    "sil-generic-dump-functions-after-specialization", llvm::cl::init(false));

static bool OptimizeGenericSubstitutions = false;

/// Max depth of a type which can be processed by the generic
/// specializer.
/// E.g. the depth of Array<Array<Array<T>>> is 3.
/// No specializations will be produced, if any of generic parameters contains
/// a bound generic type with the depth higher than this threshold
static const unsigned TypeDepthThreshold = 50;
/// Set the width threshold rather high, because some projects uses very wide
/// tuples to model fixed size arrays.
static const unsigned TypeWidthThreshold = 2000;

/// Compute the width and the depth of a type.
/// We compute both, because some pathological test-cases result in very
/// wide types and some others result in very deep types. It is important
/// to bail as soon as we hit the threshold on any of both dimensions to
/// prevent compiler hangs and crashes.
static std::pair<unsigned, unsigned> getTypeDepthAndWidth(Type t) {
  unsigned Depth = 0;
  unsigned Width = 0;
  if (auto *BGT = t->getAs<BoundGenericType>()) {
    auto *NTD = BGT->getNominalOrBoundGenericNominal();
    if (NTD) {
      auto StoredProperties = NTD->getStoredProperties();
      Width += StoredProperties.size();
    }
    ++Depth;
    unsigned MaxTypeDepth = 0;
    auto GenericArgs = BGT->getGenericArgs();
    for (auto Ty : GenericArgs) {
      unsigned TypeWidth;
      unsigned TypeDepth;
      std::tie(TypeDepth, TypeWidth) = getTypeDepthAndWidth(Ty);
      if (TypeDepth > MaxTypeDepth)
        MaxTypeDepth = TypeDepth;
      Width += TypeWidth;
    }
    Depth += MaxTypeDepth;
    return std::make_pair(Depth, Width);
  }

  if (auto *TupleTy = t->getAs<TupleType>()) {
    Width += TupleTy->getNumElements();
    ++Depth;
    unsigned MaxTypeDepth = 0;
    auto ElementTypes = TupleTy->getElementTypes();
    for (auto Ty : ElementTypes) {
      unsigned TypeWidth;
      unsigned TypeDepth;
      std::tie(TypeDepth, TypeWidth) = getTypeDepthAndWidth(Ty);
      if (TypeDepth > MaxTypeDepth)
        MaxTypeDepth = TypeDepth;
      Width += TypeWidth;
    }
    Depth += MaxTypeDepth;
    return std::make_pair(Depth, Width);
  }

  if (auto *FnTy = t->getAs<SILFunctionType>()) {
    ++Depth;
    unsigned MaxTypeDepth = 0;
    auto Params = FnTy->getParameters();
    Width += Params.size();
    for (auto Param : Params) {
      unsigned TypeWidth;
      unsigned TypeDepth;
      std::tie(TypeDepth, TypeWidth) =
        getTypeDepthAndWidth(Param.getInterfaceType());
      if (TypeDepth > MaxTypeDepth)
        MaxTypeDepth = TypeDepth;
      Width += TypeWidth;
    }
    auto Results = FnTy->getResults();
    Width += Results.size();
    for (auto Result : Results) {
      unsigned TypeWidth;
      unsigned TypeDepth;
      std::tie(TypeDepth, TypeWidth) =
        getTypeDepthAndWidth(Result.getInterfaceType());
      if (TypeDepth > MaxTypeDepth)
        MaxTypeDepth = TypeDepth;
      Width += TypeWidth;
    }
    if (FnTy->hasErrorResult()) {
      Width += 1;
      unsigned TypeWidth;
      unsigned TypeDepth;
      std::tie(TypeDepth, TypeWidth) =
          getTypeDepthAndWidth(FnTy->getErrorResult().getInterfaceType());
      if (TypeDepth > MaxTypeDepth)
        MaxTypeDepth = TypeDepth;
      Width += TypeWidth;
    }
    Depth += MaxTypeDepth;
    return std::make_pair(Depth, Width);
  }

  if (auto *FnTy = t->getAs<FunctionType>()) {
    ++Depth;
    unsigned MaxTypeDepth = 0;
    auto Params = FnTy->getParams();
    Width += Params.size();
    for (auto &Param : Params) {
      unsigned TypeWidth;
      unsigned TypeDepth;
      std::tie(TypeDepth, TypeWidth) = getTypeDepthAndWidth(Param.getParameterType());
      if (TypeDepth > MaxTypeDepth)
        MaxTypeDepth = TypeDepth;
      Width += TypeWidth;
    }
    unsigned TypeWidth;
    unsigned TypeDepth;
    std::tie(TypeDepth, TypeWidth) = getTypeDepthAndWidth(FnTy->getResult());
    if (TypeDepth > MaxTypeDepth)
      MaxTypeDepth = TypeDepth;
    Width += TypeWidth;
    Depth += MaxTypeDepth;
    return std::make_pair(Depth, Width);
  }

  if (auto *MT = t->getAs<MetatypeType>()) {
    Depth += 1;
    unsigned TypeWidth;
    unsigned TypeDepth;
    std::tie(TypeDepth, TypeWidth) = getTypeDepthAndWidth(MT->getInstanceType());
    Width += TypeWidth;
    Depth += TypeDepth;
    return std::make_pair(Depth, Width);
  }

  return std::make_pair(Depth, Width);
}

static bool isTypeTooComplex(Type t) {
  unsigned TypeWidth;
  unsigned TypeDepth;
  std::tie(TypeDepth, TypeWidth) = getTypeDepthAndWidth(t);
  return TypeWidth >= TypeWidthThreshold || TypeDepth >= TypeDepthThreshold;
}

namespace {

/// A helper class used to check whether one type is structurally contained
/// the other type either completely or partially.
class TypeComparator : public TypeMatcher<TypeComparator> {
  bool IsContained = false;

public:
  bool isEqual(CanType T1, CanType T2) { return T1 == T2; }
  /// Check whether the type T1 is different from T2 and contained in the type
  /// T2.
  bool isStrictlyContainedIn(CanType T1, CanType T2) {
    if (isEqual(T1, T2))
      return false;
    return T2.findIf([&T1, this](Type T) -> bool {
      return isEqual(T->getCanonicalType(), T1);
    });
  }

  /// Check whether the type T1 is strictly or partially contained in the type
  /// T2.
  /// Partially contained means that if you drop the common structural "prefix"
  /// of T1 and T2 and get T1' and T2' then T1' is strictly contained in T2'.
  bool isPartiallyContainedIn(CanType T1, CanType T2) {
    if (isStrictlyContainedIn(T1, T2))
      return true;
    match(T1, T2);
    return IsContained;
  }

  /// This method is invoked aftre skipping a common prefix of two types,
  /// when a structural difference is found.
  bool mismatch(TypeBase *firstType, TypeBase *secondType,
                Type sugaredFirstType) {
    auto firstCanType = firstType->getCanonicalType();
    auto secondCanType = secondType->getCanonicalType();
    if (isEqual(firstCanType, secondCanType))
      return false;
    if (isStrictlyContainedIn(firstCanType, secondCanType)) {
      IsContained = true;
      return false;
    }
    return false;
  }
};

class TypeReplacements {
private:
  Optional<SILType> resultType;
  llvm::MapVector<unsigned, CanType> indirectResultTypes;
  llvm::MapVector<unsigned, CanType> paramTypeReplacements;
  llvm::MapVector<unsigned, CanType> yieldTypeReplacements;

public:
  Optional<SILType> getResultType() const { return resultType; }

  void setResultType(SILType type) { resultType = type; }

  bool hasResultType() const { return resultType.has_value(); }

  const llvm::MapVector<unsigned, CanType> &getIndirectResultTypes() const {
    return indirectResultTypes;
  }

  void addIndirectResultType(unsigned index, CanType type) {
    indirectResultTypes.insert(std::make_pair(index, type));
  }

  bool hasIndirectResultTypes() const { return !indirectResultTypes.empty(); }

  const llvm::MapVector<unsigned, CanType> &getParamTypeReplacements() const {
    return paramTypeReplacements;
  }

  void addParameterTypeReplacement(unsigned index, CanType type) {
    paramTypeReplacements.insert(std::make_pair(index, type));
  }

  bool hasParamTypeReplacements() const {
    return !paramTypeReplacements.empty();
  }

  const llvm::MapVector<unsigned, CanType> &getYieldTypeReplacements() const {
    return yieldTypeReplacements;
  }

  void addYieldTypeReplacement(unsigned index, CanType type) {
    yieldTypeReplacements.insert(std::make_pair(index, type));
  }

  bool hasYieldTypeReplacements() const {
    return !yieldTypeReplacements.empty();
  }

  bool hasTypeReplacements() const {
    return hasResultType() || hasParamTypeReplacements() ||
           hasIndirectResultTypes() || hasYieldTypeReplacements();
  }
};

class SpecializedFunction {
private:
  SILFunction *fn;
  TypeReplacements typeReplacements;

public:
  SpecializedFunction() : fn(nullptr) {}
  SpecializedFunction(SILFunction *fn) : fn(fn) {}

  SILFunction *getFunction() { return fn; }

  void setFunction(SILFunction *newFn) { fn = newFn; }

  bool hasFunction() { return fn != nullptr; }

  TypeReplacements &getTypeReplacements() { return typeReplacements; }

  void addParameterTypeReplacement(unsigned index, CanType type) {
    typeReplacements.addParameterTypeReplacement(index, type);
  }

  void addYieldTypeReplacement(unsigned index, CanType type) {
    typeReplacements.addYieldTypeReplacement(index, type);
  }

  bool hasResultType() const { return typeReplacements.hasResultType(); }

  void setResultType(SILType type) { typeReplacements.setResultType(type); }

  bool hasIndirectResultTypes() const {
    return typeReplacements.hasIndirectResultTypes();
  }

  void addIndirectResultType(unsigned index, CanType type) {
    typeReplacements.addIndirectResultType(index, type);
  }

  bool hasTypeReplacements() const {
    return typeReplacements.hasTypeReplacements();
  }

  SILFunction *operator->() { return fn; }

  void computeTypeReplacements(const ApplySite &apply);

  operator bool() { return fn != nullptr; }
};

} // anonymous namespace

void SpecializedFunction::computeTypeReplacements(const ApplySite &apply) {
  auto fnType = fn->getLoweredFunctionType();
  if (fnType != apply.getSubstCalleeType()) {
    auto &M = fn->getModule();
    auto expansion = fn->getTypeExpansionContext();
    auto calleeTy = apply.getSubstCalleeType();
    auto substConv = apply.getSubstCalleeConv();
    auto resultType =
        fn->getConventions().getSILResultType(fn->getTypeExpansionContext());
    SmallVector<SILResultInfo, 4> indirectResults(substConv.getIndirectSILResults());

    for (auto pair : llvm::enumerate(apply.getArgumentOperands())) {
      if (pair.index() < substConv.getSILArgIndexOfFirstParam()) {
        auto formalIndex = substConv.getIndirectFormalResultIndexForSILArg(pair.index());
        auto fnResult = indirectResults[formalIndex];
        if (fnResult.isFormalIndirect()) {
          // FIXME: properly get the type
          auto indirectResultTy = M.getASTContext().getAnyObjectType();  //fnResult.getReturnValueType(M, fnType, expansion);
          addIndirectResultType(formalIndex, indirectResultTy);
        }

        continue;
      }

      unsigned paramIdx =
          pair.index() - substConv.getSILArgIndexOfFirstParam();

      auto newParamType = fnType->getParameters()[paramIdx].getArgumentType(
          M, fnType, expansion);
      auto oldParamType = calleeTy->getParameters()[paramIdx].getArgumentType(
          M, fnType, expansion);
      if (newParamType != oldParamType) {
        addParameterTypeReplacement(paramIdx, newParamType);
      }
    }

    auto newConv = fn->getConventions();
    for (auto pair : llvm::enumerate(substConv.getYields())) {
      auto index = pair.index();
      auto newType =
          newConv.getYields()[index].getYieldValueType(M, fnType, expansion);
      auto oldType = pair.value().getYieldValueType(
          M, calleeTy, apply.getCalleeFunction()->getTypeExpansionContext());

      if (oldType != newType) {
        addYieldTypeReplacement(index, oldType);
      }
    }

    if (resultType != apply.getType().getObjectType()) {
      setResultType(apply.getType().getObjectType());
    }
  }
}

/// Checks if a second substitution map is an expanded version of
/// the first substitution map.
/// This is the case if at least one of the substitution type in Subs2 is
/// "bigger" than the corresponding substitution type in Subs1.
/// Type T2 is "smaller" than type T1 if T2 is structurally contained in T1.
static bool growingSubstitutions(SubstitutionMap Subs1,
                                 SubstitutionMap Subs2) {
  auto Replacements1 = Subs1.getReplacementTypes();
  auto Replacements2 = Subs2.getReplacementTypes();
  assert(Replacements1.size() == Replacements2.size());
  TypeComparator TypeCmp;
  // Perform component-wise comparisons for substitutions.
  for (unsigned idx : indices(Replacements1)) {
    auto Type1 = Replacements1[idx]->getCanonicalType();
    auto Type2 = Replacements2[idx]->getCanonicalType();
    // If types are the same, the substitution type does not grow.
    if (TypeCmp.isEqual(Type2, Type1))
      continue;
    // If the new substitution type is getting smaller, the
    // substitution type does not grow.
    if (TypeCmp.isPartiallyContainedIn(Type2, Type1))
      continue;
    if (TypeCmp.isPartiallyContainedIn(Type1, Type2)) {
      LLVM_DEBUG(llvm::dbgs() << "Type:\n"; Type1.dump(llvm::dbgs());
                 llvm::dbgs() << "is (partially) contained in type:\n";
                 Type2.dump(llvm::dbgs());
                 llvm::dbgs() << "Replacements[" << idx
                              << "] has got bigger since last time.\n");
      return true;
    }
    // None of the types is contained in the other type.
    // They are not comparable in this sense.
  }

  // The substitution list is not growing.
  return false;
}

/// Checks whether specializing a given generic apply would create an infinite
/// cycle in the generic specializations graph. This can be the case if there is
/// a loop in the specialization graph and generic parameters at each iteration
/// of such a loop are getting bigger and bigger.
/// The specialization graph is represented by means of SpecializationInformation.
/// We use this meta-information about specializations to detect cycles in this
/// graph.
static bool createsInfiniteSpecializationLoop(ApplySite Apply) {
  if (!Apply)
    return false;
  auto *Callee = Apply.getCalleeFunction();
  SILFunction *Caller = nullptr;
  Caller = Apply.getFunction();
  int numAcceptedCycles = 1;

  // Name of the function to be specialized.
  auto GenericFunc = Callee;

  LLVM_DEBUG(llvm::dbgs() << "\n\n\nChecking for a specialization cycle:\n"
                          << "Caller: " << Caller->getName() << "\n"
                          << "Callee: " << Callee->getName() << "\n";
             llvm::dbgs() << "Substitutions:\n";
             Apply.getSubstitutionMap().dump(llvm::dbgs()));

  auto *CurSpecializationInfo = Apply.getSpecializationInfo();
  if (CurSpecializationInfo) {
    LLVM_DEBUG(llvm::dbgs() << "Scan call-site's history\n");
  } else if (Caller->isSpecialization()) {
    CurSpecializationInfo = Caller->getSpecializationInfo();
    LLVM_DEBUG(llvm::dbgs() << "Scan caller's specialization history\n");
  }

  while (CurSpecializationInfo) {
    LLVM_DEBUG(llvm::dbgs() << "Current caller is a specialization:\n"
                            << "Caller: "
                            << CurSpecializationInfo->getCaller()->getName()
                            << "\n"
                            << "Parent: "
                            << CurSpecializationInfo->getParent()->getName()
                            << "\n";
               llvm::dbgs() << "Substitutions:\n";
               for (auto Replacement :
                     CurSpecializationInfo->getSubstitutions()
                       .getReplacementTypes()) {
                 Replacement->dump(llvm::dbgs());
               });

    if (CurSpecializationInfo->getParent() == GenericFunc) {
      LLVM_DEBUG(llvm::dbgs() << "Found a call graph loop, checking "
                                 "substitutions\n");
      // Consider if components of the substitution list gets bigger compared to
      // the previously seen specialization of the same generic function.
      if (growingSubstitutions(CurSpecializationInfo->getSubstitutions(),
                               Apply.getSubstitutionMap())) {
        LLVM_DEBUG(llvm::dbgs() << "Found a generic specialization loop!\n");

        // Accept a cycles up to a limit. This is necessary to generate
        // efficient code for some library functions, like compactMap, which
        // contain small specialization cycles.
        if (numAcceptedCycles == 0)
          return true;
        --numAcceptedCycles;
      }
    }

    // Get the next element of the specialization history.
    auto *CurCaller = CurSpecializationInfo->getCaller();
    CurSpecializationInfo = nullptr;
    if (!CurCaller)
      break;
    LLVM_DEBUG(llvm::dbgs() << "\nCurrent caller is: " << CurCaller->getName()
                            << "\n");
    if (!CurCaller->isSpecialization())
      break;
    CurSpecializationInfo = CurCaller->getSpecializationInfo();
  }

  assert(!CurSpecializationInfo);
  LLVM_DEBUG(llvm::dbgs() << "Stop the scan: Current caller is not a "
                             "specialization\n");
  return false;
}

// =============================================================================
// ReabstractionInfo
// =============================================================================

static bool shouldNotSpecialize(SILFunction *Callee, SILFunction *Caller,
                                SubstitutionMap Subs = {}) {
  if (Callee->hasSemanticsAttr(semantics::OPTIMIZE_SIL_SPECIALIZE_GENERIC_NEVER))
    return true;

  if (Caller &&
      Caller->getEffectiveOptimizationMode() == OptimizationMode::ForSize &&
      Callee->hasSemanticsAttr(semantics::OPTIMIZE_SIL_SPECIALIZE_GENERIC_SIZE_NEVER)) {
    return true;
  }


  if (Subs.hasAnySubstitutableParams() &&
      Callee->hasSemanticsAttr(semantics::OPTIMIZE_SIL_SPECIALIZE_GENERIC_PARTIAL_NEVER))
    return true;

  return false;
}

/// Prepares the ReabstractionInfo object for further processing and checks
/// if the current function can be specialized at all.
/// Returns false, if the current function cannot be specialized.
/// Returns true otherwise.
bool ReabstractionInfo::prepareAndCheck(ApplySite Apply, SILFunction *Callee,
                                        SubstitutionMap ParamSubs,
                                        OptRemark::Emitter *ORE) {
  assert(ParamSubs.hasAnySubstitutableParams());

  if (shouldNotSpecialize(Callee, Apply ? Apply.getFunction() : nullptr))
    return false;

  SpecializedGenericEnv = nullptr;
  SpecializedGenericSig = nullptr;
  auto CalleeGenericSig = Callee->getLoweredFunctionType()
                                ->getInvocationGenericSignature();
  auto CalleeGenericEnv = Callee->getGenericEnvironment();

  this->Callee = Callee;
  this->Apply = Apply;

  // Get the original substitution map.
  CalleeParamSubMap = ParamSubs;

  using namespace OptRemark;
  // We do not support partial specialization.
  if (!EnablePartialSpecialization && CalleeParamSubMap.hasArchetypes()) {
    LLVM_DEBUG(llvm::dbgs() <<"    Partial specialization is not supported.\n");
    LLVM_DEBUG(ParamSubs.dump(llvm::dbgs()));
    return false;
  }

  // Perform some checks to see if we need to bail.
  if (CalleeParamSubMap.hasDynamicSelf()) {
    REMARK_OR_DEBUG(ORE, [&]() {
      return RemarkMissed("DynamicSelf", *Apply.getInstruction())
             << IndentDebug(4) << "Cannot specialize with dynamic self";
    });
    return false;
  }

  // Check if the substitution contains any generic types that are too deep.
  // If this is the case, bail to avoid the explosion in the number of 
  // generated specializations.
  for (auto Replacement : ParamSubs.getReplacementTypes()) {
    if (isTypeTooComplex(Replacement)) {
      REMARK_OR_DEBUG(ORE, [&]() {
        return RemarkMissed("TypeTooDeep", *Apply.getInstruction())
               << IndentDebug(4)
               << "Cannot specialize because the generic type is too deep";
      });
      ++NumPreventedTooComplexGenericSpecializations;
      return false;
    }
  }

  // Check if we have substitutions which replace generic type parameters with
  // concrete types or unbound generic types.
  bool HasConcreteGenericParams = false;
  bool HasNonArchetypeGenericParams = false;
  HasUnboundGenericParams = false;

  CalleeGenericSig->forEachParam([&](GenericTypeParamType *GP, bool Canonical) {
    if (!Canonical)
      return;

    // Check only the substitutions for the generic parameters.
    // Ignore any dependent types, etc.
    auto Replacement = Type(GP).subst(CalleeParamSubMap);
    if (!Replacement->is<ArchetypeType>())
      HasNonArchetypeGenericParams = true;

    if (Replacement->hasArchetype()) {
      HasUnboundGenericParams = true;
      // Check if the replacement is an archetype which is more specific
      // than the corresponding archetype in the original generic signature.
      // If this is the case, then specialization makes sense, because
      // it would produce something more specific.
      if (CalleeGenericEnv) {
        if (auto Archetype = Replacement->getAs<ArchetypeType>()) {
          auto OrigArchetype =
              CalleeGenericEnv->mapTypeIntoContext(GP)->castTo<ArchetypeType>();
          if (Archetype->requiresClass() && !OrigArchetype->requiresClass())
            HasNonArchetypeGenericParams = true;
          if (Archetype->getLayoutConstraint() &&
              !OrigArchetype->getLayoutConstraint())
            HasNonArchetypeGenericParams = true;
        }
      }
    } else {
      HasConcreteGenericParams = true;
    }
  });

  if (HasUnboundGenericParams) {
    // Bail if we cannot specialize generic substitutions, but all substitutions
    // were generic.
    if (!HasConcreteGenericParams && !SupportGenericSubstitutions) {
      LLVM_DEBUG(llvm::dbgs() << "    Partial specialization is not supported "
                            "if all substitutions are generic.\n");
      LLVM_DEBUG(ParamSubs.dump(llvm::dbgs()));
      return false;
    }

    if (!HasNonArchetypeGenericParams && !HasConcreteGenericParams) {
      LLVM_DEBUG(llvm::dbgs() << "    Partial specialization is not supported "
                            "if all substitutions are archetypes.\n");
      LLVM_DEBUG(ParamSubs.dump(llvm::dbgs()));
      return false;
    }

    // We need a generic environment for the partial specialization.
    if (!CalleeGenericEnv)
      return false;

    // Bail if the callee should not be partially specialized.
    if (shouldNotSpecialize(Callee, Apply.getFunction(), ParamSubs))
      return false;
  }

  // Check if specializing this call site would create in an infinite generic
  // specialization loop.
  if (createsInfiniteSpecializationLoop(Apply)) {
    REMARK_OR_DEBUG(ORE, [&]() {
      return RemarkMissed("SpecializationLoop", *Apply.getInstruction())
             << IndentDebug(4)
             << "Generic specialization is not supported if it would result in "
                "a generic specialization of infinite depth. Callee "
             << NV("Callee", Callee)
             << " occurs multiple times on the call chain";
    });
    if (PrintGenericSpecializationLoops)
      llvm::errs() << "Detected and prevented an infinite "
                      "generic specialization loop for callee: "
                   << Callee->getName() << '\n';
    ++NumPreventedGenericSpecializationLoops;
    return false;
  }

  return true;
}

bool ReabstractionInfo::canBeSpecialized(ApplySite Apply, SILFunction *Callee,
                                         SubstitutionMap ParamSubs) {
  ReabstractionInfo ReInfo;
  return ReInfo.prepareAndCheck(Apply, Callee, ParamSubs);
}

ReabstractionInfo::ReabstractionInfo(
    ModuleDecl *targetModule, bool isWholeModule, ApplySite Apply,
    SILFunction *Callee, SubstitutionMap ParamSubs, IsSerialized_t Serialized,
    bool ConvertIndirectToDirect, bool dropMetatypeArgs, OptRemark::Emitter *ORE)
    : ConvertIndirectToDirect(ConvertIndirectToDirect),
      dropMetatypeArgs(dropMetatypeArgs),
      TargetModule(targetModule), isWholeModule(isWholeModule),
      Serialized(Serialized) {
  if (!prepareAndCheck(Apply, Callee, ParamSubs, ORE))
    return;

  SILFunction *Caller = nullptr;
  if (Apply)
    Caller = Apply.getFunction();

  if (!EnablePartialSpecialization || !HasUnboundGenericParams) {
    // Fast path for full specializations.
    performFullSpecializationPreparation(Callee, ParamSubs);
  } else {
    performPartialSpecializationPreparation(Caller, Callee, ParamSubs);
  }
  verify();
  if (SpecializedGenericSig) {
    LLVM_DEBUG(llvm::dbgs() << "\n\nPartially specialized types for function: "
                            << Callee->getName() << "\n\n";
               llvm::dbgs() << "Original generic function type:\n"
                            << Callee->getLoweredFunctionType() << "\n"
                            << "Partially specialized generic function type:\n"
                            << SpecializedType << "\n\n");
  }

  // Some correctness checks.
  auto SpecializedFnTy = getSpecializedType();
  auto SpecializedSubstFnTy = SpecializedFnTy;

  if (SpecializedFnTy->isPolymorphic() &&
      !getCallerParamSubstitutionMap().empty()) {
    auto CalleeFnTy = Callee->getLoweredFunctionType();
    assert(CalleeFnTy->isPolymorphic());
    auto CalleeSubstFnTy = CalleeFnTy->substGenericArgs(
        Callee->getModule(), getCalleeParamSubstitutionMap(),
        getResilienceExpansion());
    assert(!CalleeSubstFnTy->isPolymorphic() &&
           "Substituted callee type should not be polymorphic");
    assert(!CalleeSubstFnTy->hasTypeParameter() &&
           "Substituted callee type should not have type parameters");

    SpecializedSubstFnTy = SpecializedFnTy->substGenericArgs(
        Callee->getModule(), getCallerParamSubstitutionMap(),
        getResilienceExpansion());

    assert(!SpecializedSubstFnTy->isPolymorphic() &&
           "Substituted callee type should not be polymorphic");
    assert(!SpecializedSubstFnTy->hasTypeParameter() &&
           "Substituted callee type should not have type parameters");

    auto SpecializedCalleeSubstFnTy =
        createSpecializedType(CalleeSubstFnTy, Callee->getModule());

    if (SpecializedSubstFnTy != SpecializedCalleeSubstFnTy) {
      llvm::dbgs() << "SpecializedFnTy:\n" << SpecializedFnTy << "\n";
      llvm::dbgs() << "SpecializedSubstFnTy:\n" << SpecializedSubstFnTy << "\n";
      getCallerParamSubstitutionMap().getCanonical().dump(llvm::dbgs());
      llvm::dbgs() << "\n\n";

      llvm::dbgs() << "CalleeFnTy:\n" << CalleeFnTy << "\n";
      llvm::dbgs() << "SpecializedCalleeSubstFnTy:\n" << SpecializedCalleeSubstFnTy << "\n";
      ParamSubs.getCanonical().dump(llvm::dbgs());
      llvm::dbgs() << "\n\n";
      assert(SpecializedSubstFnTy == SpecializedCalleeSubstFnTy &&
             "Substituted function types should be the same");
    }
  }

  // If the new type is the same, there is nothing to do and
  // no specialization should be performed.
  if (getSubstitutedType() == Callee->getLoweredFunctionType()) {
    LLVM_DEBUG(llvm::dbgs() << "The new specialized type is the same as "
                               "the original type. Don't specialize!\n";
               llvm::dbgs() << "The type is: " << getSubstitutedType() << "\n");
    SpecializedType = CanSILFunctionType();
    SubstitutedType = CanSILFunctionType();
    SpecializedGenericSig = nullptr;
    SpecializedGenericEnv = nullptr;
    return;
  }

  if (SpecializedGenericSig) {
    // It is a partial specialization.
    LLVM_DEBUG(llvm::dbgs() << "Specializing the call:\n";
               Apply.getInstruction()->dumpInContext();
               llvm::dbgs() << "\n\nPartially specialized types for function: "
                            << Callee->getName() << "\n\n";
               llvm::dbgs() << "Callee generic function type:\n"
                            << Callee->getLoweredFunctionType() << "\n\n";
               llvm::dbgs() << "Callee's call substitution:\n";
               getCalleeParamSubstitutionMap().getCanonical().dump(llvm::dbgs());

               llvm::dbgs() << "Partially specialized generic function type:\n"
                            << getSpecializedType() << "\n\n";
               llvm::dbgs() << "\nSpecialization call substitution:\n";
               getCallerParamSubstitutionMap().getCanonical().dump(llvm::dbgs());
               );
  }
}

bool ReabstractionInfo::canBeSpecialized() const {
  return getSpecializedType();
}

bool ReabstractionInfo::isFullSpecialization() const {
  return !getCalleeParamSubstitutionMap().hasArchetypes();
}

bool ReabstractionInfo::isPartialSpecialization() const {
  return getCalleeParamSubstitutionMap().hasArchetypes();
}

void ReabstractionInfo::createSubstitutedAndSpecializedTypes() {
  auto &M = Callee->getModule();

  // Find out how the function type looks like after applying the provided
  // substitutions.
  if (!SubstitutedType) {
    SubstitutedType = createSubstitutedType(Callee, CallerInterfaceSubs,
                                            HasUnboundGenericParams);
  }
  assert(!SubstitutedType->hasArchetype() &&
         "Substituted function type should not contain archetypes");

  // Check which parameters and results can be converted from
  // indirect to direct ones.
  NumFormalIndirectResults = SubstitutedType->getNumIndirectFormalResults();
  unsigned NumArgs = NumFormalIndirectResults +
    SubstitutedType->getParameters().size();
  Conversions.resize(NumArgs);
  TrivialArgs.resize(NumArgs);
  droppedMetatypeArgs.resize(NumArgs);

  SILFunctionConventions substConv(SubstitutedType, M);
  TypeExpansionContext resilienceExp = getResilienceExpansion();
  TypeExpansionContext minimalExp(ResilienceExpansion::Minimal,
                                  TargetModule, isWholeModule);

  if (SubstitutedType->getNumDirectFormalResults() == 0) {
    // The original function has no direct result yet. Try to convert the first
    // indirect result to a direct result.
    // TODO: We could also convert multiple indirect results by returning a
    // tuple type and created tuple_extract instructions at the call site.
    unsigned IdxForResult = 0;
    for (SILResultInfo RI : SubstitutedType->getIndirectFormalResults()) {
      assert(RI.isFormalIndirect());

      TypeCategory tc = getReturnTypeCategory(RI, substConv, resilienceExp);
      if (tc != NotLoadable) {
        Conversions.set(IdxForResult);
        if (tc == LoadableAndTrivial)
          TrivialArgs.set(IdxForResult);
        if (resilienceExp != minimalExp &&
            getReturnTypeCategory(RI, substConv, minimalExp) == NotLoadable) {
          hasConvertedResilientParams = true;
        }
        break;
      }
      ++IdxForResult;
    }
  }

  // Try to convert indirect incoming parameters to direct parameters.
  unsigned IdxForParam = NumFormalIndirectResults;
  for (SILParameterInfo PI : SubstitutedType->getParameters()) {
    auto IdxToInsert = IdxForParam;
    ++IdxForParam;

    TypeCategory tc = getParamTypeCategory(PI, substConv, resilienceExp);
    if (tc == NotLoadable)
      continue;

    switch (PI.getConvention()) {
    case ParameterConvention::Indirect_In:
    case ParameterConvention::Indirect_In_Guaranteed:
      Conversions.set(IdxToInsert);
      if (tc == LoadableAndTrivial)
        TrivialArgs.set(IdxToInsert);
      if (resilienceExp != minimalExp &&
          getParamTypeCategory(PI, substConv, minimalExp) == NotLoadable) {
        hasConvertedResilientParams = true;
      }
      break;
    case ParameterConvention::Indirect_Inout:
    case ParameterConvention::Indirect_InoutAliasable:
    case ParameterConvention::Pack_Inout:
    case ParameterConvention::Pack_Owned:
    case ParameterConvention::Pack_Guaranteed:
      break;
      
    case ParameterConvention::Direct_Owned:
    case ParameterConvention::Direct_Unowned:
    case ParameterConvention::Direct_Guaranteed: {
      CanType ty = PI.getInterfaceType();
      if (dropMetatypeArgs && isa<MetatypeType>(ty) && !ty->hasArchetype())
        droppedMetatypeArgs.set(IdxToInsert);
      break;
    }
    }
  }

  // Produce a specialized type, which is the substituted type with
  // the parameters/results passing conventions adjusted according
  // to the conversions selected above.
  SpecializedType = createSpecializedType(SubstitutedType, M);
}

ReabstractionInfo::TypeCategory ReabstractionInfo::
getReturnTypeCategory(const SILResultInfo &RI,
                  const SILFunctionConventions &substConv,
                  TypeExpansionContext typeExpansion) {
  auto &M = Callee->getModule();
  auto ResultTy = substConv.getSILType(RI, typeExpansion);
  ResultTy = Callee->mapTypeIntoContext(ResultTy);
  auto &TL = M.Types.getTypeLowering(ResultTy, typeExpansion);

  if (!TL.isLoadable())
    return NotLoadable;
    
  if (RI.getReturnValueType(M, SubstitutedType, typeExpansion)
        ->isVoid())
    return NotLoadable;

  if (!shouldExpand(M, ResultTy))
    return NotLoadable;
  
  return TL.isTrivial() ? LoadableAndTrivial : Loadable;
}

ReabstractionInfo::TypeCategory ReabstractionInfo::
getParamTypeCategory(const SILParameterInfo &PI,
                  const SILFunctionConventions &substConv,
                  TypeExpansionContext typeExpansion) {
  auto &M = Callee->getModule();
  auto ParamTy = substConv.getSILType(PI, typeExpansion);
  ParamTy = Callee->mapTypeIntoContext(ParamTy);
  auto &TL = M.Types.getTypeLowering(ParamTy, typeExpansion);

  if (!TL.isLoadable())
    return NotLoadable;
    
  return TL.isTrivial() ? LoadableAndTrivial : Loadable;
}

/// Create a new substituted type with the updated signature.
CanSILFunctionType
ReabstractionInfo::createSubstitutedType(SILFunction *OrigF,
                                         SubstitutionMap SubstMap,
                                         bool HasUnboundGenericParams) {
  auto &M = OrigF->getModule();
  if ((SpecializedGenericSig &&
       SpecializedGenericSig->areAllParamsConcrete()) ||
      !HasUnboundGenericParams) {
    SpecializedGenericSig = nullptr;
    SpecializedGenericEnv = nullptr;
  }

  auto CanSpecializedGenericSig = SpecializedGenericSig.getCanonicalSignature();

  auto lowered = OrigF->getLoweredFunctionType();
  auto genSub =
      lowered->substGenericArgs(M, SubstMap, getResilienceExpansion());
  auto unsub = genSub->getUnsubstitutedType(M);
  auto specialized = CanSpecializedGenericSig.getReducedType(unsub);

  // First substitute concrete types into the existing function type.
  CanSILFunctionType FnTy = cast<SILFunctionType>(specialized);
  assert(FnTy);
  assert((CanSpecializedGenericSig || !FnTy->hasTypeParameter()) &&
         "Type parameters outside generic context?");

  // Use the new specialized generic signature.
  auto NewFnTy = SILFunctionType::get(
      CanSpecializedGenericSig, FnTy->getExtInfo(), FnTy->getCoroutineKind(),
      FnTy->getCalleeConvention(), FnTy->getParameters(), FnTy->getYields(),
      FnTy->getResults(), FnTy->getOptionalErrorResult(),
      FnTy->getPatternSubstitutions(), SubstitutionMap(), M.getASTContext(),
      FnTy->getWitnessMethodConformanceOrInvalid());

  // This is an interface type. It should not have any archetypes.
  assert(!NewFnTy->hasArchetype());
  return NewFnTy;
}

/// Convert the substituted function type into a specialized function type based
/// on the ReabstractionInfo.
CanSILFunctionType ReabstractionInfo::
createSpecializedType(CanSILFunctionType SubstFTy, SILModule &M) const {
  SmallVector<SILResultInfo, 8> SpecializedResults;
  SmallVector<SILYieldInfo, 8> SpecializedYields;
  SmallVector<SILParameterInfo, 8> SpecializedParams;
  auto context = getResilienceExpansion();
  unsigned IndirectResultIdx = 0;
  for (SILResultInfo RI : SubstFTy->getResults()) {
    RI = RI.getUnsubstituted(M, SubstFTy, context);
    if (RI.isFormalIndirect()) {
      bool isTrivial = TrivialArgs.test(IndirectResultIdx);
      if (isFormalResultConverted(IndirectResultIdx++)) {
        // Convert the indirect result to a direct result.
        // Indirect results are passed as owned, so we also need to pass the
        // direct result as owned (except it's a trivial type).
        auto C = (isTrivial
                  ? ResultConvention::Unowned
                  : ResultConvention::Owned);
        SpecializedResults.push_back(RI.getWithConvention(C));
        continue;
      }
    }
    // No conversion: re-use the original, substituted result info.
    SpecializedResults.push_back(RI);
  }
  unsigned idx = 0;
  bool removedSelfParam = false;
  for (SILParameterInfo PI : SubstFTy->getParameters()) {
    unsigned paramIdx = idx++;
    PI = PI.getUnsubstituted(M, SubstFTy, context);

    if (isDroppedMetatypeArg(param2ArgIndex(paramIdx))) {
      if (SubstFTy->hasSelfParam() && paramIdx == SubstFTy->getParameters().size() - 1)
        removedSelfParam = true;
      continue;
    }

    bool isTrivial = TrivialArgs.test(param2ArgIndex(paramIdx));
    if (!isParamConverted(paramIdx)) {
      // No conversion: re-use the original, substituted parameter info.
      SpecializedParams.push_back(PI);
      continue;
    }

    // Convert the indirect parameter to a direct parameter.
    // Indirect parameters are passed as owned/guaranteed, so we also
    // need to pass the direct/guaranteed parameter as
    // owned/guaranteed (except it's a trivial type).
    auto C = ParameterConvention::Direct_Unowned;
    if (!isTrivial) {
      if (PI.isGuaranteed()) {
        C = ParameterConvention::Direct_Guaranteed;
      } else {
        C = ParameterConvention::Direct_Owned;
      }
    }
    SpecializedParams.push_back(PI.getWithConvention(C));
  }
  for (SILYieldInfo YI : SubstFTy->getYields()) {
    // For now, always re-use the original, substituted yield info.
    SpecializedYields.push_back(YI.getUnsubstituted(M, SubstFTy, context));
  }

  auto Signature = SubstFTy->isPolymorphic()
                     ? SubstFTy->getInvocationGenericSignature()
                     : CanGenericSignature();

  SILFunctionType::ExtInfo extInfo = SubstFTy->getExtInfo();
  if (extInfo.hasSelfParam() && removedSelfParam) {
    extInfo = extInfo.withRepresentation(SILFunctionTypeRepresentation::Thin);
    assert(!extInfo.hasSelfParam());
  }

  return SILFunctionType::get(
      Signature, extInfo,
      SubstFTy->getCoroutineKind(), SubstFTy->getCalleeConvention(),
      SpecializedParams, SpecializedYields, SpecializedResults,
      SubstFTy->getOptionalErrorResult(), SubstitutionMap(), SubstitutionMap(),
      M.getASTContext(), SubstFTy->getWitnessMethodConformanceOrInvalid());
}

/// Create a new generic signature from an existing one by adding
/// additional requirements.
static std::pair<GenericEnvironment *, GenericSignature>
getGenericEnvironmentAndSignatureWithRequirements(
    GenericSignature OrigGenSig, GenericEnvironment *OrigGenericEnv,
    ArrayRef<Requirement> Requirements, SILModule &M) {
  SmallVector<Requirement, 2> RequirementsCopy(Requirements.begin(),
                                               Requirements.end());

  auto NewGenSig = buildGenericSignature(M.getASTContext(),
                                         OrigGenSig, { },
                                         std::move(RequirementsCopy));
  auto NewGenEnv = NewGenSig.getGenericEnvironment();
  return { NewGenEnv, NewGenSig };
}

/// This is a fast path for full specializations.
/// There is no need to form a new generic signature in such cases,
/// because the specialized function will be non-generic.
void ReabstractionInfo::performFullSpecializationPreparation(
    SILFunction *Callee, SubstitutionMap ParamSubs) {
  assert((!EnablePartialSpecialization || !HasUnboundGenericParams) &&
         "Only full specializations are handled here");

  SILModule &M = Callee->getModule();

  this->Callee = Callee;

  // Get the original substitution map.
  ClonerParamSubMap = ParamSubs;

  SubstitutedType = Callee->getLoweredFunctionType()->substGenericArgs(
      M, ClonerParamSubMap, getResilienceExpansion());
  CallerParamSubMap = {};
  createSubstitutedAndSpecializedTypes();
}

/// If the archetype (or any of its dependent types) has requirements
/// depending on other archetypes, return true.
/// Otherwise return false.
static bool hasNonSelfContainedRequirements(ArchetypeType *Archetype,
                                            GenericSignature Sig,
                                            GenericEnvironment *Env) {
  auto Reqs = Sig.getRequirements();
  auto CurrentGP = Archetype->getInterfaceType()
                       ->getCanonicalType()
                       ->getRootGenericParam();
  for (auto Req : Reqs) {
    switch(Req.getKind()) {
    case RequirementKind::Conformance:
    case RequirementKind::Superclass:
    case RequirementKind::Layout:
      // FIXME: Second type of a superclass requirement may contain
      // generic parameters.
      continue;
    case RequirementKind::SameShape:
    case RequirementKind::SameType: {
      // Check if this requirement contains more than one generic param.
      // If this is the case, then these archetypes are interdependent and
      // we should return true.
      auto First = Req.getFirstType()->getCanonicalType();
      auto Second = Req.getSecondType()->getCanonicalType();
      SmallSetVector<TypeBase *, 2> UsedGenericParams;
      First.visit([&](Type Ty) {
        if (auto *GP = Ty->getAs<GenericTypeParamType>()) {
          UsedGenericParams.insert(GP);
        }
      });
      Second.visit([&](Type Ty) {
        if (auto *GP = Ty->getAs<GenericTypeParamType>()) {
          UsedGenericParams.insert(GP);
        }
      });

      if (UsedGenericParams.count(CurrentGP) && UsedGenericParams.size() > 1)
        return true;
    }
    }
  }
  return false;
}

/// Collect all requirements for a generic parameter corresponding to a given
/// archetype.
static void collectRequirements(ArchetypeType *Archetype, GenericSignature Sig,
                                GenericEnvironment *Env,
                                SmallVectorImpl<Requirement> &CollectedReqs) {
  auto Reqs = Sig.getRequirements();
  auto CurrentGP = Archetype->getInterfaceType()
                       ->getCanonicalType()
                       ->getRootGenericParam();
  CollectedReqs.clear();
  for (auto Req : Reqs) {
    switch(Req.getKind()) {
    case RequirementKind::Conformance:
    case RequirementKind::Superclass:
    case RequirementKind::Layout:
      // If it is a generic param or something derived from it, add this
      // requirement.

      // FIXME: Second type of a superclass requirement may contain
      // generic parameters.

      if (Req.getFirstType()->getCanonicalType()->getRootGenericParam() ==
          CurrentGP)
        CollectedReqs.push_back(Req);
      continue;
    case RequirementKind::SameShape:
    case RequirementKind::SameType: {
      // Check if this requirement contains more than one generic param.
      // If this is the case, then these archetypes are interdependent and
      // we should return true.
      auto First = Req.getFirstType()->getCanonicalType();
      auto Second = Req.getSecondType()->getCanonicalType();
      SmallSetVector<GenericTypeParamType *, 2> UsedGenericParams;
      First.visit([&](Type Ty) {
        if (auto *GP = Ty->getAs<GenericTypeParamType>()) {
          UsedGenericParams.insert(GP);
        }
      });
      Second.visit([&](Type Ty) {
        if (auto *GP = Ty->getAs<GenericTypeParamType>()) {
          UsedGenericParams.insert(GP);
        }
      });

      if (!UsedGenericParams.count(CurrentGP))
        continue;

      if (UsedGenericParams.size() != 1) {
        llvm::dbgs() << "Strange requirement for "
                     << CurrentGP->getCanonicalType() << "\n";
        Req.dump(llvm::dbgs());
      }
      assert(UsedGenericParams.size() == 1);
      CollectedReqs.push_back(Req);
      continue;
    }
    }
  }
}

/// Returns true if a given substitution should participate in the
/// partial specialization.
///
/// TODO:
/// If a replacement is an archetype or a dependent type
/// of an archetype, then it does not make sense to substitute
/// it into the signature of the specialized function, because
/// it does not provide any benefits at runtime and may actually
/// lead to performance degradations.
///
/// If a replacement is a loadable type, it is most likely
/// rather beneficial to specialize using this substitution, because
/// it would allow for more efficient codegen for this type.
///
/// If a substitution simply replaces a generic parameter in the callee
/// by a generic parameter in the caller and this generic parameter
/// in the caller does have more "specific" conformances or requirements,
/// then it does name make any sense to perform this substitutions.
/// In particular, if the generic parameter in the callee is unconstrained
/// (i.e. just T), then providing a more specific generic parameter with some
/// conformances does not help, because the body of the callee does not invoke
/// any methods from any of these new conformances, unless these conformances
/// or requirements influence the layout of the generic type, e.g. "class",
/// "Trivial of size N", "HeapAllocationObject", etc.
/// (NOTE: It could be that additional conformances can still be used due
/// to conditional conformances or something like that, if the caller
/// has an invocation like: "G<T>().method(...)". In this case, G<T>().method()
/// and G<T:P>().method() may be resolved differently).
///
/// We may need to analyze the uses of the generic type inside
/// the function body (recursively). It is ever loaded/stored?
/// Do we create objects of this type? Which conformances are
/// really used?
static bool
shouldBePartiallySpecialized(Type Replacement,
                             GenericSignature Sig, GenericEnvironment *Env) {
  // If replacement is a concrete type, this substitution
  // should participate.
  if (!Replacement->hasArchetype())
    return true;

  // We cannot handle opened existentials yet.
  if (Replacement->hasOpenedExistential())
    return false;

  if (!SupportGenericSubstitutions) {
    // Don't partially specialize if the replacement contains an archetype.
    if (Replacement->hasArchetype())
      return false;
  }

  // If the archetype used (or any of its dependent types) has requirements
  // depending on other caller's archetypes, then we don't want to specialize
  // on it as it may require introducing more generic parameters, which
  // is not beneficial.

  // Collect the archetypes used by the replacement type.
  llvm::SmallSetVector<ArchetypeType *, 2> UsedArchetypes;
  Replacement.visit([&](Type Ty) {
    if (auto Archetype = Ty->getAs<ArchetypeType>()) {
      if (auto Primary = dyn_cast<PrimaryArchetypeType>(Archetype)) {
        UsedArchetypes.insert(Primary);
      }

      if (auto Pack = dyn_cast<PackArchetypeType>(Archetype)) {
        UsedArchetypes.insert(Pack);
      }
    }
  });

  // Check if any of the used archetypes are non-self contained when
  // it comes to requirements.
  for (auto *UsedArchetype : UsedArchetypes) {
    if (hasNonSelfContainedRequirements(UsedArchetype, Sig, Env)) {
      LLVM_DEBUG(llvm::dbgs() << "Requirements of the archetype depend on "
                            "other caller's generic parameters! "
                            "It cannot be partially specialized:\n";
                 UsedArchetype->dump(llvm::dbgs());
                 llvm::dbgs() << "This archetype is used in the substitution: "
                              << Replacement << "\n");
      return false;
    }
  }

  if (OptimizeGenericSubstitutions) {
    // Is it an unconstrained generic parameter?
    if (auto Archetype = Replacement->getAs<ArchetypeType>()) {
      if (Archetype->getConformsTo().empty()) {
        // TODO: If Replacement add a new layout constraint, then
        // it may be still useful to perform the partial specialization.
        return false;
      }
    }
  }

  return true;
}

namespace swift {

/// A helper class for creating partially specialized function signatures.
///
/// The following naming convention is used to describe the members and
/// functions:
/// Caller - the function which invokes the callee.
/// Callee - the callee to be specialized.
/// Specialized - the specialized callee which is being created.
class FunctionSignaturePartialSpecializer {
  /// Maps caller's generic parameters to generic parameters of the specialized
  /// function.
  llvm::DenseMap<SubstitutableType *, Type>
      CallerInterfaceToSpecializedInterfaceMapping;

  /// Maps callee's generic parameters to generic parameters of the specialized
  /// function.
  llvm::DenseMap<SubstitutableType *, Type>
      CalleeInterfaceToSpecializedInterfaceMapping;

  /// Maps the generic parameters of the specialized function to the caller's
  /// contextual types.
  llvm::DenseMap<SubstitutableType *, Type>
      SpecializedInterfaceToCallerArchetypeMapping;

  /// A SubstitutionMap for re-mapping caller's interface types
  /// to interface types of the specialized function.
  SubstitutionMap CallerInterfaceToSpecializedInterfaceMap;

  /// Maps callee's interface types to caller's contextual types.
  /// It is computed from the original substitutions.
  SubstitutionMap CalleeInterfaceToCallerArchetypeMap;

  /// Maps callee's interface types to specialized functions interface types.
  SubstitutionMap CalleeInterfaceToSpecializedInterfaceMap;

  /// Maps the generic parameters of the specialized function to the caller's
  /// contextual types.
  SubstitutionMap SpecializedInterfaceToCallerArchetypeMap;

  /// Generic signatures and environments for the caller, callee and
  /// the specialized function.
  GenericSignature CallerGenericSig;
  GenericEnvironment *CallerGenericEnv;

  GenericSignature CalleeGenericSig;
  GenericEnvironment *CalleeGenericEnv;

  GenericSignature SpecializedGenericSig;
  GenericEnvironment *SpecializedGenericEnv;

  SILModule &M;
  ModuleDecl *SM;
  ASTContext &Ctx;

  /// Set of newly created generic type parameters.
  SmallVector<GenericTypeParamType*, 2> AllGenericParams;

  /// Set of newly created requirements.
  SmallVector<Requirement, 2> AllRequirements;

  /// Archetypes used in the substitutions of an apply instructions.
  /// These are the contextual archetypes of the caller function, which
  /// invokes a generic function that is being specialized.
  llvm::SmallSetVector<ArchetypeType *, 2> UsedCallerArchetypes;

  /// Number of created generic parameters so far.
  unsigned GPIdx = 0;

  void createGenericParamsForUsedCallerArchetypes();

  void createGenericParamsForCalleeGenericParams();

  void addRequirements(ArrayRef<Requirement> Reqs, SubstitutionMap &SubsMap);

  void addCallerRequirements();

  void addCalleeRequirements();

  std::pair<GenericEnvironment *, GenericSignature>
  getSpecializedGenericEnvironmentAndSignature();

  void computeCallerInterfaceToSpecializedInterfaceMap();

  void computeCalleeInterfaceToSpecializedInterfaceMap();

  void computeSpecializedInterfaceToCallerArchetypeMap();

  /// Collect all used archetypes from all the substitutions.
  /// Take into account only those archetypes that occur in the
  /// substitutions of generic parameters which will be partially
  /// specialized. Ignore all others.
  void collectUsedCallerArchetypes(SubstitutionMap ParamSubs);

  /// Create a new generic parameter.
  GenericTypeParamType *createGenericParam();

public:
  FunctionSignaturePartialSpecializer(SILModule &M,
                                      GenericSignature CallerGenericSig,
                                      GenericEnvironment *CallerGenericEnv,
                                      GenericSignature CalleeGenericSig,
                                      GenericEnvironment *CalleeGenericEnv,
                                      SubstitutionMap ParamSubs)
      : CallerGenericSig(CallerGenericSig), CallerGenericEnv(CallerGenericEnv),
        CalleeGenericSig(CalleeGenericSig), CalleeGenericEnv(CalleeGenericEnv),
        M(M), SM(M.getSwiftModule()), Ctx(M.getASTContext()) {
    SpecializedGenericSig = nullptr;
    SpecializedGenericEnv = nullptr;
    CalleeInterfaceToCallerArchetypeMap = ParamSubs;
  }

  /// This constructor is used by when processing @_specialize.
  /// In this case, the caller and the callee are the same function.
  FunctionSignaturePartialSpecializer(SILModule &M,
                                      GenericSignature CalleeGenericSig,
                                      GenericEnvironment *CalleeGenericEnv,
                                      GenericSignature SpecializedSig)
      : CallerGenericSig(CalleeGenericSig), CallerGenericEnv(CalleeGenericEnv),
        CalleeGenericSig(CalleeGenericSig), CalleeGenericEnv(CalleeGenericEnv),
        SpecializedGenericSig(SpecializedSig),
        M(M), SM(M.getSwiftModule()), Ctx(M.getASTContext()) {

    // Create the new generic signature using provided requirements.
    SpecializedGenericEnv = SpecializedGenericSig.getGenericEnvironment();

    // Compute SubstitutionMaps required for re-mapping.

    // Callee's generic signature and specialized generic signature
    // use the same set of generic parameters, i.e. each generic
    // parameter should be mapped to itself.
    for (auto GP : CalleeGenericSig.getGenericParams()) {
      CalleeInterfaceToSpecializedInterfaceMapping[GP] = Type(GP);
    }
    computeCalleeInterfaceToSpecializedInterfaceMap();

    // Each generic parameter of the callee is mapped to its own
    // archetype.
    SpecializedInterfaceToCallerArchetypeMap =
      SubstitutionMap::get(
        SpecializedGenericSig,
        [&](SubstitutableType *type) -> Type {
          return CalleeGenericEnv->mapTypeIntoContext(type);
        },
        LookUpConformanceInSignature(SpecializedGenericSig.getPointer()));
  }

  GenericSignature getSpecializedGenericSignature() {
    return SpecializedGenericSig;
  }

  GenericEnvironment *getSpecializedGenericEnvironment() {
    return SpecializedGenericEnv;
  }

  void createSpecializedGenericSignature(SubstitutionMap ParamSubs);

  void createSpecializedGenericSignatureWithNonGenericSubs();

  SubstitutionMap computeClonerParamSubs();

  SubstitutionMap getCallerParamSubs();

  void computeCallerInterfaceSubs(SubstitutionMap &CallerInterfaceSubs);
};

} // end of namespace

GenericTypeParamType *
FunctionSignaturePartialSpecializer::createGenericParam() {
  auto GP = GenericTypeParamType::get(/*isParameterPack*/ false, 0, GPIdx++, Ctx);
  AllGenericParams.push_back(GP);
  return GP;
}

/// Collect all used caller's archetypes from all the substitutions.
void FunctionSignaturePartialSpecializer::collectUsedCallerArchetypes(
    SubstitutionMap ParamSubs) {
  for (auto Replacement : ParamSubs.getReplacementTypes()) {
    if (!Replacement->hasArchetype())
      continue;

    // If the substitution will not be performed in the specialized
    // function, there is no need to check for any archetypes inside
    // the replacement.
    if (!shouldBePartiallySpecialized(Replacement,
                                      CallerGenericSig, CallerGenericEnv))
      continue;

    // Add used generic parameters/archetypes.
    Replacement.visit([&](Type Ty) {
      if (auto Archetype = Ty->getAs<ArchetypeType>()) {
        if (auto Primary = dyn_cast<PrimaryArchetypeType>(Archetype)) {
          UsedCallerArchetypes.insert(Primary);
        }

        if (auto Pack = dyn_cast<PackArchetypeType>(Archetype)) {
          UsedCallerArchetypes.insert(Pack);
        }
      }
    });
  }
}

void FunctionSignaturePartialSpecializer::
    computeCallerInterfaceToSpecializedInterfaceMap() {
  if (!CallerGenericSig)
    return;

  CallerInterfaceToSpecializedInterfaceMap =
    SubstitutionMap::get(
      CallerGenericSig,
      [&](SubstitutableType *type) -> Type {
        return CallerInterfaceToSpecializedInterfaceMapping.lookup(type);
      },
      LookUpConformanceInSignature(CallerGenericSig.getPointer()));

  LLVM_DEBUG(llvm::dbgs() << "\n\nCallerInterfaceToSpecializedInterfaceMap "
                             "map:\n";
             CallerInterfaceToSpecializedInterfaceMap.dump(llvm::dbgs()));
}

void FunctionSignaturePartialSpecializer::
    computeSpecializedInterfaceToCallerArchetypeMap() {
  // Define a substitution map for re-mapping interface types of
  // the specialized function to contextual types of the caller.
  SpecializedInterfaceToCallerArchetypeMap =
    SubstitutionMap::get(
      SpecializedGenericSig,
      [&](SubstitutableType *type) -> Type {
        LLVM_DEBUG(llvm::dbgs() << "Mapping specialized interface type to "
                                   "caller archetype:\n";
                   llvm::dbgs() << "Interface type: "; type->dump(llvm::dbgs());
                   llvm::dbgs() << "Archetype: ";
                   auto Archetype =
                      SpecializedInterfaceToCallerArchetypeMapping.lookup(type);
                   if (Archetype) Archetype->dump(llvm::dbgs());
                   else llvm::dbgs() << "Not found!\n";);
        return SpecializedInterfaceToCallerArchetypeMapping.lookup(type);
      },
      LookUpConformanceInSignature(SpecializedGenericSig.getPointer()));
  LLVM_DEBUG(llvm::dbgs() << "\n\nSpecializedInterfaceToCallerArchetypeMap "
                             "map:\n";
             SpecializedInterfaceToCallerArchetypeMap.dump(llvm::dbgs()));
}

void FunctionSignaturePartialSpecializer::
    computeCalleeInterfaceToSpecializedInterfaceMap() {
  CalleeInterfaceToSpecializedInterfaceMap =
    SubstitutionMap::get(
      CalleeGenericSig,
      [&](SubstitutableType *type) -> Type {
        return CalleeInterfaceToSpecializedInterfaceMapping.lookup(type);
      },
      LookUpConformanceInSignature(CalleeGenericSig.getPointer()));

  LLVM_DEBUG(llvm::dbgs() << "\n\nCalleeInterfaceToSpecializedInterfaceMap:\n";
             CalleeInterfaceToSpecializedInterfaceMap.dump(llvm::dbgs()));
}

/// Generate a new generic type parameter for each used archetype from
/// the caller.
void FunctionSignaturePartialSpecializer::
    createGenericParamsForUsedCallerArchetypes() {
  for (auto CallerArchetype : UsedCallerArchetypes) {
    auto CallerGenericParam = CallerArchetype->getInterfaceType();
    assert(CallerGenericParam->is<GenericTypeParamType>());

    LLVM_DEBUG(llvm::dbgs() << "\n\nChecking used caller archetype:\n";
               CallerArchetype->dump(llvm::dbgs());
               llvm::dbgs() << "It corresponds to the caller generic "
                               "parameter:\n";
               CallerGenericParam->dump(llvm::dbgs()));

    // Create an equivalent generic parameter.
    auto SubstGenericParam = createGenericParam();
    auto SubstGenericParamCanTy = SubstGenericParam->getCanonicalType();
    (void)SubstGenericParamCanTy;

    CallerInterfaceToSpecializedInterfaceMapping
        [CallerGenericParam->getCanonicalType()
             ->castTo<GenericTypeParamType>()] = SubstGenericParam;

    SpecializedInterfaceToCallerArchetypeMapping[SubstGenericParam] =
        CallerArchetype;

    LLVM_DEBUG(llvm::dbgs() << "\nCreated a new specialized generic "
                               "parameter:\n";
               SubstGenericParam->dump(llvm::dbgs());
               llvm::dbgs() << "Created a mapping "
                               "(caller interface -> specialize interface):\n"
                            << CallerGenericParam << " -> "
                            << SubstGenericParamCanTy << "\n";
               llvm::dbgs() << "Created a mapping"
                               "(specialized interface -> caller archetype):\n"
                            << SubstGenericParamCanTy << " -> "
                            << CallerArchetype->getCanonicalType() << "\n");
  }
}

/// Create a new generic parameter for each of the callee's generic parameters
/// which requires a substitution.
void FunctionSignaturePartialSpecializer::
    createGenericParamsForCalleeGenericParams() {
  for (auto GP : CalleeGenericSig.getGenericParams()) {
    auto CanTy = GP->getCanonicalType();
    auto CanTyInContext = CalleeGenericSig.getReducedType(CanTy);
    auto Replacement = CanTyInContext.subst(CalleeInterfaceToCallerArchetypeMap);
    LLVM_DEBUG(llvm::dbgs() << "\n\nChecking callee generic parameter:\n";
               CanTy->dump(llvm::dbgs()));
    if (!Replacement) {
      LLVM_DEBUG(llvm::dbgs() << "No replacement found. Skipping.\n");
      continue;
    }

    LLVM_DEBUG(llvm::dbgs() << "Replacement found:\n";
               Replacement->dump(llvm::dbgs()));

    bool ShouldSpecializeGP = shouldBePartiallySpecialized(
        Replacement, CallerGenericSig, CallerGenericEnv);

    if (ShouldSpecializeGP) {
      LLVM_DEBUG(llvm::dbgs() << "Should be partially specialized.\n");
    } else {
      LLVM_DEBUG(llvm::dbgs() << "Should not be partially specialized.\n");
    }

    // Create an equivalent generic parameter in the specialized
    // generic environment.
    auto SubstGenericParam = createGenericParam();
    auto SubstGenericParamCanTy = SubstGenericParam->getCanonicalType();

    // Remember which specialized generic parameter correspond's to callee's
    // generic parameter.
    CalleeInterfaceToSpecializedInterfaceMapping[GP] = SubstGenericParam;

    LLVM_DEBUG(llvm::dbgs() << "\nCreated a new specialized generic "
                               "parameter:\n";
               SubstGenericParam->dump(llvm::dbgs());
               llvm::dbgs() << "Created a mapping "
                               "(callee interface -> specialized interface):\n"
                            << CanTy << " -> "
                            << SubstGenericParamCanTy << "\n");

    if (!ShouldSpecializeGP) {
      // Remember the original substitution from the apply instruction.
      SpecializedInterfaceToCallerArchetypeMapping[SubstGenericParam] =
          Replacement;
      LLVM_DEBUG(llvm::dbgs() << "Created a mapping (specialized interface -> "
                                 "caller archetype):\n"
                              << Type(SubstGenericParam) << " -> "
                              << Replacement << "\n");
      continue;
    }

    // Add a same type requirement based on the provided generic parameter
    // substitutions.
    auto ReplacementCallerInterfaceTy = Replacement->mapTypeOutOfContext();

    auto SpecializedReplacementCallerInterfaceTy =
        ReplacementCallerInterfaceTy.subst(
            CallerInterfaceToSpecializedInterfaceMap);
    assert(!SpecializedReplacementCallerInterfaceTy->hasError());

    Requirement Req(RequirementKind::SameType, SubstGenericParamCanTy,
                    SpecializedReplacementCallerInterfaceTy);
    AllRequirements.push_back(Req);

    LLVM_DEBUG(llvm::dbgs() << "Added a requirement:\n";
               Req.dump(llvm::dbgs()));

    if (ReplacementCallerInterfaceTy->is<GenericTypeParamType>()) {
      // Remember that the new generic parameter corresponds
      // to the same caller archetype, which corresponds to
      // the ReplacementCallerInterfaceTy.
      SpecializedInterfaceToCallerArchetypeMapping[SubstGenericParam] =
          SpecializedInterfaceToCallerArchetypeMapping.lookup(
              ReplacementCallerInterfaceTy
                  .subst(CallerInterfaceToSpecializedInterfaceMap)
                  ->castTo<SubstitutableType>());
      LLVM_DEBUG(llvm::dbgs()
              << "Created a mapping (specialized interface -> "
                 "caller archetype):\n"
              << Type(SubstGenericParam) << " -> "
              << SpecializedInterfaceToCallerArchetypeMapping[SubstGenericParam]
                 ->getCanonicalType()
              << "\n");
      continue;
    }

    SpecializedInterfaceToCallerArchetypeMapping[SubstGenericParam] =
        Replacement;

    LLVM_DEBUG(llvm::dbgs()
              << "Created a mapping (specialized interface -> "
                 "caller archetype):\n"
              << Type(SubstGenericParam) << " -> "
              << SpecializedInterfaceToCallerArchetypeMapping[SubstGenericParam]
                 ->getCanonicalType()
              << "\n");
  }
}

/// Add requirements from a given list of requirements re-mapping them using
/// the provided SubstitutionMap.
void FunctionSignaturePartialSpecializer::addRequirements(
    ArrayRef<Requirement> Reqs, SubstitutionMap &SubsMap) {
  for (auto &reqReq : Reqs) {
    LLVM_DEBUG(llvm::dbgs() << "\n\nRe-mapping the requirement:\n";
               reqReq.dump(llvm::dbgs()));
    AllRequirements.push_back(reqReq.subst(SubsMap));
  }
}

/// Add requirements from the caller's signature.
void FunctionSignaturePartialSpecializer::addCallerRequirements() {
  for (auto CallerArchetype : UsedCallerArchetypes) {
    // Add requirements for this caller generic parameter and its dependent
    // types.
    SmallVector<Requirement, 4> CollectedReqs;
    collectRequirements(CallerArchetype, CallerGenericSig, CallerGenericEnv,
                        CollectedReqs);
    if (!CollectedReqs.empty()) {
      LLVM_DEBUG(llvm::dbgs() << "Adding caller archetype requirements:\n";
                 for (auto Req : CollectedReqs) {
                   Req.dump(llvm::dbgs());
                 }
                 CallerInterfaceToSpecializedInterfaceMap.dump(llvm::dbgs());
                );
      addRequirements(CollectedReqs, CallerInterfaceToSpecializedInterfaceMap);
    }
  }
}

/// Add requirements from the callee's signature.
void FunctionSignaturePartialSpecializer::addCalleeRequirements() {
  addRequirements(CalleeGenericSig.getRequirements(),
                  CalleeInterfaceToSpecializedInterfaceMap);
}

std::pair<GenericEnvironment *, GenericSignature>
FunctionSignaturePartialSpecializer::
    getSpecializedGenericEnvironmentAndSignature() {
  if (AllGenericParams.empty())
    return { nullptr, nullptr };

  // Finalize the archetype builder.
  auto GenSig = buildGenericSignature(Ctx, GenericSignature(),
                                      AllGenericParams, AllRequirements);
  auto *GenEnv = GenSig.getGenericEnvironment();
  return { GenEnv, GenSig };
}

SubstitutionMap FunctionSignaturePartialSpecializer::computeClonerParamSubs() {
  return SubstitutionMap::get(
    CalleeGenericSig,
    [&](SubstitutableType *type) -> Type {
      LLVM_DEBUG(llvm::dbgs() << "\ngetSubstitution for ClonerParamSubs:\n"
                              << Type(type) << "\n"
                              << "in generic signature:\n";
                 CalleeGenericSig->print(llvm::dbgs()));
      auto SpecializedInterfaceTy =
          Type(type).subst(CalleeInterfaceToSpecializedInterfaceMap);
      return SpecializedGenericEnv->mapTypeIntoContext(
          SpecializedInterfaceTy);
    },
    LookUpConformanceInSignature(SpecializedGenericSig.getPointer()));
}

SubstitutionMap FunctionSignaturePartialSpecializer::getCallerParamSubs() {
  return SpecializedInterfaceToCallerArchetypeMap;
}

void FunctionSignaturePartialSpecializer::computeCallerInterfaceSubs(
    SubstitutionMap &CallerInterfaceSubs) {
  CallerInterfaceSubs = SubstitutionMap::get(
    CalleeGenericSig,
    [&](SubstitutableType *type) -> Type {
      // First, map callee's interface type to specialized interface type.
      auto Ty = Type(type).subst(CalleeInterfaceToSpecializedInterfaceMap);
      Type SpecializedInterfaceTy =
        SpecializedGenericEnv->mapTypeIntoContext(Ty)
          ->mapTypeOutOfContext();
      assert(!SpecializedInterfaceTy->hasError());
      return SpecializedInterfaceTy;
    },
    LookUpConformanceInSignature(CalleeGenericSig.getPointer()));

  LLVM_DEBUG(llvm::dbgs() << "\n\nCallerInterfaceSubs map:\n";
             CallerInterfaceSubs.dump(llvm::dbgs()));
}

/// Fast-path for the case when generic substitutions are not supported.
void FunctionSignaturePartialSpecializer::
    createSpecializedGenericSignatureWithNonGenericSubs() {
  // Simply create a set of same-type requirements based on concrete
  // substitutions.
  SmallVector<Requirement, 4> Requirements;
  CalleeGenericSig->forEachParam([&](GenericTypeParamType *GP, bool Canonical) {
    if (!Canonical)
      return;
    auto Replacement = Type(GP).subst(CalleeInterfaceToCallerArchetypeMap);
    if (Replacement->hasArchetype())
      return;
    // Replacement is concrete. Add a same type requirement.
    Requirement Req(RequirementKind::SameType, GP, Replacement);
    Requirements.push_back(Req);
  });

  // Create a new generic signature by taking the existing one
  // and adding new requirements to it. No need to introduce
  // any new generic parameters.
  auto GenPair = getGenericEnvironmentAndSignatureWithRequirements(
      CalleeGenericSig, CalleeGenericEnv, Requirements, M);

  if (GenPair.second) {
    SpecializedGenericSig = GenPair.second.getCanonicalSignature();
    SpecializedGenericEnv = GenPair.first;
  }

  for (auto GP : CalleeGenericSig.getGenericParams()) {
    CalleeInterfaceToSpecializedInterfaceMapping[GP] = Type(GP);
  }
  computeCalleeInterfaceToSpecializedInterfaceMap();

  SpecializedInterfaceToCallerArchetypeMap =
    CalleeInterfaceToCallerArchetypeMap;
}

void FunctionSignaturePartialSpecializer::createSpecializedGenericSignature(
    SubstitutionMap ParamSubs) {
  // Collect all used caller's archetypes from all the substitutions.
  collectUsedCallerArchetypes(ParamSubs);

  // Generate a new generic type parameter for each used archetype from
  // the caller.
  createGenericParamsForUsedCallerArchetypes();

  // Create a SubstitutionMap for re-mapping caller's interface types
  // to interface types of the specialized function.
  computeCallerInterfaceToSpecializedInterfaceMap();

  // Add generic parameters that will come from the callee.
  // Introduce a new generic parameter in the new generic signature
  // for each generic parameter from the callee.
  createGenericParamsForCalleeGenericParams();

  computeCalleeInterfaceToSpecializedInterfaceMap();

  // Add requirements from the callee's generic signature.
  addCalleeRequirements();

  // Add requirements from the caller's generic signature.
  addCallerRequirements();

  auto GenPair = getSpecializedGenericEnvironmentAndSignature();
  if (GenPair.second) {
    SpecializedGenericSig = GenPair.second.getCanonicalSignature();
    SpecializedGenericEnv = GenPair.first;
    computeSpecializedInterfaceToCallerArchetypeMap();
  }
}

/// Builds a new generic and function signatures for a partial specialization.
/// Allows for partial specializations even if substitutions contain
/// type parameters.
///
/// The new generic signature has the following generic parameters:
/// - For each substitution with a concrete type CT as a replacement for a
/// generic type T, it introduces a generic parameter T' and a
/// requirement T' == CT
/// - For all other substitutions that are considered for partial specialization,
/// it collects first the archetypes used in the replacements. Then for each such
/// archetype A a new generic parameter T' introduced.
/// - If there is a substitution for type T and this substitution is excluded
/// from partial specialization (e.g. because it is impossible or would result
/// in a less efficient code), then a new generic parameter T' is introduced,
/// which does not get any additional, more specific requirements based on the
/// substitutions.
///
/// After all generic parameters are added according to the rules above,
/// the requirements of the callee's signature are re-mapped by re-formulating
/// them in terms of the newly introduced generic parameters. In case a remapped
/// requirement does not contain any generic types, it can be omitted, because
/// it is fulfilled already.
///
/// If any of the generic parameters were introduced for caller's archetypes,
/// their requirements from the caller's signature are re-mapped by
/// re-formulating them in terms of the newly introduced generic parameters.
void ReabstractionInfo::performPartialSpecializationPreparation(
    SILFunction *Caller, SILFunction *Callee,
    SubstitutionMap ParamSubs) {
  SILModule &M = Callee->getModule();

  // Caller is the SILFunction containing the apply instruction.
  CanGenericSignature CallerGenericSig;
  GenericEnvironment *CallerGenericEnv = nullptr;
  if (Caller) {
    CallerGenericSig = Caller->getLoweredFunctionType()
                             ->getInvocationGenericSignature();
    CallerGenericEnv = Caller->getGenericEnvironment();
  }

  // Callee is the generic function being called by the apply instruction.
  auto CalleeFnTy = Callee->getLoweredFunctionType();
  auto CalleeGenericSig = CalleeFnTy->getInvocationGenericSignature();
  auto CalleeGenericEnv = Callee->getGenericEnvironment();

  LLVM_DEBUG(llvm::dbgs() << "\n\nTrying partial specialization for: "
                          << Callee->getName() << "\n";
             llvm::dbgs() << "Callee generic signature is:\n";
             CalleeGenericSig->print(llvm::dbgs()));

  FunctionSignaturePartialSpecializer FSPS(M,
                                           CallerGenericSig, CallerGenericEnv,
                                           CalleeGenericSig, CalleeGenericEnv,
                                           ParamSubs);

  // Create the partially specialized generic signature and generic environment.
  if (SupportGenericSubstitutions)
    FSPS.createSpecializedGenericSignature(ParamSubs);
  else
    FSPS.createSpecializedGenericSignatureWithNonGenericSubs();

  // Once the specialized signature is known, compute different
  // maps and function types based on it. The specializer will need
  // them for cloning and specializing the function body, rewriting
  // the original apply instruction, etc.
  finishPartialSpecializationPreparation(FSPS);
}

void ReabstractionInfo::finishPartialSpecializationPreparation(
    FunctionSignaturePartialSpecializer &FSPS) {
  SpecializedGenericSig = FSPS.getSpecializedGenericSignature();
  SpecializedGenericEnv = FSPS.getSpecializedGenericEnvironment();

  if (SpecializedGenericSig) {
    LLVM_DEBUG(llvm::dbgs() << "\nCreated SpecializedGenericSig:\n";
               SpecializedGenericSig->print(llvm::dbgs());
               SpecializedGenericEnv->dump(llvm::dbgs()));
  }

  // Create substitution lists for the caller and cloner.
  ClonerParamSubMap = FSPS.computeClonerParamSubs();
  CallerParamSubMap = FSPS.getCallerParamSubs();

  // Create a substitution map for the caller interface substitutions.
  FSPS.computeCallerInterfaceSubs(CallerInterfaceSubs);

  if (CalleeParamSubMap.empty()) {
    // It can happen if there is no caller or it is an eager specialization.
    CalleeParamSubMap = CallerParamSubMap;
  }

  HasUnboundGenericParams =
      SpecializedGenericSig && !SpecializedGenericSig->areAllParamsConcrete();

  createSubstitutedAndSpecializedTypes();

  if (getSubstitutedType() != Callee->getLoweredFunctionType()) {
    if (getSubstitutedType()->isPolymorphic())
      LLVM_DEBUG(llvm::dbgs() << "Created new specialized type: "
                              << SpecializedType << "\n");
  }
}

/// This constructor is used when processing @_specialize.
ReabstractionInfo::ReabstractionInfo(ModuleDecl *targetModule,
                                     bool isWholeModule, SILFunction *Callee,
                                     GenericSignature SpecializedSig,
                                     bool isPrespecialization)
    : TargetModule(targetModule), isWholeModule(isWholeModule),
      isPrespecialization(isPrespecialization) {
  Serialized =
      this->isPrespecialization ? IsNotSerialized : Callee->isSerialized();

  if (shouldNotSpecialize(Callee, nullptr))
    return;

  this->Callee = Callee;
  ConvertIndirectToDirect = true;

  SILModule &M = Callee->getModule();

  auto CalleeGenericSig =
      Callee->getLoweredFunctionType()->getInvocationGenericSignature();
  auto *CalleeGenericEnv = Callee->getGenericEnvironment();

  FunctionSignaturePartialSpecializer FSPS(M,
                                           CalleeGenericSig, CalleeGenericEnv,
                                           SpecializedSig);

  finishPartialSpecializationPreparation(FSPS);
}

// =============================================================================
// GenericFuncSpecializer
// =============================================================================

GenericFuncSpecializer::GenericFuncSpecializer(
    SILOptFunctionBuilder &FuncBuilder, SILFunction *GenericFunc,
    SubstitutionMap ParamSubs,
    const ReabstractionInfo &ReInfo,
    bool isMandatory)
    : FuncBuilder(FuncBuilder), M(GenericFunc->getModule()),
      GenericFunc(GenericFunc),
      ParamSubs(ParamSubs),
      ReInfo(ReInfo), isMandatory(isMandatory) {

  assert((GenericFunc->isDefinition() || ReInfo.isPrespecialized()) &&
         "Expected definition or pre-specialized entry-point to specialize!");
  auto FnTy = ReInfo.getSpecializedType();

  if (ReInfo.isPartialSpecialization()) {
    Mangle::PartialSpecializationMangler Mangler(
        GenericFunc, FnTy, ReInfo.isSerialized(), /*isReAbstracted*/ true);
    ClonedName = Mangler.mangle();
  } else {
    Mangle::GenericSpecializationMangler Mangler(
        GenericFunc, ReInfo.isSerialized());
    if (ReInfo.isPrespecialized()) {
      ClonedName = Mangler.manglePrespecialized(ParamSubs);
    } else {
      ClonedName = Mangler.mangleReabstracted(ParamSubs,
                                              ReInfo.needAlternativeMangling(),
                                              ReInfo.hasDroppedMetatypeArgs());
    }
  }
  LLVM_DEBUG(llvm::dbgs() << "    Specialized function " << ClonedName << '\n');
}

/// Return an existing specialization if one exists.
SILFunction *GenericFuncSpecializer::lookupSpecialization() {
  SILFunction *SpecializedF = M.lookUpFunction(ClonedName);
  if (!SpecializedF) {
    // In case the specialized function is already serialized in an imported
    // module, we need to take that. This can happen in case of cross-module-
    // optimization.
    // Otherwise we could end up that another de-serialized function from the
    // same module would reference the new (non-external) specialization we
    // would create here.
    SpecializedF = M.loadFunction(ClonedName, SILModule::LinkingMode::LinkAll,
                                  SILLinkage::Shared);
  }
  if (SpecializedF) {
    if (ReInfo.getSpecializedType() != SpecializedF->getLoweredFunctionType()) {
      llvm::dbgs() << "Looking for a function: " << ClonedName << "\n"
                   << "Expected type: " << ReInfo.getSpecializedType() << "\n"
                   << "Found    type: "
                   << SpecializedF->getLoweredFunctionType() << "\n";
    }
    assert(ReInfo.getSpecializedType()
           == SpecializedF->getLoweredFunctionType() &&
           "Previously specialized function does not match expected type.");
    LLVM_DEBUG(llvm::dbgs() << "Found an existing specialization for: "
                            << ClonedName << "\n");
    return SpecializedF;
  }
  LLVM_DEBUG(llvm::dbgs() << "Could not find an existing specialization for: "
                          << ClonedName << "\n");
  return nullptr;
}

void ReabstractionInfo::verify() const {
  assert((!SpecializedGenericSig && !SpecializedGenericEnv &&
          !getSpecializedType()->isPolymorphic()) ||
         (SpecializedGenericSig && SpecializedGenericEnv &&
          getSpecializedType()->isPolymorphic()));
}

/// Create a new specialized function if possible, and cache it.
SILFunction *
GenericFuncSpecializer::tryCreateSpecialization(bool forcePrespecialization) {
  // Do not create any new specializations at Onone.
  if (!GenericFunc->shouldOptimize() && !forcePrespecialization && !isMandatory)
    return nullptr;

  LLVM_DEBUG(llvm::dbgs() << "Creating a specialization: "
                          << ClonedName << "\n");

  ReInfo.verify();

  // Create a new function.
  SILFunction *SpecializedF = GenericCloner::cloneFunction(
      FuncBuilder, GenericFunc, ReInfo,
      // Use these substitutions inside the new specialized function being
      // created.
      ReInfo.getClonerParamSubstitutionMap(),
      ClonedName);
  assert((SpecializedF->getLoweredFunctionType()->isPolymorphic() &&
          SpecializedF->getGenericEnvironment()) ||
         (!SpecializedF->getLoweredFunctionType()->isPolymorphic() &&
          !SpecializedF->getGenericEnvironment()));
  // Store the meta-information about how this specialization was created.
  auto *Caller = ReInfo.getApply() ? ReInfo.getApply().getFunction() : nullptr;
  SubstitutionMap Subs = Caller ? ReInfo.getApply().getSubstitutionMap()
                                : ReInfo.getClonerParamSubstitutionMap();
  SpecializedF->setClassSubclassScope(SubclassScope::NotApplicable);
  SpecializedF->setSpecializationInfo(
      GenericSpecializationInformation::create(Caller, GenericFunc, Subs));

  if (VerifyFunctionsAfterSpecialization) {
    PrettyStackTraceSILFunction SILFunctionDumper(
        llvm::Twine("Generic function: ") + GenericFunc->getName() +
            ". Specialized Function: " + SpecializedF->getName(),
        GenericFunc);
    SpecializedF->verify();
  }

  if (DumpFunctionsAfterSpecialization) {
    llvm::dbgs() << llvm::Twine("Generic function: ") + GenericFunc->getName() +
                        ". Specialized Function: " + SpecializedF->getName();
    GenericFunc->dump();
    SpecializedF->dump();
  }

  return SpecializedF;
}

// =============================================================================
// Apply substitution
// =============================================================================

/// Fix the case where a void function returns the result of an apply, which is
/// also a call of a void-returning function.
/// We always want a void function returning a tuple _instruction_.
static void fixUsedVoidType(SILValue VoidVal, SILLocation Loc,
                            SILBuilder &Builder) {
  assert(VoidVal->getType().isVoid());
  if (VoidVal->use_empty())
    return;
  auto *NewVoidVal = Builder.createTuple(Loc, VoidVal->getType(), { });
  VoidVal->replaceAllUsesWith(NewVoidVal);
}

static SILValue fixSpecializedReturnType(SILValue returnVal, SILType returnType,
                                         SILLocation Loc, SILBuilder &Builder) {
  SILValue newReturnVal;
  if (returnType.isAddress()) {
    newReturnVal = Builder.createUncheckedAddrCast(Loc, returnVal, returnType);
  } else if (SILType::canRefCast(returnVal->getType(), returnType,
                                 Builder.getModule())) {
    newReturnVal = Builder.createUncheckedRefCast(Loc, returnVal, returnType);
  } else {
    if (Builder.hasOwnership()) {
      newReturnVal =
          Builder.createUncheckedValueCast(Loc, returnVal, returnType);
    } else {
      newReturnVal =
          Builder.createUncheckedBitwiseCast(Loc, returnVal, returnType);
    }
  }

  return newReturnVal;
}

/// Prepare call arguments. Perform re-abstraction if required.
///
/// \p ArgAtIndexNeedsEndBorrow after return contains indices of arguments that
/// need end borrow. The reason why we are doing this in a separate array is
/// that we are going to eventually need to pass off Arguments to SILBuilder
/// which will want an ArrayRef<SILValue>() so using a composite type here would
/// force us to do some sort of conversion then.
static void
prepareCallArguments(ApplySite AI, SILBuilder &Builder,
                     const ReabstractionInfo &ReInfo,
                     const TypeReplacements &typeReplacements,
                     SmallVectorImpl<SILValue> &Arguments,
                     SmallVectorImpl<unsigned> &ArgAtIndexNeedsEndBorrow,
                     SILValue &StoreResultTo) {
  /// SIL function conventions for the original apply site with substitutions.
  SILLocation Loc = AI.getLoc();
  auto substConv = AI.getSubstCalleeConv();
  unsigned ArgIdx = AI.getCalleeArgIndexOfFirstAppliedArg();

  auto handleConversion = [&](SILValue InputValue) {
    // Rewriting SIL arguments is only for lowered addresses.
    if (!substConv.useLoweredAddresses())
      return false;

    if (ArgIdx < substConv.getSILArgIndexOfFirstParam()) {
      // Handle result arguments.
      unsigned formalIdx =
          substConv.getIndirectFormalResultIndexForSILArg(ArgIdx);

      bool converted = false;
      if (typeReplacements.hasIndirectResultTypes()) {
        auto typeReplacementIt = typeReplacements.getIndirectResultTypes().find(formalIdx);
        if (typeReplacementIt != typeReplacements.getIndirectResultTypes().end()) {
          auto specializedTy = typeReplacementIt->second;
          if (InputValue->getType().isAddress()) {
            auto argTy = SILType::getPrimitiveAddressType(specializedTy);
            InputValue = Builder.createUncheckedAddrCast(Loc, InputValue, argTy);
          } else {
            auto argTy = SILType::getPrimitiveObjectType(specializedTy);
            if (SILType::canRefCast(InputValue->getType(), argTy,
                                    Builder.getModule())) {
              InputValue = Builder.createUncheckedRefCast(Loc, InputValue, argTy);
            } else {
              if (Builder.hasOwnership()) {
                InputValue =
                    Builder.createUncheckedValueCast(Loc, InputValue, argTy);
              } else {
                InputValue =
                    Builder.createUncheckedBitwiseCast(Loc, InputValue, argTy);
              }
            }
          }
          converted = true;
        }
      }

      if (!ReInfo.isFormalResultConverted(formalIdx)) {
        if (converted)
          Arguments.push_back(InputValue);
        return converted;
      }

      // The result is converted from indirect to direct. We need to insert
      // a store later.
      assert(!StoreResultTo);
      StoreResultTo = InputValue;
      return true;
    }

    if (ReInfo.isDroppedMetatypeArg(ArgIdx))
      return true;

    // Handle arguments for formal parameters.
    unsigned paramIdx = ArgIdx - substConv.getSILArgIndexOfFirstParam();

    // Handle type conversions for shape based specializations, e.g.
    // some reference type -> AnyObject
    bool converted = false;
    if (typeReplacements.hasParamTypeReplacements()) {
      auto typeReplacementIt = typeReplacements.getParamTypeReplacements().find(paramIdx);
      if (typeReplacementIt != typeReplacements.getParamTypeReplacements().end()) {
        auto specializedTy = typeReplacementIt->second;
        if (InputValue->getType().isAddress()) {
          auto argTy = SILType::getPrimitiveAddressType(specializedTy);
          InputValue = Builder.createUncheckedAddrCast(Loc, InputValue, argTy);
        } else {
          auto argTy = SILType::getPrimitiveObjectType(specializedTy);
          if (SILType::canRefCast(InputValue->getType(), argTy,
                                  Builder.getModule())) {
            InputValue = Builder.createUncheckedRefCast(Loc, InputValue, argTy);
          } else {
            if (Builder.hasOwnership()) {
                InputValue =
                    Builder.createUncheckedValueCast(Loc, InputValue, argTy);
            } else {
              InputValue =
                  Builder.createUncheckedBitwiseCast(Loc, InputValue, argTy);
            }
          }
        }
        converted = true;
      }
    }

    if (!ReInfo.isParamConverted(paramIdx)) {
      if (converted) {
        Arguments.push_back(InputValue);
      }
      return converted;
    }

    // An argument is converted from indirect to direct. Instead of the
    // address we pass the loaded value.
    SILArgumentConvention argConv(SILArgumentConvention::Direct_Unowned);
    if (auto pai = dyn_cast<PartialApplyInst>(AI)) {
      // On-stack partial applications borrow their captures, whereas heap
      // partial applications take ownership.
      argConv = pai->isOnStack() ? SILArgumentConvention::Direct_Guaranteed
                                 : SILArgumentConvention::Direct_Owned;
    } else {
      argConv = substConv.getSILArgumentConvention(ArgIdx);
    }
    SILValue Val;
    if (!argConv.isGuaranteedConvention()) {
      Val = Builder.emitLoadValueOperation(Loc, InputValue,
                                           LoadOwnershipQualifier::Take);
    } else {
      Val = Builder.emitLoadBorrowOperation(Loc, InputValue);
      if (Val->getOwnershipKind() == OwnershipKind::Guaranteed)
        ArgAtIndexNeedsEndBorrow.push_back(Arguments.size());
    }

    Arguments.push_back(Val);
    return true;
  };

  for (auto &Op : AI.getArgumentOperands()) {
    if (!handleConversion(Op.get()))
      Arguments.push_back(Op.get());
    ++ArgIdx;
  }
}

static void
cleanupCallArguments(SILBuilder &builder, SILLocation loc,
                     ArrayRef<SILValue> values,
                     ArrayRef<unsigned> valueIndicesThatNeedEndBorrow) {
  for (int index : valueIndicesThatNeedEndBorrow) {
    auto *lbi = cast<LoadBorrowInst>(values[index]);
    builder.createEndBorrow(loc, lbi);
  }
}

/// Create a new apply based on an old one, but with a different
/// function being applied.
static ApplySite
replaceWithSpecializedCallee(ApplySite applySite, SILValue callee,
                             const ReabstractionInfo &reInfo,
                             const TypeReplacements &typeReplacements = {}) {
  SILBuilderWithScope builder(applySite.getInstruction());
  SILLocation loc = applySite.getLoc();
  SmallVector<SILValue, 4> arguments;
  SmallVector<unsigned, 4> argsNeedingEndBorrow;
  SILValue resultOut;

  prepareCallArguments(applySite, builder, reInfo,
                       typeReplacements, arguments,
                       argsNeedingEndBorrow, resultOut);

  // Create a substituted callee type.
  //
  // NOTE: We do not perform this substitution if we are promoting a full apply
  // site callee of a partial apply.
  auto canFnTy = callee->getType().castTo<SILFunctionType>();
  SubstitutionMap subs;
  if (reInfo.getSpecializedType()->isPolymorphic() &&
      canFnTy->isPolymorphic()) {
    subs = reInfo.getCallerParamSubstitutionMap();
    subs = SubstitutionMap::get(canFnTy->getSubstGenericSignature(), subs);
  }

  auto calleeSubstFnTy = canFnTy->substGenericArgs(
      *callee->getModule(), subs, reInfo.getResilienceExpansion());
  auto calleeSILSubstFnTy = SILType::getPrimitiveObjectType(calleeSubstFnTy);
  SILFunctionConventions substConv(calleeSubstFnTy, builder.getModule());

  switch (applySite.getKind()) {
  case ApplySiteKind::TryApplyInst: {
    auto *tai = cast<TryApplyInst>(applySite);
    SILBasicBlock *resultBlock = tai->getNormalBB();
    assert(resultBlock->getSinglePredecessorBlock() == tai->getParent());
    // First insert the cleanups for our arguments int he appropriate spot.
    FullApplySite(tai).insertAfterApplication(
        [&](SILBuilder &argBuilder) {
          cleanupCallArguments(argBuilder, loc, arguments,
                               argsNeedingEndBorrow);
        });
    auto *newTAI = builder.createTryApply(loc, callee, subs, arguments,
                                          resultBlock, tai->getErrorBB(),
                                          tai->getApplyOptions());
    if (resultOut) {
      assert(substConv.useLoweredAddresses());
      // The original normal result of the try_apply is an empty tuple.
      assert(resultBlock->getNumArguments() == 1);
      builder.setInsertionPoint(resultBlock->begin());
      fixUsedVoidType(resultBlock->getArgument(0), loc, builder);

      SILValue returnValue = resultBlock->replacePhiArgument(
            0, resultOut->getType().getObjectType(), OwnershipKind::Owned);

      // Store the direct result to the original result address.
      builder.emitStoreValueOperation(loc, returnValue, resultOut,
                                      StoreOwnershipQualifier::Init);
    }
    return newTAI;
  }
  case ApplySiteKind::ApplyInst: {
    auto *ai = cast<ApplyInst>(applySite);
    FullApplySite(ai).insertAfterApplication(
        [&](SILBuilder &argBuilder) {
          cleanupCallArguments(argBuilder, loc, arguments,
                               argsNeedingEndBorrow);
        });
    auto *newAI =
        builder.createApply(loc, callee, subs, arguments,
                            ai->getApplyOptions());

    SILValue returnValue = newAI;
    if (resultOut) {
      if (!calleeSILSubstFnTy.isNoReturnFunction(
              builder.getModule(), builder.getTypeExpansionContext())) {
        // Store the direct result to the original result address.
        fixUsedVoidType(ai, loc, builder);

        builder.emitStoreValueOperation(loc, returnValue, resultOut,
                                        StoreOwnershipQualifier::Init);
      } else {
        builder.createUnreachable(loc);
        // unreachable should be the terminator instruction.
        // So, split the current basic block right after the
        // inserted unreachable instruction.
        builder.getInsertionPoint()->getParent()->split(
            builder.getInsertionPoint());
      }
    } else if (typeReplacements.hasResultType()) {
      returnValue = fixSpecializedReturnType(
          newAI, *typeReplacements.getResultType(), loc, builder);
    }
    ai->replaceAllUsesWith(returnValue);

    return newAI;
  }
  case ApplySiteKind::BeginApplyInst: {
    auto *bai = cast<BeginApplyInst>(applySite);
    assert(!resultOut);
    FullApplySite(bai).insertAfterApplication(
        [&](SILBuilder &argBuilder) {
          cleanupCallArguments(argBuilder, loc, arguments,
                               argsNeedingEndBorrow);
        });
    auto *newBAI = builder.createBeginApply(loc, callee, subs, arguments,
                                            bai->getApplyOptions());
    for (auto pair : llvm::enumerate(bai->getYieldedValues())) {
      auto index = pair.index();
      SILValue oldYield = pair.value();
      SILValue newYield = newBAI->getYieldedValues()[index];

      auto it = typeReplacements.getYieldTypeReplacements().find(index);
      if (it != typeReplacements.getYieldTypeReplacements().end()) {
        SILType newType;
        if (newYield->getType().isObject()) {
          newType = SILType::getPrimitiveObjectType(it->second);
        } else {
          newType = SILType::getPrimitiveAddressType(it->second);
        }
        auto converted =
            fixSpecializedReturnType(newYield, newType, loc, builder);
        oldYield->replaceAllUsesWith(converted);
      }
    }
    bai->replaceAllUsesPairwiseWith(newBAI);
    return newBAI;
  }
  case ApplySiteKind::PartialApplyInst: {
    auto *pai = cast<PartialApplyInst>(applySite);
    // Let go of borrows introduced for stack closures.
    if (pai->isOnStack() && pai->getFunction()->hasOwnership()) {
      pai->visitOnStackLifetimeEnds([&](Operand *op) -> bool {
        SILBuilderWithScope argBuilder(op->getUser()->getNextInstruction());
        cleanupCallArguments(argBuilder, loc, arguments, argsNeedingEndBorrow);
        return true;
      });
    }
    auto *newPAI = builder.createPartialApply(
        loc, callee, subs, arguments,
        pai->getType().getAs<SILFunctionType>()->getCalleeConvention(),
        pai->isOnStack());
    pai->replaceAllUsesWith(newPAI);
    return newPAI;
  }
  }

  llvm_unreachable("unhandled kind of apply");
}

namespace {

/// local overload of `replaceWithSpecializedFunction` that takes a
/// `SpecializedFunction`
ApplySite replaceWithSpecializedFunction(ApplySite AI,
                                         SpecializedFunction &NewF,
                                         const ReabstractionInfo &ReInfo) {
  SILBuilderWithScope Builder(AI.getInstruction());
  FunctionRefInst *FRI =
      Builder.createFunctionRef(AI.getLoc(), NewF.getFunction());
  return replaceWithSpecializedCallee(AI, FRI, ReInfo,
                                      NewF.getTypeReplacements());
}

} // anonymous namespace

/// Create a new apply based on an old one, but with a different
/// function being applied.
ApplySite swift::
replaceWithSpecializedFunction(ApplySite AI, SILFunction *NewF,
                               const ReabstractionInfo &ReInfo) {
  SpecializedFunction SpecializedF(NewF);
  return replaceWithSpecializedFunction(AI, SpecializedF, ReInfo);
}

namespace {

class ReabstractionThunkGenerator {
  SILOptFunctionBuilder &FunctionBuilder;
  SILFunction *OrigF;
  SILModule &M;
  SILFunction *SpecializedFunc;
  const ReabstractionInfo &ReInfo;
  PartialApplyInst *OrigPAI;

  std::string ThunkName;
  RegularLocation Loc;
  SmallVector<SILValue, 4> Arguments;

public:
  ReabstractionThunkGenerator(SILOptFunctionBuilder &FunctionBuilder,
                              const ReabstractionInfo &ReInfo,
                              PartialApplyInst *OrigPAI,
                              SILFunction *SpecializedFunc)
      : FunctionBuilder(FunctionBuilder), OrigF(OrigPAI->getCalleeFunction()), M(OrigF->getModule()),
        SpecializedFunc(SpecializedFunc), ReInfo(ReInfo), OrigPAI(OrigPAI),
        Loc(RegularLocation::getAutoGeneratedLocation()) {
    if (!ReInfo.isPartialSpecialization()) {
      Mangle::GenericSpecializationMangler Mangler(OrigF, ReInfo.isSerialized());
      ThunkName = Mangler.mangleNotReabstracted(
          ReInfo.getCalleeParamSubstitutionMap());
    } else {
      Mangle::PartialSpecializationMangler Mangler(
          OrigF, ReInfo.getSpecializedType(), ReInfo.isSerialized(),
          /*isReAbstracted*/ false);

      ThunkName = Mangler.mangle();
    }
  }

  SILFunction *createThunk();

protected:
  FullApplySite createReabstractionThunkApply(SILBuilder &Builder);
  SILArgument *convertReabstractionThunkArguments(
      SILBuilder &Builder, SmallVectorImpl<unsigned> &ArgsNeedingEndBorrows);
};

} // anonymous namespace

SILFunction *ReabstractionThunkGenerator::createThunk() {
  SILFunction *Thunk = FunctionBuilder.getOrCreateSharedFunction(
      Loc, ThunkName, ReInfo.getSubstitutedType(), IsBare, IsTransparent,
      ReInfo.isSerialized(), ProfileCounter(), IsThunk, IsNotDynamic,
      IsNotDistributed, IsNotRuntimeAccessible);
  // Re-use an existing thunk.
  if (!Thunk->empty())
    return Thunk;

  Thunk->setGenericEnvironment(ReInfo.getSpecializedGenericEnvironment());

  SILBasicBlock *EntryBB = Thunk->createBasicBlock();
  SILBuilder Builder(EntryBB);

  // If the original specialized function had unqualified ownership, set the
  // thunk to have unqualified ownership as well.
  //
  // This is a stop gap measure to allow for easy inlining. We could always make
  // the Thunk qualified, but then we would need to either fix the inliner to
  // inline qualified into unqualified functions /or/ have the
  // OwnershipModelEliminator run as part of the normal compilation pipeline
  // (which we are not doing yet).
  if (!SpecializedFunc->hasOwnership()) {
    Thunk->setOwnershipEliminated();
  }

  if (!SILModuleConventions(M).useLoweredAddresses()) {
    for (auto SpecArg : SpecializedFunc->getArguments()) {
      auto *NewArg = EntryBB->createFunctionArgument(SpecArg->getType(),
                                                     SpecArg->getDecl());
      NewArg->copyFlags(cast<SILFunctionArgument>(SpecArg));
      Arguments.push_back(NewArg);
    }
    FullApplySite ApplySite = createReabstractionThunkApply(Builder);
    SILValue ReturnValue = ApplySite.getResult();
    assert(ReturnValue && "getPseudoResult out of sync with ApplySite?!");
    Builder.createReturn(Loc, ReturnValue);

    return Thunk;
  }
  // Handle lowered addresses.
  SmallVector<unsigned, 4> ArgsThatNeedEndBorrow;
  SILArgument *ReturnValueAddr =
      convertReabstractionThunkArguments(Builder, ArgsThatNeedEndBorrow);

  FullApplySite ApplySite = createReabstractionThunkApply(Builder);

  SILValue ReturnValue = ApplySite.getResult();
  assert(ReturnValue && "getPseudoResult out of sync with ApplySite?!");

  if (ReturnValueAddr) {
    // Need to store the direct results to the original indirect address.
    Builder.emitStoreValueOperation(Loc, ReturnValue, ReturnValueAddr,
                                    StoreOwnershipQualifier::Init);
    SILType VoidTy = OrigPAI->getSubstCalleeType()->getDirectFormalResultsType(
        M, Builder.getTypeExpansionContext());
    assert(VoidTy.isVoid());
    ReturnValue = Builder.createTuple(Loc, VoidTy, {});
  }
  Builder.createReturn(Loc, ReturnValue);

  // Now that we have finished constructing our CFG (note the return above),
  // insert any compensating end borrows that we need.
  ApplySite.insertAfterApplication([&](SILBuilder &argBuilder) {
    cleanupCallArguments(argBuilder, Loc, Arguments, ArgsThatNeedEndBorrow);
  });

  return Thunk;
}

/// Create a call to a reabstraction thunk. Return the call's direct result.
FullApplySite ReabstractionThunkGenerator::createReabstractionThunkApply(
    SILBuilder &Builder) {
  SILFunction *Thunk = &Builder.getFunction();
  auto *FRI = Builder.createFunctionRef(Loc, SpecializedFunc);
  auto Subs = Thunk->getForwardingSubstitutionMap();
  auto specConv = SpecializedFunc->getConventions();
  if (!SpecializedFunc->getLoweredFunctionType()->hasErrorResult()) {
    return Builder.createApply(Loc, FRI, Subs, Arguments);
  }
  // Create the logic for calling a throwing function.
  SILBasicBlock *NormalBB = Thunk->createBasicBlock();
  SILBasicBlock *ErrorBB = Thunk->createBasicBlock();
  auto *TAI =
      Builder.createTryApply(Loc, FRI, Subs, Arguments, NormalBB, ErrorBB);
  auto *ErrorVal = ErrorBB->createPhiArgument(
      SpecializedFunc->mapTypeIntoContext(
          specConv.getSILErrorType(Builder.getTypeExpansionContext())),
      OwnershipKind::Owned);
  Builder.setInsertionPoint(ErrorBB);
  Builder.createThrow(Loc, ErrorVal);
  NormalBB->createPhiArgument(
      SpecializedFunc->mapTypeIntoContext(
          specConv.getSILResultType(Builder.getTypeExpansionContext())),
      OwnershipKind::Owned);
  Builder.setInsertionPoint(NormalBB);
  return FullApplySite(TAI);
}

/// Create SIL arguments for a reabstraction thunk with lowered addresses. This
/// may involve replacing indirect arguments with loads and stores. Return the
/// SILArgument for the address of an indirect result, or nullptr.
///
/// FIXME: Remove this if we don't need to create reabstraction thunks after
/// address lowering.
SILArgument *ReabstractionThunkGenerator::convertReabstractionThunkArguments(
    SILBuilder &Builder, SmallVectorImpl<unsigned> &ArgsThatNeedEndBorrow) {
  SILFunction *Thunk = &Builder.getFunction();
  CanSILFunctionType SpecType = SpecializedFunc->getLoweredFunctionType();
  CanSILFunctionType SubstType = ReInfo.getSubstitutedType();
  auto specConv = SpecializedFunc->getConventions();
  (void)specConv;
  SILFunctionConventions substConv(SubstType, M);

  assert(specConv.useLoweredAddresses());

  // ReInfo.NumIndirectResults corresponds to SubstTy's formal indirect
  // results. SpecTy may have fewer formal indirect results.
  assert(SubstType->getNumIndirectFormalResults()
         >= SpecType->getNumIndirectFormalResults());

  SILBasicBlock *EntryBB = Thunk->getEntryBlock();
  SILArgument *ReturnValueAddr = nullptr;
  auto SpecArgIter = SpecializedFunc->getArguments().begin();
  auto cloneSpecializedArgument = [&]() {
    // No change to the argument.
    SILArgument *SpecArg = *SpecArgIter++;
    auto *NewArg =
        EntryBB->createFunctionArgument(SpecArg->getType(), SpecArg->getDecl());
    NewArg->setNoImplicitCopy(
        cast<SILFunctionArgument>(SpecArg)->isNoImplicitCopy());
    NewArg->setLifetimeAnnotation(
        cast<SILFunctionArgument>(SpecArg)->getLifetimeAnnotation());
    NewArg->setClosureCapture(
        cast<SILFunctionArgument>(SpecArg)->isClosureCapture());
    Arguments.push_back(NewArg);
  };
  // ReInfo.NumIndirectResults corresponds to SubstTy's formal indirect
  // results. SpecTy may have fewer formal indirect results.
  assert(SubstType->getNumIndirectFormalResults()
         >= SpecType->getNumIndirectFormalResults());
  unsigned resultIdx = 0;
  for (auto substRI : SubstType->getIndirectFormalResults()) {
    if (ReInfo.isFormalResultConverted(resultIdx++)) {
      // Convert an originally indirect to direct specialized result.
      // Store the result later.
      // FIXME: This only handles a single result! Partial specialization could
      // induce some combination of direct and indirect results.
      SILType ResultTy = SpecializedFunc->mapTypeIntoContext(
          substConv.getSILType(substRI, Builder.getTypeExpansionContext()));
      assert(ResultTy.isAddress());
      assert(!ReturnValueAddr);
      ReturnValueAddr = EntryBB->createFunctionArgument(ResultTy);
      continue;
    }
    // If the specialized result is already indirect, simply clone the indirect
    // result argument.
    assert((*SpecArgIter)->getType().isAddress());
    cloneSpecializedArgument();
  }
  assert(SpecArgIter
         == SpecializedFunc->getArgumentsWithoutIndirectResults().begin());
  unsigned numParams = SpecType->getNumParameters();
  assert(numParams == SubstType->getNumParameters());
  for (unsigned paramIdx = 0; paramIdx < numParams; ++paramIdx) {
    if (ReInfo.isParamConverted(paramIdx)) {
      // Convert an originally indirect to direct specialized parameter.
      assert(!specConv.isSILIndirect(SpecType->getParameters()[paramIdx]));
      // Instead of passing the address, pass the loaded value.
      SILType ParamTy = SpecializedFunc->mapTypeIntoContext(
          substConv.getSILType(SubstType->getParameters()[paramIdx],
                               Builder.getTypeExpansionContext()));
      assert(ParamTy.isAddress());
      SILArgument *SpecArg = *SpecArgIter++;
      SILFunctionArgument *NewArg =
          EntryBB->createFunctionArgument(ParamTy, SpecArg->getDecl());
      NewArg->setNoImplicitCopy(
          cast<SILFunctionArgument>(SpecArg)->isNoImplicitCopy());
      NewArg->setLifetimeAnnotation(
          cast<SILFunctionArgument>(SpecArg)->getLifetimeAnnotation());
      NewArg->setClosureCapture(
          cast<SILFunctionArgument>(SpecArg)->isClosureCapture());
      if (!NewArg->getArgumentConvention().isGuaranteedConvention()) {
        SILValue argVal = Builder.emitLoadValueOperation(
            Loc, NewArg, LoadOwnershipQualifier::Take);
        Arguments.push_back(argVal);
      } else {
        SILValue argVal = Builder.emitLoadBorrowOperation(Loc, NewArg);
        if (argVal->getOwnershipKind() == OwnershipKind::Guaranteed)
          ArgsThatNeedEndBorrow.push_back(Arguments.size());
        Arguments.push_back(argVal);
      }
      continue;
    }
    // Simply clone unconverted direct or indirect parameters.
    cloneSpecializedArgument();
  }
  assert(SpecArgIter == SpecializedFunc->getArguments().end());
  return ReturnValueAddr;
}

/// Create a pre-specialization of the library function with
/// \p UnspecializedName, using the substitutions from \p Apply.
static bool createPrespecialized(StringRef UnspecializedName,
                                 ApplySite Apply,
                                 SILOptFunctionBuilder &FuncBuilder) {
  SILModule &M = FuncBuilder.getModule();
  SILFunction *UnspecFunc = M.lookUpFunction(UnspecializedName);
  if (UnspecFunc) {
    if (!UnspecFunc->isDefinition())
      M.loadFunction(UnspecFunc, SILModule::LinkingMode::LinkAll);
  } else {
    UnspecFunc = M.loadFunction(UnspecializedName,
                                SILModule::LinkingMode::LinkAll);
  }

  if (!UnspecFunc || !UnspecFunc->isDefinition())
    return false;

  ReabstractionInfo ReInfo(M.getSwiftModule(), M.isWholeModule(), ApplySite(),
                           UnspecFunc, Apply.getSubstitutionMap(),
                           IsNotSerialized,
                           /*ConvertIndirectToDirect=*/true);

  if (!ReInfo.canBeSpecialized())
    return false;

  GenericFuncSpecializer FuncSpecializer(FuncBuilder,
                                         UnspecFunc, Apply.getSubstitutionMap(),
                                         ReInfo);
  SILFunction *SpecializedF = FuncSpecializer.lookupSpecialization();
  if (!SpecializedF)
    SpecializedF = FuncSpecializer.tryCreateSpecialization();
  if (!SpecializedF)
    return false;

  // Link after prespecializing to pull in everything referenced from another
  // module in case some referenced functions have non-public linkage.
  M.linkFunction(SpecializedF, SILModule::LinkingMode::LinkAll);

  SpecializedF->setLinkage(SILLinkage::Public);
  SpecializedF->setSerialized(IsNotSerialized);
  return true;
}

/// Create pre-specializations of the library function X if \p ProxyFunc has
/// @_semantics("prespecialize.X") attributes.
static bool createPrespecializations(ApplySite Apply, SILFunction *ProxyFunc,
                                     SILOptFunctionBuilder &FuncBuilder) {
  if (Apply.getSubstitutionMap().hasArchetypes())
    return false;

  SILModule &M = FuncBuilder.getModule();

  bool prespecializeFound = false;
  for (const std::string &semAttrStr : ProxyFunc->getSemanticsAttrs()) {
    StringRef semAttr(semAttrStr);
    if (semAttr.consume_front("prespecialize.")) {
      prespecializeFound = true;
      if (!createPrespecialized(semAttr, Apply, FuncBuilder)) {
        M.getASTContext().Diags.diagnose(Apply.getLoc().getSourceLoc(),
                                         diag::cannot_prespecialize,
                                         semAttr);
      }
    }
  }
  return prespecializeFound;
}

static SILFunction *
lookupOrCreatePrespecialization(SILOptFunctionBuilder &funcBuilder,
                                SILFunction *origF, std::string clonedName,
                                ReabstractionInfo &reInfo) {

  if (auto *specializedF = funcBuilder.getModule().lookUpFunction(clonedName)) {
    assert(reInfo.getSpecializedType() ==
               specializedF->getLoweredFunctionType() &&
           "Previously specialized function does not match expected type.");
    return specializedF;
  }

  auto *declaration =
      GenericCloner::createDeclaration(funcBuilder, origF, reInfo, clonedName);
  declaration->setLinkage(SILLinkage::PublicExternal);

  ScopeCloner scopeCloner(*declaration);

  return declaration;
}

bool usePrespecialized(
    SILOptFunctionBuilder &funcBuilder, ApplySite apply, SILFunction *refF,
    const ReabstractionInfo &specializedReInfo,
    ReabstractionInfo &prespecializedReInfo, SpecializedFunction &result) {

  SmallVector<std::tuple<unsigned, ReabstractionInfo, AvailabilityContext>, 4>
      layoutMatches;

  for (auto *SA : refF->getSpecializeAttrs()) {
    if (!SA->isExported())
      continue;
    // Check whether SPI allows using this function.
    auto spiGroup = SA->getSPIGroup();
    if (!spiGroup.empty()) {
      auto currentModule = funcBuilder.getModule().getSwiftModule();
      auto funcModule = SA->getSPIModule();
      // Don't use this SPI if the current module does not import the function's
      // module with @_spi(<spiGroup>).
      if (currentModule != funcModule &&
          !currentModule->isImportedAsSPI(spiGroup, funcModule))
        continue;
    }
    // Check whether the availability of the specialization allows for using
    // it. We check the  deployment target or the current functions availability
    // target depending which one is more recent.
    auto specializationAvail = SA->getAvailability();
    auto &ctxt = funcBuilder.getModule().getSwiftModule()->getASTContext();
    auto deploymentAvail = AvailabilityContext::forDeploymentTarget(ctxt);
    auto currentFn = apply.getFunction();
    auto isInlinableCtxt = (currentFn->getResilienceExpansion()
                             == ResilienceExpansion::Minimal);
    auto currentFnAvailability = currentFn->getAvailabilityForLinkage();

    // If we are in an inlineable function we can't use the specialization except
    // the inlinable function itself has availability we can use.
    if (currentFnAvailability.isAlwaysAvailable() && isInlinableCtxt) {
      continue;
    }
    else if (isInlinableCtxt) {
      deploymentAvail = currentFnAvailability;
    }

    if (!currentFnAvailability.isAlwaysAvailable() &&
        !deploymentAvail.isContainedIn(currentFnAvailability))
      deploymentAvail = currentFnAvailability;
    if (!deploymentAvail.isContainedIn(specializationAvail))
      continue;

    ReabstractionInfo reInfo(funcBuilder.getModule().getSwiftModule(),
                             funcBuilder.getModule().isWholeModule(), refF,
                             SA->getSpecializedSignature(),
                             /*isPrespecialization*/ true);

    if (specializedReInfo.getSpecializedType() != reInfo.getSpecializedType()) {
      SmallVector<Type, 4> newSubs;
      auto specializedSig = SA->getSpecializedSignature();

      auto erasedParams = SA->getTypeErasedParams();
      if(!ctxt.LangOpts.hasFeature(Feature::LayoutPrespecialization) || erasedParams.empty()) {
        continue;
      }

      unsigned score = 0;
      for (auto &entry :
           llvm::enumerate(apply.getSubstitutionMap().getReplacementTypes())) {

        auto genericParam = specializedSig.getGenericParams()[entry.index()];

        bool erased = std::any_of(erasedParams.begin(), erasedParams.end(), [&](auto Ty) {
          return Ty->isEqual(genericParam);
        });

        auto layout = specializedSig->getLayoutConstraint(genericParam);

        if (!erased || !layout || !layout->isClass()) {
          newSubs.push_back(entry.value());
        } else if (!entry.value()->isAnyClassReferenceType() ||
                   entry.value()->isAnyExistentialType()) {
          // non-reference or existential type can't be applied
          break;
        } else if (!specializedSig->getRequiredProtocols(genericParam)
                        .empty()) {
          llvm::report_fatal_error("Unexpected protocol requirements");
        } else if (layout->isNativeClass()) {
          newSubs.push_back(genericParam->getASTContext().TheNativeObjectType);
          score += 1;
        } else {
          newSubs.push_back(genericParam->getASTContext().getAnyObjectType());
        }
      }

      if (newSubs.size() !=
          apply.getSubstitutionMap().getReplacementTypes().size()) {
        continue;
      }

      auto newSubstMap = SubstitutionMap::get(
          apply.getSubstitutionMap().getGenericSignature(), newSubs,
          apply.getSubstitutionMap().getConformances());

      ReabstractionInfo layoutReInfo = ReabstractionInfo(
          funcBuilder.getModule().getSwiftModule(),
          funcBuilder.getModule().isWholeModule(), apply, refF, newSubstMap,
          apply.getFunction()->isSerialized() ? IsSerialized : IsNotSerialized,
          /*ConvertIndirectToDirect=*/true, /*dropMetatypeArgs*/true, nullptr);

      if (layoutReInfo.getSpecializedType() == reInfo.getSpecializedType()) {
        layoutMatches.push_back(
            std::make_tuple(score, reInfo, specializationAvail));
      }

      continue;
    }

    SubstitutionMap subs = reInfo.getCalleeParamSubstitutionMap();
    Mangle::GenericSpecializationMangler mangler(refF, reInfo.isSerialized());
    std::string name = reInfo.isPrespecialized() ?
        mangler.manglePrespecialized(subs) :
        mangler.mangleReabstracted(subs, reInfo.needAlternativeMangling());

    prespecializedReInfo = reInfo;
    auto fn = lookupOrCreatePrespecialization(funcBuilder, refF, name, reInfo);
    if (!specializationAvail.isAlwaysAvailable())
      fn->setAvailabilityForLinkage(specializationAvail);

    result.setFunction(fn);

    return true;
  }

  if (!layoutMatches.empty()) {

    std::tuple<unsigned, ReabstractionInfo, AvailabilityContext> res =
        layoutMatches[0];
    for (auto &tuple : layoutMatches) {
      if (std::get<0>(tuple) > std::get<0>(res))
        res = tuple;
    }

    auto reInfo = std::get<1>(res);
    auto specializationAvail = std::get<2>(res);

    // TODO: Deduplicate
    SubstitutionMap subs = reInfo.getCalleeParamSubstitutionMap();
    Mangle::GenericSpecializationMangler mangler(refF, reInfo.isSerialized());
    std::string name = reInfo.isPrespecialized()
                           ? mangler.manglePrespecialized(subs)
                           : mangler.mangleReabstracted(
                                 subs, reInfo.needAlternativeMangling());

    prespecializedReInfo = reInfo;
    auto fn = lookupOrCreatePrespecialization(funcBuilder, refF, name, reInfo);
    if (!specializationAvail.isAlwaysAvailable())
      fn->setAvailabilityForLinkage(specializationAvail);

    result.setFunction(fn);
    result.computeTypeReplacements(apply);

    return true;
  }

  return false;
}

void swift::trySpecializeApplyOfGeneric(
    SILOptFunctionBuilder &FuncBuilder,
    ApplySite Apply, DeadInstructionSet &DeadApplies,
    SmallVectorImpl<SILFunction *> &NewFunctions,
    OptRemark::Emitter &ORE,
    bool isMandatory) {
  assert(Apply.hasSubstitutions() && "Expected an apply with substitutions!");
  auto *F = Apply.getFunction();
  auto *RefF =
      cast<FunctionRefInst>(Apply.getCallee())->getReferencedFunction();

  LLVM_DEBUG(llvm::dbgs() << "\n\n*** ApplyInst in function " << F->getName()
                          << ":\n";
             Apply.getInstruction()->dumpInContext());

  // If the caller is fragile but the callee is not, bail out.
  // Specializations have shared linkage, which means they do
  // not have an external entry point, Since the callee is not
  // fragile we cannot serialize the body of the specialized
  // callee either.
  if (F->isSerialized() && !RefF->hasValidLinkageForFragileInline())
      return;

  if (shouldNotSpecialize(RefF, F))
    return;

  // If the caller and callee are both fragile, preserve the fragility when
  // cloning the callee. Otherwise, strip it off so that we can optimize
  // the body more.
  IsSerialized_t Serialized = IsNotSerialized;
  if (F->isSerialized())
    Serialized = IsSerialized;

  // If it is OnoneSupport consider all specializations as non-serialized
  // as we do not SIL serialize their bodies.
  // It is important to set this flag here, because it affects the
  // mangling of the specialization's name.
  if (Apply.getModule().isOptimizedOnoneSupportModule()) {
    if (createPrespecializations(Apply, RefF, FuncBuilder)) {
      return;
    }
    Serialized = IsNotSerialized;
  }

  ReabstractionInfo ReInfo(FuncBuilder.getModule().getSwiftModule(),
                           FuncBuilder.getModule().isWholeModule(), Apply, RefF,
                           Apply.getSubstitutionMap(), Serialized,
                           /*ConvertIndirectToDirect=*/ true,
                           /*dropMetatypeArgs=*/ isMandatory,
                           &ORE);
  if (!ReInfo.canBeSpecialized())
    return;

  // Check if there is a pre-specialization available in a library.
  SpecializedFunction prespecializedF{};
  ReabstractionInfo prespecializedReInfo;
  bool replacePartialApplyWithoutReabstraction = false;

  if (usePrespecialized(FuncBuilder, Apply, RefF, ReInfo, prespecializedReInfo,
                        prespecializedF)) {
    ReInfo = prespecializedReInfo;
  }

  // If there is not pre-specialization and we don't have a body give up.
  if (!prespecializedF.hasFunction() && !RefF->isDefinition())
    return;

  SILModule &M = F->getModule();

  bool needAdaptUsers = false;

  auto *PAI = dyn_cast<PartialApplyInst>(Apply);

  if (PAI && ReInfo.hasConversions()) {
    // If we have a partial_apply and we converted some results/parameters from
    // indirect to direct there are 3 cases:
    // 1) All uses of the partial_apply are apply sites again. In this case
    //    we can just adapt all the apply sites which use the partial_apply.
    // 2) The result of the partial_apply is re-abstracted anyway (and the
    //    re-abstracted function type matches with our specialized type). In
    //    this case we can just skip the existing re-abstraction.
    // 3) For all other cases we need to create a new re-abstraction thunk.
    needAdaptUsers = true;
    SmallVector<Operand *, 4> worklist(PAI->getUses());
    while (!worklist.empty()) {
      auto *Use = worklist.pop_back_val();

      SILInstruction *User = Use->getUser();

      // Look through copy_value.
      if (auto *cvi = dyn_cast<CopyValueInst>(User)) {
        llvm::copy(cvi->getUses(), std::back_inserter(worklist));
        continue;
      }
      // Ignore destroy_value.
      if (isa<DestroyValueInst>(User))
        continue;
      // Ignore older ref count instructions.
      if (isa<RefCountingInst>(User))
        continue;
      if (isIncidentalUse(User))
        continue;

      auto FAS = FullApplySite::isa(User);
      if (FAS && FAS.getCallee() == PAI)
        continue;

      auto *PAIUser = dyn_cast<PartialApplyInst>(User);
      if (PAIUser && isPartialApplyOfReabstractionThunk(PAIUser)) {
        CanSILFunctionType NewPAType =
          ReInfo.createSpecializedType(PAI->getFunctionType(), M);
        if (PAIUser->getFunctionType() == NewPAType)
          continue;
      }
      replacePartialApplyWithoutReabstraction = true;
      break;
    }
  }

  GenericFuncSpecializer FuncSpecializer(FuncBuilder,
                                         RefF, Apply.getSubstitutionMap(),
                                         ReInfo, isMandatory);
  SpecializedFunction SpecializedF =
      prespecializedF.hasFunction() ? prespecializedF
                                    : FuncSpecializer.lookupSpecialization();
  if (!SpecializedF.hasFunction()) {
    SpecializedF = FuncSpecializer.tryCreateSpecialization();
    if (!SpecializedF)
      return;
    LLVM_DEBUG(llvm::dbgs() << "Created specialized function: "
                            << SpecializedF->getName() << "\n"
                            << "Specialized function type: "
                            << SpecializedF->getLoweredFunctionType() << "\n");
    NewFunctions.push_back(SpecializedF.getFunction());
  }
  if (replacePartialApplyWithoutReabstraction &&
      SpecializedF.getFunction()->isExternalDeclaration()) {
    // Cannot create a tunk without having the body of the function.
    return;
  }

  if (F->isSerialized() && !SpecializedF->hasValidLinkageForFragileInline()) {
    // If the specialized function already exists as a "IsNotSerialized" function,
    // but now it's called from a "IsSerialized" function, we need to mark it as
    // IsSerialized.
    SpecializedF->setSerialized(IsSerialized);
    assert(SpecializedF->hasValidLinkageForFragileInline());
    
    // ... including all referenced shared functions.
    FuncBuilder.getModule().linkFunction(SpecializedF.getFunction(),
                                         SILModule::LinkingMode::LinkAll);
  }

  ORE.emit([&]() {
    std::string Str;
    llvm::raw_string_ostream OS(Str);
    SpecializedF->getLoweredFunctionType().print(
        OS, PrintOptions::printQuickHelpDeclaration());

    using namespace OptRemark;
    return RemarkPassed("Specialized", *Apply.getInstruction())
           << "Specialized function " << NV("Function", RefF) << " with type "
           << NV("FuncType", OS.str());
  });

  // Verify our function after we have finished fixing up call sites/etc. Dump
  // the generic function if there is an assertion failure (or a crash) to make
  // it easier to debug such problems since the original generic function is
  // easily at hand.
  SWIFT_DEFER {
    if (VerifyFunctionsAfterSpecialization) {
      PrettyStackTraceSILFunction SILFunctionDumper(
          llvm::Twine("Generic function: ") + RefF->getName() +
              ". Specialized Function: " + SpecializedF->getName(),
          RefF);
      SpecializedF->verify();
    }
  };

  assert(ReInfo.getSpecializedType()
         == SpecializedF->getLoweredFunctionType() &&
         "Previously specialized function does not match expected type.");

  DeadApplies.insert(Apply.getInstruction());

  if (replacePartialApplyWithoutReabstraction) {
    // There are some unknown users of the partial_apply. Therefore we need a
    // thunk which converts from the re-abstracted function back to the
    // original function with indirect parameters/results.
    auto *PAI = cast<PartialApplyInst>(Apply.getInstruction());
    SILFunction *Thunk = ReabstractionThunkGenerator(FuncBuilder, ReInfo, PAI,
                                                     SpecializedF.getFunction())
                             .createThunk();
    if (VerifyFunctionsAfterSpecialization) {
      PrettyStackTraceSILFunction SILFunctionDumper(
          llvm::Twine("Thunk For Generic function: ") + RefF->getName() +
              ". Specialized Function: " + SpecializedF->getName(),
          RefF);
      Thunk->verify();
    }
    NewFunctions.push_back(Thunk);
    SILBuilderWithScope Builder(PAI);
    auto *FRI = Builder.createFunctionRef(PAI->getLoc(), Thunk);
    SmallVector<SILValue, 4> Arguments;
    for (auto &Op : PAI->getArgumentOperands()) {
      Arguments.push_back(Op.get());
    }
    auto Subs = ReInfo.getCallerParamSubstitutionMap();
    auto FnTy = Thunk->getLoweredFunctionType();
    Subs = SubstitutionMap::get(FnTy->getSubstGenericSignature(), Subs);
    auto *NewPAI = Builder.createPartialApply(
        PAI->getLoc(), FRI, Subs, Arguments,
        PAI->getType().getAs<SILFunctionType>()->getCalleeConvention(),
        PAI->isOnStack());
    PAI->replaceAllUsesWith(NewPAI);
    DeadApplies.insert(PAI);
    return;
  }
  // Make the required changes to the call site.
  ApplySite newApply =
      replaceWithSpecializedFunction(Apply, SpecializedF, ReInfo);

  if (needAdaptUsers) {
    // Adapt all known users of the partial_apply. This is needed in case we
    // converted some indirect parameters/results to direct ones.
    auto *NewPAI = cast<PartialApplyInst>(newApply);
    ReInfo.prunePartialApplyArgs(NewPAI->getNumArguments());
    for (Operand *Use : NewPAI->getUses()) {
      SILInstruction *User = Use->getUser();
      if (auto FAS = FullApplySite::isa(User)) {
        replaceWithSpecializedCallee(FAS, NewPAI, ReInfo);
        DeadApplies.insert(FAS.getInstruction());
        continue;
      }
      if (auto *PAI = dyn_cast<PartialApplyInst>(User)) {
        SILValue result = NewPAI;
        if (SpecializedF.hasTypeReplacements()) {
          SILBuilderWithScope builder(Apply.getInstruction());
          auto fnType = PAI->getType();
          result =
              builder.createConvertFunction(Apply.getLoc(), NewPAI, fnType,
                                            /*withoutActuallyEscaping*/ false);
        }
        // This is a partial_apply of a re-abstraction thunk. Just skip this.
        assert(PAI->getType() == result->getType());
        PAI->replaceAllUsesWith(result);
        DeadApplies.insert(PAI);
      }
    }
  }
}

// =============================================================================
// Prespecialized symbol lookup.
// =============================================================================

#define PRESPEC_SYMBOL(s) MANGLE_AS_STRING(s),
static const char *PrespecSymbols[] = {
#include "OnonePrespecializations.def"
  nullptr
};
#undef PRESPEC_SYMBOL

llvm::DenseSet<StringRef> PrespecSet;

bool swift::isKnownPrespecialization(StringRef SpecName) {
  if (PrespecSet.empty()) {
    const char **Pos = &PrespecSymbols[0];
    while (const char *Sym = *Pos++) {
      PrespecSet.insert(Sym);
    }
    assert(!PrespecSet.empty());
  }
  return PrespecSet.count(SpecName) != 0;
}

void swift::checkCompletenessOfPrespecializations(SILModule &M) {
  const char **Pos = &PrespecSymbols[0];
  while (const char *Sym = *Pos++) {
    StringRef FunctionName(Sym);
    SILFunction *F = M.lookUpFunction(FunctionName);
    if (!F || F->getLinkage() != SILLinkage::Public) {
      M.getASTContext().Diags.diagnose(SourceLoc(),
                                       diag::missing_prespecialization,
                                       FunctionName);
    }
  }

}

/// Try to look up an existing specialization in the specialization cache.
/// If it is found, it tries to link this specialization.
///
/// For now, it performs a lookup only in the standard library.
/// But in the future, one could think of maintaining a cache
/// of optimized specializations.
static SILFunction *lookupExistingSpecialization(SILModule &M,
                                                 StringRef FunctionName) {
  // Try to link existing specialization only in -Onone mode.
  // All other compilation modes perform specialization themselves.
  // TODO: Cache optimized specializations and perform lookup here?
  // Only check that this function exists, but don't read
  // its body. It can save some compile-time.
  if (isKnownPrespecialization(FunctionName)){
    return M.loadFunction(FunctionName, SILModule::LinkingMode::LinkAll,
                          SILLinkage::PublicExternal);
  }
  return nullptr;
}

SILFunction *swift::lookupPrespecializedSymbol(SILModule &M,
                                               StringRef FunctionName) {
  // First check if the module contains a required specialization already.
  auto *Specialization = M.lookUpFunction(FunctionName);
  if (Specialization) {
    if (Specialization->getLinkage() == SILLinkage::PublicExternal)
      return Specialization;
  }

  // Then check if the required specialization can be found elsewhere.
  Specialization = lookupExistingSpecialization(M, FunctionName);
  if (!Specialization)
    return nullptr;

  assert(hasPublicVisibility(Specialization->getLinkage()) &&
         "Pre-specializations should have public visibility");

  Specialization->setLinkage(SILLinkage::PublicExternal);

  assert(Specialization->isExternalDeclaration()  &&
         "Specialization should be a public external declaration");

  LLVM_DEBUG(llvm::dbgs() << "Found existing specialization for: "
                          << FunctionName << '\n';
             llvm::dbgs() << swift::Demangle::demangleSymbolAsString(
                             Specialization->getName())
                          << "\n\n");

  return Specialization;
}
