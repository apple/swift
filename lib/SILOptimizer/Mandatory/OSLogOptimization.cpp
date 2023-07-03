//===--- OSLogOptimizer.cpp - Optimizes calls to OS Log -------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// This pass implements SIL-level optimizations and diagnostics for the
/// os log APIs based on string interpolations. A mock version of the APIs
/// are available in the private module: OSLogTestHelper. This pass constant
/// evaluates the log calls along with the auto-generated calls to the custom
/// string interpolation methods, which processes the string interpolation
/// passed to the log calls, and folds the constants found during the
/// evaluation. The constants that are folded include the printf-style format
/// string that is constructed by the custom string interpolation methods from
/// the string interpolation, and the size and headers of the byte buffer into
/// which arguments are packed. This pass is closely tied to the implementation
/// of the log APIs.
///
/// Pass Dependencies:  This pass depends on MandatoryInlining and Mandatory
/// Linking happening before this pass and ConstantPropagation happening after
/// this pass. This pass also uses `ConstExprStepEvaluator` defined in
/// `Utils/ConstExpr.cpp`.
///
/// Algorithm Overview:
///
/// This pass implements a function-level transformation that collects calls
/// to the initializer of the custom string interpolation type: OSLogMessage,
/// which are annotated with an @_semantics attribute, and performs the
/// following steps on each such call.
///
///  1. Determines the range of instructions to constant evaluate.
///     The range starts from the first SIL instruction that begins the
///     construction of the custom string interpolation type: OSLogMessage to
///     the last transitive users of OSLogMessage. The log call which is marked
///     as @_transparent will be inlined into the caller before this pass
///     begins.
///
///  2. Constant evaluates the range of instruction identified in Step 1 and
///     collects string and integer-valued instructions who values were found
///     to be constants. The evaluation uses 'ConsExprStepEvaluator' utility.
///
///  3. After constant evaluation, the string and integer-value properties
///     of the custom string interpolation type: `OSLogInterpolation` must be
///     constants. This property is checked and any violations are diagnosed.
///     The errors discovered here may arise from the implementation of the
///     log APIs in the  overlay or could be because of wrong usage of the
///     log APIs.
///
///  4. The constant instructions that were found in step 2 are folded by
///     generating SIL code that produces the constants. This also removes
///     instructions that are dead after folding.
///
/// Code Overview:
///
/// The function 'OSLogOptimization::run' implements the overall driver for
/// steps 1 to 4. The function 'beginOfInterpolation' identifies the beginning of
/// interpolation (step 1) and the function 'getEndPointsOfDataDependentChain'
/// identifies the last transitive users of the OSLogMessage instance (step 1).
/// The function 'constantFold' is a driver for the steps 2 to 4. Step 2 is
/// implemented by the function 'collectConstants', step 3 by
/// 'detectAndDiagnoseErrors' and 'checkOSLogMessageIsConstant', and step 4 by
/// 'substituteConstants' and 'emitCodeForSymbolicValue'. The remaining
/// functions in the file implement the subtasks and utilities needed by the
/// above functions.
///
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/Basic/OptimizationMode.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Demangling/Demangler.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/CFG.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILConstants.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILLocation.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/TypeLowering.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/CompileTimeInterpolationUtils.h"
#include "swift/SILOptimizer/Utils/ConstExpr.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"
#include "llvm/ADT/BreadthFirstIterator.h"
#include "llvm/ADT/MapVector.h"

using namespace swift;
using namespace Lowering;

template <typename... T, typename... U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
                     U &&... args) {
  // The lifetime of StringRef arguments will be extended as necessary by this
  // utility. The copy happens in onTentativeDiagnosticFlush at the bottom of
  // DiagnosticEngine.cpp, which is called when the destructor of the
  // InFlightDiagnostic returned by diagnose runs.
  Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

namespace {

/// If the given instruction is a call to the compiler-intrinsic initializer
/// of String that accepts string literals, return the called function.
/// Otherwise, return nullptr.
static SILFunction *getStringMakeUTF8Init(SILInstruction *inst) {
  auto *apply = dyn_cast<ApplyInst>(inst);
  if (!apply)
    return nullptr;

  SILFunction *callee = apply->getCalleeFunction();
  if (!callee || !callee->hasSemanticsAttr(semantics::STRING_MAKE_UTF8))
    return nullptr;
  return callee;
}

// A cache of string-related, SIL information that is needed to create and
// initialize strings from raw string literals. This information is
// extracted from instructions while they are constant evaluated. Though the
// information contained here can be constructed from scratch, extracting it
// from existing instructions is more efficient.
class StringSILInfo {
  /// SILFunction corresponding to an intrinsic string initializer that
  /// constructs a Swift String from a string literal.
  SILFunction *stringInitIntrinsic = nullptr;

  /// SIL metatype of String.
  SILType stringMetatype = SILType();

public:
  /// Extract and cache the required string-related information from the
  /// given instruction, if possible.
  void extractStringInfoFromInstruction(SILInstruction *inst) {
    // If the cache is already initialized do nothing.
    if (stringInitIntrinsic)
      return;

    SILFunction *callee = getStringMakeUTF8Init(inst);
    if (!callee)
      return;

    this->stringInitIntrinsic = callee;

    MetatypeInst *stringMetatypeInst =
        dyn_cast<MetatypeInst>(inst->getOperand(4)->getDefiningInstruction());
    this->stringMetatype = stringMetatypeInst->getType();
  }

  bool isInitialized() { return stringInitIntrinsic != nullptr; }

  SILFunction *getStringInitIntrinsic() const {
    assert(stringInitIntrinsic);
    return stringInitIntrinsic;
  }

  SILType getStringMetatype() const {
    assert(stringMetatype);
    return stringMetatype;
  }
};

/// State needed for constant folding.
class FoldState {
public:
  /// Storage for symbolic values constructed during interpretation.
  SymbolicValueBumpAllocator allocator;

  /// Evaluator for evaluating instructions one by one.
  ConstExprStepEvaluator constantEvaluator;

  /// Information needed for folding strings.
  StringSILInfo stringInfo;

  /// Instruction from where folding must begin.
  SILInstruction *beginInstruction;

  /// Instructions that mark the end points of constant evaluation.
  SmallSetVector<SILInstruction *, 2> endInstructions;

private:
  /// SIL values that were found to be constants during
  /// constant evaluation.
  SmallVector<SILValue, 4> constantSILValues;

public:
  FoldState(SILFunction *fun, unsigned assertConfig, SILInstruction *beginInst,
            ArrayRef<SILInstruction *> endInsts)
      : constantEvaluator(allocator, fun, assertConfig),
        beginInstruction(beginInst),
        endInstructions(endInsts.begin(), endInsts.end()) {}

  void addConstantSILValue(SILValue value) {
    constantSILValues.push_back(value);
  }

  ArrayRef<SILValue> getConstantSILValues() {
    return ArrayRef<SILValue>(constantSILValues);
  }
};

/// Return true if and only if the given nominal type declaration is that of
/// a stdlib Int or stdlib Bool.
static bool isStdlibIntegerOrBoolDecl(NominalTypeDecl *numberDecl,
                                      ASTContext &astCtx) {
  return (numberDecl == astCtx.getIntDecl() ||
          numberDecl == astCtx.getInt8Decl() ||
          numberDecl == astCtx.getInt16Decl() ||
          numberDecl == astCtx.getInt32Decl() ||
          numberDecl == astCtx.getInt64Decl() ||
          numberDecl == astCtx.getUIntDecl() ||
          numberDecl == astCtx.getUInt8Decl() ||
          numberDecl == astCtx.getUInt16Decl() ||
          numberDecl == astCtx.getUInt32Decl() ||
          numberDecl == astCtx.getUInt64Decl() ||
          numberDecl == astCtx.getBoolDecl());
}

/// Return true if and only if the given SIL type represents a Stdlib or builtin
/// integer type or a Bool type.
static bool isIntegerOrBoolType(SILType silType, ASTContext &astContext) {
  if (silType.is<BuiltinIntegerType>()) {
    return true;
  }
  NominalTypeDecl *nominalDecl = silType.getNominalOrBoundGenericNominal();
  return nominalDecl && isStdlibIntegerOrBoolDecl(nominalDecl, astContext);
}

/// Return true iff the given value is a stdlib Int or Bool and it not a direct
/// construction of Int or Bool.
static bool isFoldableIntOrBool(SILValue value, ASTContext &astContext) {
  return isIntegerOrBoolType(value->getType(), astContext) &&
         !isa<StructInst>(value);
}

/// Return true iff the given value is a string and is not an initialization
/// of an string from a string literal.
static bool isFoldableString(SILValue value, ASTContext &astContext) {
  return value->getType().getASTType()->isString() &&
         (!isa<ApplyInst>(value) ||
          !getStringMakeUTF8Init(cast<ApplyInst>(value)));
}

/// Return true iff the given value is an array and is not an initialization
/// of an array from an array literal.
static bool isFoldableArray(SILValue value, ASTContext &astContext) {
  if (!value->getType().getASTType()->isArray())
    return false;
  // If value is an initialization of an array from a literal or an empty array
  // initializer, it need not be folded. Arrays constructed from literals use a
  // function with semantics: "array.uninitialized_intrinsic" that returns
  // a pair, where the first element of the pair is the array.
  SILInstruction *definingInst = value->getDefiningInstruction();
  if (!definingInst)
    return true;
  SILInstruction *constructorInst = definingInst;
  if (isa<DestructureTupleInst>(definingInst) ||
      isa<TupleExtractInst>(definingInst)) {
    constructorInst = definingInst->getOperand(0)->getDefiningInstruction();
  }
  if (!constructorInst || !isa<ApplyInst>(constructorInst))
    return true;
  SILFunction *callee = cast<ApplyInst>(constructorInst)->getCalleeFunction();
  return !callee ||
         (!callee->hasSemanticsAttr(semantics::ARRAY_INIT_EMPTY) &&
          !callee->hasSemanticsAttr(semantics::ARRAY_UNINITIALIZED_INTRINSIC) &&
          !callee->hasSemanticsAttr(semantics::ARRAY_FINALIZE_INTRINSIC));
}

/// Return true iff the given value is a closure but is not a creation of a
/// closure e.g., through partial_apply or thin_to_thick_function or
/// convert_function.
static bool isFoldableClosure(SILValue value) {
  return value->getType().is<SILFunctionType>() &&
         (!isa<FunctionRefInst>(value) && !isa<PartialApplyInst>(value) &&
          !isa<ThinToThickFunctionInst>(value) &&
          !isa<ConvertFunctionInst>(value));
}

/// Check whether a SILValue is foldable. String, integer, array and
/// function values are foldable with the following exceptions:
///  - Addresses cannot be folded.
///  - Literals need not be folded.
///  - Results of ownership instructions like load_borrow/copy_value need not
///  be folded
///  - Constructors such as \c struct Int or \c string.init() need not be folded.
static bool isSILValueFoldable(SILValue value) {
  SILInstruction *definingInst = value->getDefiningInstruction();
  if (!definingInst)
    return false;
  ASTContext &astContext = definingInst->getFunction()->getASTContext();
  SILType silType = value->getType();
  return (!silType.isAddress() && !isa<LiteralInst>(definingInst) &&
          !isa<LoadBorrowInst>(definingInst) &&
          !isa<BeginBorrowInst>(definingInst) &&
          !isa<CopyValueInst>(definingInst) &&
          (isFoldableIntOrBool(value, astContext) ||
           isFoldableString(value, astContext) ||
           isFoldableArray(value, astContext) || isFoldableClosure(value)));
}

/// Diagnose traps and instruction-limit exceeded errors. These have customized
/// error messages. \returns true if the given error is diagnosed. Otherwise,
/// returns false.
static bool diagnoseSpecialErrors(SILInstruction *unevaluableInst,
                                  SymbolicValue errorInfo) {
  SourceLoc sourceLoc = unevaluableInst->getLoc().getSourceLoc();
  ASTContext &ctx = unevaluableInst->getFunction()->getASTContext();
  UnknownReason unknownReason = errorInfo.getUnknownReason();

  if (unknownReason.getKind() == UnknownReason::Trap) {
    // We have an assertion failure or fatal error.
    diagnose(ctx, sourceLoc, diag::oslog_constant_eval_trap,
             unknownReason.getTrapMessage());
    return true;
  }
  if (unknownReason.getKind() == UnknownReason::TooManyInstructions) {
    // This should not normally happen. But could be because of extensions
    // defined by users, or very rarely due to unknown bugs in the os_log API
    // implementation. These errors may get hidden during testing as it is input
    // specific.
    diagnose(ctx, sourceLoc, diag::oslog_too_many_instructions);
    return true;
  }
  return false;
}

/// Diagnose failure during evaluation of a call to a constant-evaluable
/// function that is not a specially-handled error. These are errors that
/// happen within  'appendInterpolation' calls, which must be constant
/// evaluable by the definition of APIs.
static void diagnoseErrorInConstantEvaluableFunction(ApplyInst *call,
                                                     SymbolicValue errorInfo) {
  SILFunction *callee = call->getCalleeFunction();
  assert(callee);
  SILLocation loc = call->getLoc();
  SourceLoc sourceLoc = loc.getSourceLoc();
  ASTContext &astContext = callee->getASTContext();

  // Here, we know very little about what actually went wrong. It could be due
  // to bugs in the library implementation or in extensions created by users.
  // Emit a general message here and some diagnostic notes.
  std::string demangledCalleeName = Demangle::demangleSymbolAsString(
      callee->getName(),
      Demangle::DemangleOptions::SimplifiedUIDemangleOptions());
  diagnose(astContext, sourceLoc, diag::oslog_invalid_log_message);
  diagnose(astContext, sourceLoc, diag::oslog_const_evaluable_fun_error,
           demangledCalleeName);
  errorInfo.emitUnknownDiagnosticNotes(loc);
}

/// Detect and emit diagnostics for errors found during evaluation. Errors
/// can happen due to bugs in the implementation of the os log API, or
/// due to incorrect use of the os log API.
static bool detectAndDiagnoseErrors(SymbolicValue errorInfo,
                                    SILInstruction *unevaluableInst) {
  // TODO: fix the globalStrinTableBuiltin error after emitting diagnostics.
  SILFunction *parentFun = unevaluableInst->getFunction();
  ASTContext &astContext = parentFun->getASTContext();

  if (diagnoseSpecialErrors(unevaluableInst, errorInfo))
    return true;
  // If evaluation of any constant_evaluable function call fails, point
  // to that failed function along with a reason.
  ApplyInst *call = dyn_cast<ApplyInst>(unevaluableInst);
  if (call) {
    SILFunction *callee = call->getCalleeFunction();
    if (callee && isConstantEvaluable(callee)) {
      diagnoseErrorInConstantEvaluableFunction(call, errorInfo);
      return true; // abort evaluation.
    }
  }
  // Every other error must happen in the top-level code containing the string
  // interpolation construction and body of the log methods. If we have a
  // fail-stop error, point to the error and abort evaluation. Otherwise, just
  // ignore the error and continue evaluation as this error might not affect the
  // constant value of the OSLogMessage instance.
  if (isFailStopError(errorInfo)) {
    SILLocation loc = unevaluableInst->getLoc();
    diagnose(astContext, loc.getSourceLoc(), diag::oslog_invalid_log_message);
    errorInfo.emitUnknownDiagnosticNotes(loc);
    return true;
  }
  return false;
}

/// Given a 'foldState', constant evaluate instructions from
/// 'foldState.beginInstruction' until an instruction in
/// 'foldState.endInstructions' is seen. Add foldable, constant-valued
/// instructions discovered during the evaluation to
/// 'foldState.constantSILValues'.
/// \returns error information if the evaluation failed.
static llvm::Optional<SymbolicValue> collectConstants(FoldState &foldState) {

  ConstExprStepEvaluator &constantEvaluator = foldState.constantEvaluator;
  SILBasicBlock::iterator currI = foldState.beginInstruction->getIterator();
  auto &endInstructions = foldState.endInstructions;

  // The loop will break when it sees a return instruction or an instruction in
  // endInstructions or when the next instruction to evaluate cannot be
  // determined (which may happened due to non-constant branches).
  while (true) {
    SILInstruction *currInst = &(*currI);
    if (endInstructions.count(currInst))
      break;

    // Initialize string info from this instruction if possible.
    foldState.stringInfo.extractStringInfoFromInstruction(currInst);

    llvm::Optional<SymbolicValue> errorInfo = llvm::None;
    llvm::Optional<SILBasicBlock::iterator> nextI = llvm::None;

    std::tie(nextI, errorInfo) = evaluateOrSkip(constantEvaluator, currI);

    // If the evaluation of this instruction failed, check whether it should be
    // diagnosed and reported. If so, abort evaluation. Otherwise, continue
    // evaluation if possible as this error could be due to an instruction that
    // doesn't affect the OSLogMessage value.
    if (errorInfo && detectAndDiagnoseErrors(errorInfo.value(), currInst)) {
      return errorInfo;
    }

    if (!nextI) {
      // We cannot find the next instruction to continue evaluation, and we
      // haven't seen any reportable errors during evaluation. Therefore,
      // consider this the end point of evaluation.
      return llvm::None; // No error.
    }

    // Set the next instruction to continue evaluation from.
    currI = nextI.value();

    // If the instruction results are foldable and if we found a constant value
    // for the results, record it.
    for (SILValue instructionResult : currInst->getResults()) {
      if (!isSILValueFoldable(instructionResult))
        continue;

      llvm::Optional<SymbolicValue> constantVal =
          constantEvaluator.lookupConstValue(instructionResult);
      if (constantVal.has_value()) {
        foldState.addConstantSILValue(instructionResult);
      }
    }
  }
  return llvm::None; // No error.
}

/// Generate SIL code to create an array of constant size from the given
/// SILValues \p elements. This function creates the same sequence of SIL
/// instructions that would be generated for initializing an array from an array
/// literal of the form [element1, element2, ..., elementn].
///
/// \param elements SILValues that the array should contain
/// \param arrayType the type of the array that must be created.
/// \param builder SILBuilder that provides the context for emitting the code
/// for the array.
/// \param loc SILLocation to use in the emitted instructions.
/// \return the SILValue of the array that is created with the given \c
/// elements.
static SILValue emitCodeForConstantArray(ArrayRef<SILValue> elements,
                                         CanType arrayType, SILBuilder &builder,
                                         SILLocation loc) {
  ASTContext &astContext = builder.getASTContext();
  assert(arrayType->isArray());
  SILModule &module = builder.getModule();

  // Create a SILValue for the number of elements.
  unsigned numElements = elements.size();
  SILValue numElementsSIL = builder.createIntegerLiteral(
      loc, SILType::getBuiltinWordType(astContext), numElements);

  // Find the SILFunction that corresponds to _allocateUninitializedArray.
  FuncDecl *arrayAllocateDecl = astContext.getAllocateUninitializedArray();
  assert(arrayAllocateDecl);
  std::string allocatorMangledName =
      SILDeclRef(arrayAllocateDecl, SILDeclRef::Kind::Func).mangle();
  SILFunction *arrayAllocateFun =
      module.loadFunction(allocatorMangledName,
                          SILModule::LinkingMode::LinkNormal);
  assert(arrayAllocateFun);

  SILFunction *arrayFinalizeFun = nullptr;
  if (numElements != 0) {
    if (FuncDecl *arrayFinalizeDecl = astContext.getFinalizeUninitializedArray()) {
      std::string finalizeMangledName =
          SILDeclRef(arrayFinalizeDecl, SILDeclRef::Kind::Func).mangle();
      arrayFinalizeFun =
          module.loadFunction(finalizeMangledName,
                              SILModule::LinkingMode::LinkNormal);
      assert(arrayFinalizeFun);
    }
  }

  // Call the _allocateUninitializedArray function with numElementsSIL. The
  // call returns a two-element tuple, where the first element is the newly
  // created array and the second element is a pointer to the internal storage
  // of the array.
  SubstitutionMap subMap = arrayType->getContextSubstitutionMap(
      module.getSwiftModule(), astContext.getArrayDecl());
  FunctionRefInst *arrayAllocateRef =
      builder.createFunctionRef(loc, arrayAllocateFun);
  ApplyInst *applyInst = builder.createApply(
      loc, arrayAllocateRef, subMap, ArrayRef<SILValue>(numElementsSIL));

  // Extract the elements of the tuple returned by the call to the allocator.
  DestructureTupleInst *destructureInst =
      builder.createDestructureTuple(loc, applyInst);
  SILValue arraySIL = destructureInst->getResults()[0];
  SILValue storagePointerSIL = destructureInst->getResults()[1];

  if (elements.empty()) {
    // Nothing more to be done if we are creating an empty array.
    return arraySIL;
  }

  // Convert the pointer to the storage to an address. The elements will be
  // stored into offsets from this address.
  SILType elementSILType = elements[0]->getType();
  PointerToAddressInst *storageAddr = builder.createPointerToAddress(
      loc, storagePointerSIL, elementSILType.getAddressType(),
      /*isStrict*/ true,
      /*isInvariant*/ false);

  // Iterate over the elements and store them into the storage address
  // after offsetting it appropriately.

  // Create a TypeLowering for emitting stores. Note that TypeLowering
  // provides a utility for emitting stores for storing trivial and
  // non-trivial values, and also handles OSSA and non-OSSA.
  const TypeLowering &elementTypeLowering =
      builder.getTypeLowering(elementSILType);

  unsigned elementIndex = 0;
  for (SILValue elementSIL : elements) {
    // Compute the address where the element must be stored.
    SILValue currentStorageAddr;
    if (elementIndex != 0) {
      SILValue indexSIL = builder.createIntegerLiteral(
          loc, SILType::getBuiltinWordType(astContext), elementIndex);
      currentStorageAddr = builder.createIndexAddr(loc, storageAddr, indexSIL,
                              /*needsStackProtection=*/ false);
    } else {
      currentStorageAddr = storageAddr;
    }
    // Store the generated element into the currentStorageAddr. This is an
    // initializing store and therefore there is no need to free any existing
    // element.
    elementTypeLowering.emitStore(builder, loc, elementSIL, currentStorageAddr,
                                  StoreOwnershipQualifier::Init);
    ++elementIndex;
  }
  if (arrayFinalizeFun) {
    FunctionRefInst *arrayFinalizeRef =
        builder.createFunctionRef(loc, arrayFinalizeFun);
    arraySIL = builder.createApply(loc, arrayFinalizeRef, subMap,
                                   ArrayRef<SILValue>(arraySIL));
  }
  return arraySIL;
}

/// Given a SILValue \p value, return the instruction immediately following the
/// definition of the value. That is, if the value is defined by an
/// instruction, return the instruction following the definition. Otherwise, if
/// the value is a basic block parameter, return the first instruction of the
/// basic block.
SILInstruction *getInstructionFollowingValueDefinition(SILValue value) {
  SILInstruction *definingInst = value->getDefiningInstruction();
  if (definingInst) {
    return &*std::next(definingInst->getIterator());
  }
  // Here value must be a basic block argument.
  SILBasicBlock *bb = value->getParentBlock();
  return &*bb->begin();
}

/// Given a SILValue \p value, create a copy of the value using copy_value in
/// OSSA or retain in non-OSSA, if \p value is a non-trivial type. Otherwise, if
/// \p value is a trivial type, return the value itself.
SILValue makeOwnedCopyOfSILValue(SILValue value, SILFunction &fun) {
  SILType type = value->getType();
  if (type.isTrivial(fun) || type.isAddress())
    return value;

  SILInstruction *instAfterValueDefinition =
      getInstructionFollowingValueDefinition(value);
  SILLocation copyLoc = instAfterValueDefinition->getLoc();
  SILBuilderWithScope builder(instAfterValueDefinition);
  const TypeLowering &typeLowering = builder.getTypeLowering(type);
  SILValue copy = typeLowering.emitCopyValue(builder, copyLoc, value);
  return copy;
}

/// Generate SIL code that computes the constant given by the symbolic value
/// `symVal`. Note that strings and struct-typed constant values will require
/// multiple instructions to be emitted.
/// \param symVal symbolic value for which SIL code needs to be emitted.
/// \param expectedType the expected type of the instruction that would be
/// computing the symbolic value `symVal`. The type is accepted as a
/// parameter as some symbolic values like integer constants can inhabit more
/// than one type.
/// \param builder SILBuilder that provides the context for emitting the code
/// for the symbolic value
/// \param loc SILLocation to use in the emitted instructions.
/// \param stringInfo String.init and metatype information for generating code
/// for string literals.
static SILValue emitCodeForSymbolicValue(SymbolicValue symVal,
                                         Type expectedType, SILBuilder &builder,
                                         SILLocation &loc,
                                         StringSILInfo &stringInfo) {
  ASTContext &astContext = expectedType->getASTContext();

  switch (symVal.getKind()) {
  case SymbolicValue::String: {
    assert(expectedType->isString());

    StringRef stringVal = symVal.getStringValue();
    StringLiteralInst *stringLitInst = builder.createStringLiteral(
        loc, stringVal, StringLiteralInst::Encoding::UTF8);

    // Create a builtin word for the size of the string
    IntegerLiteralInst *sizeInst = builder.createIntegerLiteral(
        loc, SILType::getBuiltinWordType(astContext), stringVal.size());
    // Set isAscii to false.
    IntegerLiteralInst *isAscii = builder.createIntegerLiteral(
        loc, SILType::getBuiltinIntegerType(1, astContext), 0);
    // Create a metatype inst.
    MetatypeInst *metatypeInst =
        builder.createMetatype(loc, stringInfo.getStringMetatype());

    auto args = SmallVector<SILValue, 4>();
    args.push_back(stringLitInst);
    args.push_back(sizeInst);
    args.push_back(isAscii);
    args.push_back(metatypeInst);

    FunctionRefInst *stringInitRef =
        builder.createFunctionRef(loc, stringInfo.getStringInitIntrinsic());
    ApplyInst *applyInst = builder.createApply(
        loc, stringInitRef, SubstitutionMap(), ArrayRef<SILValue>(args));
    return applyInst;
  }
  case SymbolicValue::Integer: { // Builtin integer types.
    APInt resInt = symVal.getIntegerValue();
    assert(expectedType->is<BuiltinIntegerType>());

    SILType builtinIntType =
        SILType::getPrimitiveObjectType(expectedType->getCanonicalType());
    IntegerLiteralInst *intLiteralInst =
        builder.createIntegerLiteral(loc, builtinIntType, resInt);
    return intLiteralInst;
  }
  case SymbolicValue::Aggregate: {
    // Support only stdlib integer or bool structs.
    StructDecl *structDecl = expectedType->getStructOrBoundGenericStruct();
    assert(structDecl);
    assert(isStdlibIntegerOrBoolDecl(structDecl, astContext));
    assert(symVal.getAggregateType()->isEqual(expectedType) &&
           "aggregate symbolic value's type and expected type do not match");

    VarDecl *propertyDecl = structDecl->getStoredProperties().front();
    Type propertyType = expectedType->getTypeOfMember(
        propertyDecl->getModuleContext(), propertyDecl);
    SymbolicValue propertyVal = symVal.lookThroughSingleElementAggregates();
    SILValue newPropertySIL = emitCodeForSymbolicValue(
        propertyVal, propertyType, builder, loc, stringInfo);
    // The lowered SIL type of an integer/bool type is just the primitive
    // object type containing the Swift type.
    SILType aggregateType =
        SILType::getPrimitiveObjectType(expectedType->getCanonicalType());
    StructInst *newStructInst = builder.createStruct(
        loc, aggregateType, ArrayRef<SILValue>(newPropertySIL));
    return newStructInst;
  }
  case SymbolicValue::Array: {
    assert(expectedType->isEqual(symVal.getArrayType()));
    CanType elementType;
    ArrayRef<SymbolicValue> arrayElements =
        symVal.getStorageOfArray().getStoredElements(elementType);
    auto elementSILType = builder.getModule().Types
      .getLoweredType(AbstractionPattern::getOpaque(), elementType,
                      TypeExpansionContext(builder.getFunction()));

    // Emit code for the symbolic values corresponding to the array elements.
    SmallVector<SILValue, 8> elementSILValues;
    for (SymbolicValue elementSymVal : arrayElements) {
      SILValue elementSIL = emitCodeForSymbolicValue(elementSymVal,
                                                     elementSILType.getASTType(),
                                                     builder, loc, stringInfo);
      elementSILValues.push_back(elementSIL);
    }
    SILValue arraySIL = emitCodeForConstantArray(
        elementSILValues, expectedType->getCanonicalType(), builder, loc);
    return arraySIL;
  }
  case SymbolicValue::Closure: {
    assert(expectedType->is<AnyFunctionType>() ||
           expectedType->is<SILFunctionType>());

    SILModule &module = builder.getModule();
    SymbolicClosure *closure = symVal.getClosure();
    SILValue resultVal;

    // If the closure was created in the context of this function where the code
    // is generated, reuse the original closure value (after extending its
    // lifetime by copying).
    SingleValueInstruction *originalClosureInst = closure->getClosureInst();
    SILFunction &fun = builder.getFunction();
    if (originalClosureInst->getFunction() == &fun) {
      // Copy the closure, since the returned value must be owned and the
      // closure's lifetime must be extended until this point.
      resultVal = makeOwnedCopyOfSILValue(originalClosureInst, fun);
    } else {
      // If the closure captures a value that is not a constant, it should only
      // come from the caller of the log call. It should be handled by the then
      // case and we should never reach here. Assert this.
      assert(closure->hasOnlyConstantCaptures() &&
             "closure with non-constant captures not defined in this function");
      SubstitutionMap callSubstMap = closure->getCallSubstitutionMap();
      ArrayRef<SymbolicClosureArgument> captures = closure->getCaptures();
      // Recursively emit code for all captured values which must be mapped to a
      // symbolic value.
      SmallVector<SILValue, 4> capturedSILVals;
      for (SymbolicClosureArgument capture : captures) {
        SILValue captureOperand = capture.first;
        llvm::Optional<SymbolicValue> captureSymVal = capture.second;
        assert(captureSymVal);
        // Note that the captured operand type may have generic parameters which
        // has to be substituted with the substitution map that was inferred by
        // the constant evaluator at the partial-apply site.
        SILType operandType = captureOperand->getType();
        SILType captureType = operandType.subst(module, callSubstMap);
        SILValue captureSILVal = emitCodeForSymbolicValue(
            captureSymVal.value(), captureType.getASTType(), builder, loc,
            stringInfo);
        capturedSILVals.push_back(captureSILVal);
      }
      FunctionRefInst *functionRef =
          builder.createFunctionRef(loc, closure->getTarget());
      SILType closureType = closure->getClosureType();
      ParameterConvention convention =
          closureType.getAs<SILFunctionType>()->getCalleeConvention();
      resultVal = builder.createPartialApply(loc, functionRef, callSubstMap,
                                             capturedSILVals, convention);
    }
    // If the expected type is a SILFunctionType convert the closure to the
    // expected type using a convert_function instruction. Otherwise, if the
    // expected type is AnyFunctionType, nothing needs to be done.
    // Note that we cannot assert the lowering in the latter case, as that
    // utility doesn't exist yet.
    auto resultType = resultVal->getType().castTo<SILFunctionType>();
    CanType expectedCanType = expectedType->getCanonicalType();
    if (auto expectedFnType = dyn_cast<SILFunctionType>(expectedCanType)) {
      assert(expectedFnType->getUnsubstitutedType(module)
               == resultType->getUnsubstitutedType(module));
      // Convert to the expected type if necessary.
      if (expectedFnType != resultType) {
        auto convert = builder.createConvertFunction(
            loc, resultVal, SILType::getPrimitiveObjectType(expectedFnType),
            false);
        return convert;
      }
    }
    return resultVal;
  }
  default: {
    llvm_unreachable("Symbolic value kind is not supported");
  }
  }
}

/// Collect the end points of the instructions that are data dependent on \c
/// value. A instruction is data dependent on \c value if its result may
/// transitively depends on \c value. Note that data dependencies through
/// addresses are not tracked by this function.
///
/// \param value SILValue that is not an address.
/// \param fun SILFunction that defines \c value.
/// \param endUsers buffer for storing the found end points of the data
/// dependence chain.
static void
getEndPointsOfDataDependentChain(SingleValueInstruction *value, SILFunction *fun,
                                 SmallVectorImpl<SILInstruction *> &endUsers) {
  assert(!value->getType().isAddress());

  SmallVector<SILInstruction *, 16> transitiveUsers;
  // Get transitive users of value, ignoring use-def chain going through
  // branches. These transitive users define the end points of the constant
  // evaluation. Igoring use-def chains through branches causes constant
  // evaluation to miss some constant folding opportunities. This can be
  // relaxed in the future, if necessary.
  getTransitiveUsers(value, transitiveUsers);

  // Compute the lifetime frontier of all the transitive uses which are the
  // instructions following the last uses. Every exit from the last uses will
  // have a lifetime frontier.
  SILInstruction *valueDefinition = value->getDefiningInstruction();
  SILInstruction *def =
      valueDefinition ? valueDefinition : &(value->getParentBlock()->front());
  ValueLifetimeAnalysis lifetimeAnalysis(def, transitiveUsers);
  ValueLifetimeAnalysis::Frontier frontier;
  bool hasCriticalEdges = lifetimeAnalysis.computeFrontier(
      frontier, ValueLifetimeAnalysis::DontModifyCFG);
  endUsers.append(frontier.begin(), frontier.end());
  if (!hasCriticalEdges)
    return;
  // If there are some lifetime frontiers on the critical edges, take the
  // first instruction of the target of the critical edge as the frontier. This
  // will suffice as every exit from the visitedUsers must go through one of
  // them.
  for (auto edgeIndexPair : lifetimeAnalysis.getCriticalEdges()) {
    SILBasicBlock *targetBB =
        edgeIndexPair.first->getSuccessors()[edgeIndexPair.second];
    endUsers.push_back(&targetBB->front());
  }
}

/// Given a guaranteed SILValue \p value, return a borrow-scope introducing
/// value, if there is exactly one such introducing value. Otherwise, return
/// None. There can be multiple borrow scopes for a SILValue iff it is derived
/// from a guaranteed basic block parameter representing a phi node.
static llvm::Optional<BorrowedValue>
getUniqueBorrowScopeIntroducingValue(SILValue value) {
  assert(value->getOwnershipKind() == OwnershipKind::Guaranteed &&
         "parameter must be a guaranteed value");
  return getSingleBorrowIntroducingValue(value);
}

/// Replace all uses of \c originalVal by \c foldedVal and adjust lifetimes of
/// original and folded values by emitting required destroy/release instructions
/// at the right places. Note that this function does not remove any
/// instruction.
///
/// \param originalVal the SIL value that is replaced.
/// \param foldedVal the SIL value that replaces the \c originalVal.
/// \param fun the SIL function containing the \c foldedVal and \c originalVal
static void replaceAllUsesAndFixLifetimes(SILValue foldedVal,
                                          SILValue originalVal,
                                          SILFunction *fun) {
  SILInstruction *originalInst = originalVal->getDefiningInstruction();
  SILInstruction *foldedInst = foldedVal->getDefiningInstruction();
  assert(originalInst &&
         "cannot constant fold function or basic block parameter");
  assert(!isa<TermInst>(originalInst) &&
         "cannot constant fold a terminator instruction");
  assert(foldedInst && "constant value does not have a defining instruction");

  if (originalVal->getType().isTrivial(*fun)) {
    assert(foldedVal->getType().isTrivial(*fun));
    // Just replace originalVal by foldedVal.
    originalVal->replaceAllUsesWith(foldedVal);
    return;
  }
  assert(!foldedVal->getType().isTrivial(*fun));
  assert(fun->hasOwnership());
  assert(foldedVal->getOwnershipKind() == OwnershipKind::Owned &&
         "constant value must have owned ownership kind");

  if (originalVal->getOwnershipKind() == OwnershipKind::Owned) {
    originalVal->replaceAllUsesWith(foldedVal);
    // Destroy originalVal, which is now unused, immediately after its
    // definition. Note that originalVal's destroys are now transferred to
    // foldedVal.
    SILInstruction *insertionPoint = &(*std::next(originalInst->getIterator()));
    SILBuilderWithScope builder(insertionPoint);
    SILLocation loc = insertionPoint->getLoc();
    builder.emitDestroyValueOperation(loc, originalVal);
    return;
  }

  // Here, originalVal is guaranteed. It must belong to a borrow scope that
  // begins at a scope introducing instruction e.g. begin_borrow or load_borrow.
  // The foldedVal should also have been inserted at the beginning of the scope.
  // Therefore, create a borrow of foldedVal at the beginning of the scope and
  // use the borrow in place of the originalVal. Also, end the borrow and
  // destroy foldedVal at the end of the borrow scope.
  assert(originalVal->getOwnershipKind() == OwnershipKind::Guaranteed);

  // FIXME: getUniqueBorrowScopeIntroducingValue may look though various storage
  // casts. There's no reason to think that it's valid to replace uses of
  // originalVal with a new borrow of the "introducing value". All casts
  // potentially need to be cloned.
  llvm::Optional<BorrowedValue> originalScopeBegin =
      getUniqueBorrowScopeIntroducingValue(originalVal);
  assert(originalScopeBegin &&
         "value without a unique borrow scope should not have been folded");
  SILInstruction *scopeBeginInst =
      originalScopeBegin->value->getDefiningInstruction();
  assert(scopeBeginInst);

  SILBuilderWithScope builder(scopeBeginInst);
  SILValue borrow =
      builder.emitBeginBorrowOperation(scopeBeginInst->getLoc(), foldedVal);

  originalVal->replaceAllUsesWith(borrow);

  SmallVector<SILInstruction *, 4> scopeEndingInsts;
  originalScopeBegin->getLocalScopeEndingInstructions(scopeEndingInsts);

  for (SILInstruction *scopeEndingInst : scopeEndingInsts) {
    SILBuilderWithScope builder(scopeEndingInst);
    builder.emitEndBorrowOperation(scopeEndingInst->getLoc(), borrow);
    builder.emitDestroyValueOperation(scopeEndingInst->getLoc(), foldedVal);
  }
  return;
}

/// Given a fold state with constant-valued instructions, substitute the
/// instructions with the constant values. The constant values could be strings
/// or Stdlib integer-struct values or builtin integers.
static void substituteConstants(FoldState &foldState) {
  ConstExprStepEvaluator &evaluator = foldState.constantEvaluator;
  // Instructions that are possibly dead since their results are folded.
  SmallVector<SILInstruction *, 8> possiblyDeadInsts;

  for (SILValue constantSILValue : foldState.getConstantSILValues()) {
    SymbolicValue constantSymbolicVal =
        evaluator.lookupConstValue(constantSILValue).value();
    // Make sure that the symbolic value tracked in the foldState is a constant.
    // In the case of ArraySymbolicValue, the array storage could be a non-constant
    // if some instruction in the array initialization sequence was not evaluated
    // and skipped.
    if (!constantSymbolicVal.containsOnlyConstants()) {
      assert(constantSymbolicVal.getKind() != SymbolicValue::String && "encountered non-constant string symbolic value");
      continue;
    }

    SILInstruction *definingInst = constantSILValue->getDefiningInstruction();
    assert(definingInst);
    SILFunction *fun = definingInst->getFunction();

    // Find an insertion point for inserting the new constant value. If we are
    // folding a value like struct_extract within a borrow scope, we need to
    // insert the constant value at the beginning of the borrow scope. This
    // is because the borrowed value is expected to be alive during its entire
    // borrow scope and could be stored into memory and accessed indirectly
    // without a copy e.g. using store_borrow within the borrow scope. On the
    // other hand, if we are folding an owned value, we can insert the constant
    // value at the point where the owned value is defined.
    SILInstruction *insertionPoint = definingInst;
    if (constantSILValue->getOwnershipKind() == OwnershipKind::Guaranteed) {
      llvm::Optional<BorrowedValue> borrowIntroducer =
          getUniqueBorrowScopeIntroducingValue(constantSILValue);
      if (!borrowIntroducer) {
        // This case happens only if constantSILValue is derived from a
        // guaranteed basic block parameter. This is unlikely because the values
        // that have to be folded should just be a struct-extract of an owned
        // instance of OSLogMessage.
        continue;
      }
      insertionPoint = borrowIntroducer->value->getDefiningInstruction();
      assert(insertionPoint && "borrow scope beginning is a parameter");
    }

    SILBuilderWithScope builder(insertionPoint);
    SILLocation loc = insertionPoint->getLoc();
    CanType instType = constantSILValue->getType().getASTType();
    SILValue foldedSILVal = emitCodeForSymbolicValue(
        constantSymbolicVal, instType, builder, loc, foldState.stringInfo);

    // Replace constantSILValue with foldedSILVal and adjust the lifetime and
    // ownership of the values appropriately.
    replaceAllUsesAndFixLifetimes(foldedSILVal, constantSILValue, fun);
    possiblyDeadInsts.push_back(definingInst);
  }
}

/// Check whether OSLogMessage and OSLogInterpolation instances and all their
/// stored properties are constants. If not, it indicates errors that are due to
/// incorrect implementation of OSLogMessage either in the os module or in the
/// extensions created by users. Detect and emit diagnostics for such errors.
/// The diagnostics here are for os log library authors.
static bool checkOSLogMessageIsConstant(SingleValueInstruction *osLogMessage,
                                        FoldState &foldState) {
  ConstExprStepEvaluator &constantEvaluator = foldState.constantEvaluator;
  SILLocation loc = osLogMessage->getLoc();
  SourceLoc sourceLoc = loc.getSourceLoc();
  SILFunction *fn = osLogMessage->getFunction();
  SILModule &module = fn->getModule();
  ASTContext &astContext = fn->getASTContext();

  llvm::Optional<SymbolicValue> osLogMessageValueOpt =
      constantEvaluator.lookupConstValue(osLogMessage);
  if (!osLogMessageValueOpt ||
      osLogMessageValueOpt->getKind() != SymbolicValue::Aggregate) {
    diagnose(astContext, sourceLoc, diag::oslog_non_constant_message);
    return true;
  }

  // The first (and only) property of OSLogMessage is the OSLogInterpolation
  // instance.
  SymbolicValue osLogInterpolationValue =
      osLogMessageValueOpt->getAggregateMembers()[0];
  if (!osLogInterpolationValue.isConstant()) {
    diagnose(astContext, sourceLoc, diag::oslog_non_constant_interpolation);
    return true;
  }

  // Check if every property of the OSLogInterpolation instance has a constant
  // value.
  SILType osLogMessageType = osLogMessage->getType();
  StructDecl *structDecl = osLogMessageType.getStructOrBoundGenericStruct();
  assert(structDecl);

  auto typeExpansionContext =
      TypeExpansionContext(*osLogMessage->getFunction());
  VarDecl *interpolationPropDecl = structDecl->getStoredProperties().front();
  SILType osLogInterpolationType = osLogMessageType.getFieldType(
      interpolationPropDecl, module, typeExpansionContext);
  StructDecl *interpolationStruct =
      osLogInterpolationType.getStructOrBoundGenericStruct();
  assert(interpolationStruct);

  auto propertyDecls = interpolationStruct->getStoredProperties();
  ArrayRef<SymbolicValue> propertyValues =
      osLogInterpolationValue.getAggregateMembers();
  auto propValueI = propertyValues.begin();
  bool errorDetected = false;
  // Also, track if there is a string-valued property.
  bool hasStringValuedProperty = false;

  for (auto *propDecl : propertyDecls) {
    SymbolicValue propertyValue = *(propValueI++);
    if (!propertyValue.isConstant()) {
      diagnose(astContext, sourceLoc, diag::oslog_property_not_constant,
               propDecl->getNameStr());
      errorDetected = true;
      break;
    }
    hasStringValuedProperty = propertyValue.getKind() == SymbolicValue::String;
  }

  // If we have a string-valued property but don't have the stringInfo
  // initialized here, it means the initializer OSLogInterpolation is explicitly
  // called, which should be diagnosed.
  if (hasStringValuedProperty && !foldState.stringInfo.isInitialized()) {
    diagnose(astContext, sourceLoc, diag::oslog_message_explicitly_created);
    errorDetected = true;
  }
  return errorDetected;
}

/// Return true iff the given address-valued instruction has only stores into
/// it. This function tests for the conditions under which a call, that was
/// constant evaluated, that writes into the address-valued instruction can be
/// considered as a point store and exploits it to remove such uses.
/// TODO: eventually some of this logic can be moved to
/// PredictableDeadAllocElimination pass, but the assumption about constant
/// evaluable functions taking inout parameters is not easily generalizable to
/// arbitrary non-constant contexts where the function could be used. The logic
/// here is relying on the fact that the constant_evaluable function has been
/// evaluated and therefore doesn't have any side-effects.
static bool hasOnlyStoreUses(SingleValueInstruction *addressInst) {
  for (Operand *use : addressInst->getUses()) {
    SILInstruction *user = use->getUser();
    switch (user->getKind()) {
    default:
      return false;
    case SILInstructionKind::BeginAccessInst: {
      if (!hasOnlyStoreUses(cast<BeginAccessInst>(user)))
        return false;
      continue;
    }
    case SILInstructionKind::StoreInst: {
      // For now, ignore assigns as we need to destroy_addr its dest if it
      // is deleted.
      if (cast<StoreInst>(user)->getOwnershipQualifier() ==
          StoreOwnershipQualifier::Assign)
        return false;
      continue;
    }
    case SILInstructionKind::EndAccessInst:
    case SILInstructionKind::DestroyAddrInst:
    case SILInstructionKind::InjectEnumAddrInst:
    case SILInstructionKind::DeallocStackInst:
      continue;
    case SILInstructionKind::ApplyInst: {
      ApplyInst *apply = cast<ApplyInst>(user);
      SILFunction *callee = apply->getCalleeFunction();
      if (!callee || !isConstantEvaluable(callee) || !apply->use_empty())
        return false;
      // Note that since we are looking at an alloc_stack used to produce the
      // OSLogMessage instance, this constant_evaluable call should have been
      // evaluated successfully by the evaluator. Otherwise, we would have
      // reported an error earlier. Therefore, all values manipulated by such
      // a call are symbolic constants and the call would not have any global
      // side effects. The following logic relies on this property.
      // If there are other indirect writable results for the call other than
      // the alloc_stack we are checking, it may not be dead. Therefore, bail
      // out.
      FullApplySite applySite(apply);
      unsigned numWritableArguments =
        getNumInOutArguments(applySite) + applySite.getNumIndirectSILResults();
      if (numWritableArguments > 1)
        return false;
      SILArgumentConvention convention = applySite.getArgumentConvention(*use);
      if (convention == SILArgumentConvention::Indirect_In ||
          convention == SILArgumentConvention::Indirect_In_Guaranteed) {
        if (numWritableArguments > 0)
          return false;
      }
      // Here, either there are no writable parameters or the alloc_stack
      // is the only writable parameter.
      continue;
    }
    }
  }
  return true;
}

/// Delete the given alloc_stack instruction by deleting the users of the
/// instruction. In case the user is a begin_apply, recursively delete the users
/// of begin_apply. This will also fix the lifetimes of the deleted instructions
/// whenever possible.
static void forceDeleteAllocStack(SingleValueInstruction *inst,
                                  InstructionDeleter &deleter) {
  SmallVector<SILInstruction *, 8> users;
  for (Operand *use : inst->getUses())
    users.push_back(use->getUser());

  for (SILInstruction *user : users) {
    if (isIncidentalUse(user))
      continue;
    if (isa<DestroyAddrInst>(user)) {
      deleter.forceDelete(user);
      continue;
    }
    if (isa<BeginAccessInst>(user)) {
      forceDeleteAllocStack(cast<BeginAccessInst>(user), deleter);
      continue;
    }
    // Notify the deletion worklist in case user's other operands become dead.
    deleter.getCallbacks().notifyWillBeDeleted(user);
    deleter.forceDeleteAndFixLifetimes(user);
  }
  deleter.forceDelete(inst);
}

/// Delete \c inst , if it is dead, along with its dead users and invoke the
/// callback whenever an instruction is deleted.
static void
deleteInstructionWithUsersAndFixLifetimes(SILInstruction *inst,
                                          InstructionDeleter &deleter) {
  // If this is an alloc_stack, it can be eliminated as long as it is only
  // stored into or destroyed.
  if (AllocStackInst *allocStack = dyn_cast<AllocStackInst>(inst)) {
    if (hasOnlyStoreUses(allocStack))
      forceDeleteAllocStack(allocStack, deleter);
    return;
  }
  deleter.recursivelyDeleteUsersIfDead(inst);
}

/// Try to dead-code eliminate the OSLogMessage instance \c oslogMessage passed
/// to the os log call and clean up its dependencies. If the instance cannot be
/// eliminated, emit diagnostics.
/// \returns true if elimination is successful and false if it is not successful
/// and diagnostics is emitted.
static bool tryEliminateOSLogMessage(SingleValueInstruction *oslogMessage) {
  // List of instructions that are possibly dead.
  SmallVector<SILInstruction *, 4> worklist = {oslogMessage};
  // Set of all deleted instructions.
  SmallPtrSet<SILInstruction *, 4> deletedInstructions;

  auto callbacks =
      InstModCallbacks().onNotifyWillBeDeleted([&](SILInstruction *deadInst) {
        // Add operands of all deleted instructions to the worklist so that
        // they can be recursively deleted if possible.
        for (Operand &operand : deadInst->getAllOperands()) {
          if (SILInstruction *definingInstruction =
                  operand.get()->getDefiningInstruction()) {
            if (!deletedInstructions.count(definingInstruction))
              worklist.push_back(definingInstruction);
          }
        }
        (void)deletedInstructions.insert(deadInst);
      });
  InstructionDeleter deleter(std::move(callbacks));

  unsigned startIndex = 0;
  while (startIndex < worklist.size()) {
    SILInstruction *inst = worklist[startIndex++];
    if (deletedInstructions.count(inst))
      continue;
    deleteInstructionWithUsersAndFixLifetimes(inst, deleter);
    // Call cleanupDeadInstructions incrementally because it may expose a dead
    // alloc_stack, which will only be deleted by this pass via
    // deleteInstructionWithUsersAndFixLifetimes().
    deleter.cleanupDeadInstructions();
  }

  // If the OSLogMessage instance is not deleted, either we couldn't see the
  // body of the log call or there is a bug in the library implementation.
  // Assuming that the library implementation is correct, it means that either
  // OSLogMessage is used in a context where it is not supposed to be used, or
  // we somehow saw a conditional branch with a non-constant argument before
  // completing evaluation (this can happen with the os_log(_:log:type)
  // overload, when log or type is an optional unwrapping). Report an error
  // that covers both contexts. (Note that it is very hard to distinguish these
  // error cases in the current state.)
  if (!deletedInstructions.count(oslogMessage)) {
    SILFunction *fun = oslogMessage->getFunction();
    diagnose(fun->getASTContext(), oslogMessage->getLoc().getSourceLoc(),
             diag::oslog_message_alive_after_opts);
    return false;
  }
  return true;
}

/// Constant evaluate instructions starting from \p start and fold the uses
/// of the SIL value \p oslogMessage.
/// \returns true if folding is successful and false if it is not successful and
/// diagnostics is emitted.
static bool constantFold(SILInstruction *start,
                         SingleValueInstruction *oslogMessage,
                         unsigned assertConfig) {
  SILFunction *fun = start->getFunction();
  assert(fun->hasOwnership() && "function not in ownership SIL");

  // Initialize fold state.
  SmallVector<SILInstruction *, 2> endUsersOfOSLogMessage;
  getEndPointsOfDataDependentChain(oslogMessage, fun, endUsersOfOSLogMessage);
  assert(!endUsersOfOSLogMessage.empty());

  FoldState state(fun, assertConfig, start, endUsersOfOSLogMessage);

  auto errorInfo = collectConstants(state);
  if (errorInfo) // Evaluation failed with diagnostics.
    return false;

  // At this point, the `OSLogMessage` instance should be mapped to a constant
  // value in the interpreter state. If this is not the case, it means the
  // overlay implementation of OSLogMessage (or its extensions by users) are
  // incorrect. Detect and diagnose this scenario.
  bool errorDetected = checkOSLogMessageIsConstant(oslogMessage, state);
  if (errorDetected)
    return false;

  substituteConstants(state);
  return tryEliminateOSLogMessage(oslogMessage);
}

/// Given a call to the initializer of OSLogMessage, which conforms to
/// 'ExpressibleByStringInterpolation', find the first instruction, if any, that
/// marks the beginning of the string interpolation that is used to create an
/// OSLogMessage instance. This function traverses the backward data-dependence
/// chain of the given OSLogMessage initializer: \p oslogInit. As a special case
/// it avoids chasing the data-dependencies from the captured values of
/// partial-apply instructions, as a partial apply instruction is considered as
/// a constant regardless of the constantness of its captures.
static SILInstruction *beginOfInterpolation(ApplyInst *oslogInit) {
  auto oslogInitCallSite = FullApplySite(oslogInit);
  SILFunction *callee = oslogInitCallSite.getCalleeFunction();

  assert (callee->hasSemanticsAttrThatStartsWith("oslog.message.init"));
  // The initializer must return the OSLogMessage instance directly.
  assert(oslogInitCallSite.getNumArguments() >= 1 &&
         oslogInitCallSite.getNumIndirectSILResults() == 0);

  // List of backward dependencies that needs to be analyzed.
  SmallVector<SILInstruction *, 4> worklist = { oslogInit };
  SmallPtrSet<SILInstruction *, 4> seenInstructions = { oslogInit };
  // List of instructions that could potentially mark the beginning of the
  // interpolation.
  SmallPtrSet<SILInstruction *, 4> candidateStartInstructions;

  unsigned i = 0;
  while (i < worklist.size()) {
    SILInstruction *inst = worklist[i++];

    if (isa<PartialApplyInst>(inst)) {
      // Partial applies are used to capture the dynamic arguments passed to
      // the string interpolation. Their arguments are not required to be
      // known at compile time and they need not be constant evaluated.
      // Therefore, follow only the dependency chain along function ref operand.
      SILInstruction *definingInstruction =
          inst->getOperand(0)->getDefiningInstruction();
      assert(definingInstruction && "no function-ref operand in partial-apply");
      if (seenInstructions.insert(definingInstruction).second) {
        worklist.push_back(definingInstruction);
        candidateStartInstructions.insert(definingInstruction);
      }
      continue;
    }

    for (Operand &operand : inst->getAllOperands()) {
      if (SILInstruction *definingInstruction =
            operand.get()->getDefiningInstruction()) {
        if (seenInstructions.count(definingInstruction))
          continue;
        worklist.push_back(definingInstruction);
        seenInstructions.insert(definingInstruction);
        candidateStartInstructions.insert(definingInstruction);
      }
      // If there is no defining instruction for this operand, it could be a
      // basic block or function parameter. Such operands are not considered
      // in the backward slice. Dependencies through them are safe to ignore
      // in this context.
    }

    // If the instruction: `inst` has an operand, its definition should precede
    // `inst` in the control-flow order. Therefore, remove `inst` from the
    // candidate start instructions.
    if (inst->getNumOperands() > 0) {
      candidateStartInstructions.erase(inst);
    }

    if (!isa<AllocStackInst>(inst)) {
      continue;
    }

    // If we have an alloc_stack instruction, include stores into it into the
    // backward dependency list. However, whether alloc_stack precedes the
    // definitions of values stored into the location in the control-flow order
    // can only be determined by traversing the instructions in the control-flow
    // order.
    AllocStackInst *allocStackInst = cast<AllocStackInst>(inst);
    for (StoreInst *storeInst : allocStackInst->getUsersOfType<StoreInst>()) {
      worklist.push_back(storeInst);
      candidateStartInstructions.insert(storeInst);
    }
    // Skip other uses of alloc_stack including function calls on the
    // alloc_stack and data dependencies through them. This is done because
    // all functions using the alloc_stack are expected to be constant evaluated
    // and therefore should only be passed constants or auto closures. These
    // constants must be constructed immediately before the call and would only
    // appear in the SIL after the alloc_stack instruction. This invariant is
    // relied upon here so as to restrict the backward dependency search, which
    // in turn keeps the code that is constant evaluated small.
    // Note that if the client code violates this assumption, it will be
    // diagnosed by this pass (in function detectAndDiagnoseErrors) as it will
    // result in non-constant values for OSLogMessage instance.
  }

  // Find the first basic block in the control-flow order. Typically, if
  // formatting and privacy options are literals, all candidate instructions
  // must be in the same basic block. But, this code doesn't rely on that
  // assumption.
  BasicBlockSet candidateBBs(oslogInit->getFunction());
  SILBasicBlock *candidateBB = nullptr;
  unsigned numCandidateBBsFound = 0;
  for (auto *candidate: candidateStartInstructions) {
    candidateBB = candidate->getParent();
    if (candidateBBs.insert(candidateBB))
      ++numCandidateBBsFound;
  }

  SILBasicBlock *firstBB = nullptr;
  if (numCandidateBBsFound == 1) {
    assert(candidateBB);
    firstBB = candidateBB;
  } else {
    SILBasicBlock *entryBB = oslogInit->getFunction()->getEntryBlock();
    for (SILBasicBlock *bb : llvm::breadth_first<SILBasicBlock *>(entryBB)) {
      if (candidateBBs.contains(bb)) {
        firstBB = bb;
        break;
      }
    }
    if (!firstBB) {
      // This case will be reached only if the log call appears in unreachable
      // code and, for some reason, its data dependencies extend beyond a basic
      // block. This case should generally not happen unless the library
      // implementation of the os log APIs change. It is better to warn in this
      // case, rather than skipping the call silently.
      diagnose(callee->getASTContext(), oslogInit->getLoc().getSourceLoc(),
               diag::oslog_call_in_unreachable_code);
      return nullptr;
    }
  }

  // Iterate over the instructions in the firstBB and find the instruction that
  // starts the interpolation.
  SILInstruction *startInst = nullptr;
  for (SILInstruction &inst : *firstBB) {
    if (candidateStartInstructions.count(&inst)) {
      startInst = &inst;
      break;
    }
  }
  assert(startInst && "could not find beginning of interpolation");
  return startInst;
}

/// Replace every _globalStringTablePointer builtin in the transitive users of
/// oslogMessage with an empty string literal. This would suppress the errors
/// emitted by a later pass on _globalStringTablePointerBuiltins. This utility
/// should be called only when this pass emits diagnostics.
static void
suppressGlobalStringTablePointerError(SingleValueInstruction *oslogMessage) {
  SmallVector<SILInstruction *, 8> users;
  getTransitiveUsers(oslogMessage, users);

  // Collect all globalStringTablePointer instructions.
  SmallVector<BuiltinInst *, 4> globalStringTablePointerInsts;
  for (SILInstruction *user : users) {
    BuiltinInst *bi = dyn_cast<BuiltinInst>(user);
    if (bi &&
        bi->getBuiltinInfo().ID == BuiltinValueKind::GlobalStringTablePointer)
      globalStringTablePointerInsts.push_back(bi);
  }

  // Replace the globalStringTablePointer builtins by a string_literal
  // instruction for an empty string and clean up dead code.
  InstructionDeleter deleter;
  for (BuiltinInst *bi : globalStringTablePointerInsts) {
    SILBuilderWithScope builder(bi);
    StringLiteralInst *stringLiteral = builder.createStringLiteral(
        bi->getLoc(), StringRef(""), StringLiteralInst::Encoding::UTF8);
    bi->replaceAllUsesWith(stringLiteral);
    // The builtin instruction is likely dead. But since we are iterating over
    // many instructions, do the cleanup at the end.
    deleter.trackIfDead(bi);
  }
  deleter.cleanupDeadInstructions();
}

/// If the SILInstruction is an initialization of OSLogMessage, return the
/// initialization call as an ApplyInst. Otherwise, return nullptr.
static ApplyInst *getAsOSLogMessageInit(SILInstruction *inst) {
  auto *applyInst = dyn_cast<ApplyInst>(inst);
  if (!applyInst) {
    return nullptr;
  }

  SILFunction *callee = applyInst->getCalleeFunction();
  if (!callee ||
      !callee->hasSemanticsAttrThatStartsWith("oslog.message.init")) {
    return nullptr;
  }

  // Default argument generators created for a function also inherit
  // the semantics attribute of the function. Therefore, check that there are
  // at least two operands for this apply instruction.
  if (applyInst->getNumOperands() > 1) {
    return applyInst;
  }
  return nullptr;
}

/// Return true iff the SIL function \c fun is a method of the \c OSLogMessage
/// type or a type that has the @_semantics("oslog.message.type") annotation.
static bool isMethodOfOSLogMessage(SILFunction &fun) {
  DeclContext *declContext = fun.getDeclContext();
  if (!declContext)
    return false;
  Decl *decl = declContext->getAsDecl();
  if (!decl)
    return false;
  ConstructorDecl *ctor = dyn_cast<ConstructorDecl>(decl);
  if (!ctor)
    return false;
  DeclContext *parentContext = ctor->getParent();
  if (!parentContext)
    return false;
  NominalTypeDecl *typeDecl = parentContext->getSelfNominalTypeDecl();
  if (!typeDecl)
    return false;
  return typeDecl->getName() == fun.getASTContext().Id_OSLogMessage
    || typeDecl->hasSemanticsAttr(semantics::OSLOG_MESSAGE_TYPE);
}

class OSLogOptimization : public SILFunctionTransform {

  ~OSLogOptimization() override {}

  /// The entry point to the transformation.
  void run() override {
    auto &fun = *getFunction();
    unsigned assertConfig = getOptions().AssertConfig;

    // Don't rerun optimization on deserialized functions or stdlib functions.
    if (fun.wasDeserializedCanonical()) {
      return;
    }

    // Skip methods of OSLogMessage type. This avoid unnecessary work and also
    // avoids falsely diagnosing the auto-generated (transparent) witness method
    // of OSLogMessage, which ends up invoking the OSLogMessage initializer:
    // "oslog.message.init_interpolation" without an interpolated string
    // literal that is expected by this pass.
    if (isMethodOfOSLogMessage(fun)) {
      return;
    }

    // Collect all 'OSLogMessage.init' in the function. 'OSLogMessage' is a
    // custom string interpolation type used by the new OS log APIs.
    SmallVector<ApplyInst *, 4> oslogMessageInits;
    for (auto &bb : fun) {
      for (auto &inst : bb) {
        auto init = getAsOSLogMessageInit(&inst);
        if (!init)
          continue;
        oslogMessageInits.push_back(init);
      }
    }

    bool madeChange = false;

    // Constant fold the uses of properties of OSLogMessage instance. Note that
    // the function body will change due to constant folding, after each
    // iteration.
    for (auto *oslogInit : oslogMessageInits) {
      SILInstruction *interpolationStart = beginOfInterpolation(oslogInit);
      if (!interpolationStart) {
        // The log call is in unreachable code here.
        continue;
      }
      bool foldingSucceeded =
          constantFold(interpolationStart, oslogInit, assertConfig);
      // If folding did not succeeded, it implies that an error was diagnosed.
      // However, this will also trigger a diagnostics later on since
      // _globalStringTablePointerBuiltin would not be passed a string literal.
      // Suppress this error by synthesizing a dummy string literal for the
      // builtin.
      if (!foldingSucceeded)
        suppressGlobalStringTablePointerError(oslogInit);
      madeChange = true;
    }
    if (madeChange) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createOSLogOptimization() {
  return new OSLogOptimization();
}
