//===--- SILBuilder.h - Class for creating SIL Constructs -------*- C++ -*-===//
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

#ifndef SWIFT_SIL_SILBUILDER_H
#define SWIFT_SIL_SILBUILDER_H

#include "SILDebugVariable.h"
#include "SILInstruction.h"
#include "swift/Basic/ProfileCounter.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILUndef.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/StringExtras.h"
#include <type_traits>

namespace swift {

using Atomicity = RefCountingInst::Atomicity;

class SILDebugScope;
class IntegerLiteralExpr;
class FloatLiteralExpr;
class SILGlobalVariable;

/// Manage the state needed for a SIL pass across multiple, independent
/// SILBuilder invocations.
///
/// A SIL pass can instantiate a SILBuilderContext object to track information
/// across multiple, potentially independent invocations of SILBuilder. This
/// allows utilities used within the pass to construct a new SILBuilder instance
/// whenever it is convenient or appropriate. For example, a separate SILBuilder
/// should be constructed whenever the current debug location or insertion point
/// changed. Reusing the same SILBuilder and calling setInsertionPoint() easily
/// leads to incorrect debug information.
class SILBuilderContext {
  friend class SILBuilder;

  SILModule &Module;

  /// Allow the SIL module conventions to be overridden within the builder.
  /// This supports passes that lower SIL to a new stage.
  SILModuleConventions silConv = SILModuleConventions(Module);

  /// If this pointer is non-null, then any inserted instruction is
  /// recorded in this list.
  ///
  /// TODO: Give this ownership of InsertedInstrs and migrate users that
  /// currently provide their own InsertedInstrs.
  SmallVectorImpl<SILInstruction *> *InsertedInstrs = nullptr;

public:
  explicit SILBuilderContext(
      SILModule &M, SmallVectorImpl<SILInstruction *> *InsertedInstrs = 0)
      : Module(M), InsertedInstrs(InsertedInstrs) {}

  SILModule &getModule() { return Module; }

  // Allow a pass to override the current SIL module conventions. This should
  // only be done by a pass responsible for lowering SIL to a new stage
  // (e.g. AddressLowering).
  void setSILConventions(SILModuleConventions silConv) {
    this->silConv = silConv;
  }

protected:
  /// Notify the context of each new instruction after it is inserted in the
  /// instruction stream.
  void notifyInserted(SILInstruction *Inst) {
    // If the SILBuilder client wants to know about new instructions, record
    // this.
    if (InsertedInstrs)
      InsertedInstrs->push_back(Inst);
  }
};

class SILBuilder {
  friend class SILBuilderWithScope;

  /// Temporary context for clients that don't provide their own.
  SILBuilderContext TempContext;

  /// Reference to the provided SILBuilderContext.
  SILBuilderContext &C;

  /// The SILFunction that we are currently inserting into if we have one.
  ///
  /// If we are building into a block associated with a SILGlobalVariable this
  /// will be a nullptr.
  ///
  /// TODO: This can be made cleaner by using a PointerUnion or the like so we
  /// can store the SILGlobalVariable here as well.
  SILFunction *F;

  /// If this is non-null, the instruction is inserted in the specified
  /// basic block, at the specified InsertPt.  If null, created instructions
  /// are not auto-inserted.
  SILBasicBlock *BB;
  SILBasicBlock::iterator InsertPt;
  const SILDebugScope *CurDebugScope = nullptr;
  llvm::Optional<SILLocation> CurDebugLocOverride = llvm::None;

public:
  explicit SILBuilder(SILFunction &F)
      : TempContext(F.getModule()), C(TempContext), F(&F), BB(nullptr) {}

  SILBuilder(SILFunction &F, SmallVectorImpl<SILInstruction *> *InsertedInstrs)
      : TempContext(F.getModule(), InsertedInstrs), C(TempContext), F(&F),
        BB(nullptr) {}

  explicit SILBuilder(SILInstruction *I,
                      SmallVectorImpl<SILInstruction *> *InsertedInstrs = 0)
      : TempContext(I->getFunction()->getModule(), InsertedInstrs),
        C(TempContext), F(I->getFunction()) {
    setInsertionPoint(I);
  }

  explicit SILBuilder(SILInstruction *I, const SILDebugScope *debugScope)
      : TempContext(I->getFunction()->getModule()),
        C(TempContext), F(I->getFunction()), CurDebugScope(debugScope) {
    setInsertionPoint(I);
  }

  explicit SILBuilder(SILBasicBlock::iterator I,
                      SmallVectorImpl<SILInstruction *> *InsertedInstrs = 0)
      : SILBuilder(&*I, InsertedInstrs) {}

  explicit SILBuilder(SILBasicBlock *BB,
                      SmallVectorImpl<SILInstruction *> *InsertedInstrs = 0)
      : TempContext(BB->getParent()->getModule(), InsertedInstrs),
        C(TempContext), F(BB->getParent()) {
    setInsertionPoint(BB);
  }

  explicit SILBuilder(SILBasicBlock *BB, const SILDebugScope *debugScope)
      : TempContext(BB->getParent()->getModule()),
        C(TempContext), F(BB->getParent()), CurDebugScope(debugScope) {
    setInsertionPoint(BB);
  }

  explicit SILBuilder(SILGlobalVariable *GlobVar,
                      SmallVectorImpl<SILInstruction *> *InsertedInstrs = 0);

  SILBuilder(SILBasicBlock *BB, SILBasicBlock::iterator InsertPt,
             SmallVectorImpl<SILInstruction *> *InsertedInstrs = 0)
      : TempContext(BB->getParent()->getModule(), InsertedInstrs),
        C(TempContext), F(BB->getParent()) {
    setInsertionPoint(BB, InsertPt);
  }

  /// Build instructions before the given insertion point, inheriting the debug
  /// location.
  ///
  /// SILBuilderContext must outlive this SILBuilder instance.
  SILBuilder(SILInstruction *I, SILBuilderContext &C,
             const SILDebugScope *DS = nullptr)
      : TempContext(C.getModule()), C(C), F(I->getFunction()) {
    setInsertionPoint(I);
    if (DS)
      setCurrentDebugScope(DS);
  }

  SILBuilder(SILBasicBlock *BB, SILBuilder &B,
             const SILDebugScope *DS = nullptr)
      : SILBuilder(BB, B.getBuilderContext(), DS) {}

  /// Build instructions before the given insertion point, inheriting the debug
  /// location.
  ///
  /// SILBuilderContext must outlive this SILBuilder instance.
  SILBuilder(SILBasicBlock *BB, SILBuilderContext &C,
             const SILDebugScope *DS = nullptr)
      : TempContext(C.getModule()), C(C), F(BB->getParent()) {
    assert(DS && "block has no debug scope");
    setInsertionPoint(BB);
    if (DS)
      setCurrentDebugScope(DS);
  }

  virtual ~SILBuilder() {}

  // Allow a pass to override the current SIL module conventions. This should
  // only be done by a pass responsible for lowering SIL to a new stage
  // (e.g. AddressLowering).
  void setSILConventions(SILModuleConventions silConv) { C.silConv = silConv; }

  SILFunction &getFunction() const {
    assert(F && "cannot create this instruction without a function context");
    return *F;
  }
  
  bool isInsertingIntoGlobal() const { return F == nullptr; }

  TypeExpansionContext getTypeExpansionContext() const {
    if (!F)
      return TypeExpansionContext::minimal();
    return TypeExpansionContext(getFunction());
  }

  SILBuilderContext &getBuilderContext() const { return C; }
  SILModule &getModule() const { return C.Module; }
  ASTContext &getASTContext() const { return getModule().getASTContext(); }
  const Lowering::TypeLowering &getTypeLowering(SILType T) const {

    auto expansion = TypeExpansionContext::maximal(getModule().getSwiftModule(),
                                                   getModule().isWholeModule());
    // If there's no current SILFunction, we're inserting into a global
    // variable initializer.
    if (F) {
      expansion = TypeExpansionContext(getFunction());
    }
    return getModule().Types.getTypeLowering(T, expansion);
  }

  void setCurrentDebugScope(const SILDebugScope *DS) { CurDebugScope = DS; }
  const SILDebugScope *getCurrentDebugScope() const { return CurDebugScope; }

  /// Apply a debug location override. If loc is None, the current override is
  /// removed. Otherwise, newly created debug locations use the given location.
  /// Note: the override location does not apply to debug_value[_addr].
  void applyDebugLocOverride(llvm::Optional<SILLocation> loc) {
    CurDebugLocOverride = loc;
  }

  /// Get the current debug location override.
  llvm::Optional<SILLocation> getCurrentDebugLocOverride() const {
    return CurDebugLocOverride;
  }

  /// Convenience function for building a SILDebugLocation.
  virtual SILDebugLocation
  getSILDebugLocation(SILLocation Loc, bool ForMetaInstruction = false) {
    // FIXME: Audit all uses and enable this assertion.
    // assert(getCurrentDebugScope() && "no debug scope");
    auto Scope = getCurrentDebugScope();
    if (!Scope && F)
        Scope = F->getDebugScope();
    auto overriddenLoc = CurDebugLocOverride ? *CurDebugLocOverride : Loc;
    return SILDebugLocation(overriddenLoc, Scope);
  }

  /// When the frontend generates synthesized conformances it generates a
  /// fully-typechecked AST without source locations. This means that the
  /// ASTScope-based mechanism to generate SILDebugScopes doesn't work, which
  /// means we can't disambiguate local variables in different lexical
  /// scopes. To avoid a verification error later in the pipeline, drop all
  /// variables without a proper source location.
  bool shouldDropVariable(SILDebugVariable Var, SILLocation Loc) {
    return !Var.ArgNo && Loc.isSynthesizedAST();
  }


  /// If we have a SILFunction, return SILFunction::hasOwnership(). If we have a
  /// SILGlobalVariable, just return false.
  bool hasOwnership() const {
    if (F)
      return F->hasOwnership();
    return false;
  }

  //===--------------------------------------------------------------------===//
  // Insertion Point Management
  //===--------------------------------------------------------------------===//

  bool hasValidInsertionPoint() const { return BB != nullptr; }
  SILBasicBlock *getInsertionBB() const { return BB; }
  SILBasicBlock::iterator getInsertionPoint() const { return InsertPt; }
  SILLocation getInsertionPointLoc() const {
    assert(!insertingAtEndOfBlock());
    return InsertPt->getLoc();
  }

  /// insertingAtEndOfBlock - Return true if the insertion point is at the end
  /// of the current basic block.  False if we're inserting before an existing
  /// instruction.
  bool insertingAtEndOfBlock() const {
    assert(hasValidInsertionPoint() &&
           "Must have insertion point to ask about it");
    return InsertPt == BB->end();
  }

  /// clearInsertionPoint - Clear the insertion point: created instructions will
  /// not be inserted into a block.
  void clearInsertionPoint() { BB = nullptr; }

  /// setInsertionPoint - Set the insertion point.
  void setInsertionPoint(SILBasicBlock *BB, SILBasicBlock::iterator insertPt) {
    this->BB = BB;
    this->InsertPt = insertPt;
    assert(insertPt == BB->end() || insertPt->getParent() == BB);
  }

  /// setInsertionPoint - Set the insertion point to insert before the specified
  /// instruction.
  void setInsertionPoint(SILInstruction *I) {
    assert(I && "can't set insertion point to a null instruction");
    setInsertionPoint(I->getParent(), I->getIterator());
  }

  /// setInsertionPoint - Set the insertion point to insert before the specified
  /// instruction.
  void setInsertionPoint(SILBasicBlock::iterator IIIter) {
    setInsertionPoint(IIIter->getParent(), IIIter);
  }

  /// setInsertionPoint - Set the insertion point to insert at the end of the
  /// specified block.
  void setInsertionPoint(SILBasicBlock *BB) {
    assert(BB && "can't set insertion point to a null basic block");
    setInsertionPoint(BB, BB->end());
  }

  /// setInsertionPoint - Set the insertion point to insert at the end of the
  /// specified block.
  void setInsertionPoint(SILFunction::iterator BBIter) {
    setInsertionPoint(&*BBIter);
  }

  //===--------------------------------------------------------------------===//
  // Instruction Tracking
  //===--------------------------------------------------------------------===//

  /// Clients of SILBuilder who want to know about any newly created
  /// instructions can install a SmallVector into the builder to collect them.
  void setTrackingList(SmallVectorImpl<SILInstruction *> *II) {
    C.InsertedInstrs = II;
  }

  SmallVectorImpl<SILInstruction *> *getTrackingList() {
    return C.InsertedInstrs;
  }

  //===--------------------------------------------------------------------===//
  // Type remapping
  //===--------------------------------------------------------------------===//

  static SILType getPartialApplyResultType(
      TypeExpansionContext context, SILType Ty, unsigned ArgCount, SILModule &M,
      SubstitutionMap subs, ParameterConvention calleeConvention,
      PartialApplyInst::OnStackKind onStack =
          PartialApplyInst::OnStackKind::NotOnStack);

  //===--------------------------------------------------------------------===//
  // CFG Manipulation
  //===--------------------------------------------------------------------===//

  /// Move the insertion point to the end of the given block.
  ///
  /// Assumes that no insertion point is currently active.
  void emitBlock(SILBasicBlock *BB) {
    assert(!hasValidInsertionPoint());
    setInsertionPoint(BB);
  }

  /// Branch to the given block if there's an active insertion point,
  /// then move the insertion point to the end of that block.
  void emitBlock(SILBasicBlock *BB, SILLocation BranchLoc);

  /// splitBlockForFallthrough - Prepare for the insertion of a terminator.  If
  /// the builder's insertion point is at the end of the current block (as when
  /// SILGen is creating the initial code for a function), just create and
  /// return a new basic block that will be later used for the continue point.
  ///
  /// If the insertion point is valid (i.e., pointing to an existing
  /// instruction) then split the block at that instruction and return the
  /// continuation block.
  SILBasicBlock *splitBlockForFallthrough();

  /// Convenience for creating a fall-through basic block on-the-fly without
  /// affecting the insertion point.
  SILBasicBlock *createFallthroughBlock(SILLocation loc,
                                        SILBasicBlock *targetBB) {
    auto *newBB = F->createBasicBlock();
    SILBuilder(newBB, this->getBuilderContext(), this->getCurrentDebugScope())
        .createBranch(loc, targetBB);
    return newBB;
  }

  //===--------------------------------------------------------------------===//
  // SILInstruction Creation Methods
  //===--------------------------------------------------------------------===//

  /// Substitute anonymous function arguments with "_$ArgNo".
  llvm::Optional<SILDebugVariable>
  substituteAnonymousArgs(llvm::SmallString<4> Name,
                          llvm::Optional<SILDebugVariable> Var,
                          SILLocation Loc) {
    if (Var && shouldDropVariable(*Var, Loc))
      return {};
    if (!Var || !Var->ArgNo || !Var->Name.empty())
      return Var;

    auto *VD = Loc.getAsASTNode<VarDecl>();
    if (VD && !VD->getName().empty())
      return Var;

    llvm::raw_svector_ostream(Name) << '_' << (Var->ArgNo - 1);
    Var->Name = Name;
    return Var;
  }

  AllocStackInst *
  createAllocStack(SILLocation Loc, SILType elementType,
                   llvm::Optional<SILDebugVariable> Var = llvm::None,
                   bool hasDynamicLifetime = false, bool isLexical = false,
                   bool wasMoved = false, bool skipVarDeclAssert = false) {
    llvm::SmallString<4> Name;
    Loc.markAsPrologue();
#ifndef NDEBUG
    if (dyn_cast_or_null<VarDecl>(Loc.getAsASTNode<Decl>()))
      assert((skipVarDeclAssert || Loc.isSynthesizedAST() || Var) &&
             "location is a VarDecl, but SILDebugVariable is empty");
#else
    (void)skipVarDeclAssert;
#endif
    return insert(AllocStackInst::create(
        getSILDebugLocation(Loc, true), elementType, getFunction(),
        substituteAnonymousArgs(Name, Var, Loc), hasDynamicLifetime, isLexical,
        wasMoved));
  }

  AllocPackInst *createAllocPack(SILLocation loc, SILType packType) {
    return insert(AllocPackInst::create(getSILDebugLocation(loc), packType,
                                        getFunction()));
  }
  AllocPackMetadataInst *
  createAllocPackMetadata(SILLocation loc,
                          llvm::Optional<SILType> elementType = llvm::None) {
    return insert(new (getModule()) AllocPackMetadataInst(
        getSILDebugLocation(loc),
        elementType.value_or(
            SILType::getEmptyTupleType(getModule().getASTContext())
                .getAddressType())));
  }

  AllocRefInst *createAllocRef(SILLocation Loc, SILType ObjectType,
                               bool objc, bool canAllocOnStack, bool isBare,
                               ArrayRef<SILType> ElementTypes,
                               ArrayRef<SILValue> ElementCountOperands) {
    // AllocRefInsts expand to function calls and can therefore not be
    // counted towards the function prologue.
    assert(!Loc.isInPrologue());
    return insert(AllocRefInst::create(getSILDebugLocation(Loc), getFunction(),
                                       ObjectType, objc, canAllocOnStack, isBare,
                                       ElementTypes, ElementCountOperands));
  }

  AllocRefDynamicInst *createAllocRefDynamic(SILLocation Loc, SILValue operand,
                                             SILType type, bool objc,
                                             bool canAllocOnStack,
                                    ArrayRef<SILType> ElementTypes,
                                    ArrayRef<SILValue> ElementCountOperands) {
    // AllocRefDynamicInsts expand to function calls and can therefore
    // not be counted towards the function prologue.
    assert(!Loc.isInPrologue());
    return insert(AllocRefDynamicInst::create(
        getSILDebugLocation(Loc), *F, operand, type, objc, canAllocOnStack,
        ElementTypes, ElementCountOperands));
  }

  /// Helper function that calls \p createAllocBox after constructing a
  /// SILBoxType for \p fieldType.
  AllocBoxInst *
  createAllocBox(SILLocation loc, SILType fieldType,
                 llvm::Optional<SILDebugVariable> Var = llvm::None,
                 bool hasDynamicLifetime = false, bool reflection = false,
                 bool usesMoveableValueDebugInfo = false,
                 bool hasPointerEscape = false) {
    return createAllocBox(loc, SILBoxType::get(fieldType.getASTType()), Var,
                          hasDynamicLifetime, reflection,
                          usesMoveableValueDebugInfo,
                          /*skipVarDeclAssert*/ false,
                          hasPointerEscape);
  }

  AllocBoxInst *
  createAllocBox(SILLocation Loc, CanSILBoxType BoxType,
                 llvm::Optional<SILDebugVariable> Var = llvm::None,
                 bool hasDynamicLifetime = false, bool reflection = false,
                 bool usesMoveableValueDebugInfo = false,
                 bool skipVarDeclAssert = false,
                 bool hasPointerEscape = false) {
#if NDEBUG
    (void)skipVarDeclAssert;
#endif
    llvm::SmallString<4> Name;
    Loc.markAsPrologue();
#if defined(NDEBUG)
    (void) skipVarDeclAssert;
#endif
    assert((skipVarDeclAssert ||
            !dyn_cast_or_null<VarDecl>(Loc.getAsASTNode<Decl>()) || Var) &&
           "location is a VarDecl, but SILDebugVariable is empty");
    return insert(AllocBoxInst::create(
        getSILDebugLocation(Loc, true), BoxType, *F,
        substituteAnonymousArgs(Name, Var, Loc), hasDynamicLifetime, reflection,
        usesMoveableValueDebugInfo, hasPointerEscape));
  }

  AllocExistentialBoxInst *
  createAllocExistentialBox(SILLocation Loc, SILType ExistentialType,
                            CanType ConcreteType,
                            ArrayRef<ProtocolConformanceRef> Conformances) {
    return insert(AllocExistentialBoxInst::create(
      getSILDebugLocation(Loc), ExistentialType, ConcreteType, Conformances, F));
  }

  ApplyInst *createApply(
      SILLocation Loc, SILValue Fn, SubstitutionMap Subs,
      ArrayRef<SILValue> Args) {
    return createApply(Loc, Fn, Subs, Args,
                       /*options=*/ApplyOptions(),
                       /*SpecializationInfo=*/nullptr);
  }

  ApplyInst *createApply(
      SILLocation Loc, SILValue Fn, SubstitutionMap Subs,
      ArrayRef<SILValue> Args,
      ApplyOptions options,
      const GenericSpecializationInformation *SpecializationInfo = nullptr) {
    return insert(ApplyInst::create(getSILDebugLocation(Loc), Fn, Subs, Args,
                                    options, C.silConv, *F,
                                    SpecializationInfo));
  }

  TryApplyInst *createTryApply(
      SILLocation Loc, SILValue fn, SubstitutionMap subs,
      ArrayRef<SILValue> args, SILBasicBlock *normalBB, SILBasicBlock *errorBB,
      ApplyOptions options = ApplyOptions(),
      const GenericSpecializationInformation *SpecializationInfo = nullptr) {
    return insertTerminator(TryApplyInst::create(
        getSILDebugLocation(Loc), fn, subs, args, normalBB, errorBB,
        options, *F, SpecializationInfo));
  }

  PartialApplyInst *createPartialApply(
      SILLocation Loc, SILValue Fn, SubstitutionMap Subs,
      ArrayRef<SILValue> Args, ParameterConvention CalleeConvention,
      PartialApplyInst::OnStackKind OnStack =
          PartialApplyInst::OnStackKind::NotOnStack,
      const GenericSpecializationInformation *SpecializationInfo = nullptr) {
    return insert(PartialApplyInst::create(
        getSILDebugLocation(Loc), Fn, Args, Subs, CalleeConvention, *F,
        SpecializationInfo, OnStack));
  }

  BeginApplyInst *createBeginApply(
      SILLocation Loc, SILValue Fn, SubstitutionMap Subs,
      ArrayRef<SILValue> Args, ApplyOptions options = ApplyOptions(),
      const GenericSpecializationInformation *SpecializationInfo = nullptr) {
    return insert(BeginApplyInst::create(
        getSILDebugLocation(Loc), Fn, Subs, Args, options, C.silConv, *F,
        SpecializationInfo));
  }

  AbortApplyInst *createAbortApply(SILLocation loc, SILValue beginApply) {
    return insert(new (getModule()) AbortApplyInst(getSILDebugLocation(loc),
                                                   beginApply));
  }

  EndApplyInst *createEndApply(SILLocation loc, SILValue beginApply) {
    return insert(new (getModule()) EndApplyInst(getSILDebugLocation(loc),
                                                 beginApply));
  }

  BuiltinInst *createBuiltin(SILLocation Loc, Identifier Name, SILType ResultTy,
                             SubstitutionMap Subs,
                             ArrayRef<SILValue> Args) {
    return insert(BuiltinInst::create(getSILDebugLocation(Loc), Name,
                                      ResultTy, Subs, Args, getModule()));
  }

  /// Create a binary function with the signature: OpdTy, OpdTy -> ResultTy.
  BuiltinInst *createBuiltinBinaryFunction(SILLocation Loc, StringRef Name,
                                           SILType OpdTy, SILType ResultTy,
                                           ArrayRef<SILValue> Args) {
    auto &C = getASTContext();

    llvm::SmallString<16> NameStr = Name;
    appendOperandTypeName(OpdTy, NameStr);
    auto Ident = C.getIdentifier(NameStr);
    return insert(BuiltinInst::create(getSILDebugLocation(Loc), Ident, ResultTy,
                                      {}, Args, getModule()));
  }

  // Create a binary function with the signature: OpdTy1, OpdTy2 -> ResultTy.
  BuiltinInst *createBuiltinBinaryFunctionWithTwoOpTypes(
      SILLocation Loc, StringRef Name, SILType OpdTy1, SILType OpdTy2,
      SILType ResultTy, ArrayRef<SILValue> Args) {
    auto &C = getASTContext();

    llvm::SmallString<16> NameStr = Name;
    appendOperandTypeName(OpdTy1, NameStr);
    appendOperandTypeName(OpdTy2, NameStr);
    auto Ident = C.getIdentifier(NameStr);
    return insert(BuiltinInst::create(getSILDebugLocation(Loc), Ident,
                                      ResultTy, {}, Args, getModule()));
  }

  /// Create a binary function with the signature:
  /// OpdTy, OpdTy, Int1 -> (OpdTy, Int1)
  BuiltinInst *
  createBuiltinBinaryFunctionWithOverflow(SILLocation Loc, StringRef Name,
                                          ArrayRef<SILValue> Args) {
    assert(Args.size() == 3 && "Need three arguments");
    assert(Args[0]->getType() == Args[1]->getType() &&
           "Binary operands must match");
    assert(Args[2]->getType().is<BuiltinIntegerType>() &&
           Args[2]->getType().getASTType()->isBuiltinIntegerType(1) &&
           "Must have a third Int1 operand");

    SILType OpdTy = Args[0]->getType();
    SILType Int1Ty = Args[2]->getType();

    TupleTypeElt ResultElts[] = {OpdTy.getASTType(), Int1Ty.getASTType()};
    Type ResultTy = TupleType::get(ResultElts, getASTContext());
    SILType SILResultTy =
        SILType::getPrimitiveObjectType(ResultTy->getCanonicalType());

    return createBuiltinBinaryFunction(Loc, Name, OpdTy, SILResultTy, Args);
  }

  // Creates a dynamic_function_ref or function_ref depending on whether f is
  // dynamically_replaceable.
  FunctionRefBaseInst *createFunctionRefFor(SILLocation Loc, SILFunction *f) {
    if (f->isDynamicallyReplaceable())
      return createDynamicFunctionRef(Loc, f);
    else
      return createFunctionRef(Loc, f);
  }

  FunctionRefBaseInst *createFunctionRef(SILLocation Loc, SILFunction *f,
                                         SILInstructionKind kind) {
    if (kind == SILInstructionKind::FunctionRefInst)
      return createFunctionRef(Loc, f);
    else if (kind == SILInstructionKind::DynamicFunctionRefInst)
      return createDynamicFunctionRef(Loc, f);
    else if (kind == SILInstructionKind::PreviousDynamicFunctionRefInst)
      return createPreviousDynamicFunctionRef(Loc, f);
    assert(false && "Should not get here");
    return nullptr;
  }

  FunctionRefInst *createFunctionRef(SILLocation Loc, SILFunction *f) {
    return insert(new (getModule()) FunctionRefInst(getSILDebugLocation(Loc), f,
                                                    getTypeExpansionContext()));
  }

  DynamicFunctionRefInst *
  createDynamicFunctionRef(SILLocation Loc, SILFunction *f) {
    return insert(new (getModule()) DynamicFunctionRefInst(
        getSILDebugLocation(Loc), f, getTypeExpansionContext()));
  }

  PreviousDynamicFunctionRefInst *
  createPreviousDynamicFunctionRef(SILLocation Loc, SILFunction *f) {
    return insert(new (getModule()) PreviousDynamicFunctionRefInst(
        getSILDebugLocation(Loc), f, getTypeExpansionContext()));
  }

  AllocGlobalInst *createAllocGlobal(SILLocation Loc, SILGlobalVariable *g) {
    return insert(new (getModule())
                      AllocGlobalInst(getSILDebugLocation(Loc), g));
  }
  GlobalAddrInst *createGlobalAddr(SILLocation Loc, SILGlobalVariable *g) {
    return insert(new (getModule()) GlobalAddrInst(getSILDebugLocation(Loc), g,
                                                   getTypeExpansionContext()));
  }
  GlobalAddrInst *createGlobalAddr(SILLocation Loc, SILType Ty) {
    return insert(new (F->getModule())
                  GlobalAddrInst(getSILDebugLocation(Loc), Ty));
  }
  GlobalValueInst *createGlobalValue(SILLocation Loc, SILGlobalVariable *g, bool isBare) {
    return insert(new (getModule()) GlobalValueInst(getSILDebugLocation(Loc), g,
                                                    getTypeExpansionContext(), isBare));
  }
  BaseAddrForOffsetInst *createBaseAddrForOffset(SILLocation Loc, SILType Ty) {
    return insert(new (F->getModule())
                  BaseAddrForOffsetInst(getSILDebugLocation(Loc), Ty));
  }
  IntegerLiteralInst *createIntegerLiteral(IntegerLiteralExpr *E);

  IntegerLiteralInst *createIntegerLiteral(SILLocation Loc, SILType Ty,
                                           intmax_t Value) {
    return insert(
        IntegerLiteralInst::create(getSILDebugLocation(Loc), Ty, Value,
                                   getModule()));
  }
  IntegerLiteralInst *createIntegerLiteral(SILLocation Loc, SILType Ty,
                                           const APInt &Value) {
    return insert(
        IntegerLiteralInst::create(getSILDebugLocation(Loc), Ty, Value,
                                   getModule()));
  }

  FloatLiteralInst *createFloatLiteral(FloatLiteralExpr *E);

  FloatLiteralInst *createFloatLiteral(SILLocation Loc, SILType Ty,
                                       const APFloat &Value) {
    return insert(
        FloatLiteralInst::create(getSILDebugLocation(Loc), Ty, Value,
                                 getModule()));
  }

  StringLiteralInst *createStringLiteral(SILLocation Loc, StringRef text,
                                         StringLiteralInst::Encoding encoding) {
    return insert(StringLiteralInst::create(getSILDebugLocation(Loc), text,
                                            encoding, getModule()));
  }

  StringLiteralInst *createStringLiteral(SILLocation Loc, const Twine &text,
                                         StringLiteralInst::Encoding encoding) {
    SmallVector<char, 256> Out;
    return insert(StringLiteralInst::create(
        getSILDebugLocation(Loc), text.toStringRef(Out), encoding, getModule()));
  }

  /// If \p LV is non-trivial, return a \p Qualifier load of \p LV. If \p LV is
  /// trivial, use trivial instead.
  ///
  /// *NOTE* The SupportUnqualifiedSIL is an option to ease the bring up of
  /// Semantic SIL. It enables a pass that must be able to run on both Semantic
  /// SIL and non-Semantic SIL. It has a default argument of false, so if this
  /// is not necessary for your pass, just ignore the parameter.
  LoadInst *createTrivialLoadOr(SILLocation Loc, SILValue LV,
                                LoadOwnershipQualifier Qualifier,
                                bool SupportUnqualifiedSIL = false) {
    if (SupportUnqualifiedSIL && !hasOwnership()) {
      assert(
          Qualifier != LoadOwnershipQualifier::Copy &&
          "In unqualified SIL, a copy must be done separately form the load");
      return createLoad(Loc, LV, LoadOwnershipQualifier::Unqualified);
    }

    if (LV->getType().isTrivial(getFunction())) {
      return createLoad(Loc, LV, LoadOwnershipQualifier::Trivial);
    }
    return createLoad(Loc, LV, Qualifier);
  }

  LoadInst *createLoad(SILLocation Loc, SILValue LV,
                       LoadOwnershipQualifier Qualifier) {
    assert((Qualifier != LoadOwnershipQualifier::Unqualified) ||
           !hasOwnership() && "Unqualified inst in qualified function");
    assert((Qualifier == LoadOwnershipQualifier::Unqualified) ||
           hasOwnership() && "Qualified inst in unqualified function");
    assert(isLoadableOrOpaque(LV->getType()));
    return insert(new (getModule())
                      LoadInst(getSILDebugLocation(Loc), LV, Qualifier));
  }
  
  KeyPathInst *createKeyPath(SILLocation Loc,
                             KeyPathPattern *Pattern,
                             SubstitutionMap Subs,
                             ArrayRef<SILValue> Args,
                             SILType Ty) {
    return insert(KeyPathInst::create(getSILDebugLocation(Loc),
                                      Pattern, Subs, Args,
                                      Ty, getFunction()));
  }

  /// Convenience function for calling emitLoad on the type lowering for
  /// non-address values.
  SILValue emitLoadValueOperation(SILLocation Loc, SILValue LV,
                                  LoadOwnershipQualifier Qualifier) {
    assert(isLoadableOrOpaque(LV->getType()));
    const auto &lowering = getTypeLowering(LV->getType());
    return lowering.emitLoad(*this, Loc, LV, Qualifier);
  }

  /// Convenience function for calling emitLoad on the type lowering for
  /// non-address values.
  SILValue emitLoweredLoadValueOperation(
      SILLocation Loc, SILValue LV, LoadOwnershipQualifier Qualifier,
      Lowering::TypeLowering::TypeExpansionKind ExpansionKind) {
    assert(isLoadableOrOpaque(LV->getType()));
    const auto &lowering = getTypeLowering(LV->getType());
    return lowering.emitLoweredLoad(*this, Loc, LV, Qualifier, ExpansionKind);
  }

  /// Convenience function for calling emitLoweredStore on the type lowering for
  /// non-address values.
  void emitLoweredStoreValueOperation(
      SILLocation Loc, SILValue Value, SILValue Addr,
      StoreOwnershipQualifier Qual,
      Lowering::TypeLowering::TypeExpansionKind ExpansionKind) {
    assert(isLoadableOrOpaque(Value->getType()));
    const auto &lowering = getTypeLowering(Value->getType());
    lowering.emitLoweredStore(*this, Loc, Value, Addr, Qual, ExpansionKind);
  }

  LoadBorrowInst *createLoadBorrow(SILLocation Loc, SILValue LV) {
    assert(isLoadableOrOpaque(LV->getType()) &&
           !LV->getType().isTrivial(getFunction()));
    return insert(new (getModule())
                      LoadBorrowInst(getSILDebugLocation(Loc), LV));
  }

  BeginBorrowInst *createBeginBorrow(SILLocation Loc, SILValue LV,
                                     bool isLexical = false,
                                     bool hasPointerEscape = false) {
    assert(getFunction().hasOwnership());
    assert(!LV->getType().isAddress());
    return insert(new (getModule()) BeginBorrowInst(getSILDebugLocation(Loc),
                                                    LV, isLexical, hasPointerEscape));
  }

  /// Convenience function for creating a load_borrow on non-trivial values and
  /// load [trivial] on trivial values. Becomes load unqualified in non-ossa
  /// functions.
  SILValue emitLoadBorrowOperation(SILLocation loc, SILValue v) {
    if (!hasOwnership()) {
      return emitLoadValueOperation(loc, v,
                                    LoadOwnershipQualifier::Unqualified);
    }

    if (v->getType().isTrivial(getFunction())) {
      return emitLoadValueOperation(loc, v, LoadOwnershipQualifier::Trivial);
    }

    return createLoadBorrow(loc, v);
  }

  SILValue emitBeginBorrowOperation(SILLocation loc, SILValue v) {
    if (!hasOwnership() ||
        v->getOwnershipKind().isCompatibleWith(OwnershipKind::Guaranteed))
      return v;
    return createBeginBorrow(loc, v);
  }

  void emitEndBorrowOperation(SILLocation loc, SILValue v) {
    if (!hasOwnership() || (!v->getType().isAddress() &&
                            v->getOwnershipKind() == OwnershipKind::None))
      return;
    createEndBorrow(loc, v);
  }

  // Pass in an address or value, perform a begin_borrow/load_borrow and pass
  // the value to the passed in closure. After the closure has finished
  // executing, automatically insert the end_borrow. The closure can assume that
  // it will receive a loaded loadable value.
  void emitScopedBorrowOperation(SILLocation loc, SILValue original,
                                 function_ref<void(SILValue)> &&fun);

  /// Utility function that returns a trivial store if the stored type is
  /// trivial and a \p Qualifier store if the stored type is non-trivial.
  ///
  /// *NOTE* The SupportUnqualifiedSIL is an option to ease the bring up of
  /// Semantic SIL. It enables a pass that must be able to run on both Semantic
  /// SIL and non-Semantic SIL. It has a default argument of false, so if this
  /// is not necessary for your pass, just ignore the parameter.
  StoreInst *createTrivialStoreOr(SILLocation Loc, SILValue Src,
                                  SILValue DestAddr,
                                  StoreOwnershipQualifier Qualifier,
                                  bool SupportUnqualifiedSIL = false) {
    if (SupportUnqualifiedSIL && !hasOwnership()) {
      assert(
          Qualifier != StoreOwnershipQualifier::Assign &&
          "In unqualified SIL, assigns must be represented via 2 instructions");
      return createStore(Loc, Src, DestAddr,
                         StoreOwnershipQualifier::Unqualified);
    }
    if (Src->getType().isTrivial(getFunction())) {
      return createStore(Loc, Src, DestAddr, StoreOwnershipQualifier::Trivial);
    }
    return createStore(Loc, Src, DestAddr, Qualifier);
  }

  StoreInst *createStore(SILLocation Loc, SILValue Src, SILValue DestAddr,
                         StoreOwnershipQualifier Qualifier) {
    assert((Qualifier != StoreOwnershipQualifier::Unqualified) ||
           !hasOwnership() && "Unqualified inst in qualified function");
    assert((Qualifier == StoreOwnershipQualifier::Unqualified) ||
           hasOwnership() && "Qualified inst in unqualified function");
    return insert(new (getModule()) StoreInst(getSILDebugLocation(Loc), Src,
                                                DestAddr, Qualifier));
  }

  /// Convenience function for calling emitStore on the type lowering for
  /// non-address values.
  void emitStoreValueOperation(SILLocation Loc, SILValue Src, SILValue DestAddr,
                               StoreOwnershipQualifier Qualifier) {
    assert(!Src->getType().isAddress());
    const auto &lowering = getTypeLowering(Src->getType());
    return lowering.emitStore(*this, Loc, Src, DestAddr, Qualifier);
  }

  EndBorrowInst *createEndBorrow(SILLocation loc, SILValue borrowedValue) {
    assert(!SILArgument::isTerminatorResult(borrowedValue) &&
               "terminator results do not have end_borrow");
    return insert(new (getModule())
                      EndBorrowInst(getSILDebugLocation(loc), borrowedValue));
  }

  BeginAccessInst *createBeginAccess(SILLocation loc, SILValue address,
                                     SILAccessKind accessKind,
                                     SILAccessEnforcement enforcement,
                                     bool noNestedConflict,
                                     bool fromBuiltin) {
    return insert(new (getModule()) BeginAccessInst(
        getSILDebugLocation(loc), address, accessKind, enforcement,
        noNestedConflict, fromBuiltin));
  }

  EndAccessInst *createEndAccess(SILLocation loc, SILValue address,
                                 bool aborted) {
    return insert(new (getModule()) EndAccessInst(
        getSILDebugLocation(loc), address, aborted));
  }

  BeginUnpairedAccessInst *
  createBeginUnpairedAccess(SILLocation loc, SILValue address, SILValue buffer,
                            SILAccessKind accessKind,
                            SILAccessEnforcement enforcement,
                            bool noNestedConflict,
                            bool fromBuiltin) {
    return insert(new (getModule()) BeginUnpairedAccessInst(
        getSILDebugLocation(loc), address, buffer, accessKind, enforcement,
        noNestedConflict, fromBuiltin));
  }

  EndUnpairedAccessInst *
  createEndUnpairedAccess(SILLocation loc, SILValue buffer,
                          SILAccessEnforcement enforcement, bool aborted,
                          bool fromBuiltin) {
    return insert(new (getModule()) EndUnpairedAccessInst(
        getSILDebugLocation(loc), buffer, enforcement, aborted, fromBuiltin));
  }

  AssignInst *createAssign(SILLocation Loc, SILValue Src, SILValue DestAddr,
                           AssignOwnershipQualifier Qualifier) {
    return insert(new (getModule())
                      AssignInst(getSILDebugLocation(Loc), Src, DestAddr,
                                 Qualifier));
  }

  AssignByWrapperInst *createAssignByWrapper(SILLocation Loc, SILValue Src,
                                             SILValue Dest,
                                             SILValue Initializer,
                                             SILValue Setter,
                                             AssignByWrapperInst::Mode mode) {
    return insert(new (getModule()) AssignByWrapperInst(
        getSILDebugLocation(Loc), Src, Dest, Initializer, Setter, mode));
  }

  AssignOrInitInst *createAssignOrInit(SILLocation Loc,
                                       VarDecl *Property,
                                       SILValue Self,
                                       SILValue Src,
                                       SILValue Initializer,
                                       SILValue Setter,
                                       AssignOrInitInst::Mode Mode) {
    return insert(new (getModule())
                      AssignOrInitInst(getSILDebugLocation(Loc), Property, Self,
                                       Src, Initializer, Setter, Mode));
  }

  StoreBorrowInst *createStoreBorrow(SILLocation Loc, SILValue Src,
                                     SILValue DestAddr) {
    return insert(new (getModule())
                      StoreBorrowInst(getSILDebugLocation(Loc), Src, DestAddr));
  }

  /// A helper function for emitting store_borrow in operations where one must
  /// handle both ossa and non-ossa code.
  ///
  /// In words:
  ///
  /// * If the function does not have ownership, this just emits an unqualified
  ///   store.
  ///
  /// * If the function has ownership, but the type is trivial, use store
  ///   [trivial].
  ///
  /// * Otherwise, emit an actual store_borrow.
  SILInstruction *emitStoreBorrowOperation(SILLocation loc, SILValue src,
                                           SILValue destAddr) {
    if (!hasOwnership()) {
      emitStoreValueOperation(loc, src, destAddr,
                              StoreOwnershipQualifier::Unqualified);
    } else if (src->getType().isTrivial(getFunction())) {
      emitStoreValueOperation(loc, src, destAddr,
                              StoreOwnershipQualifier::Trivial);
    } else {
      createStoreBorrow(loc, src, destAddr);
    }
    return &*std::prev(getInsertionPoint());
  }

  MarkUninitializedInst *
  createMarkUninitialized(SILLocation Loc, SILValue src,
                          MarkUninitializedInst::Kind k) {
    return createMarkUninitialized(Loc, src, k, src->getOwnershipKind());
  }

  MarkUninitializedInst *
  createMarkUninitialized(SILLocation Loc, SILValue src,
                          MarkUninitializedInst::Kind k,
                          ValueOwnershipKind forwardingOwnershipKind) {
    return insert(new (getModule()) MarkUninitializedInst(
        getSILDebugLocation(Loc), src, k, forwardingOwnershipKind));
  }

  MarkUninitializedInst *createMarkUninitializedVar(SILLocation Loc,
                                                    SILValue src) {
    return createMarkUninitialized(Loc, src, MarkUninitializedInst::Var);
  }
  MarkUninitializedInst *createMarkUninitializedRootSelf(SILLocation Loc,
                                                         SILValue src) {
    return createMarkUninitialized(Loc, src, MarkUninitializedInst::RootSelf);
  }
  MarkUninitializedInst *createMarkUninitializedOut(SILLocation Loc,
                                                    SILValue src) {
    return createMarkUninitialized(Loc, src, MarkUninitializedInst::Out);
  }

  MarkFunctionEscapeInst *createMarkFunctionEscape(SILLocation Loc,
                                                   ArrayRef<SILValue> vars) {
    return insert(
        MarkFunctionEscapeInst::create(getSILDebugLocation(Loc), vars, getFunction()));
  }

  DebugValueInst *createDebugValue(SILLocation Loc, SILValue src,
                                   SILDebugVariable Var,
                                   bool poisonRefs = false,
                                   bool wasMoved = false,
                                   bool trace = false);
  DebugValueInst *createDebugValueAddr(SILLocation Loc, SILValue src,
                                       SILDebugVariable Var,
                                       bool wasMoved = false,
                                       bool trace = false);

  DebugStepInst *createDebugStep(SILLocation Loc) {
    return insert(new (getModule()) DebugStepInst(getSILDebugLocation(Loc)));
  }

  /// Create a debug_value according to the type of \p src
  SILInstruction *emitDebugDescription(SILLocation Loc, SILValue src,
                                       SILDebugVariable Var) {
    if (src->getType().isAddress())
      return createDebugValueAddr(Loc, src, Var);
    return createDebugValue(Loc, src, Var);
  }

  TestSpecificationInst *
  createTestSpecificationInst(SILLocation Loc,
                              StringRef ArgumentsSpecification) {
    return insert(TestSpecificationInst::create(
        getSILDebugLocation(Loc), ArgumentsSpecification, getModule()));
  }

  UnownedCopyValueInst *createUnownedCopyValue(SILLocation Loc,
                                               SILValue operand) {
    assert(!getFunction().getModule().useLoweredAddresses());
    auto type = operand->getType()
                    .getReferenceStorageType(getFunction().getASTContext(),
                                             ReferenceOwnership::Unowned)
                    .getObjectType();
    return insert(new (getModule()) UnownedCopyValueInst(
        getSILDebugLocation(Loc), operand, type));
  }

  WeakCopyValueInst *createWeakCopyValue(SILLocation Loc, SILValue operand) {
    assert(!getFunction().getModule().useLoweredAddresses());
    auto type = operand->getType()
                    .getReferenceStorageType(getFunction().getASTContext(),
                                             ReferenceOwnership::Weak)
                    .getObjectType();
    return insert(new (getModule()) WeakCopyValueInst(getSILDebugLocation(Loc),
                                                      operand, type));
  }

#define COPYABLE_STORAGE_HELPER(Name)                                          \
  StrongCopy##Name##ValueInst *createStrongCopy##Name##Value(                  \
      SILLocation Loc, SILValue operand) {                                     \
    auto type = getFunction().getLoweredType(                                  \
        operand->getType().getASTType().getReferenceStorageReferent());        \
    return insert(new (getModule()) StrongCopy##Name##ValueInst(               \
        getSILDebugLocation(Loc), operand, type));                             \
  }

#define LOADABLE_STORAGE_HELPER(Name)                                          \
  Load##Name##Inst *createLoad##Name(SILLocation Loc, SILValue src,            \
                                     IsTake_t isTake) {                        \
    return insert(new (getModule()) Load##Name##Inst(getSILDebugLocation(Loc), \
                                                     src, isTake));            \
  }                                                                            \
  Store##Name##Inst *createStore##Name(SILLocation Loc, SILValue value,        \
                                       SILValue dest,                          \
                                       IsInitialization_t isInit) {            \
    return insert(new (getModule()) Store##Name##Inst(                         \
        getSILDebugLocation(Loc), value, dest, isInit));                       \
  }

#define LOADABLE_REF_STORAGE_HELPER(Name)                                      \
  Name##ToRefInst *create##Name##ToRef(SILLocation Loc, SILValue op,           \
                                       SILType ty) {                           \
    return insert(new (getModule())                                            \
                      Name##ToRefInst(getSILDebugLocation(Loc), op, ty));      \
  }                                                                            \
  RefTo##Name##Inst *createRefTo##Name(SILLocation Loc, SILValue op,           \
                                       SILType ty) {                           \
    return insert(new (getModule())                                            \
                      RefTo##Name##Inst(getSILDebugLocation(Loc), op, ty));    \
  }

#define RETAINABLE_STORAGE_HELPER(Name)                                        \
  StrongRetain##Name##Inst *createStrongRetain##Name(                          \
      SILLocation Loc, SILValue Operand, Atomicity atomicity) {                \
    return insert(new (getModule()) StrongRetain##Name##Inst(                  \
        getSILDebugLocation(Loc), Operand, atomicity));                        \
  }                                                                            \
  Name##RetainInst *create##Name##Retain(SILLocation Loc, SILValue Operand,    \
                                         Atomicity atomicity) {                \
    return insert(new (getModule()) Name##RetainInst(getSILDebugLocation(Loc), \
                                                     Operand, atomicity));     \
  }                                                                            \
  Name##ReleaseInst *create##Name##Release(SILLocation Loc, SILValue Operand,  \
                                           Atomicity atomicity) {              \
    return insert(new (getModule()) Name##ReleaseInst(                         \
        getSILDebugLocation(Loc), Operand, atomicity));                        \
  }

#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                         \
  COPYABLE_STORAGE_HELPER(Name)                                                \
  LOADABLE_REF_STORAGE_HELPER(Name)                                            \
  RETAINABLE_STORAGE_HELPER(Name)
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                      \
  COPYABLE_STORAGE_HELPER(Name)                                                \
  LOADABLE_REF_STORAGE_HELPER(Name)                                            \
  LOADABLE_STORAGE_HELPER(Name)                                                \
  RETAINABLE_STORAGE_HELPER(Name)
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                          \
  COPYABLE_STORAGE_HELPER(Name)                                                \
  LOADABLE_STORAGE_HELPER(Name)
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  COPYABLE_STORAGE_HELPER(Name)                                                \
  LOADABLE_REF_STORAGE_HELPER(Name)
#include "swift/AST/ReferenceStorage.def"
#undef LOADABLE_STORAGE_HELPER
#undef LOADABLE_REF_STORAGE_HELPER
#undef COPYABLE_STORAGE_HELPER
#undef RETAINABLE_STORAGE_HELPER

  CopyAddrInst *createCopyAddr(SILLocation Loc, SILValue srcAddr,
                               SILValue destAddr, IsTake_t isTake,
                               IsInitialization_t isInitialize) {
    assert(srcAddr->getType() == destAddr->getType());
    return insert(new (getModule()) CopyAddrInst(
        getSILDebugLocation(Loc), srcAddr, destAddr, isTake, isInitialize));
  }

  ExplicitCopyAddrInst *
  createExplicitCopyAddr(SILLocation Loc, SILValue srcAddr, SILValue destAddr,
                         IsTake_t isTake, IsInitialization_t isInitialize) {
    assert(srcAddr->getType() == destAddr->getType());
    return insert(new (getModule()) ExplicitCopyAddrInst(
        getSILDebugLocation(Loc), srcAddr, destAddr, isTake, isInitialize));
  }

  BindMemoryInst *createBindMemory(SILLocation Loc, SILValue base,
                                   SILValue index, SILType boundType) {
    return insert(BindMemoryInst::create(getSILDebugLocation(Loc), base, index,
                                         boundType, getFunction()));
  }

  RebindMemoryInst *createRebindMemory(SILLocation Loc, SILValue base,
                                       SILValue inToken) {
    auto tokenTy = SILType::getBuiltinWordType(F->getASTContext());
    return insert(new (getModule()) RebindMemoryInst(getSILDebugLocation(Loc),
                                                     base, inToken, tokenTy));
  }

  ConvertFunctionInst *createConvertFunction(SILLocation Loc, SILValue Op,
                                             SILType Ty,
                                             bool WithoutActuallyEscaping) {
    return createConvertFunction(Loc, Op, Ty, WithoutActuallyEscaping,
                                 Op->getOwnershipKind());
  }

  ConvertFunctionInst *
  createConvertFunction(SILLocation Loc, SILValue Op, SILType Ty,
                        bool WithoutActuallyEscaping,
                        ValueOwnershipKind forwardingOwnershipKind) {
    return insert(ConvertFunctionInst::create(
        getSILDebugLocation(Loc), Op, Ty, getModule(), F,
        WithoutActuallyEscaping, forwardingOwnershipKind));
  }

  ConvertEscapeToNoEscapeInst *
  createConvertEscapeToNoEscape(SILLocation Loc, SILValue Op, SILType Ty,
                                bool lifetimeGuaranteed) {
    return insert(ConvertEscapeToNoEscapeInst::create(
        getSILDebugLocation(Loc), Op, Ty, getFunction(),
        lifetimeGuaranteed));
  }

  UpcastInst *createUpcast(SILLocation Loc, SILValue Op, SILType Ty) {
    return createUpcast(Loc, Op, Ty, Op->getOwnershipKind());
  }

  UpcastInst *createUpcast(SILLocation Loc, SILValue Op, SILType Ty,
                           ValueOwnershipKind forwardingOwnershipKind) {
    assert(Ty.isObject());
    return insert(UpcastInst::create(getSILDebugLocation(Loc), Op, Ty,
                                     getFunction(), forwardingOwnershipKind));
  }

  AddressToPointerInst *createAddressToPointer(SILLocation Loc, SILValue Op,
                                               SILType Ty, bool needsStackProtection) {
    return insert(new (getModule()) AddressToPointerInst(
        getSILDebugLocation(Loc), Op, Ty, needsStackProtection));
  }

  PointerToAddressInst *
  createPointerToAddress(SILLocation Loc, SILValue Op, SILType Ty,
                         bool isStrict, bool isInvariant = false,
                         llvm::MaybeAlign alignment = llvm::MaybeAlign()) {
    return insert(new (getModule()) PointerToAddressInst(
        getSILDebugLocation(Loc), Op, Ty, isStrict, isInvariant, alignment));
  }

  UncheckedRefCastInst *createUncheckedRefCast(SILLocation Loc, SILValue Op,
                                               SILType Ty) {
    return insert(UncheckedRefCastInst::create(getSILDebugLocation(Loc), Op, Ty,
                                               getFunction(),
                                               Op->getOwnershipKind()));
  }

  UncheckedRefCastInst *
  createUncheckedRefCast(SILLocation Loc, SILValue Op, SILType Ty,
                         ValueOwnershipKind forwardingOwnershipKind) {
    return insert(UncheckedRefCastInst::create(
        getSILDebugLocation(Loc), Op, Ty, getFunction(),
        forwardingOwnershipKind));
  }

  UncheckedRefCastAddrInst *
  createUncheckedRefCastAddr(SILLocation Loc,
                             SILValue src, CanType sourceFormalType,
                             SILValue dest, CanType targetFormalType) {
    return insert(UncheckedRefCastAddrInst::create(
        getSILDebugLocation(Loc), src, sourceFormalType,
        dest, targetFormalType, getFunction()));
  }

  UncheckedAddrCastInst *createUncheckedAddrCast(SILLocation Loc, SILValue Op,
                                                 SILType Ty) {
    return insert(UncheckedAddrCastInst::create(
        getSILDebugLocation(Loc), Op, Ty, getFunction()));
  }

  UncheckedTrivialBitCastInst *
  createUncheckedTrivialBitCast(SILLocation Loc, SILValue Op, SILType Ty) {
    return insert(UncheckedTrivialBitCastInst::create(
        getSILDebugLocation(Loc), Op, Ty, getFunction()));
  }

  UncheckedBitwiseCastInst *
  createUncheckedBitwiseCast(SILLocation Loc, SILValue Op, SILType Ty) {
    return insert(UncheckedBitwiseCastInst::create(
        getSILDebugLocation(Loc), Op, Ty, getFunction()));
  }

  UncheckedValueCastInst *createUncheckedValueCast(SILLocation Loc, SILValue Op,
                                                   SILType Ty) {
    return createUncheckedValueCast(Loc, Op, Ty, Op->getOwnershipKind());
  }

  UncheckedValueCastInst *
  createUncheckedValueCast(SILLocation Loc, SILValue Op, SILType Ty,
                           ValueOwnershipKind forwardingOwnershipKind) {
    assert(hasOwnership());
    return insert(UncheckedValueCastInst::create(
        getSILDebugLocation(Loc), Op, Ty, getFunction(),
        forwardingOwnershipKind));
  }

  RefToBridgeObjectInst *createRefToBridgeObject(SILLocation Loc, SILValue Ref,
                                                 SILValue Bits) {
    return createRefToBridgeObject(Loc, Ref, Bits, Ref->getOwnershipKind());
  }

  RefToBridgeObjectInst *
  createRefToBridgeObject(SILLocation Loc, SILValue Ref, SILValue Bits,
                          ValueOwnershipKind forwardingOwnershipKind) {
    auto Ty = SILType::getBridgeObjectType(getASTContext());
    return insert(new (getModule()) RefToBridgeObjectInst(
        getSILDebugLocation(Loc), Ref, Bits, Ty, forwardingOwnershipKind));
  }

  BridgeObjectToRefInst *createBridgeObjectToRef(SILLocation Loc, SILValue Op,
                                                 SILType Ty) {
    return createBridgeObjectToRef(Loc, Op, Ty, Op->getOwnershipKind());
  }

  BridgeObjectToRefInst *
  createBridgeObjectToRef(SILLocation Loc, SILValue Op, SILType Ty,
                          ValueOwnershipKind forwardingOwnershipKind) {
    return insert(new (getModule()) BridgeObjectToRefInst(
        getSILDebugLocation(Loc), Op, Ty, forwardingOwnershipKind));
  }

  ValueToBridgeObjectInst *createValueToBridgeObject(SILLocation Loc,
                                                     SILValue value) {
    auto Ty = SILType::getBridgeObjectType(getASTContext());
    return insert(new (getModule()) ValueToBridgeObjectInst(
        getSILDebugLocation(Loc), value, Ty));
  }

  BridgeObjectToWordInst *createBridgeObjectToWord(SILLocation Loc,
                                                   SILValue Op) {
    auto Ty = SILType::getBuiltinWordType(getASTContext());
    return createBridgeObjectToWord(Loc, Op, Ty);
  }

  BridgeObjectToWordInst *createBridgeObjectToWord(SILLocation Loc, SILValue Op,
                                                   SILType Ty) {
    return insert(new (getModule()) BridgeObjectToWordInst(
        getSILDebugLocation(Loc), Op, Ty));
  }

  RefToRawPointerInst *createRefToRawPointer(SILLocation Loc, SILValue Op,
                                             SILType Ty) {
    return insert(new (getModule())
                      RefToRawPointerInst(getSILDebugLocation(Loc), Op, Ty));
  }

  RawPointerToRefInst *createRawPointerToRef(SILLocation Loc, SILValue Op,
                                             SILType Ty) {
    return insert(new (getModule())
                      RawPointerToRefInst(getSILDebugLocation(Loc), Op, Ty));
  }

  ThinToThickFunctionInst *createThinToThickFunction(SILLocation Loc,
                                                     SILValue Op, SILType Ty) {
    return createThinToThickFunction(Loc, Op, Ty, Op->getOwnershipKind());
  }

  ThinToThickFunctionInst *
  createThinToThickFunction(SILLocation Loc, SILValue Op, SILType Ty,
                            ValueOwnershipKind forwardingOwnershipKind) {
    return insert(ThinToThickFunctionInst::create(
        getSILDebugLocation(Loc), Op, Ty, getModule(), F,
        forwardingOwnershipKind));
  }

  ThickToObjCMetatypeInst *createThickToObjCMetatype(SILLocation Loc,
                                                     SILValue Op, SILType Ty) {
    return insert(new (getModule()) ThickToObjCMetatypeInst(
        getSILDebugLocation(Loc), Op, Ty));
  }

  ObjCToThickMetatypeInst *createObjCToThickMetatype(SILLocation Loc,
                                                     SILValue Op, SILType Ty) {
    return insert(new (getModule()) ObjCToThickMetatypeInst(
        getSILDebugLocation(Loc), Op, Ty));
  }

  ObjCProtocolInst *createObjCProtocol(SILLocation Loc, ProtocolDecl *P,
                                       SILType Ty) {
    return insert(new (getModule())
                      ObjCProtocolInst(getSILDebugLocation(Loc), P, Ty));
  }

  CopyValueInst *createCopyValue(SILLocation Loc, SILValue operand) {
    assert(getFunction().hasOwnership());
    assert(!operand->getType().isTrivial(getFunction()) &&
           "Should not be passing trivial values to this api. Use instead "
           "emitCopyValueOperation");
    return insert(new (getModule())
                      CopyValueInst(getSILDebugLocation(Loc), operand));
  }

  ExplicitCopyValueInst *createExplicitCopyValue(SILLocation Loc,
                                                 SILValue operand) {
    assert(!operand->getType().isTrivial(getFunction()) &&
           "Should not be passing trivial values to this api. Use instead "
           "emitCopyValueOperation");
    return insert(new (getModule())
                      ExplicitCopyValueInst(getSILDebugLocation(Loc), operand));
  }

  DestroyValueInst *createDestroyValue(SILLocation Loc, SILValue operand,
                                       bool poisonRefs = false) {
    assert(getFunction().hasOwnership());
    assert(isLoadableOrOpaque(operand->getType()));
    assert(!operand->getType().isTrivial(getFunction()) &&
           "Should not be passing trivial values to this api. Use instead "
           "emitDestroyValueOperation");
    return insert(new (getModule()) DestroyValueInst(getSILDebugLocation(Loc),
                                                     operand, poisonRefs));
  }

  MoveValueInst *createMoveValue(SILLocation loc, SILValue operand,
                                 bool isLexical = false,
                                 bool hasPointerEscape = false) {
    assert(getFunction().hasOwnership());
    assert(!operand->getType().isTrivial(getFunction()) &&
           "Should not be passing trivial values to this api. Use instead "
           "emitMoveValueOperation");
    return insert(new (getModule()) MoveValueInst(
        getSILDebugLocation(loc), operand, isLexical, hasPointerEscape));
  }

  DropDeinitInst *createDropDeinit(SILLocation loc, SILValue operand) {
    assert(getFunction().hasOwnership());
    assert(!operand->getType().isTrivial(getFunction()) &&
           "Should not be passing trivial values to this api.");
    return insert(new (getModule()) DropDeinitInst(getSILDebugLocation(loc),
                                                   operand));
  }

  MarkUnresolvedMoveAddrInst *createMarkUnresolvedMoveAddr(SILLocation loc,
                                                           SILValue srcAddr,
                                                           SILValue takeAddr) {
    return insert(new (getModule()) MarkUnresolvedMoveAddrInst(
        getSILDebugLocation(loc), srcAddr, takeAddr));
  }

  MarkMustCheckInst *
  createMarkMustCheckInst(SILLocation loc, SILValue src,
                          MarkMustCheckInst::CheckKind kind) {
    return insert(new (getModule())
                      MarkMustCheckInst(getSILDebugLocation(loc), src, kind));
  }

  MarkUnresolvedReferenceBindingInst *createMarkUnresolvedReferenceBindingInst(
      SILLocation loc, SILValue src,
      MarkUnresolvedReferenceBindingInst::Kind kind) {
    return insert(new (getModule()) MarkUnresolvedReferenceBindingInst(
        getSILDebugLocation(loc), src, kind));
  }

  CopyableToMoveOnlyWrapperValueInst *
  createOwnedCopyableToMoveOnlyWrapperValue(SILLocation loc, SILValue src) {
    return insert(new (getModule()) CopyableToMoveOnlyWrapperValueInst(
        getSILDebugLocation(loc), src,
        CopyableToMoveOnlyWrapperValueInst::Owned));
  }

  CopyableToMoveOnlyWrapperValueInst *
  createGuaranteedCopyableToMoveOnlyWrapperValue(SILLocation loc,
                                                 SILValue src) {
    assert(!src->getType().isTrivial(*F) &&
           "trivial types can only use the owned version of this API");
    return insert(new (getModule()) CopyableToMoveOnlyWrapperValueInst(
        getSILDebugLocation(loc), src,
        CopyableToMoveOnlyWrapperValueInst::Guaranteed));
  }

  MoveOnlyWrapperToCopyableBoxInst *
  createMoveOnlyWrapperToCopyableBox(SILLocation loc, SILValue src) {
    return insert(new (getModule()) MoveOnlyWrapperToCopyableBoxInst(
        getSILDebugLocation(loc), src, src->getOwnershipKind()));
  }

  MoveOnlyWrapperToCopyableAddrInst *
  createMoveOnlyWrapperToCopyableAddr(SILLocation loc, SILValue src) {
    return insert(new (getModule()) MoveOnlyWrapperToCopyableAddrInst(
        getSILDebugLocation(loc), src));
  }

  CopyableToMoveOnlyWrapperAddrInst *
  createCopyableToMoveOnlyWrapperAddr(SILLocation loc, SILValue src) {
    return insert(new (getModule()) CopyableToMoveOnlyWrapperAddrInst(
        getSILDebugLocation(loc), src));
  }

  MoveOnlyWrapperToCopyableValueInst *
  createOwnedMoveOnlyWrapperToCopyableValue(SILLocation loc, SILValue src) {
    return insert(new (getModule()) MoveOnlyWrapperToCopyableValueInst(
        *F, getSILDebugLocation(loc), src,
        MoveOnlyWrapperToCopyableValueInst::Owned));
  }

  MoveOnlyWrapperToCopyableValueInst *
  createGuaranteedMoveOnlyWrapperToCopyableValue(SILLocation loc,
                                                 SILValue src) {
    return insert(new (getModule()) MoveOnlyWrapperToCopyableValueInst(
        *F, getSILDebugLocation(loc), src,
        MoveOnlyWrapperToCopyableValueInst::Guaranteed));
  }

  UnconditionalCheckedCastInst *
  createUnconditionalCheckedCast(SILLocation Loc, SILValue op,
                                 SILType destLoweredTy,
                                 CanType destFormalTy) {
    return createUnconditionalCheckedCast(Loc, op, destLoweredTy, destFormalTy,
                                          op->getOwnershipKind());
  }

  UnconditionalCheckedCastInst *
  createUnconditionalCheckedCast(SILLocation Loc, SILValue op,
                                 SILType destLoweredTy, CanType destFormalTy,
                                 ValueOwnershipKind forwardingOwnershipKind) {
    return insert(UnconditionalCheckedCastInst::create(
        getSILDebugLocation(Loc), op, destLoweredTy, destFormalTy,
        getFunction(), forwardingOwnershipKind));
  }

  UnconditionalCheckedCastAddrInst *
  createUnconditionalCheckedCastAddr(SILLocation Loc,
                                     SILValue src, CanType sourceFormalType,
                                     SILValue dest, CanType targetFormalType) {
    return insert(UnconditionalCheckedCastAddrInst::create(
        getSILDebugLocation(Loc), src, sourceFormalType,
        dest, targetFormalType, getFunction()));
  }

  RetainValueInst *createRetainValue(SILLocation Loc, SILValue operand,
                                     Atomicity atomicity) {
    assert(!hasOwnership());
    assert(isLoadableOrOpaque(operand->getType()));
    return insert(new (getModule()) RetainValueInst(getSILDebugLocation(Loc),
                                                      operand, atomicity));
  }

  RetainValueAddrInst *createRetainValueAddr(SILLocation Loc, SILValue operand,
                                             Atomicity atomicity) {
    assert(!hasOwnership());
    return insert(new (getModule()) RetainValueAddrInst(
        getSILDebugLocation(Loc), operand, atomicity));
  }

  ReleaseValueInst *createReleaseValue(SILLocation Loc, SILValue operand,
                                       Atomicity atomicity) {
    assert(!hasOwnership());
    assert(isLoadableOrOpaque(operand->getType()));
    return insert(new (getModule()) ReleaseValueInst(getSILDebugLocation(Loc),
                                                       operand, atomicity));
  }

  ReleaseValueAddrInst *createReleaseValueAddr(SILLocation Loc,
                                               SILValue operand,
                                               Atomicity atomicity) {
    assert(!hasOwnership());
    return insert(new (getModule()) ReleaseValueAddrInst(
        getSILDebugLocation(Loc), operand, atomicity));
  }

  UnmanagedRetainValueInst *createUnmanagedRetainValue(SILLocation Loc,
                                                       SILValue operand,
                                                       Atomicity atomicity) {
    assert(hasOwnership());
    assert(isLoadableOrOpaque(operand->getType()));
    return insert(new (getModule()) UnmanagedRetainValueInst(
        getSILDebugLocation(Loc), operand, atomicity));
  }

  UnmanagedReleaseValueInst *createUnmanagedReleaseValue(SILLocation Loc,
                                                         SILValue operand,
                                                         Atomicity atomicity) {
    assert(hasOwnership());
    assert(isLoadableOrOpaque(operand->getType()));
    return insert(new (getModule()) UnmanagedReleaseValueInst(
        getSILDebugLocation(Loc), operand, atomicity));
  }

  AutoreleaseValueInst *createAutoreleaseValue(SILLocation Loc,
                                               SILValue operand,
                                               Atomicity atomicity) {
    return insert(new (getModule()) AutoreleaseValueInst(
        getSILDebugLocation(Loc), operand, atomicity));
  }

  UnmanagedAutoreleaseValueInst *
  createUnmanagedAutoreleaseValue(SILLocation Loc, SILValue operand,
                                  Atomicity atomicity) {
    return insert(new (getModule()) UnmanagedAutoreleaseValueInst(
                      getSILDebugLocation(Loc), operand, atomicity));
  }

  SetDeallocatingInst *createSetDeallocating(SILLocation Loc,
                                            SILValue operand,
                                            Atomicity atomicity) {
    return insert(new (getModule()) SetDeallocatingInst(
        getSILDebugLocation(Loc), operand, atomicity));
  }

  ObjectInst *createObject(SILLocation Loc, SILType Ty,
                           ArrayRef<SILValue> Elements,
                           unsigned NumBaseElements) {
    return createObject(Loc, Ty, Elements, NumBaseElements,
                        hasOwnership()
                            ? mergeSILValueOwnership(Elements)
                            : ValueOwnershipKind(OwnershipKind::None));
  }

  ObjectInst *createObject(SILLocation Loc, SILType Ty,
                           ArrayRef<SILValue> Elements,
                           unsigned NumBaseElements,
                           ValueOwnershipKind forwardingOwnershipKind) {
    return insert(ObjectInst::create(getSILDebugLocation(Loc), Ty, Elements,
                                     NumBaseElements, getModule(),
                                     forwardingOwnershipKind));
  }

  StructInst *createStruct(SILLocation Loc, SILType Ty,
                           ArrayRef<SILValue> Elements) {
    return createStruct(Loc, Ty, Elements,
                        hasOwnership()
                            ? mergeSILValueOwnership(Elements)
                            : ValueOwnershipKind(OwnershipKind::None));
  }

  StructInst *createStruct(SILLocation Loc, SILType Ty,
                           ArrayRef<SILValue> Elements,
                           ValueOwnershipKind forwardingOwnershipKind) {
    assert(isLoadableOrOpaque(Ty));
    return insert(StructInst::create(getSILDebugLocation(Loc), Ty, Elements,
                                     getModule(), forwardingOwnershipKind));
  }

  TupleInst *createTuple(SILLocation Loc, SILType Ty,
                         ArrayRef<SILValue> Elements) {
    return createTuple(Loc, Ty, Elements,
                       hasOwnership()
                           ? mergeSILValueOwnership(Elements)
                           : ValueOwnershipKind(OwnershipKind::None));
  }

  TupleInst *createTuple(SILLocation Loc, SILType Ty,
                         ArrayRef<SILValue> Elements,
                         ValueOwnershipKind forwardingOwnershipKind) {
    assert(isLoadableOrOpaque(Ty));
    return insert(TupleInst::create(getSILDebugLocation(Loc), Ty, Elements,
                                    getModule(), forwardingOwnershipKind));
  }

  TupleInst *createTuple(SILLocation loc, ArrayRef<SILValue> elts);

  EnumInst *createEnum(SILLocation Loc, SILValue Operand,
                       EnumElementDecl *Element, SILType Ty) {
    return createEnum(Loc, Operand, Element, Ty,
                      Operand ? Operand->getOwnershipKind()
                              : ValueOwnershipKind(OwnershipKind::None));
  }

  EnumInst *createEnum(SILLocation Loc, SILValue Operand,
                       EnumElementDecl *Element, SILType Ty,
                       ValueOwnershipKind forwardingOwnershipKind) {
    assert(isLoadableOrOpaque(Ty));
    return insert(new (getModule())
                      EnumInst(getSILDebugLocation(Loc), Operand, Element, Ty,
                               forwardingOwnershipKind));
  }

  /// Inject a loadable value into the corresponding optional type.
  EnumInst *createOptionalSome(SILLocation Loc, SILValue operand, SILType ty) {
    assert(isLoadableOrOpaque(ty));
    auto someDecl = getModule().getASTContext().getOptionalSomeDecl();
    return createEnum(Loc, operand, someDecl, ty);
  }

  /// Create the nil value of a loadable optional type.
  EnumInst *createOptionalNone(SILLocation Loc, SILType ty) {
    assert(isLoadableOrOpaque(ty));
    auto noneDecl = getModule().getASTContext().getOptionalNoneDecl();
    return createEnum(Loc, nullptr, noneDecl, ty);
  }

  InitEnumDataAddrInst *createInitEnumDataAddr(SILLocation Loc,
                                               SILValue Operand,
                                               EnumElementDecl *Element,
                                               SILType Ty) {
    return insert(new (getModule()) InitEnumDataAddrInst(
        getSILDebugLocation(Loc), Operand, Element, Ty));
  }

  UncheckedEnumDataInst *createUncheckedEnumData(SILLocation Loc,
                                                 SILValue Operand,
                                                 EnumElementDecl *Element,
                                                 SILType Ty) {
    return createUncheckedEnumData(Loc, Operand, Element, Ty,
                                   Operand->getOwnershipKind());
  }

  UncheckedEnumDataInst *createUncheckedEnumData(SILLocation Loc,
                                                 SILValue Operand,
                                                 EnumElementDecl *Element) {
    SILType EltType = Operand->getType().getEnumElementType(
        Element, getModule(), getTypeExpansionContext());
    return createUncheckedEnumData(Loc, Operand, Element, EltType);
  }

  UncheckedEnumDataInst *
  createUncheckedEnumData(SILLocation Loc, SILValue Operand,
                          EnumElementDecl *Element, SILType Ty,
                          ValueOwnershipKind forwardingOwnershipKind) {
    assert(isLoadableOrOpaque(Ty));
    return insert(new (getModule()) UncheckedEnumDataInst(
        getSILDebugLocation(Loc), Operand, Element, Ty,
        forwardingOwnershipKind));
  }

  /// Return unchecked_enum_data %Operand, #Optional<T>.some.
  SILValue emitExtractOptionalPayloadOperation(SILLocation Loc,
                                               SILValue Operand) {
    auto *Decl = F->getASTContext().getOptionalSomeDecl();
    return createUncheckedEnumData(Loc, Operand, Decl);
  }

  UncheckedTakeEnumDataAddrInst *
  createUncheckedTakeEnumDataAddr(SILLocation Loc, SILValue Operand,
                                  EnumElementDecl *Element, SILType Ty) {
    return insert(new (getModule()) UncheckedTakeEnumDataAddrInst(
        getSILDebugLocation(Loc), Operand, Element, Ty));
  }

  UncheckedTakeEnumDataAddrInst *
  createUncheckedTakeEnumDataAddr(SILLocation Loc, SILValue Operand,
                                  EnumElementDecl *Element) {
    SILType EltType = Operand->getType().getEnumElementType(
        Element, getModule(), getTypeExpansionContext());
    return createUncheckedTakeEnumDataAddr(Loc, Operand, Element, EltType);
  }

  InjectEnumAddrInst *createInjectEnumAddr(SILLocation Loc, SILValue Operand,
                                           EnumElementDecl *Element) {
    return insert(new (getModule()) InjectEnumAddrInst(
        getSILDebugLocation(Loc), Operand, Element));
  }

  SelectEnumInst *createSelectEnum(
      SILLocation Loc, SILValue Operand, SILType Ty, SILValue DefaultValue,
      ArrayRef<std::pair<EnumElementDecl *, SILValue>> CaseValues,
      llvm::Optional<ArrayRef<ProfileCounter>> CaseCounts = llvm::None,
      ProfileCounter DefaultCount = ProfileCounter()) {
    return createSelectEnum(Loc, Operand, Ty, DefaultValue, CaseValues,
                            CaseCounts, DefaultCount,
                            Operand->getOwnershipKind());
  }

  SelectEnumInst *createSelectEnum(
      SILLocation Loc, SILValue Operand, SILType Ty, SILValue DefaultValue,
      ArrayRef<std::pair<EnumElementDecl *, SILValue>> CaseValues,
      llvm::Optional<ArrayRef<ProfileCounter>> CaseCounts,
      ProfileCounter DefaultCount, ValueOwnershipKind forwardingOwnershipKind) {
    assert(isLoadableOrOpaque(Ty));
    return insert(SelectEnumInst::create(
        getSILDebugLocation(Loc), Operand, Ty, DefaultValue, CaseValues,
        getModule(), CaseCounts, DefaultCount, forwardingOwnershipKind));
  }

  SelectEnumAddrInst *createSelectEnumAddr(
      SILLocation Loc, SILValue Operand, SILType Ty, SILValue DefaultValue,
      ArrayRef<std::pair<EnumElementDecl *, SILValue>> CaseValues,
      llvm::Optional<ArrayRef<ProfileCounter>> CaseCounts = llvm::None,
      ProfileCounter DefaultCount = ProfileCounter()) {
    return insert(SelectEnumAddrInst::create(
        getSILDebugLocation(Loc), Operand, Ty, DefaultValue, CaseValues,
        getModule(), CaseCounts, DefaultCount));
  }

  TupleExtractInst *createTupleExtract(SILLocation Loc, SILValue Operand,
                                       unsigned FieldNo, SILType ResultTy) {
    return createTupleExtract(Loc, Operand, FieldNo, ResultTy,
                              Operand->getOwnershipKind());
  }

  TupleExtractInst *createTupleExtract(SILLocation Loc, SILValue Operand,
                                       unsigned FieldNo) {
    auto type = Operand->getType().getTupleElementType(FieldNo);
    return createTupleExtract(Loc, Operand, FieldNo, type,
                              Operand->getOwnershipKind());
  }

  TupleExtractInst *
  createTupleExtract(SILLocation Loc, SILValue Operand, unsigned FieldNo,
                     SILType ResultTy,
                     ValueOwnershipKind forwardingOwnershipKind) {
    return insert(new (getModule()) TupleExtractInst(getSILDebugLocation(Loc),
                                                     Operand, FieldNo, ResultTy,
                                                     forwardingOwnershipKind));
  }

  TupleElementAddrInst *createTupleElementAddr(SILLocation Loc,
                                               SILValue Operand,
                                               unsigned FieldNo,
                                               SILType ResultTy) {
    assert(!Operand->getType().castTo<TupleType>().containsPackExpansionType()
           && "tuples with pack expansions must be indexed with "
              "tuple_pack_element_addr");
    return insert(new (getModule()) TupleElementAddrInst(
        getSILDebugLocation(Loc), Operand, FieldNo, ResultTy));
  }

  TupleElementAddrInst *
  createTupleElementAddr(SILLocation Loc, SILValue Operand, unsigned FieldNo) {
    return insert(new (getModule()) TupleElementAddrInst(
        getSILDebugLocation(Loc), Operand, FieldNo,
        Operand->getType().getTupleElementType(FieldNo)));
  }

  StructExtractInst *createStructExtract(SILLocation Loc, SILValue Operand,
                                         VarDecl *Field, SILType ResultTy) {
    return createStructExtract(Loc, Operand, Field, ResultTy,
                               Operand->getOwnershipKind());
  }

  StructExtractInst *createStructExtract(SILLocation Loc, SILValue Operand,
                                         VarDecl *Field) {
    auto type = Operand->getType().getFieldType(Field, getModule(),
                                                getTypeExpansionContext());
    return createStructExtract(Loc, Operand, Field, type,
                               Operand->getOwnershipKind());
  }

  StructExtractInst *
  createStructExtract(SILLocation Loc, SILValue Operand, VarDecl *Field,
                      SILType ResultTy,
                      ValueOwnershipKind forwardingOwnershipKind) {
    return insert(new (getModule()) StructExtractInst(
        getSILDebugLocation(Loc), Operand, Field, ResultTy,
        Operand->getOwnershipKind()));
  }

  StructElementAddrInst *createStructElementAddr(SILLocation Loc,
                                                 SILValue Operand,
                                                 VarDecl *Field,
                                                 SILType ResultTy) {
    return insert(new (getModule()) StructElementAddrInst(
        getSILDebugLocation(Loc), Operand, Field, ResultTy));
  }

  StructElementAddrInst *
  createStructElementAddr(SILLocation Loc, SILValue Operand, VarDecl *Field) {
    auto ResultTy = Operand->getType().getFieldType(Field, getModule(),
                                                    getTypeExpansionContext());
    return createStructElementAddr(Loc, Operand, Field, ResultTy);
  }

  RefElementAddrInst *createRefElementAddr(SILLocation Loc, SILValue Operand,
                                           VarDecl *Field, SILType ResultTy,
                                           bool IsImmutable = false) {
    return insert(new (getModule()) RefElementAddrInst(
        getSILDebugLocation(Loc), Operand, Field, ResultTy, IsImmutable));
  }
  RefElementAddrInst *createRefElementAddr(SILLocation Loc, SILValue Operand,
                                           VarDecl *Field) {
    auto ResultTy = Operand->getType().getFieldType(Field, getModule(),
                                                    getTypeExpansionContext());
    return createRefElementAddr(Loc, Operand, Field, ResultTy);
  }

  RefTailAddrInst *createRefTailAddr(SILLocation Loc, SILValue Ref,
                                     SILType ResultTy,
                                     bool IsImmutable = false) {
    return insert(new (getModule()) RefTailAddrInst(getSILDebugLocation(Loc),
                                                  Ref, ResultTy, IsImmutable));
  }

  DestructureStructInst *createDestructureStruct(SILLocation Loc,
                                                 SILValue Operand) {
    return insert(
        DestructureStructInst::create(getFunction(), getSILDebugLocation(Loc),
                                      Operand, Operand->getOwnershipKind()));
  }

  DestructureStructInst *
  createDestructureStruct(SILLocation Loc, SILValue Operand,
                          ValueOwnershipKind forwardingOwnershipKind) {
    return insert(
        DestructureStructInst::create(getFunction(), getSILDebugLocation(Loc),
                                      Operand, forwardingOwnershipKind));
  }

  DestructureTupleInst *createDestructureTuple(SILLocation Loc,
                                               SILValue Operand) {
    return createDestructureTuple(Loc, Operand, Operand->getOwnershipKind());
  }

  DestructureTupleInst *
  createDestructureTuple(SILLocation Loc, SILValue Operand,
                         ValueOwnershipKind forwardingOwnershipKind) {
    return insert(
        DestructureTupleInst::create(getFunction(), getSILDebugLocation(Loc),
                                     Operand, forwardingOwnershipKind));
  }

  MultipleValueInstruction *emitDestructureValueOperation(SILLocation loc,
                                                          SILValue operand) {
    // If you hit this assert, you are using the wrong method. Use instead:
    //
    // emitDestructureValueOperation(SILLocation, SILValue,
    //                               SmallVectorImpl<SILValue> &);
    assert(hasOwnership() && "Expected to be called in ownership code only.");
    SILType opTy = operand->getType();
    if (opTy.is<TupleType>())
      return createDestructureTuple(loc, operand);
    if (opTy.getStructOrBoundGenericStruct())
      return createDestructureStruct(loc, operand);
    llvm_unreachable("Can not emit a destructure for this type of operand.");
  }

  void
  emitDestructureValueOperation(SILLocation loc, SILValue operand,
                                function_ref<void(unsigned, SILValue)> func);

  void emitDestructureValueOperation(SILLocation loc, SILValue operand,
                                     SmallVectorImpl<SILValue> &result);

  void emitDestructureAddressOperation(SILLocation loc, SILValue operand,
                                       SmallVectorImpl<SILValue> &result);

  void emitDestructureAddressOperation(
      SILLocation loc, SILValue operand,
      function_ref<void(unsigned, SILValue)> result);

  void emitDestructureOperation(SILLocation loc, SILValue operand,
                                SmallVectorImpl<SILValue> &result) {
    if (operand->getType().isAddress())
      return emitDestructureAddressOperation(loc, operand, result);
    return emitDestructureValueOperation(loc, operand, result);
  }

  void emitDestructureOperation(SILLocation loc, SILValue operand,
                                function_ref<void(unsigned, SILValue)> result) {
    if (operand->getType().isAddress())
      return emitDestructureAddressOperation(loc, operand, result);
    return emitDestructureValueOperation(loc, operand, result);
  }

  ClassMethodInst *createClassMethod(SILLocation Loc, SILValue Operand,
                                     SILDeclRef Member, SILType MethodTy) {
    return insert(new (getModule()) ClassMethodInst(
        getSILDebugLocation(Loc), Operand, Member, MethodTy));
  }

  SuperMethodInst *createSuperMethod(SILLocation Loc, SILValue Operand,
                                     SILDeclRef Member, SILType MethodTy) {
    return insert(new (getModule()) SuperMethodInst(
        getSILDebugLocation(Loc), Operand, Member, MethodTy));
  }

  ObjCMethodInst *createObjCMethod(SILLocation Loc, SILValue Operand,
                                   SILDeclRef Member, SILType MethodTy) {
    return insert(ObjCMethodInst::create(getSILDebugLocation(Loc), Operand,
                                         Member, MethodTy, &getFunction()));
  }

  ObjCSuperMethodInst *createObjCSuperMethod(SILLocation Loc, SILValue Operand,
                                             SILDeclRef Member, SILType MethodTy) {
    return insert(new (getModule()) ObjCSuperMethodInst(
        getSILDebugLocation(Loc), Operand, Member, MethodTy));
  }

  WitnessMethodInst *createWitnessMethod(SILLocation Loc, CanType LookupTy,
                                         ProtocolConformanceRef Conformance,
                                         SILDeclRef Member, SILType MethodTy) {
    return insert(WitnessMethodInst::create(
        getSILDebugLocation(Loc), LookupTy, Conformance, Member, MethodTy,
        &getFunction()));
  }

  OpenExistentialAddrInst *
  createOpenExistentialAddr(SILLocation Loc, SILValue Operand, SILType SelfTy,
                            OpenedExistentialAccess ForAccess) {
    return insert(new (getModule()) OpenExistentialAddrInst(
        getSILDebugLocation(Loc), Operand, SelfTy, ForAccess));
  }

  OpenExistentialValueInst *createOpenExistentialValue(SILLocation Loc,
                                                       SILValue Operand,
                                                       SILType SelfTy) {
    return createOpenExistentialValue(Loc, Operand, SelfTy,
                                      Operand->getOwnershipKind());
  }

  OpenExistentialValueInst *
  createOpenExistentialValue(SILLocation Loc, SILValue Operand, SILType SelfTy,
                             ValueOwnershipKind forwardingOwnershipKind) {
    return insert(new (getModule()) OpenExistentialValueInst(
        getSILDebugLocation(Loc), Operand, SelfTy, forwardingOwnershipKind));
  }

  OpenExistentialMetatypeInst *createOpenExistentialMetatype(SILLocation Loc,
                                                             SILValue operand,
                                                             SILType selfTy) {
    return insert(new (getModule()) OpenExistentialMetatypeInst(
        getSILDebugLocation(Loc), operand, selfTy));
  }

  OpenExistentialRefInst *
  createOpenExistentialRef(SILLocation Loc, SILValue Operand, SILType Ty) {
    return createOpenExistentialRef(Loc, Operand, Ty,
                                    Operand->getOwnershipKind());
  }

  OpenExistentialRefInst *
  createOpenExistentialRef(SILLocation Loc, SILValue Operand, SILType Ty,
                           ValueOwnershipKind forwardingOwnershipKind) {
    return insert(new (getModule()) OpenExistentialRefInst(
        getSILDebugLocation(Loc), Operand, Ty, forwardingOwnershipKind));
  }

  OpenExistentialBoxInst *
  createOpenExistentialBox(SILLocation Loc, SILValue Operand, SILType Ty) {
    return insert(new (getModule()) OpenExistentialBoxInst(
        getSILDebugLocation(Loc), Operand, Ty));
  }

  OpenExistentialBoxValueInst *
  createOpenExistentialBoxValue(SILLocation Loc, SILValue Operand, SILType Ty) {
    return createOpenExistentialBoxValue(Loc, Operand, Ty,
                                         Operand->getOwnershipKind());
  }

  OpenExistentialBoxValueInst *
  createOpenExistentialBoxValue(SILLocation Loc, SILValue Operand, SILType Ty,
                                ValueOwnershipKind forwardingOwnershipKind) {
    return insert(new (getModule()) OpenExistentialBoxValueInst(
        getSILDebugLocation(Loc), Operand, Ty, forwardingOwnershipKind));
  }

  InitExistentialAddrInst *
  createInitExistentialAddr(SILLocation Loc, SILValue Existential,
                            CanType FormalConcreteType,
                            SILType LoweredConcreteType,
                            ArrayRef<ProtocolConformanceRef> Conformances) {
    return insert(InitExistentialAddrInst::create(
        getSILDebugLocation(Loc), Existential, FormalConcreteType,
        LoweredConcreteType, Conformances, &getFunction()));
  }

  InitExistentialValueInst *
  createInitExistentialValue(SILLocation Loc, SILType ExistentialType,
                              CanType FormalConcreteType, SILValue Concrete,
                              ArrayRef<ProtocolConformanceRef> Conformances) {
    return insert(InitExistentialValueInst::create(
        getSILDebugLocation(Loc), ExistentialType, FormalConcreteType, Concrete,
        Conformances, &getFunction()));
  }

  InitExistentialMetatypeInst *
  createInitExistentialMetatype(SILLocation Loc, SILValue metatype,
                                SILType existentialType,
                                ArrayRef<ProtocolConformanceRef> conformances) {
    return insert(InitExistentialMetatypeInst::create(
        getSILDebugLocation(Loc), existentialType, metatype, conformances,
        &getFunction()));
  }

  InitExistentialRefInst *
  createInitExistentialRef(SILLocation Loc, SILType ExistentialType,
                           CanType FormalConcreteType, SILValue Concrete,
                           ArrayRef<ProtocolConformanceRef> Conformances) {
    return createInitExistentialRef(Loc, ExistentialType, FormalConcreteType,
                                    Concrete, Conformances,
                                    Concrete->getOwnershipKind());
  }

  InitExistentialRefInst *
  createInitExistentialRef(SILLocation Loc, SILType ExistentialType,
                           CanType FormalConcreteType, SILValue Concrete,
                           ArrayRef<ProtocolConformanceRef> Conformances,
                           ValueOwnershipKind forwardingOwnershipKind) {
    return insert(InitExistentialRefInst::create(
        getSILDebugLocation(Loc), ExistentialType, FormalConcreteType, Concrete,
        Conformances, &getFunction(), forwardingOwnershipKind));
  }

  DeinitExistentialAddrInst *createDeinitExistentialAddr(SILLocation Loc,
                                                         SILValue Existential) {
    return insert(new (getModule()) DeinitExistentialAddrInst(
        getSILDebugLocation(Loc), Existential));
  }

  DeinitExistentialValueInst *
  createDeinitExistentialValue(SILLocation Loc, SILValue Existential) {
    return insert(new (getModule()) DeinitExistentialValueInst(
        getSILDebugLocation(Loc), Existential));
  }

  PackLengthInst *
  createPackLength(SILLocation loc, CanPackType packType) {
    return insert(PackLengthInst::create(getFunction(),
                              getSILDebugLocation(loc),
                              packType));
  }

  DynamicPackIndexInst *
  createDynamicPackIndex(SILLocation loc, SILValue indexValue,
                         CanPackType indexedPackType) {
    return insert(DynamicPackIndexInst::create(getFunction(),
                              getSILDebugLocation(loc),
                              indexValue, indexedPackType));
  }

  PackPackIndexInst *
  createPackPackIndex(SILLocation loc, unsigned sliceStartIndex,
                      SILValue indexWithinSlice,
                      CanPackType indexedPackType) {
    return insert(PackPackIndexInst::create(getFunction(),
                              getSILDebugLocation(loc),
                              sliceStartIndex, indexWithinSlice,
                              indexedPackType));
  }

  ScalarPackIndexInst *
  createScalarPackIndex(SILLocation loc, unsigned componentIndex,
                        CanPackType indexedPackType) {
    return insert(ScalarPackIndexInst::create(getFunction(),
                              getSILDebugLocation(loc),
                              componentIndex, indexedPackType));
  }

  OpenPackElementInst *
  createOpenPackElement(SILLocation loc, SILValue packIndex,
                        GenericEnvironment *openedElementEnvironment) {
    return insert(OpenPackElementInst::create(getFunction(),
                              getSILDebugLocation(loc),
                              packIndex, openedElementEnvironment));
  }

  PackElementGetInst *
  createPackElementGet(SILLocation loc, SILValue packIndex,
                       SILValue pack, SILType elementType) {
    return insert(PackElementGetInst::create(getFunction(),
                              getSILDebugLocation(loc),
                              packIndex, pack, elementType));
  }

  PackElementSetInst *
  createPackElementSet(SILLocation loc, SILValue elementValue,
                       SILValue packIndex, SILValue pack) {
    return insert(new (getModule()) PackElementSetInst(
                              getSILDebugLocation(loc),
                              elementValue, packIndex, pack));
  }

  TuplePackElementAddrInst *
  createTuplePackElementAddr(SILLocation loc, SILValue packIndex,
                             SILValue tupleAddr, SILType elementType) {
    return insert(TuplePackElementAddrInst::create(getFunction(),
                              getSILDebugLocation(loc),
                              packIndex, tupleAddr, elementType));
  }

  ProjectBlockStorageInst *createProjectBlockStorage(SILLocation Loc,
                                                     SILValue Storage) {
    auto CaptureTy = Storage->getType()
                         .castTo<SILBlockStorageType>()
                         ->getCaptureAddressType();
    return createProjectBlockStorage(Loc, Storage, CaptureTy);
  }
  ProjectBlockStorageInst *createProjectBlockStorage(SILLocation Loc,
                                                     SILValue Storage,
                                                     SILType CaptureTy) {
    return insert(new (getModule()) ProjectBlockStorageInst(
        getSILDebugLocation(Loc), Storage, CaptureTy));
  }

  InitBlockStorageHeaderInst *
  createInitBlockStorageHeader(SILLocation Loc, SILValue BlockStorage,
                               SILValue InvokeFunction, SILType BlockType,
                               SubstitutionMap Subs) {
    return insert(InitBlockStorageHeaderInst::create(getFunction(),
      getSILDebugLocation(Loc), BlockStorage, InvokeFunction, BlockType, Subs));
  }

  MetatypeInst *createMetatype(SILLocation Loc, SILType Metatype) {
    return insert(MetatypeInst::create(getSILDebugLocation(Loc), Metatype,
                                       &getFunction()));
  }

  ObjCMetatypeToObjectInst *
  createObjCMetatypeToObject(SILLocation Loc, SILValue Op, SILType Ty) {
    return insert(new (getModule()) ObjCMetatypeToObjectInst(
        getSILDebugLocation(Loc), Op, Ty));
  }

  ObjCExistentialMetatypeToObjectInst *
  createObjCExistentialMetatypeToObject(SILLocation Loc, SILValue Op,
                                        SILType Ty) {
    return insert(new (getModule()) ObjCExistentialMetatypeToObjectInst(
        getSILDebugLocation(Loc), Op, Ty));
  }

  ValueMetatypeInst *createValueMetatype(SILLocation Loc, SILType Metatype,
                                         SILValue Base);

  ExistentialMetatypeInst *
  createExistentialMetatype(SILLocation Loc, SILType Metatype, SILValue Base) {
    return insert(new (getModule()) ExistentialMetatypeInst(
        getSILDebugLocation(Loc), Metatype, Base));
  }

  CopyBlockInst *createCopyBlock(SILLocation Loc, SILValue Operand) {
    return insert(new (getModule())
                      CopyBlockInst(getSILDebugLocation(Loc), Operand));
  }

  CopyBlockWithoutEscapingInst *
  createCopyBlockWithoutEscaping(SILLocation Loc, SILValue Block,
                                 SILValue Closure) {
    return insert(new (getModule()) CopyBlockWithoutEscapingInst(
        getSILDebugLocation(Loc), Block, Closure));
  }

  StrongRetainInst *createStrongRetain(SILLocation Loc, SILValue Operand,
                                       Atomicity atomicity) {
    assert(!hasOwnership());
    return insert(new (getModule()) StrongRetainInst(getSILDebugLocation(Loc),
                                                       Operand, atomicity));
  }
  StrongReleaseInst *createStrongRelease(SILLocation Loc, SILValue Operand,
                                         Atomicity atomicity) {
    assert(!hasOwnership());
    return insert(new (getModule()) StrongReleaseInst(
        getSILDebugLocation(Loc), Operand, atomicity));
  }

  EndLifetimeInst *createEndLifetime(SILLocation Loc, SILValue Operand) {
    return insert(new (getModule())
                      EndLifetimeInst(getSILDebugLocation(Loc), Operand));
  }

  UncheckedOwnershipConversionInst *
  createUncheckedOwnershipConversion(SILLocation Loc, SILValue Operand,
                                     ValueOwnershipKind Kind) {
    return insert(new (getModule()) UncheckedOwnershipConversionInst(
        getSILDebugLocation(Loc), Operand, Kind));
  }

  FixLifetimeInst *createFixLifetime(SILLocation Loc, SILValue Operand) {
    return insert(new (getModule())
                      FixLifetimeInst(getSILDebugLocation(Loc), Operand));
  }
  void emitFixLifetime(SILLocation Loc, SILValue Operand) {
    if (getTypeLowering(Operand->getType()).isTrivial())
      return;
    createFixLifetime(Loc, Operand);
  }
  ClassifyBridgeObjectInst *createClassifyBridgeObject(SILLocation Loc,
                                                       SILValue value);

  MarkDependenceInst *createMarkDependence(SILLocation Loc, SILValue value,
                                           SILValue base) {
    return createMarkDependence(Loc, value, base, value->getOwnershipKind());
  }

  MarkDependenceInst *
  createMarkDependence(SILLocation Loc, SILValue value, SILValue base,
                       ValueOwnershipKind forwardingOwnershipKind) {
    return insert(new (getModule()) MarkDependenceInst(
        getSILDebugLocation(Loc), value, base, forwardingOwnershipKind));
  }

  IsUniqueInst *createIsUnique(SILLocation Loc, SILValue operand) {
    auto Int1Ty = SILType::getBuiltinIntegerType(1, getASTContext());
    return insert(new (getModule()) IsUniqueInst(getSILDebugLocation(Loc),
                                                   operand, Int1Ty));
  }
  BeginCOWMutationInst *createBeginCOWMutation(SILLocation Loc,
                                    SILValue operand, bool isNative) {
    auto Int1Ty = SILType::getBuiltinIntegerType(1, getASTContext());
    return insert(BeginCOWMutationInst::create(getSILDebugLocation(Loc), operand,
                                        Int1Ty, getFunction(), isNative));
  }
  EndCOWMutationInst *createEndCOWMutation(SILLocation Loc, SILValue operand,
                                           bool keepUnique = false) {
    return insert(new (getModule()) EndCOWMutationInst(getSILDebugLocation(Loc),
                                                  operand, keepUnique));
  }
  IsEscapingClosureInst *createIsEscapingClosure(SILLocation Loc,
                                                 SILValue operand,
                                                 unsigned VerificationType) {
    auto Int1Ty = SILType::getBuiltinIntegerType(1, getASTContext());
    return insert(new (getModule()) IsEscapingClosureInst(
        getSILDebugLocation(Loc), operand, Int1Ty, VerificationType));
  }

  DeallocStackInst *createDeallocStack(SILLocation Loc, SILValue operand) {
    return insert(new (getModule())
                      DeallocStackInst(getSILDebugLocation(Loc), operand));
  }
  DeallocPackInst *createDeallocPack(SILLocation loc, SILValue operand) {
    return insert(new (getModule())
                      DeallocPackInst(getSILDebugLocation(loc), operand));
  }
  DeallocPackMetadataInst *createDeallocPackMetadata(SILLocation loc,
                                                     SILValue alloc) {
    return insert(new (getModule())
                      DeallocPackMetadataInst(getSILDebugLocation(loc), alloc));
  }
  DeallocStackRefInst *createDeallocStackRef(SILLocation Loc,
                                                     SILValue operand) {
    return insert(new (getModule())
                    DeallocStackRefInst(getSILDebugLocation(Loc), operand));
  }
  DeallocRefInst *createDeallocRef(SILLocation Loc, SILValue operand) {
    return insert(new (getModule()) DeallocRefInst(
        getSILDebugLocation(Loc), operand));
  }
  DeallocPartialRefInst *createDeallocPartialRef(SILLocation Loc,
                                                 SILValue operand,
                                                 SILValue metatype) {
    return insert(new (getModule()) DeallocPartialRefInst(
        getSILDebugLocation(Loc), operand, metatype));
  }
  DeallocBoxInst *createDeallocBox(SILLocation Loc,
                                   SILValue operand) {
    return insert(new (getModule()) DeallocBoxInst(
        getSILDebugLocation(Loc), operand));
  }
  DeallocExistentialBoxInst *createDeallocExistentialBox(SILLocation Loc,
                                                         CanType concreteType,
                                                         SILValue operand) {
    return insert(new (getModule()) DeallocExistentialBoxInst(
        getSILDebugLocation(Loc), concreteType, operand));
  }
  DestroyAddrInst *createDestroyAddr(SILLocation Loc, SILValue Operand) {
    return insert(new (getModule())
                      DestroyAddrInst(getSILDebugLocation(Loc), Operand));
  }

  ProjectBoxInst *createProjectBox(SILLocation Loc, SILValue boxOperand,
                                   unsigned index);
  ProjectExistentialBoxInst *createProjectExistentialBox(SILLocation Loc,
                                                         SILType valueTy,
                                                         SILValue boxOperand) {
    return insert(new (getModule()) ProjectExistentialBoxInst(
        getSILDebugLocation(Loc), valueTy, boxOperand));
  }

  //===--------------------------------------------------------------------===//
  // Unchecked cast helpers
  //===--------------------------------------------------------------------===//

  /// Create the appropriate cast instruction based on result type.
  ///
  /// NOTE: We allow for non-layout compatible casts that shrink the underlying
  /// type we are bit casting!
  SingleValueInstruction *
  createUncheckedReinterpretCast(SILLocation Loc, SILValue Op, SILType Ty);

  /// Create an appropriate cast instruction based on result type. This cast
  /// forwards ownership from the operand to the result.
  ///
  /// WARNING: Because it forwards ownership, this cast is only valid with the
  /// source and destination types are layout equivalent. The destination type
  /// must include all the same references in the same positions.
  ///
  /// Note: Forwarding casts do not exist outside of OSSA. When ownership is
  /// disabled, this reduces to createUncheckedReinterpretCast, which may
  /// fall-back to unchecked_bitwise_cast. It is the caller's responsibility to
  /// emit the correct retains and releases.
  SingleValueInstruction *createUncheckedForwardingCast(SILLocation Loc,
                                                        SILValue Op,
                                                        SILType Ty);

  //===--------------------------------------------------------------------===//
  // Runtime failure
  //===--------------------------------------------------------------------===//

  CondFailInst *createCondFail(SILLocation Loc, SILValue Operand,
                               StringRef Message, bool Inverted = false) {
    if (Inverted) {
      SILType Ty = Operand->getType();
      SILValue True(createIntegerLiteral(Loc, Ty, 1));
      Operand =
          createBuiltinBinaryFunction(Loc, "xor", Ty, Ty, {Operand, True});
    }
    return insert(CondFailInst::create(getSILDebugLocation(Loc), Operand,
                                       Message, getModule()));
  }

  CondFailInst *createUnconditionalFail(SILLocation loc, StringRef message) {
    Type int1Ty = BuiltinIntegerType::get(1, getASTContext());
    auto int1SILTy = SILType::getPrimitiveObjectType(int1Ty->getCanonicalType());
    auto *one = createIntegerLiteral(loc, int1SILTy, 1);
    return createCondFail(loc, one, message);
  }

  //===--------------------------------------------------------------------===//
  // Profiler
  //===--------------------------------------------------------------------===//

  IncrementProfilerCounterInst *
  createIncrementProfilerCounter(SILLocation Loc, unsigned CounterIdx,
                                 StringRef PGOFuncName, unsigned NumCounters,
                                 uint64_t PGOFuncHash) {
    return insert(IncrementProfilerCounterInst::create(
        getSILDebugLocation(Loc), CounterIdx, PGOFuncName, NumCounters,
        PGOFuncHash, getModule()));
  }

  //===--------------------------------------------------------------------===//
  // Array indexing instructions
  //===--------------------------------------------------------------------===//

  IndexAddrInst *createIndexAddr(SILLocation Loc, SILValue Operand,
                                 SILValue Index, bool needsStackProtection) {
    return insert(new (getModule()) IndexAddrInst(getSILDebugLocation(Loc),
                                    Operand, Index, needsStackProtection));
  }

  TailAddrInst *createTailAddr(SILLocation Loc, SILValue Operand,
                               SILValue Count, SILType ResultTy) {
    return insert(new (getModule()) TailAddrInst(getSILDebugLocation(Loc),
                                                   Operand, Count, ResultTy));
  }

  IndexRawPointerInst *createIndexRawPointer(SILLocation Loc, SILValue Operand,
                                             SILValue Index) {
    return insert(new (getModule()) IndexRawPointerInst(
        getSILDebugLocation(Loc), Operand, Index));
  }

  //===--------------------------------------------------------------------===//
  // Concurrency instructions
  //===--------------------------------------------------------------------===//

  GetAsyncContinuationInst *createGetAsyncContinuation(SILLocation Loc,
                                                       CanType ResumeType,
                                                       bool Throws) {
    auto ContinuationType = SILType::getPrimitiveObjectType(
        getASTContext().TheRawUnsafeContinuationType);
    return insert(new (getModule()) GetAsyncContinuationInst(getSILDebugLocation(Loc),
                                                             ContinuationType,
                                                             ResumeType,
                                                             Throws));
  }

  GetAsyncContinuationAddrInst *createGetAsyncContinuationAddr(SILLocation Loc,
                                                               SILValue Operand,
                                                               CanType ResumeType,
                                                               bool Throws) {
    auto ContinuationType = SILType::getPrimitiveObjectType(
        getASTContext().TheRawUnsafeContinuationType);
    return insert(new (getModule()) GetAsyncContinuationAddrInst(getSILDebugLocation(Loc),
                                                                 Operand,
                                                                 ContinuationType,
                                                                 ResumeType,
                                                                 Throws));
  }

  HopToExecutorInst *createHopToExecutor(SILLocation Loc, SILValue Actor,
                                         bool mandatory) {
    return insert(new (getModule()) HopToExecutorInst(getSILDebugLocation(Loc),
                                                      Actor, hasOwnership(),
                                                      mandatory));
  }

  ExtractExecutorInst *createExtractExecutor(SILLocation Loc, SILValue Actor) {
    auto resultType = SILType::getPrimitiveObjectType(getASTContext().TheExecutorType);
    return insert(new (getModule()) ExtractExecutorInst(getSILDebugLocation(Loc),
                                                        Actor, hasOwnership(),
                                                        resultType));
  }

  //===--------------------------------------------------------------------===//
  // Terminator SILInstruction Creation Methods
  //===--------------------------------------------------------------------===//

  UnreachableInst *createUnreachable(SILLocation Loc) {
    return insertTerminator(new (getModule())
                                UnreachableInst(getSILDebugLocation(Loc)));
  }

  ReturnInst *createReturn(SILLocation Loc, SILValue ReturnValue) {
    return insertTerminator(new (getModule()) ReturnInst(
        getFunction(), getSILDebugLocation(Loc), ReturnValue));
  }

  ThrowInst *createThrow(SILLocation Loc, SILValue errorValue) {
    return insertTerminator(
        new (getModule()) ThrowInst(getSILDebugLocation(Loc), errorValue));
  }

  UnwindInst *createUnwind(SILLocation loc) {
    return insertTerminator(
        new (getModule()) UnwindInst(getSILDebugLocation(loc)));
  }

  YieldInst *createYield(SILLocation loc, ArrayRef<SILValue> yieldedValues,
                         SILBasicBlock *resumeBB, SILBasicBlock *unwindBB) {
    return insertTerminator(
        YieldInst::create(getSILDebugLocation(loc), yieldedValues,
                          resumeBB, unwindBB, getFunction()));
  }
  
  AwaitAsyncContinuationInst *createAwaitAsyncContinuation(SILLocation loc,
                                                           SILValue continuation,
                                                           SILBasicBlock *resumeBB,
                                                           SILBasicBlock *errorBB) {
    return insertTerminator(
        new (getModule()) AwaitAsyncContinuationInst(getSILDebugLocation(loc),
                                                     continuation,
                                                     resumeBB, errorBB));
  }
  
  CondBranchInst *
  createCondBranch(SILLocation Loc, SILValue Cond, SILBasicBlock *Target1,
                   SILBasicBlock *Target2,
                   ProfileCounter Target1Count = ProfileCounter(),
                   ProfileCounter Target2Count = ProfileCounter()) {
    return insertTerminator(
        CondBranchInst::create(getSILDebugLocation(Loc), Cond, Target1, Target2,
                               Target1Count, Target2Count, getFunction()));
  }

  CondBranchInst *
  createCondBranch(SILLocation Loc, SILValue Cond, SILBasicBlock *Target1,
                   ArrayRef<SILValue> Args1, SILBasicBlock *Target2,
                   ArrayRef<SILValue> Args2,
                   ProfileCounter Target1Count = ProfileCounter(),
                   ProfileCounter Target2Count = ProfileCounter()) {
    return insertTerminator(
        CondBranchInst::create(getSILDebugLocation(Loc), Cond, Target1, Args1,
                               Target2, Args2, Target1Count, Target2Count, getFunction()));
  }

  CondBranchInst *
  createCondBranch(SILLocation Loc, SILValue Cond, SILBasicBlock *Target1,
                   OperandValueArrayRef Args1, SILBasicBlock *Target2,
                   OperandValueArrayRef Args2,
                   ProfileCounter Target1Count = ProfileCounter(),
                   ProfileCounter Target2Count = ProfileCounter()) {
    SmallVector<SILValue, 6> ArgsCopy1;
    SmallVector<SILValue, 6> ArgsCopy2;

    ArgsCopy1.reserve(Args1.size());
    ArgsCopy2.reserve(Args2.size());

    for (auto I = Args1.begin(), E = Args1.end(); I != E; ++I)
      ArgsCopy1.push_back(*I);
    for (auto I = Args2.begin(), E = Args2.end(); I != E; ++I)
      ArgsCopy2.push_back(*I);

    return insertTerminator(CondBranchInst::create(
        getSILDebugLocation(Loc), Cond, Target1, ArgsCopy1, Target2, ArgsCopy2,
        Target1Count, Target2Count, getFunction()));
  }

  BranchInst *createBranch(SILLocation Loc, SILBasicBlock *TargetBlock) {
    return insertTerminator(
        BranchInst::create(getSILDebugLocation(Loc), TargetBlock, getFunction()));
  }

  BranchInst *createBranch(SILLocation Loc, SILBasicBlock *TargetBlock,
                           ArrayRef<SILValue> Args) {
    return insertTerminator(
        BranchInst::create(getSILDebugLocation(Loc), TargetBlock, Args,
                           getFunction()));
  }

  BranchInst *createBranch(SILLocation Loc, SILBasicBlock *TargetBlock,
                           OperandValueArrayRef Args);

  // This only creates the terminator, not the results. Create the results with
  // OwnershipForwardingTermInst::createResult() and
  // SwitchEnumInst::createDefaultResult() to ensure that the result ownership
  // is correct (it must be consistent with the switch_enum's forwarding
  // ownership, which may differ from \p Operand's ownership).
  SwitchValueInst *
  createSwitchValue(SILLocation Loc, SILValue Operand, SILBasicBlock *DefaultBB,
                    ArrayRef<std::pair<SILValue, SILBasicBlock *>> CaseBBs) {
    return insertTerminator(SwitchValueInst::create(
        getSILDebugLocation(Loc), Operand, DefaultBB, CaseBBs, getFunction()));
  }

  SwitchEnumInst *createSwitchEnum(
      SILLocation Loc, SILValue Operand, SILBasicBlock *DefaultBB,
      ArrayRef<std::pair<EnumElementDecl *, SILBasicBlock *>> CaseBBs,
      llvm::Optional<ArrayRef<ProfileCounter>> CaseCounts = llvm::None,
      ProfileCounter DefaultCount = ProfileCounter());

  SwitchEnumInst *createSwitchEnum(
      SILLocation Loc, SILValue Operand, SILBasicBlock *DefaultBB,
      ArrayRef<std::pair<EnumElementDecl *, SILBasicBlock *>> CaseBBs,
      llvm::Optional<ArrayRef<ProfileCounter>> CaseCounts,
      ProfileCounter DefaultCount, ValueOwnershipKind forwardingOwnershipKind) {
    return insertTerminator(SwitchEnumInst::create(
        getSILDebugLocation(Loc), Operand, DefaultBB, CaseBBs, getFunction(),
        CaseCounts, DefaultCount, forwardingOwnershipKind));
  }

  SwitchEnumAddrInst *createSwitchEnumAddr(
      SILLocation Loc, SILValue Operand, SILBasicBlock *DefaultBB,
      ArrayRef<std::pair<EnumElementDecl *, SILBasicBlock *>> CaseBBs,
      llvm::Optional<ArrayRef<ProfileCounter>> CaseCounts = llvm::None,
      ProfileCounter DefaultCount = ProfileCounter()) {
    return insertTerminator(SwitchEnumAddrInst::create(
        getSILDebugLocation(Loc), Operand, DefaultBB, CaseBBs, getFunction(),
        CaseCounts, DefaultCount));
  }

  DynamicMethodBranchInst *
  createDynamicMethodBranch(SILLocation Loc, SILValue Operand,
                            SILDeclRef Member, SILBasicBlock *HasMethodBB,
                            SILBasicBlock *NoMethodBB) {
    return insertTerminator(
        DynamicMethodBranchInst::create(getSILDebugLocation(Loc), Operand,
                              Member, HasMethodBB, NoMethodBB, getFunction()));
  }

  CheckedCastBranchInst *
  createCheckedCastBranch(SILLocation Loc, bool isExact, SILValue op, 
                          CanType srcFormalTy, SILType destLoweredTy, 
                          CanType destFormalTy, SILBasicBlock *successBB,
                          SILBasicBlock *failureBB,
                          ProfileCounter Target1Count = ProfileCounter(),
                          ProfileCounter Target2Count = ProfileCounter());

  CheckedCastBranchInst *
  createCheckedCastBranch(SILLocation Loc, bool isExact, SILValue op, 
                          CanType srcFormalTy, SILType destLoweredTy, 
                          CanType destFormalTy, SILBasicBlock *successBB, 
                          SILBasicBlock *failureBB,
                          ValueOwnershipKind forwardingOwnershipKind,
                          ProfileCounter Target1Count = ProfileCounter(),
                          ProfileCounter Target2Count = ProfileCounter());

  CheckedCastAddrBranchInst *
  createCheckedCastAddrBranch(SILLocation Loc, CastConsumptionKind consumption,
                              SILValue src, CanType sourceFormalType,
                              SILValue dest, CanType targetFormalType,
                              SILBasicBlock *successBB,
                              SILBasicBlock *failureBB,
                              ProfileCounter Target1Count = ProfileCounter(),
                              ProfileCounter Target2Count = ProfileCounter()) {
    return insertTerminator(CheckedCastAddrBranchInst::create(
        getSILDebugLocation(Loc), consumption, src, sourceFormalType, dest,
        targetFormalType, successBB, failureBB, Target1Count, Target2Count,
        getFunction()));
  }

  //===--------------------------------------------------------------------===//
  // Memory management helpers
  //===--------------------------------------------------------------------===//

  /// Returns the default atomicity of the module.
  Atomicity getDefaultAtomicity() {
    return getModule().isDefaultAtomic() ? Atomicity::Atomic : Atomicity::NonAtomic;
  }

  /// Try to fold a destroy_addr operation into the previous instructions, or
  /// generate an explicit one if that fails.  If this inserts a new
  /// instruction, it returns it, otherwise it returns null.
  DestroyAddrInst *emitDestroyAddrAndFold(SILLocation Loc, SILValue Operand) {
    auto U = emitDestroyAddr(Loc, Operand);
    if (U.isNull() || !U.is<DestroyAddrInst *>())
      return nullptr;
    return U.get<DestroyAddrInst *>();
  }

  /// Perform a strong_release instruction at the current location, attempting
  /// to fold it locally into nearby retain instructions or emitting an explicit
  /// strong release if necessary.  If this inserts a new instruction, it
  /// returns it, otherwise it returns null.
  StrongReleaseInst *emitStrongReleaseAndFold(SILLocation Loc,
                                              SILValue Operand) {
    auto U = emitStrongRelease(Loc, Operand);
    if (U.isNull())
      return nullptr;
    if (auto *SRI = U.dyn_cast<StrongReleaseInst *>())
      return SRI;
    U.get<StrongRetainInst *>()->eraseFromParent();
    return nullptr;
  }

  /// Emit a release_value instruction at the current location, attempting to
  /// fold it locally into another nearby retain_value instruction.  This
  /// returns the new instruction if it inserts one, otherwise it returns null.
  ///
  /// This instruction doesn't handle strength reduction of release_value into
  /// a noop / strong_release / unowned_release.  For that, use the
  /// emitReleaseValueOperation method below or use the TypeLowering API.
  ReleaseValueInst *emitReleaseValueAndFold(SILLocation Loc, SILValue Operand) {
    auto U = emitReleaseValue(Loc, Operand);
    if (U.isNull())
      return nullptr;
    if (auto *RVI = U.dyn_cast<ReleaseValueInst *>())
      return RVI;
    U.get<RetainValueInst *>()->eraseFromParent();
    return nullptr;
  }

  /// Emit a release_value instruction at the current location, attempting to
  /// fold it locally into another nearby retain_value instruction.  This
  /// returns the new instruction if it inserts one, otherwise it returns null.
  ///
  /// This instruction doesn't handle strength reduction of release_value into
  /// a noop / strong_release / unowned_release.  For that, use the
  /// emitReleaseValueOperation method below or use the TypeLowering API.
  DestroyValueInst *emitDestroyValueAndFold(SILLocation Loc, SILValue Operand) {
    auto U = emitDestroyValue(Loc, Operand);
    if (U.isNull())
      return nullptr;
    if (auto *DVI = U.dyn_cast<DestroyValueInst *>())
      return DVI;
    auto *CVI = U.get<CopyValueInst *>();
    CVI->replaceAllUsesWith(CVI->getOperand());
    CVI->eraseFromParent();
    return nullptr;
  }

  /// Emit a release_value instruction at the current location, attempting to
  /// fold it locally into another nearby retain_value instruction. Returns a
  /// pointer union initialized with a release value inst if it inserts one,
  /// otherwise returns the retain. It is expected that the caller will remove
  /// the retain_value. This allows for the caller to update any state before
  /// the retain_value is destroyed.
  PointerUnion<RetainValueInst *, ReleaseValueInst *>
  emitReleaseValue(SILLocation Loc, SILValue Operand);

  /// Emit a strong_release instruction at the current location, attempting to
  /// fold it locally into another nearby strong_retain instruction. Returns a
  /// pointer union initialized with a strong_release inst if it inserts one,
  /// otherwise returns the pointer union initialized with the strong_retain. It
  /// is expected that the caller will remove the returned strong_retain. This
  /// allows for the caller to update any state before the release value is
  /// destroyed.
  PointerUnion<StrongRetainInst *, StrongReleaseInst *>
  emitStrongRelease(SILLocation Loc, SILValue Operand);

  /// Emit a destroy_addr instruction at \p Loc attempting to fold the
  /// destroy_addr locally into a copy_addr instruction. Returns a pointer union
  /// initialized with the folded copy_addr if the destroy_addr was folded into
  /// a copy_addr. Otherwise, returns the newly inserted destroy_addr.
  PointerUnion<CopyAddrInst *, DestroyAddrInst *>
  emitDestroyAddr(SILLocation Loc, SILValue Operand);

  /// Emit a destroy_value instruction at the current location, attempting to
  /// fold it locally into another nearby copy_value instruction. Returns a
  /// pointer union initialized with a destroy_value inst if it inserts one,
  /// otherwise returns the copy_value. It is expected that the caller will
  /// remove the copy_value. This allows for the caller to update any state
  /// before the copy_value is destroyed.
  PointerUnion<CopyValueInst *, DestroyValueInst *>
  emitDestroyValue(SILLocation Loc, SILValue Operand);

  /// Convenience function for calling emitCopy on the type lowering
  /// for the non-address value.
  SILValue emitCopyValueOperation(SILLocation Loc, SILValue v) {
    assert(!v->getType().isAddress());
    auto &lowering = getTypeLowering(v->getType());
    return lowering.emitCopyValue(*this, Loc, v);
  }

  /// Convenience function for calling emitCopy on the type lowering
  /// for the non-address value.
  SILValue emitLoweredCopyValueOperation(
      SILLocation Loc, SILValue v,
      Lowering::TypeLowering::TypeExpansionKind expansionKind) {
    assert(!v->getType().isAddress());
    auto &lowering = getTypeLowering(v->getType());
    return lowering.emitLoweredCopyValue(*this, Loc, v, expansionKind);
  }

  /// Convenience function for calling TypeLowering.emitDestroy on the type
  /// lowering for the non-address value.
  void emitDestroyValueOperation(SILLocation Loc, SILValue v) {
    assert(!v->getType().isAddress());
    if (F->hasOwnership() && v->getOwnershipKind() == OwnershipKind::None)
      return;
    auto &lowering = getTypeLowering(v->getType());
    lowering.emitDestroyValue(*this, Loc, v);
  }

  /// Convenience function for calling TypeLowering.emitDestroy on the type
  /// lowering for the non-address value.
  void emitLoweredDestroyValueOperation(
      SILLocation Loc, SILValue v,
      Lowering::TypeLowering::TypeExpansionKind expansionKind) {
    assert(!v->getType().isAddress());
    if (F->hasOwnership() && v->getOwnershipKind() == OwnershipKind::None)
      return;
    auto &lowering = getTypeLowering(v->getType());
    lowering.emitLoweredDestroyValue(*this, Loc, v, expansionKind);
  }

  /// Convenience function for destroying objects and addresses.
  ///
  /// Objects are destroyed using emitDestroyValueOperation and addresses by
  /// emitting destroy_addr.
  void emitDestroyOperation(SILLocation loc, SILValue v) {
    if (v->getType().isObject())
      return emitDestroyValueOperation(loc, v);
    createDestroyAddr(loc, v);
  }

  /// Convenience function that is a no-op for trivial values and inserts a
  /// move_value on non-trivial instructions.
  SILValue emitMoveValueOperation(SILLocation Loc, SILValue v) {
    assert(!v->getType().isAddress());
    if (v->getType().isTrivial(*getInsertionBB()->getParent()))
      return v;
    assert(v->getOwnershipKind() == OwnershipKind::Owned &&
           "move_value consumes its argument");
    return createMoveValue(Loc, v);
  }

  SILValue emitTupleExtract(SILLocation Loc, SILValue Operand, unsigned FieldNo,
                            SILType ResultTy) {
    // Fold tuple_extract(tuple(x,y,z),2)
    if (auto *TI = dyn_cast<TupleInst>(Operand))
      return TI->getOperand(FieldNo);

    return createTupleExtract(Loc, Operand, FieldNo, ResultTy);
  }

  SILValue emitTupleExtract(SILLocation Loc, SILValue Operand,
                            unsigned FieldNo) {
    return emitTupleExtract(Loc, Operand, FieldNo,
                            Operand->getType().getTupleElementType(FieldNo));
  }

  SILValue emitStructExtract(SILLocation Loc, SILValue Operand, VarDecl *Field,
                             SILType ResultTy) {
    if (auto *SI = dyn_cast<StructInst>(Operand))
      return SI->getFieldValue(Field);

    return createStructExtract(Loc, Operand, Field, ResultTy);
  }

  SILValue emitStructExtract(SILLocation Loc, SILValue Operand,
                             VarDecl *Field) {
    auto type = Operand->getType().getFieldType(Field, getModule(),
                                                getTypeExpansionContext());
    return emitStructExtract(Loc, Operand, Field, type);
  }

  SILValue emitThickToObjCMetatype(SILLocation Loc, SILValue Op, SILType Ty);
  SILValue emitObjCToThickMetatype(SILLocation Loc, SILValue Op, SILType Ty);

  //===--------------------------------------------------------------------===//
  // Differentiable programming instructions
  //===--------------------------------------------------------------------===//

  DifferentiableFunctionInst *createDifferentiableFunction(
      SILLocation Loc, IndexSubset *ParameterIndices,
      IndexSubset *ResultIndices, SILValue OriginalFunction,
      llvm::Optional<std::pair<SILValue, SILValue>> JVPAndVJPFunctions =
          llvm::None) {
    SILValue jvpAndVJPArray[2];
    if (JVPAndVJPFunctions.has_value()) {
      jvpAndVJPArray[0] = JVPAndVJPFunctions->first;
      jvpAndVJPArray[1] = JVPAndVJPFunctions->second;
    }

    return createDifferentiableFunction(
        Loc, ParameterIndices, ResultIndices, OriginalFunction,
        JVPAndVJPFunctions,
        hasOwnership()
            ? DifferentiableFunctionInst::getMergedOwnershipKind(
                  OriginalFunction, JVPAndVJPFunctions.has_value()
                                        ? ArrayRef<SILValue>(jvpAndVJPArray, 2)
                                        : ArrayRef<SILValue>())
            : ValueOwnershipKind(OwnershipKind::None));
  }

  DifferentiableFunctionInst *createDifferentiableFunction(
      SILLocation Loc, IndexSubset *ParameterIndices,
      IndexSubset *ResultIndices, SILValue OriginalFunction,
      llvm::Optional<std::pair<SILValue, SILValue>> JVPAndVJPFunctions,
      ValueOwnershipKind forwardingOwnershipKind) {

    return insert(DifferentiableFunctionInst::create(
        getModule(), getSILDebugLocation(Loc), ParameterIndices, ResultIndices,
        OriginalFunction, JVPAndVJPFunctions, forwardingOwnershipKind));
  }

  LinearFunctionInst *createLinearFunction(
      SILLocation Loc, IndexSubset *ParameterIndices, SILValue OriginalFunction,
      llvm::Optional<SILValue> TransposeFunction = llvm::None) {
    auto ownershipKind =
        hasOwnership()
            ? (TransposeFunction ? mergeSILValueOwnership(
                                       {OriginalFunction, *TransposeFunction})
                                 : mergeSILValueOwnership({OriginalFunction}))
            : ValueOwnershipKind(OwnershipKind::None);
    return createLinearFunction(Loc, ParameterIndices, OriginalFunction,
                                ownershipKind, TransposeFunction);
  }

  LinearFunctionInst *createLinearFunction(
      SILLocation Loc, IndexSubset *ParameterIndices, SILValue OriginalFunction,
      ValueOwnershipKind forwardingOwnershipKind,
      llvm::Optional<SILValue> TransposeFunction = llvm::None) {
    return insert(LinearFunctionInst::create(
        getModule(), getSILDebugLocation(Loc), ParameterIndices,
        OriginalFunction, TransposeFunction, forwardingOwnershipKind));
  }

  /// Note: explicit extractee type may be specified only in lowered SIL.
  DifferentiableFunctionExtractInst *createDifferentiableFunctionExtract(
      SILLocation Loc, NormalDifferentiableFunctionTypeComponent Extractee,
      SILValue Function, llvm::Optional<SILType> ExtracteeType = llvm::None) {
    return createDifferentiableFunctionExtract(
        Loc, Extractee, Function, OwnershipKind::Guaranteed, ExtracteeType);
  }

  DifferentiableFunctionExtractInst *createDifferentiableFunctionExtract(
      SILLocation Loc, NormalDifferentiableFunctionTypeComponent Extractee,
      SILValue Function, ValueOwnershipKind forwardingOwnershipKind,
      llvm::Optional<SILType> ExtracteeType = llvm::None) {
    return insert(new (getModule()) DifferentiableFunctionExtractInst(
        getModule(), getSILDebugLocation(Loc), Extractee, Function,
        forwardingOwnershipKind, ExtracteeType));
  }

  DifferentiableFunctionExtractInst *
  createDifferentiableFunctionExtractOriginal(SILLocation Loc,
                                              SILValue TheFunction) {
    return createDifferentiableFunctionExtract(
        Loc, NormalDifferentiableFunctionTypeComponent::Original, TheFunction);
  }

  LinearFunctionExtractInst *createLinearFunctionExtract(
      SILLocation Loc, LinearDifferentiableFunctionTypeComponent Extractee,
      SILValue Function) {
    return createLinearFunctionExtract(Loc, Extractee, Function,
                                       Function->getOwnershipKind());
  }

  LinearFunctionExtractInst *createLinearFunctionExtract(
      SILLocation Loc, LinearDifferentiableFunctionTypeComponent Extractee,
      SILValue Function, ValueOwnershipKind forwardingOwnershipKind) {
    return insert(new (getModule()) LinearFunctionExtractInst(
        getModule(), getSILDebugLocation(Loc), Extractee, Function,
        forwardingOwnershipKind));
  }

  /// Note: explicit function type may be specified only in lowered SIL.
  DifferentiabilityWitnessFunctionInst *createDifferentiabilityWitnessFunction(
      SILLocation Loc, DifferentiabilityWitnessFunctionKind WitnessKind,
      SILDifferentiabilityWitness *Witness,
      llvm::Optional<SILType> FunctionType = llvm::None) {
    return insert(new (getModule()) DifferentiabilityWitnessFunctionInst(
        getModule(), getSILDebugLocation(Loc), WitnessKind, Witness,
        FunctionType));
  }

  //===--------------------------------------------------------------------===//
  // Weak linking support
  //===--------------------------------------------------------------------===//
  HasSymbolInst *createHasSymbol(SILLocation Loc, ValueDecl *Decl) {
    return insert(new (getModule()) HasSymbolInst(
        getModule(), getSILDebugLocation(Loc), Decl));
  }

  //===--------------------------------------------------------------------===//
  // Private Helper Methods
  //===--------------------------------------------------------------------===//

private:
  /// insert - This is a template to avoid losing type info on the result.
  template <typename T> T *insert(T *TheInst) {
    insertImpl(TheInst);
    return TheInst;
  }

  /// insertTerminator - This is the same as insert, but clears the insertion
  /// point after doing the insertion.  This is used by terminators, since it
  /// isn't valid to insert something after a terminator.
  template <typename T> T *insertTerminator(T *TheInst) {
    insertImpl(TheInst);
    clearInsertionPoint();
    return TheInst;
  }

  void insertImpl(SILInstruction *TheInst) {
    assert(hasValidInsertionPoint());
    BB->insert(InsertPt, TheInst);
    getModule().notifyAddedInstruction(TheInst);
    C.notifyInserted(TheInst);

#ifndef NDEBUG
    // If we are inserting into a specific function (rather than a block for a
    // global_addr), verify that our instruction/the associated location are in
    // sync. We don't care if an instruction is used in global_addr.
    if (F)
      TheInst->verifyDebugInfo();
    TheInst->verifyOperandOwnership(&C.silConv);
#endif
  }

  bool isLoadableOrOpaque(SILType Ty) {
    auto &M = C.Module;

    if (!SILModuleConventions(M).useLoweredAddresses())
      return true;

    return getTypeLowering(Ty).isLoadable();
  }

  void appendOperandTypeName(SILType OpdTy, llvm::SmallString<16> &Name) {
    if (auto BuiltinIntTy =
            dyn_cast<BuiltinIntegerType>(OpdTy.getASTType())) {
      if (BuiltinIntTy == BuiltinIntegerType::getWordType(getASTContext())) {
        Name += "_Word";
      } else {
        unsigned NumBits = BuiltinIntTy->getWidth().getFixedWidth();
        Name += "_Int" + llvm::utostr(NumBits);
      }
    } else if (auto BuiltinFloatTy =
                   dyn_cast<BuiltinFloatType>(OpdTy.getASTType())) {
      Name += "_FP";
      switch (BuiltinFloatTy->getFPKind()) {
      case BuiltinFloatType::IEEE16: Name += "IEEE16"; break;
      case BuiltinFloatType::IEEE32: Name += "IEEE32"; break;
      case BuiltinFloatType::IEEE64: Name += "IEEE64"; break;
      case BuiltinFloatType::IEEE80: Name += "IEEE80"; break;
      case BuiltinFloatType::IEEE128: Name += "IEEE128"; break;
      case BuiltinFloatType::PPC128: Name += "PPC128"; break;
      }
    } else {
      assert(OpdTy.getASTType() == getASTContext().TheRawPointerType);
      Name += "_RawPointer";
    }
  }
};

/// A wrapper on top of SILBuilder's constructor that automatically sets the
/// current SILDebugScope based on the specified insertion point. This is useful
/// for situations where a single SIL instruction is lowered into a sequence of
/// SIL instructions.
class SILBuilderWithScope : public SILBuilder {
  void inheritScopeFrom(SILInstruction *I) {
    assert(I->getDebugScope() && "instruction has no debug scope");
    SILBasicBlock::iterator II(*I);
    auto End = I->getParent()->end();
    const SILDebugScope *DS = II->getDebugScope();
    assert(DS);
    // Skip over meta instructions, since debug_values may originate from outer
    // scopes. Don't do any of this after inlining.
    while (!DS->InlinedCallSite && II != End && II->isMetaInstruction())
      ++II;
    if (II != End) {
      auto nextScope = II->getDebugScope();
      if (!nextScope->InlinedCallSite)
        DS = nextScope;
    }
    assert(DS);
    setCurrentDebugScope(DS);
  }

public:
  /// Build instructions before the given insertion point, inheriting the debug
  /// location.
  ///
  /// Clients should prefer this constructor.
  SILBuilderWithScope(SILInstruction *I, SILBuilderContext &C)
      : SILBuilder(I, C) {
    inheritScopeFrom(I);
  }

  /// Build instructions before the given insertion point, inheriting the debug
  /// location and using the context from the passed in builder.
  ///
  /// Clients should prefer this constructor.
  SILBuilderWithScope(SILInstruction *I, SILBuilder &B)
      : SILBuilder(I, B.getBuilderContext()) {
    inheritScopeFrom(I);
  }

  explicit SILBuilderWithScope(
      SILInstruction *I,
      SmallVectorImpl<SILInstruction *> *InsertedInstrs = nullptr)
      : SILBuilder(I, InsertedInstrs) {
    inheritScopeFrom(I);
  }

  explicit SILBuilderWithScope(SILBasicBlock::iterator I)
      : SILBuilderWithScope(&*I) {}

  explicit SILBuilderWithScope(SILBasicBlock::iterator I, SILBuilder &B)
      : SILBuilder(&*I, B.getBuilderContext()) {
    inheritScopeFrom(&*I);
  }

  explicit SILBuilderWithScope(SILInstruction *I,
                               SILInstruction *InheritScopeFrom)
      : SILBuilderWithScope(I) {
    inheritScopeFrom(InheritScopeFrom);
  }

  explicit SILBuilderWithScope(SILBasicBlock::iterator I,
                               SILInstruction *InheritScopeFrom)
      : SILBuilderWithScope(&*I) {
    inheritScopeFrom(InheritScopeFrom);
  }

  explicit SILBuilderWithScope(SILBasicBlock *BB,
                               SILInstruction *InheritScopeFrom)
      : SILBuilder(BB) {
    inheritScopeFrom(InheritScopeFrom);
  }

  explicit SILBuilderWithScope(SILBasicBlock *BB, SILBuilder &B,
                               SILInstruction *InheritScopeFrom)
      : SILBuilder(BB, B.getBuilderContext()) {
    inheritScopeFrom(InheritScopeFrom);
  }

  explicit SILBuilderWithScope(SILBasicBlock *BB, SILBuilderContext &C,
                               const SILDebugScope *debugScope)
      : SILBuilder(BB, C, debugScope) {}

  /// Creates a new SILBuilder with an insertion point at the
  /// beginning of BB and the debug scope from the first
  /// non-metainstruction in the BB.
  explicit SILBuilderWithScope(SILBasicBlock *BB) : SILBuilder(BB->begin()) {
    const SILDebugScope *DS = BB->getScopeOfFirstNonMetaInstruction();
    assert(DS && "Instruction without debug scope associated!");
    setCurrentDebugScope(DS);
  }

  /// If \p inst is a terminator apply site, then pass a builder to insert at
  /// the first instruction of each successor to \p func. Otherwise, pass a
  /// builder to insert at std::next(inst).
  ///
  /// The intention is that this abstraction will enable the compiler writer to
  /// ignore whether or not \p inst is a terminator when inserting instructions
  /// after \p inst.
  ///
  /// Precondition: It's the responsibility of the caller to ensure that if
  /// \p inst is a terminator, all successor blocks have only a single
  /// predecessor block: the parent of \p inst.
  static void insertAfter(SILInstruction *inst,
                          function_ref<void(SILBuilder &)> func);

  /// If \p is an inst, then this is equivalent to insertAfter(inst). If a
  /// SILArgument is passed in, we use the first instruction in its parent
  /// block. We assert on undef.
  static void insertAfter(SILValue value,
                          function_ref<void(SILBuilder &)> func) {
    if (auto *i = dyn_cast<SingleValueInstruction>(value))
      return insertAfter(i, func);
    if (auto *mvir = dyn_cast<MultipleValueInstructionResult>(value))
      return insertAfter(mvir->getParent(), func);
    if (auto *arg = dyn_cast<SILArgument>(value))
      return insertAfter(&*arg->getParent()->begin(), func);
    assert(!isa<SILUndef>(value) && "This API can not use undef");
    llvm_unreachable("Unhandled case?!");
  }
};

class SavedInsertionPointRAII {
  SILBuilder &builder;
  PointerUnion<SILInstruction *, SILBasicBlock *> savedInsertionPoint;

public:
  /// Constructor that saves a Builder's insertion point without changing the
  /// builder's underlying insertion point.
  SavedInsertionPointRAII(SILBuilder &B) : builder(B), savedInsertionPoint() {
    // If our builder does not have a valid insertion point, just put nullptr
    // into SavedIP.
    if (!builder.hasValidInsertionPoint()) {
      savedInsertionPoint = static_cast<SILBasicBlock *>(nullptr);
      return;
    }

    // If we are inserting into the end of the block, stash the insertion block.
    if (builder.insertingAtEndOfBlock()) {
      savedInsertionPoint = builder.getInsertionBB();
      return;
    }

    // Otherwise, stash the instruction.
    SILInstruction *i = &*builder.getInsertionPoint();
    savedInsertionPoint = i;
  }

  SavedInsertionPointRAII(SILBuilder &b, SILInstruction *insertionPoint)
      : SavedInsertionPointRAII(b) {
    builder.setInsertionPoint(insertionPoint);
  }

  SavedInsertionPointRAII(SILBuilder &b, SILBasicBlock *block,
                          SILBasicBlock::iterator iter)
      : SavedInsertionPointRAII(b) {
    builder.setInsertionPoint(block, iter);
  }

  SavedInsertionPointRAII(SILBuilder &b, SILBasicBlock *insertionBlock)
      : SavedInsertionPointRAII(b) {
    builder.setInsertionPoint(insertionBlock);
  }

  SavedInsertionPointRAII(const SavedInsertionPointRAII &) = delete;
  SavedInsertionPointRAII &operator=(const SavedInsertionPointRAII &) = delete;
  SavedInsertionPointRAII(SavedInsertionPointRAII &&) = delete;
  SavedInsertionPointRAII &operator=(SavedInsertionPointRAII &&) = delete;

  ~SavedInsertionPointRAII() {
    if (savedInsertionPoint.isNull()) {
      builder.clearInsertionPoint();
    } else if (savedInsertionPoint.is<SILInstruction *>()) {
      builder.setInsertionPoint(savedInsertionPoint.get<SILInstruction *>());
    } else {
      builder.setInsertionPoint(savedInsertionPoint.get<SILBasicBlock *>());
    }
  }
};

/// Apply a debug location override for the duration of the current scope.
class DebugLocOverrideRAII {
  SILBuilder &Builder;
  llvm::Optional<SILLocation> oldOverride;
#ifndef NDEBUG
  llvm::Optional<SILLocation> installedOverride;
#endif

public:
  DebugLocOverrideRAII(SILBuilder &B, llvm::Optional<SILLocation> Loc)
      : Builder(B) {
    oldOverride = B.getCurrentDebugLocOverride();
    Builder.applyDebugLocOverride(Loc);
#ifndef NDEBUG
    installedOverride = Loc;
#endif
  }

  ~DebugLocOverrideRAII() {
    assert(Builder.getCurrentDebugLocOverride() == installedOverride &&
           "Restoring debug location override to an unexpected state");
    Builder.applyDebugLocOverride(oldOverride);
  }

  DebugLocOverrideRAII(const DebugLocOverrideRAII &) = delete;
  DebugLocOverrideRAII &operator=(const DebugLocOverrideRAII &) = delete;
  DebugLocOverrideRAII(DebugLocOverrideRAII &&) = delete;
  DebugLocOverrideRAII &operator=(DebugLocOverrideRAII &&) = delete;
};

} // end swift namespace

#endif
