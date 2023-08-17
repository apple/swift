//===--- SerializeSIL.cpp - Read and write SIL ----------------------------===//
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

#define DEBUG_TYPE "sil-serialize"

#include "SILFormat.h"
#include "Serialization.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Module.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/SIL/CFG.h"
#include "swift/SIL/PrettyStackTrace.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILUndef.h"
#include "swift/SIL/TerminatorUtils.h"
#include "swift/SILOptimizer/Utils/Generics.h"
#include "swift/Strings.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/DJB.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/OnDiskHashTable.h"

#include <type_traits>

using namespace swift;
using namespace swift::serialization;
using namespace swift::serialization::sil_block;
using namespace llvm::support;
using llvm::BCBlockRAII;

static unsigned toStableStringEncoding(StringLiteralInst::Encoding encoding) {
  switch (encoding) {
  case StringLiteralInst::Encoding::Bytes: return SIL_BYTES;
  case StringLiteralInst::Encoding::UTF8: return SIL_UTF8;
  case StringLiteralInst::Encoding::ObjCSelector: return SIL_OBJC_SELECTOR;
  }
  llvm_unreachable("bad string encoding");
}

static unsigned toStableSILLinkage(SILLinkage linkage) {
  switch (linkage) {
  case SILLinkage::Public: return SIL_LINKAGE_PUBLIC;
  case SILLinkage::PublicNonABI: return SIL_LINKAGE_PUBLIC_NON_ABI;
  case SILLinkage::Hidden: return SIL_LINKAGE_HIDDEN;
  case SILLinkage::Shared: return SIL_LINKAGE_SHARED;
  case SILLinkage::Private: return SIL_LINKAGE_PRIVATE;
  case SILLinkage::PublicExternal: return SIL_LINKAGE_PUBLIC_EXTERNAL;
  case SILLinkage::HiddenExternal: return SIL_LINKAGE_HIDDEN_EXTERNAL;
  }
  llvm_unreachable("bad linkage");
}

static unsigned toStableVTableEntryKind(SILVTable::Entry::Kind kind) {
  switch (kind) {
  case SILVTable::Entry::Kind::Normal: return SIL_VTABLE_ENTRY_NORMAL;
  case SILVTable::Entry::Kind::Inherited: return SIL_VTABLE_ENTRY_INHERITED;
  case SILVTable::Entry::Kind::Override: return SIL_VTABLE_ENTRY_OVERRIDE;
  }
  llvm_unreachable("bad vtable entry kind");
}

static unsigned toStableCastConsumptionKind(CastConsumptionKind kind) {
  switch (kind) {
  case CastConsumptionKind::TakeAlways:
    return SIL_CAST_CONSUMPTION_TAKE_ALWAYS;
  case CastConsumptionKind::TakeOnSuccess:
    return SIL_CAST_CONSUMPTION_TAKE_ON_SUCCESS;
  case CastConsumptionKind::CopyOnSuccess:
    return SIL_CAST_CONSUMPTION_COPY_ON_SUCCESS;
  case CastConsumptionKind::BorrowAlways:
    return SIL_CAST_CONSUMPTION_BORROW_ALWAYS;
  }
  llvm_unreachable("bad cast consumption kind");
}

static unsigned
toStableDifferentiabilityKind(swift::DifferentiabilityKind kind) {
  switch (kind) {
  case swift::DifferentiabilityKind::NonDifferentiable:
    return (unsigned)serialization::DifferentiabilityKind::NonDifferentiable;
  case swift::DifferentiabilityKind::Forward:
    return (unsigned)serialization::DifferentiabilityKind::Forward;
  case swift::DifferentiabilityKind::Reverse:
    return (unsigned)serialization::DifferentiabilityKind::Reverse;
  case swift::DifferentiabilityKind::Normal:
    return (unsigned)serialization::DifferentiabilityKind::Normal;
  case swift::DifferentiabilityKind::Linear:
    return (unsigned)serialization::DifferentiabilityKind::Linear;
  }
  llvm_unreachable("covered switch");
}

static unsigned encodeValueOwnership(ValueOwnershipKind ownership) {
  assert(ownership.value > 0 && "invalid value ownership");
  return ownership.value - 1;
}

namespace {
    /// Used to serialize the on-disk func hash table.
  class FuncTableInfo {
    Serializer &S;

  public:
    using key_type = StringRef;
    using key_type_ref = key_type;
    using data_type = DeclID;
    using data_type_ref = const data_type &;
    using hash_value_type = uint32_t;
    using offset_type = unsigned;

    explicit FuncTableInfo(Serializer &S) : S(S) {}

    hash_value_type ComputeHash(key_type_ref key) {
      assert(!key.empty());
      return llvm::djbHash(key, SWIFTMODULE_HASH_SEED);
    }

    std::pair<unsigned, unsigned> EmitKeyDataLength(raw_ostream &out,
                                                    key_type_ref key,
                                                    data_type_ref data) {
      return { sizeof(uint32_t), sizeof(uint32_t) };
    }

    void EmitKey(raw_ostream &out, key_type_ref key, unsigned len) {
      uint32_t keyID = S.addUniquedStringRef(key);
      endian::write<uint32_t>(out, keyID, little);
    }

    void EmitData(raw_ostream &out, key_type_ref key, data_type_ref data,
                  unsigned len) {
      endian::write<uint32_t>(out, data, little);
    }
  };

  class SILSerializer {
    using TypeID = serialization::TypeID;
    
    Serializer &S;

    llvm::BitstreamWriter &Out;

    /// A reusable buffer for emitting records.
    SmallVector<uint64_t, 64> ScratchRecord;

    /// In case we want to encode the relative of InstID vs ValueID.
    uint32_t /*ValueID*/ InstID = 0;

    llvm::DenseMap<const ValueBase*, ValueID> ValueIDs;
    ValueID addValueRef(const ValueBase *Val);

  public:
    using TableData = FuncTableInfo::data_type;
    using Table = llvm::MapVector<FuncTableInfo::key_type, TableData>;
  private:
    /// FuncTable maps function name to an ID.
    Table FuncTable;
    std::vector<BitOffset> Funcs;
    /// The current function ID.
    uint32_t /*DeclID*/ NextFuncID = 1;

    /// Maps class name to a VTable ID.
    Table VTableList;
    /// Holds the list of VTables.
    std::vector<BitOffset> VTableOffset;
    uint32_t /*DeclID*/ NextVTableID = 1;

    /// Maps nominal type name to a MoveOnlyDeinit ID.
    Table MoveOnlyDeinitList;
    /// Holds the list of MoveOnlyDeinits.
    std::vector<BitOffset> MoveOnlyDeinitOffset;
    uint32_t /*DeclID*/ NextMoveOnlyDeinitOffsetID = 1;

    /// Maps global variable name to an ID.
    Table GlobalVarList;
    /// Holds the list of SIL global variables.
    std::vector<BitOffset> GlobalVarOffset;
    uint32_t /*DeclID*/ NextGlobalVarID = 1;

    /// Maps witness table identifier to an ID.
    Table WitnessTableList;
    /// Holds the list of WitnessTables.
    std::vector<BitOffset> WitnessTableOffset;
    uint32_t /*DeclID*/ NextWitnessTableID = 1;

    /// Maps default witness table identifier to an ID.
    Table DefaultWitnessTableList;
    /// Holds the list of DefaultWitnessTables.
    std::vector<BitOffset> DefaultWitnessTableOffset;
    uint32_t /*DeclID*/ NextDefaultWitnessTableID = 1;
    
    /// Holds the list of Properties.
    std::vector<BitOffset> PropertyOffset;

    /// Maps differentiability witness identifier to an ID.
    Table DifferentiabilityWitnessList;
    /// Holds the list of SIL differentiability witnesses.
    std::vector<BitOffset> DifferentiabilityWitnessOffset;
    uint32_t /*DeclID*/ NextDifferentiabilityWitnessID = 1;

    /// Give each SILBasicBlock a unique ID.
    llvm::DenseMap<const SILBasicBlock *, unsigned> BasicBlockMap;

    /// Functions that we've emitted a reference to. If the key maps
    /// to true, we want to emit a declaration only.
    llvm::DenseMap<const SILFunction *, bool> FuncsToEmit;

    /// Global variables that we've emitted a reference to.
    llvm::DenseSet<const SILGlobalVariable *> GlobalsToEmit;

    /// Referenced differentiability witnesses that need to be emitted.
    llvm::DenseSet<const SILDifferentiabilityWitness *>
        DifferentiabilityWitnessesToEmit;

    /// Additional functions we might need to serialize.
    llvm::SmallVector<const SILFunction *, 16> functionWorklist;

    llvm::SmallVector<const SILGlobalVariable *, 16> globalWorklist;

    /// String storage for temporarily created strings which are referenced from
    /// the tables.
    llvm::BumpPtrAllocator StringTable;

    std::array<unsigned, 256> SILAbbrCodes;
    template <typename Layout>
    void registerSILAbbr() {
      using AbbrArrayTy = decltype(SILAbbrCodes);
      static_assert(Layout::Code <= std::tuple_size<AbbrArrayTy>::value,
                    "layout has invalid record code");
      SILAbbrCodes[Layout::Code] = Layout::emitAbbrev(Out);
      LLVM_DEBUG(llvm::dbgs() << "SIL abbre code " << SILAbbrCodes[Layout::Code]
                              << " for layout " << Layout::Code << "\n");
    }

    bool ShouldSerializeAll;

    void addMandatorySILFunction(const SILFunction *F,
                                 bool emitDeclarationsForOnoneSupport);
    void addReferencedSILFunction(const SILFunction *F,
                                  bool DeclOnly = false);
    void addReferencedGlobalVariable(const SILGlobalVariable *gl);

    void processWorklists();

    /// Helper function to update ListOfValues for MethodInst. Format:
    /// Attr, SILDeclRef (DeclID, Kind, uncurryLevel), and an operand.
    void handleMethodInst(const MethodInst *MI, SILValue operand,
                          SmallVectorImpl<uint64_t> &ListOfValues);

    void writeSILFunction(const SILFunction &F, bool DeclOnly = false);
    void writeSILBasicBlock(const SILBasicBlock &BB);
    void writeSILInstruction(const SILInstruction &SI);
    void writeSILVTable(const SILVTable &vt);
    void writeSILMoveOnlyDeinit(const SILMoveOnlyDeinit &deinit);
    void writeSILGlobalVar(const SILGlobalVariable &g);
    void writeSILWitnessTable(const SILWitnessTable &wt);
    void writeSILWitnessTableEntry(const SILWitnessTable::Entry &entry);
    void writeSILDefaultWitnessTable(const SILDefaultWitnessTable &wt);
    void
    writeSILDifferentiabilityWitness(const SILDifferentiabilityWitness &dw);
    void writeSILProperty(const SILProperty &prop);

    void writeSILBlock(const SILModule *SILMod);
    void writeIndexTables();

    void writeNoOperandLayout(const SILInstruction *I) {
      unsigned abbrCode = SILAbbrCodes[SILInstNoOperandLayout::Code];
      SILInstNoOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                         (unsigned)I->getKind());
    }

    void writeConversionLikeInstruction(const SingleValueInstruction *I,
                                        unsigned attrs);
    void writeOneTypeLayout(SILInstructionKind valueKind,
                            unsigned attrs, SILType type);
    void writeOneTypeLayout(SILInstructionKind valueKind,
                            unsigned attrs, CanType type);
    void writeOneTypeOneOperandLayout(SILInstructionKind valueKind,
                                      unsigned attrs,
                                      SILType type,
                                      SILValue operand);
    void writeOneTypeOneOperandLayout(SILInstructionKind valueKind,
                                      unsigned attrs,
                                      CanType type,
                                      SILValue operand);
    void writeOneTypeOneOperandExtraAttributeLayout(
      SILInstructionKind valueKind, unsigned attrs,
      SILType type, SILValue operand);
    void writeOneOperandLayout(SILInstructionKind valueKind,
                               unsigned attrs,
                               SILValue operand);
    void writeOneOperandExtraAttributeLayout(SILInstructionKind valueKind,
                                             unsigned attrs, SILValue operand);

    void writeKeyPathPatternComponent(
                    const KeyPathPatternComponent &component,
                    SmallVectorImpl<uint64_t> &ListOfValues);

    /// Helper function to determine if given the current state of the
    /// deserialization if the function body for F should be deserialized.
    bool shouldEmitFunctionBody(const SILFunction *F, bool isReference = true);

    IdentifierID addSILFunctionRef(SILFunction *F);

  public:
    SILSerializer(Serializer &S, llvm::BitstreamWriter &Out, bool serializeAll)
      : S(S), Out(Out), ShouldSerializeAll(serializeAll) {}

    void writeSILModule(const SILModule *SILMod);
  };
} // end anonymous namespace

void SILSerializer::addMandatorySILFunction(const SILFunction *F,
                                            bool emitDeclarationsForOnoneSupport) {
  // If this function is not fragile, don't do anything.
  if (!emitDeclarationsForOnoneSupport &&
      !shouldEmitFunctionBody(F, /* isReference */ false))
    return;

  auto iter = FuncsToEmit.find(F);
  if (iter != FuncsToEmit.end()) {
    // We've already visited this function. Make sure that we decided
    // to emit its body the first time around.
    assert(iter->second == emitDeclarationsForOnoneSupport
           && "Already emitting declaration");
    return;
  }

  // We haven't seen this function before. Record that we want to
  // emit its body, and add it to the worklist.
  FuncsToEmit[F] = emitDeclarationsForOnoneSupport;

  // Function body should be serialized unless it is a KeepAsPublic function
  // (which is typically a pre-specialization).
  if (!emitDeclarationsForOnoneSupport)
    functionWorklist.push_back(F);
}

void SILSerializer::addReferencedSILFunction(const SILFunction *F,
                                             bool DeclOnly) {
  assert(F != nullptr);

  if (FuncsToEmit.count(F) > 0)
    return;

  // We haven't seen this function before. Let's see if we should
  // serialize the body or just the declaration.
  if (shouldEmitFunctionBody(F)) {
    FuncsToEmit[F] = false;
    functionWorklist.push_back(F);
    return;
  }

  if (F->getLinkage() == SILLinkage::Shared) {
    assert(F->isSerialized() || F->hasForeignBody());

    FuncsToEmit[F] = false;
    functionWorklist.push_back(F);
    return;
  }

  // Ok, we just need to emit a declaration.
  FuncsToEmit[F] = true;
}

void SILSerializer::addReferencedGlobalVariable(const SILGlobalVariable *gl) {
  if (GlobalsToEmit.insert(gl).second)
    globalWorklist.push_back(gl);
}


void SILSerializer::processWorklists() {
  do {
    while (!functionWorklist.empty()) {
      const SILFunction *F = functionWorklist.pop_back_val();
      assert(F != nullptr);

      assert(FuncsToEmit.count(F) > 0);
      writeSILFunction(*F, FuncsToEmit[F]);
    }
    while (!globalWorklist.empty()) {
      const SILGlobalVariable *gl = globalWorklist.pop_back_val();
      assert(GlobalsToEmit.count(gl) > 0);
      writeSILGlobalVar(*gl);
    }
  } while (!functionWorklist.empty());
}

/// We enumerate all values in a SILFunction beforehand to correctly
/// handle forward references of values.
ValueID SILSerializer::addValueRef(const ValueBase *Val) {
  if (!Val)
    return 0;

  if (auto *Undef = dyn_cast<SILUndef>(Val)) {
    // The first two IDs are reserved for SILUndef.
    if (Undef->getOwnershipKind() == OwnershipKind::None)
      return 0;

    assert(Undef->getOwnershipKind() == OwnershipKind::Owned);
    return 1;
  }
  ValueID id = ValueIDs[Val];
  assert(id != 0 && "We should have assigned a value ID to each value.");
  return id;
}

void SILSerializer::writeSILFunction(const SILFunction &F, bool DeclOnly) {
  PrettyStackTraceSILFunction stackTrace("Serializing", &F);

  ValueIDs.clear();
  InstID = 0;

  FuncTable[F.getName()] = NextFuncID++;
  Funcs.push_back(Out.GetCurrentBitNo());
  unsigned abbrCode = SILAbbrCodes[SILFunctionLayout::Code];
  TypeID FnID = S.addTypeRef(F.getLoweredType().getRawASTType());
  LLVM_DEBUG(llvm::dbgs() << "SILFunction " << F.getName() << " @ BitNo "
                          << Out.GetCurrentBitNo() << " abbrCode " << abbrCode
                          << " FnID " << FnID << "\n");
  LLVM_DEBUG(llvm::dbgs() << "Serialized SIL:\n"; F.dump());

  SmallVector<IdentifierID, 1> SemanticsIDs;
  for (auto SemanticAttr : F.getSemanticsAttrs()) {
    SemanticsIDs.push_back(S.addUniquedStringRef(SemanticAttr));
  }

  SILLinkage Linkage = F.getLinkage();

  // Check if we need to emit a body for this function.
  bool NoBody = DeclOnly || isAvailableExternally(Linkage) ||
                F.isExternalDeclaration();

  // If we don't emit a function body then make sure to mark the declaration
  // as available externally.
  if (NoBody) {
    Linkage = addExternalToLinkage(Linkage);
  }

  // If we have a body, we might have a generic environment.
  GenericSignatureID genericSigID = 0;
  if (!NoBody)
    if (auto *genericEnv = F.getGenericEnvironment())
      genericSigID = S.addGenericSignatureRef(genericEnv->getGenericSignature());

  DeclID clangNodeOwnerID;
  if (F.hasClangNode())
    clangNodeOwnerID = S.addDeclRef(F.getClangNodeOwner());

  ModuleID parentModuleID;
  if (auto *parentModule = F.getParentModule())
    parentModuleID = S.addModuleRef(parentModule);

  IdentifierID replacedFunctionID = 0;
  if (auto *fun = F.getDynamicallyReplacedFunction()) {
    addReferencedSILFunction(fun, true);
    replacedFunctionID = S.addUniquedStringRef(fun->getName());
  } else if (F.hasObjCReplacement()) {
    replacedFunctionID =
        S.addUniquedStringRef(F.getObjCReplacement().str());
  }

  IdentifierID usedAdHocWitnessFunctionID = 0;
  if (auto *fun = F.getReferencedAdHocRequirementWitnessFunction()) {
    addReferencedSILFunction(fun, true);
    usedAdHocWitnessFunctionID = S.addUniquedStringRef(fun->getName());
  }

  unsigned numAttrs = NoBody ? 0 : F.getSpecializeAttrs().size();

  auto resilience = F.getModule().getSwiftModule()->getResilienceStrategy();
  bool serializeDerivedEffects = (resilience != ResilienceStrategy::Resilient) &&
                                 !F.hasSemanticsAttr("optimize.no.crossmodule");

  F.visitArgEffects(
    [&](int effectIdx, int argumentIndex, bool isDerived) {
      if (isDerived && !serializeDerivedEffects)
        return;
      numAttrs++;
    });

  llvm::Optional<llvm::VersionTuple> available;
  auto availability = F.getAvailabilityForLinkage();
  if (!availability.isAlwaysAvailable()) {
    available = availability.getOSVersion().getLowerEndpoint();
  }
  ENCODE_VER_TUPLE(available, available)

  SILFunctionLayout::emitRecord(
      Out, ScratchRecord, abbrCode, toStableSILLinkage(Linkage),
      (unsigned)F.isTransparent(), (unsigned)F.isSerialized(),
      (unsigned)F.isThunk(), (unsigned)F.isWithoutActuallyEscapingThunk(),
      (unsigned)F.getSpecialPurpose(), (unsigned)F.getInlineStrategy(),
      (unsigned)F.getOptimizationMode(), (unsigned)F.getPerfConstraints(),
      (unsigned)F.getClassSubclassScope(), (unsigned)F.hasCReferences(),
      (unsigned)F.getEffectsKind(), (unsigned)numAttrs,
      (unsigned)F.hasOwnership(), F.isAlwaysWeakImported(),
      LIST_VER_TUPLE_PIECES(available), (unsigned)F.isDynamicallyReplaceable(),
      (unsigned)F.isExactSelfClass(), (unsigned)F.isDistributed(),
      (unsigned)F.isRuntimeAccessible(),
      (unsigned)F.forceEnableLexicalLifetimes(), FnID, replacedFunctionID,
      usedAdHocWitnessFunctionID, genericSigID, clangNodeOwnerID,
      parentModuleID, SemanticsIDs);

  F.visitArgEffects(
    [&](int effectIdx, int argumentIndex, bool isDerived) {
      if (isDerived && !serializeDerivedEffects)
        return;

      llvm::SmallString<64> buffer;
      llvm::raw_svector_ostream OS(buffer);
      F.writeEffect(OS, effectIdx);

      IdentifierID effectsStrID = S.addUniquedStringRef(OS.str());
      unsigned abbrCode = SILAbbrCodes[SILArgEffectsAttrLayout::Code];
      bool isGlobalSideEffects = (argumentIndex < 0);
      unsigned argIdx = (isGlobalSideEffects ? 0 : (unsigned)argumentIndex);

      SILArgEffectsAttrLayout::emitRecord(
          Out, ScratchRecord, abbrCode, effectsStrID,
          argIdx, (unsigned)isGlobalSideEffects, (unsigned)isDerived);
    });

  if (NoBody)
    return;

  for (auto *SA : F.getSpecializeAttrs()) {
    unsigned specAttrAbbrCode = SILAbbrCodes[SILSpecializeAttrLayout::Code];
    IdentifierID targetFunctionNameID = 0;
    if (auto *target = SA->getTargetFunction()) {
      addReferencedSILFunction(target, true);
      targetFunctionNameID = S.addUniquedStringRef(target->getName());
    }
    IdentifierID spiGroupID = 0;
    IdentifierID spiModuleDeclID = 0;
    auto ident = SA->getSPIGroup();
    if (!ident.empty()) {
      spiGroupID = S.addUniquedStringRef(ident.str());
      spiModuleDeclID = S.addModuleRef(SA->getSPIModule());
    }
    auto availability = SA->getAvailability();
    if (!availability.isAlwaysAvailable()) {
      available = availability.getOSVersion().getLowerEndpoint();
    }
    ENCODE_VER_TUPLE(available, available)

    llvm::SmallVector<IdentifierID, 4> typeErasedParamsIDs;
    for (auto ty : SA->getTypeErasedParams()) {
      typeErasedParamsIDs.push_back(S.addTypeRef(ty));
    }

    SILSpecializeAttrLayout::emitRecord(
        Out, ScratchRecord, specAttrAbbrCode, (unsigned)SA->isExported(),
        (unsigned)SA->getSpecializationKind(),
        S.addGenericSignatureRef(SA->getSpecializedSignature()),
        targetFunctionNameID, spiGroupID, spiModuleDeclID,
        LIST_VER_TUPLE_PIECES(available), typeErasedParamsIDs
        );
  }

  // Assign a unique ID to each basic block of the SILFunction.
  unsigned BasicID = 0;
  BasicBlockMap.clear();
  // Assign a value ID to each SILInstruction that has value and to each basic
  // block argument.
  //
  // FIXME: Add reverse iteration to SILSuccessor and convert this to a "stable"
  // RPO order. Currently, the serializer inverts the order of successors each
  // time they are processed.
  //
  // The first valid value ID is 2. 0 and 1 are reserved for SILUndef.
  unsigned ValueID = 2;
  llvm::ReversePostOrderTraversal<SILFunction *> RPOT(
      const_cast<SILFunction *>(&F));
  for (auto Iter = RPOT.begin(), E = RPOT.end(); Iter != E; ++Iter) {
    auto &BB = **Iter;
    BasicBlockMap.insert(std::make_pair(&BB, BasicID++));

    for (auto I = BB.args_begin(), E = BB.args_end(); I != E; ++I)
      ValueIDs[static_cast<const ValueBase*>(*I)] = ValueID++;

    for (const SILInstruction &SI : BB)
      for (auto result : SI.getResults())
        ValueIDs[result] = ValueID++;
  }

  // Write SIL basic blocks in the RPOT order
  // to make sure that instructions defining open archetypes
  // are serialized before instructions using those opened
  // archetypes.
  unsigned SerializedBBNum = 0;
  for (auto Iter = RPOT.begin(), E = RPOT.end(); Iter != E; ++Iter) {
    auto *BB = *Iter;
    writeSILBasicBlock(*BB);
    SerializedBBNum++;
  }
  assert(BasicID == SerializedBBNum && "Wrong number of BBs was serialized");
}

void SILSerializer::writeSILBasicBlock(const SILBasicBlock &BB) {
  SmallVector<DeclID, 4> Args;
  for (auto I = BB.args_begin(), E = BB.args_end(); I != E; ++I) {
    SILArgument *SA = *I;
    DeclID tId = S.addTypeRef(SA->getType().getRawASTType());
    DeclID vId = addValueRef(static_cast<const ValueBase*>(SA));
    Args.push_back(tId);

    // We put these static asserts here to formalize our assumption that both
    // SILValueCategory and ValueOwnershipKind have uint8_t as their underlying
    // pointer values.
    static_assert(
        std::is_same<
            std::underlying_type<decltype(SA->getType().getCategory())>::type,
            uint8_t>::value,
        "Expected an underlying uint8_t type");
    // We put these static asserts here to formalize our assumption that both
    // SILValueCategory and ValueOwnershipKind have uint8_t as their underlying
    // pointer values.
    static_assert(std::is_same<std::underlying_type<decltype(
                                   SA->getOwnershipKind())::innerty>::type,
                               uint8_t>::value,
                  "Expected an underlying uint8_t type");
    // This is 31 bits in size.
    unsigned packedMetadata = 0;
    packedMetadata |= unsigned(SA->getType().getCategory()); // 8 bits
    packedMetadata |= unsigned(SA->getOwnershipKind()) << 8; // 3 bits
    packedMetadata |= unsigned(SA->isReborrow()) << 11;      // 1 bit
    packedMetadata |= unsigned(SA->hasPointerEscape()) << 12; // 1 bit
    if (auto *SFA = dyn_cast<SILFunctionArgument>(SA)) {
      packedMetadata |= unsigned(SFA->isNoImplicitCopy()) << 13;      // 1 bit
      packedMetadata |= unsigned(SFA->getLifetimeAnnotation()) << 14; // 2 bits
      packedMetadata |= unsigned(SFA->isClosureCapture()) << 16;      // 1 bit
      packedMetadata |= unsigned(SFA->isFormalParameterPack()) << 17; // 1 bit
    }
    // Used: 17 bits. Free: 15.
    //
    // TODO: We should be able to shrink the packed metadata of the first two.
    Args.push_back(packedMetadata);

    Args.push_back(vId);
  }

  unsigned abbrCode = SILAbbrCodes[SILBasicBlockLayout::Code];
  SILBasicBlockLayout::emitRecord(Out, ScratchRecord, abbrCode, Args);

  for (const SILInstruction &SI : BB)
    writeSILInstruction(SI);
}

/// Add SILDeclRef to ListOfValues, so we can reconstruct it at
/// deserialization.
static void handleSILDeclRef(Serializer &S, const SILDeclRef &Ref,
                             SmallVectorImpl<uint64_t> &ListOfValues) {
  ListOfValues.push_back(S.addDeclRef(Ref.getDecl()));
  ListOfValues.push_back((unsigned)Ref.kind);
  ListOfValues.push_back(Ref.isForeign);
}

/// Get an identifier ref for a SILFunction and add it to the list of referenced
/// functions.
IdentifierID SILSerializer::addSILFunctionRef(SILFunction *F) {
  addReferencedSILFunction(F);
  return S.addUniquedStringRef(F->getName());
}

/// Helper function to update ListOfValues for MethodInst. Format:
/// Attr, SILDeclRef (DeclID, Kind, uncurryLevel), and an operand.
void SILSerializer::handleMethodInst(const MethodInst *MI,
                                     SILValue operand,
                                     SmallVectorImpl<uint64_t> &ListOfValues) {
  handleSILDeclRef(S, MI->getMember(), ListOfValues);
  ListOfValues.push_back(
      S.addTypeRef(operand->getType().getRawASTType()));
  ListOfValues.push_back((unsigned)operand->getType().getCategory());
  ListOfValues.push_back(addValueRef(operand));
}

void SILSerializer::writeOneTypeLayout(SILInstructionKind valueKind,
                                       unsigned attrs, SILType type) {
  unsigned abbrCode = SILAbbrCodes[SILOneTypeLayout::Code];
  SILOneTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
        (unsigned) valueKind, attrs,
        S.addTypeRef(type.getRawASTType()),
        (unsigned)type.getCategory());
}

void SILSerializer::writeOneTypeLayout(SILInstructionKind valueKind,
                                       unsigned attrs, CanType type) {
  unsigned abbrCode = SILAbbrCodes[SILOneTypeLayout::Code];
  SILOneTypeLayout::emitRecord(Out, ScratchRecord, abbrCode,
        (unsigned) valueKind, attrs,
        S.addTypeRef(type), 0);
}

void SILSerializer::writeOneOperandLayout(SILInstructionKind valueKind,
                                          unsigned attrs,
                                          SILValue operand) {

  auto operandType = operand->getType();
  auto operandTypeRef = S.addTypeRef(operandType.getRawASTType());
  auto operandRef = addValueRef(operand);

  SILOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneOperandLayout::Code],
        unsigned(valueKind), attrs,
        operandTypeRef, unsigned(operandType.getCategory()),
        operandRef);
}

void SILSerializer::
writeOneOperandExtraAttributeLayout(SILInstructionKind valueKind,
                                    unsigned attrs,
                                    SILValue operand) {

  auto operandType = operand->getType();
  auto operandTypeRef = S.addTypeRef(operandType.getRawASTType());
  auto operandRef = addValueRef(operand);

  SILOneOperandExtraAttributeLayout::emitRecord(
      Out, ScratchRecord, SILAbbrCodes[SILOneOperandExtraAttributeLayout::Code],
      unsigned(valueKind), attrs, operandTypeRef,
      unsigned(operandType.getCategory()), operandRef);
}

void SILSerializer::writeOneTypeOneOperandLayout(SILInstructionKind valueKind,
                                                 unsigned attrs,
                                                 SILType type,
                                                 SILValue operand) {
  auto typeRef = S.addTypeRef(type.getRawASTType());
  auto operandType = operand->getType();
  auto operandTypeRef = S.addTypeRef(operandType.getRawASTType());
  auto operandRef = addValueRef(operand);

  SILOneTypeOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeOneOperandLayout::Code],
        unsigned(valueKind), attrs,
        typeRef, unsigned(type.getCategory()),
        operandTypeRef, unsigned(operandType.getCategory()),
        operandRef);
}

void SILSerializer::writeOneTypeOneOperandLayout(SILInstructionKind valueKind,
                                                 unsigned attrs,
                                                 CanType type,
                                                 SILValue operand) {
  auto typeRef = S.addTypeRef(type);
  auto operandType = operand->getType();
  auto operandTypeRef = S.addTypeRef(operandType.getRawASTType());
  auto operandRef = addValueRef(operand);

  SILOneTypeOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeOneOperandLayout::Code],
        unsigned(valueKind), attrs,
        typeRef, 0,
        operandTypeRef, unsigned(operandType.getCategory()),
        operandRef);
}

void SILSerializer::
writeOneTypeOneOperandExtraAttributeLayout(SILInstructionKind valueKind,
                                           unsigned attrs,
                                           SILType type,
                                           SILValue operand) {
  auto typeRef = S.addTypeRef(type.getRawASTType());
  auto operandType = operand->getType();
  auto operandTypeRef = S.addTypeRef(operandType.getRawASTType());
  auto operandRef = addValueRef(operand);

  SILOneTypeOneOperandExtraAttributeLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeOneOperandExtraAttributeLayout::Code],
        unsigned(valueKind), attrs,
        typeRef, unsigned(type.getCategory()),
        operandTypeRef, unsigned(operandType.getCategory()),
        operandRef);
}

/// Write an instruction that looks exactly like a conversion: all
/// important information is encoded in the operand and the result type.
void SILSerializer::writeConversionLikeInstruction(
    const SingleValueInstruction *I, unsigned attrs) {
  assert(I->getNumOperands() - I->getTypeDependentOperands().size() == 1);
  writeOneTypeOneOperandLayout(I->getKind(), attrs, I->getType(),
                               I->getOperand(0));
}

void
SILSerializer::writeKeyPathPatternComponent(
                   const KeyPathPatternComponent &component,
                   SmallVectorImpl<uint64_t> &ListOfValues) {
  
  auto handleComponentCommon = [&](KeyPathComponentKindEncoding kind) {
    ListOfValues.push_back((unsigned)kind);
    ListOfValues.push_back(S.addTypeRef(component.getComponentType()));
  };
  auto handleComputedId = [&](KeyPathPatternComponent::ComputedPropertyId id) {
    switch (id.getKind()) {
    case KeyPathPatternComponent::ComputedPropertyId::Property:
      ListOfValues.push_back(
        (unsigned)KeyPathComputedComponentIdKindEncoding::Property);
      ListOfValues.push_back(S.addDeclRef(id.getProperty()));
      break;
    case KeyPathPatternComponent::ComputedPropertyId::Function:
      ListOfValues.push_back(
        (unsigned)KeyPathComputedComponentIdKindEncoding::Function);
      ListOfValues.push_back(addSILFunctionRef(id.getFunction()));
      break;
    case KeyPathPatternComponent::ComputedPropertyId::DeclRef:
      ListOfValues.push_back(
        (unsigned)KeyPathComputedComponentIdKindEncoding::DeclRef);
      handleSILDeclRef(S, id.getDeclRef(), ListOfValues);
      break;
    }
  };
  auto handleComputedExternalReferenceAndIndices
    = [&](const KeyPathPatternComponent &component) {
      ListOfValues.push_back(S.addDeclRef(component.getExternalDecl()));
      ListOfValues.push_back(
        S.addSubstitutionMapRef(component.getExternalSubstitutions()));
  
      auto indices = component.getSubscriptIndices();
      ListOfValues.push_back(indices.size());
      for (auto &index : indices) {
        ListOfValues.push_back(index.Operand);
        ListOfValues.push_back(S.addTypeRef(index.FormalType));
        ListOfValues.push_back(
          S.addTypeRef(index.LoweredType.getRawASTType()));
        ListOfValues.push_back((unsigned)index.LoweredType.getCategory());
        ListOfValues.push_back(S.addConformanceRef(index.Hashable));
      }
      if (!indices.empty()) {
        ListOfValues.push_back(
          addSILFunctionRef(component.getSubscriptIndexEquals()));
        ListOfValues.push_back(
          addSILFunctionRef(component.getSubscriptIndexHash()));
      }
    };

  switch (component.getKind()) {
  case KeyPathPatternComponent::Kind::StoredProperty:
    handleComponentCommon(KeyPathComponentKindEncoding::StoredProperty);
    ListOfValues.push_back(S.addDeclRef(component.getStoredPropertyDecl()));
    break;
  case KeyPathPatternComponent::Kind::GettableProperty:
    handleComponentCommon(KeyPathComponentKindEncoding::GettableProperty);
    handleComputedId(component.getComputedPropertyId());
    ListOfValues.push_back(
                  addSILFunctionRef(component.getComputedPropertyGetter()));
    handleComputedExternalReferenceAndIndices(component);
    break;
  case KeyPathPatternComponent::Kind::SettableProperty:
    handleComponentCommon(KeyPathComponentKindEncoding::SettableProperty);
    handleComputedId(component.getComputedPropertyId());
    ListOfValues.push_back(
                  addSILFunctionRef(component.getComputedPropertyGetter()));
    ListOfValues.push_back(
                  addSILFunctionRef(component.getComputedPropertySetter()));
    handleComputedExternalReferenceAndIndices(component);
    break;
  case KeyPathPatternComponent::Kind::OptionalChain:
    handleComponentCommon(KeyPathComponentKindEncoding::OptionalChain);
    break;
  case KeyPathPatternComponent::Kind::OptionalForce:
    handleComponentCommon(KeyPathComponentKindEncoding::OptionalForce);
    break;
  case KeyPathPatternComponent::Kind::OptionalWrap:
    handleComponentCommon(KeyPathComponentKindEncoding::OptionalWrap);
    break;
  case KeyPathPatternComponent::Kind::TupleElement:
    handleComponentCommon(KeyPathComponentKindEncoding::TupleElement);
    ListOfValues.push_back((unsigned)component.getTupleIndex());
    break;
  }
}

void SILSerializer::writeSILInstruction(const SILInstruction &SI) {
  PrettyStackTraceSILNode stackTrace("Serializing", &SI);

  switch (SI.getKind()) {
  case SILInstructionKind::ObjectInst: {
    const ObjectInst *OI = cast<ObjectInst>(&SI);
    unsigned abbrCode = SILAbbrCodes[SILOneTypeValuesLayout::Code];
    SmallVector<ValueID, 4> Args;
    Args.push_back((unsigned)OI->getBaseElements().size());
    for (const Operand &op : OI->getAllOperands()) {
      SILValue OpVal = op.get();
      Args.push_back(addValueRef(OpVal));
      SILType OpType = OpVal->getType();
      assert(OpType.isObject());
      Args.push_back(S.addTypeRef(OpType.getRawASTType()));
    }
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                       (unsigned)SI.getKind(),
                                       S.addTypeRef(
                                         OI->getType().getRawASTType()),
                                       (unsigned)OI->getType().getCategory(),
                                       Args);
    break;
  }

  case SILInstructionKind::DebugValueInst:
  case SILInstructionKind::DebugStepInst:
    // Currently we don't serialize debug info, so it doesn't make
    // sense to write those instructions at all.
    // TODO: decide if we want to serialize those instructions.
    return;
  case SILInstructionKind::TestSpecificationInst:
    // Instruction exists only for tests.  Ignore it.
    return;
  case SILInstructionKind::AllocPackMetadataInst:
  case SILInstructionKind::DeallocPackMetadataInst:
    // Shoulud never be serialized: only introduced in an IRGen pass
    // (PackMetadataMarkerInserter).
    return;

  case SILInstructionKind::UnwindInst:
  case SILInstructionKind::UnreachableInst: {
    writeNoOperandLayout(&SI);
    break;
  }
  case SILInstructionKind::AllocExistentialBoxInst:
  case SILInstructionKind::InitExistentialAddrInst:
  case SILInstructionKind::InitExistentialValueInst:
  case SILInstructionKind::InitExistentialMetatypeInst:
  case SILInstructionKind::InitExistentialRefInst: {
    SILValue operand;
    SILType Ty;
    CanType FormalConcreteType;
    ArrayRef<ProtocolConformanceRef> conformances;

    switch (SI.getKind()) {
    default: llvm_unreachable("out of sync with parent");
    case SILInstructionKind::InitExistentialAddrInst: {
      auto &IEI = cast<InitExistentialAddrInst>(SI);
      operand = IEI.getOperand();
      Ty = IEI.getLoweredConcreteType();
      FormalConcreteType = IEI.getFormalConcreteType();
      conformances = IEI.getConformances();
      break;
    }
    case SILInstructionKind::InitExistentialValueInst: {
      auto &IEOI = cast<InitExistentialValueInst>(SI);
      operand = IEOI.getOperand();
      Ty = IEOI.getType();
      FormalConcreteType = IEOI.getFormalConcreteType();
      conformances = IEOI.getConformances();
      break;
    }
    case SILInstructionKind::InitExistentialRefInst: {
      auto &IERI = cast<InitExistentialRefInst>(SI);
      operand = IERI.getOperand();
      Ty = IERI.getType();
      FormalConcreteType = IERI.getFormalConcreteType();
      conformances = IERI.getConformances();
      break;
    }
    case SILInstructionKind::InitExistentialMetatypeInst: {
      auto &IEMI = cast<InitExistentialMetatypeInst>(SI);
      operand = IEMI.getOperand();
      Ty = IEMI.getType();
      conformances = IEMI.getConformances();
      break;
    }
    case SILInstructionKind::AllocExistentialBoxInst: {
      auto &AEBI = cast<AllocExistentialBoxInst>(SI);
      Ty = AEBI.getExistentialType();
      FormalConcreteType = AEBI.getFormalConcreteType();
      conformances = AEBI.getConformances();
      break;
    }
    }

    TypeID operandType = 0;
    SILValueCategory operandCategory = SILValueCategory::Object;
    ValueID operandID = 0;
    if (operand) {
      operandType = S.addTypeRef(operand->getType().getRawASTType());
      operandCategory = operand->getType().getCategory();
      operandID = addValueRef(operand);
    }

    TypeID formalConcreteTypeID = S.addTypeRef(FormalConcreteType);
    auto conformanceIDs = S.addConformanceRefs(conformances);

    unsigned abbrCode = SILAbbrCodes[SILInitExistentialLayout::Code];
    SILInitExistentialLayout::emitRecord(Out, ScratchRecord, abbrCode,
       (unsigned)SI.getKind(),
       S.addTypeRef(Ty.getRawASTType()),
       (unsigned)Ty.getCategory(),
       operandType,
       (unsigned)operandCategory,
       operandID,
       formalConcreteTypeID,
       conformanceIDs);
    break;
  }
  case SILInstructionKind::DeallocBoxInst: {
    auto DBI = cast<DeallocBoxInst>(&SI);
    writeOneTypeOneOperandLayout(DBI->getKind(), 0,
                                 DBI->getOperand()->getType(),
                                 DBI->getOperand());
    break;
  }
  case SILInstructionKind::DeallocExistentialBoxInst: {
    auto DBI = cast<DeallocExistentialBoxInst>(&SI);
    writeOneTypeOneOperandLayout(DBI->getKind(), 0,
                                 DBI->getConcreteType(),
                                 DBI->getOperand());
    break;
  }
  case SILInstructionKind::ValueMetatypeInst: {
    auto VMI = cast<ValueMetatypeInst>(&SI);
    writeOneTypeOneOperandLayout(VMI->getKind(), 0,
                                 VMI->getType(),
                                 VMI->getOperand());
    break;
  }
  case SILInstructionKind::ExistentialMetatypeInst: {
    auto EMI = cast<ExistentialMetatypeInst>(&SI);
    writeOneTypeOneOperandLayout(EMI->getKind(), 0,
                                 EMI->getType(),
                                 EMI->getOperand());
    break;
  }
  case SILInstructionKind::AllocBoxInst: {
    const AllocBoxInst *ABI = cast<AllocBoxInst>(&SI);
    unsigned flags = 0;
    flags |= unsigned(ABI->hasDynamicLifetime());
    flags |= unsigned(ABI->emitReflectionMetadata()) << 1;
    flags |= unsigned(ABI->getUsesMoveableValueDebugInfo()) << 2;
    flags |= unsigned(ABI->hasPointerEscape()) << 3;
    writeOneTypeLayout(ABI->getKind(),
                       flags,
                       ABI->getType());
    break;
  }
  case SILInstructionKind::AllocRefInst:
  case SILInstructionKind::AllocRefDynamicInst: {
    const AllocRefInstBase *ARI = cast<AllocRefInstBase>(&SI);
    unsigned abbrCode = SILAbbrCodes[SILOneTypeValuesLayout::Code];
    SmallVector<ValueID, 4> Args;
    bool isBare = false;
    if (auto *ar = dyn_cast<AllocRefInst>(&SI))
      isBare = ar->isBare();
    Args.push_back((unsigned)ARI->isObjC() |
                   ((unsigned)ARI->canAllocOnStack() << 1) |
                   ((unsigned)isBare << 2));
    ArrayRef<SILType> TailTypes = ARI->getTailAllocatedTypes();
    ArrayRef<Operand> AllOps = ARI->getAllOperands();
    unsigned NumTailAllocs = TailTypes.size();
    unsigned NumOpsToWrite = NumTailAllocs;
    if (SI.getKind() == SILInstructionKind::AllocRefDynamicInst)
      ++NumOpsToWrite;
    for (unsigned Idx = 0; Idx < NumOpsToWrite; ++Idx) {
      if (Idx < NumTailAllocs) {
        assert(TailTypes[Idx].isObject());
        Args.push_back(S.addTypeRef(TailTypes[Idx].getRawASTType()));
      }
      SILValue OpVal = AllOps[Idx].get();
      Args.push_back(addValueRef(OpVal));
      SILType OpType = OpVal->getType();
      assert(OpType.isObject());
      Args.push_back(S.addTypeRef(OpType.getRawASTType()));
    }
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                       (unsigned)SI.getKind(),
                                       S.addTypeRef(
                                         ARI->getType().getRawASTType()),
                                       (unsigned)ARI->getType().getCategory(),
                                       Args);
    break;
  }
  case SILInstructionKind::AllocStackInst: {
    const AllocStackInst *ASI = cast<AllocStackInst>(&SI);
    unsigned attr = 0;
    attr |= unsigned(ASI->hasDynamicLifetime());
    attr |= unsigned(ASI->isLexical()) << 1;
    attr |= unsigned(ASI->getUsesMoveableValueDebugInfo()) << 2;
    writeOneTypeLayout(ASI->getKind(), attr, ASI->getElementType());
    break;
  }
  case SILInstructionKind::AllocPackInst: {
    const AllocPackInst *API = cast<AllocPackInst>(&SI);
    writeOneTypeLayout(API->getKind(), 0, API->getPackType());
    break;
  }
  case SILInstructionKind::PackLengthInst: {
    const PackLengthInst *PLI = cast<PackLengthInst>(&SI);
    writeOneTypeLayout(PLI->getKind(), 0, PLI->getPackType());
    break;
  }
  case SILInstructionKind::ProjectBoxInst: {
    auto PBI = cast<ProjectBoxInst>(&SI);
    
    // Use SILOneTypeOneOperandLayout with the field index crammed in the TypeID
    auto boxOperand = PBI->getOperand();
    auto boxRef = addValueRef(boxOperand);
    auto boxType = boxOperand->getType();
    auto boxTypeRef = S.addTypeRef(boxType.getRawASTType());
    
    SILOneTypeOneOperandLayout::emitRecord(Out, ScratchRecord,
          SILAbbrCodes[SILOneTypeOneOperandLayout::Code],
          unsigned(PBI->getKind()), 0,
          PBI->getFieldIndex(), 0,
          boxTypeRef, unsigned(boxType.getCategory()),
          boxRef);
    break;
  }
  case SILInstructionKind::ProjectExistentialBoxInst: {
    auto PEBI = cast<ProjectExistentialBoxInst>(&SI);
    writeOneTypeOneOperandLayout(PEBI->getKind(), 0,
                                 PEBI->getType(),
                                 PEBI->getOperand());
    break;
  }
  case SILInstructionKind::BuiltinInst: {
    // Format: substitutions map ID, the builtin name, result type, and
    // a list of values for the arguments. Each value in the list
    // is represented with 4 IDs:
    //   ValueID, ValueResultNumber, TypeID, TypeCategory.
    // The record is followed by the substitution list.
    const BuiltinInst *BI = cast<BuiltinInst>(&SI);
    SmallVector<ValueID, 4> Args;
    for (auto Arg : BI->getArguments()) {
      Args.push_back(addValueRef(Arg));
      Args.push_back(S.addTypeRef(Arg->getType().getRawASTType()));
      Args.push_back((unsigned)Arg->getType().getCategory());
    }
    SILInstApplyLayout::emitRecord(Out, ScratchRecord,
                             SILAbbrCodes[SILInstApplyLayout::Code],
                             SIL_BUILTIN, 0,
                             S.addSubstitutionMapRef(BI->getSubstitutions()),
                             S.addTypeRef(BI->getType().getRawASTType()),
                             (unsigned)BI->getType().getCategory(),
                             S.addDeclBaseNameRef(BI->getName()),
                             Args);
    break;
  }
  case SILInstructionKind::ApplyInst: {
    // Format: attributes such as transparent and number of substitutions,
    // the callee's substituted and unsubstituted types, a value for
    // the callee and a list of values for the arguments. Each value in the list
    // is represented with 2 IDs: ValueID and ValueResultNumber. The record
    // is followed by the substitution list.
    const ApplyInst *AI = cast<ApplyInst>(&SI);
    SmallVector<ValueID, 4> Args;
    for (auto Arg: AI->getArguments()) {
      Args.push_back(addValueRef(Arg));
    }
    SILInstApplyLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILInstApplyLayout::Code],
        SIL_APPLY,
        unsigned(AI->getApplyOptions().toRaw()),
        S.addSubstitutionMapRef(AI->getSubstitutionMap()),
        S.addTypeRef(AI->getCallee()->getType().getRawASTType()),
        S.addTypeRef(AI->getSubstCalleeType()),
        addValueRef(AI->getCallee()),
        Args);
    break;
  }
  case SILInstructionKind::BeginApplyInst: {
    // Format: attributes such as transparent and number of substitutions,
    // the callee's substituted and unsubstituted types, a value for
    // the callee and a list of values for the arguments. Each value in the list
    // is represented with 2 IDs: ValueID and ValueResultNumber. The record
    // is followed by the substitution list.
    const BeginApplyInst *AI = cast<BeginApplyInst>(&SI);
    SmallVector<ValueID, 4> Args;
    for (auto Arg: AI->getArguments()) {
      Args.push_back(addValueRef(Arg));
    }
    SILInstApplyLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILInstApplyLayout::Code], SIL_BEGIN_APPLY,
        unsigned(AI->getApplyOptions().toRaw()),
        S.addSubstitutionMapRef(AI->getSubstitutionMap()),
        S.addTypeRef(AI->getCallee()->getType().getRawASTType()),
        S.addTypeRef(AI->getSubstCalleeType()),
        addValueRef(AI->getCallee()),
        Args);
    break;
  }
  case SILInstructionKind::TryApplyInst: {
    // Format: attributes such as transparent and number of substitutions,
    // the callee's substituted and unsubstituted types, a value for
    // the callee and a list of values for the arguments. Each value in the list
    // is represented with 2 IDs: ValueID and ValueResultNumber. The final two
    // entries in the list are the basic block destinations. The record
    // is followed by the substitution list.
    const TryApplyInst *AI = cast<TryApplyInst>(&SI);
    SmallVector<ValueID, 4> Args;
    for (auto Arg: AI->getArguments()) {
      Args.push_back(addValueRef(Arg));
    }
    Args.push_back(BasicBlockMap[AI->getNormalBB()]);
    Args.push_back(BasicBlockMap[AI->getErrorBB()]);
    SILInstApplyLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILInstApplyLayout::Code], SIL_TRY_APPLY,
        unsigned(AI->getApplyOptions().toRaw()),
        S.addSubstitutionMapRef(AI->getSubstitutionMap()),
        S.addTypeRef(AI->getCallee()->getType().getRawASTType()),
        S.addTypeRef(AI->getSubstCalleeType()),
        addValueRef(AI->getCallee()),
        Args);
    break;
  }
  case SILInstructionKind::PartialApplyInst: {
    const PartialApplyInst *PAI = cast<PartialApplyInst>(&SI);
        SmallVector<ValueID, 4> Args;
    for (auto Arg: PAI->getArguments()) {
      Args.push_back(addValueRef(Arg));
    }
    SILInstApplyLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILInstApplyLayout::Code], SIL_PARTIAL_APPLY,
        0,
        S.addSubstitutionMapRef(PAI->getSubstitutionMap()),
        S.addTypeRef(PAI->getCallee()->getType().getRawASTType()),
        S.addTypeRef(PAI->getType().getRawASTType()),
        addValueRef(PAI->getCallee()),
        Args);
    break;
  }
  case SILInstructionKind::AllocGlobalInst: {
    // Format: Name and type. Use SILOneOperandLayout.
    const AllocGlobalInst *AGI = cast<AllocGlobalInst>(&SI);
    auto *G = AGI->getReferencedGlobal();
    addReferencedGlobalVariable(G);
    SILOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneOperandLayout::Code],
        (unsigned)SI.getKind(), 0, 0, 0,
        S.addUniquedStringRef(G->getName()));
    break;
  }
  case SILInstructionKind::GlobalAddrInst:
  case SILInstructionKind::GlobalValueInst: {
    // Format: Name and type. Use SILOneOperandLayout.
    const GlobalAccessInst *GI = cast<GlobalAccessInst>(&SI);
    auto *G = GI->getReferencedGlobal();
    addReferencedGlobalVariable(G);
    bool isBare = false;
    if (auto *gv = dyn_cast<GlobalValueInst>(&SI))
      isBare = gv->isBare();
    SILOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneOperandLayout::Code],
        (unsigned)SI.getKind(), isBare ? 1 : 0,
        S.addTypeRef(GI->getType().getRawASTType()),
        (unsigned)GI->getType().getCategory(),
        S.addUniquedStringRef(G->getName()));
    break;
  }
  case SILInstructionKind::BaseAddrForOffsetInst: {
    const BaseAddrForOffsetInst *BAI = cast<BaseAddrForOffsetInst>(&SI);
    writeOneTypeLayout(BAI->getKind(), /*attrs*/ 0, BAI->getType());
    break;
  }

  case SILInstructionKind::BranchInst: {
    // Format: destination basic block ID, a list of arguments. Use
    // SILOneTypeValuesLayout.
    const BranchInst *BrI = cast<BranchInst>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    for (auto Elt : BrI->getArgs()) {
      ListOfValues.push_back(S.addTypeRef(Elt->getType().getRawASTType()));
      ListOfValues.push_back((unsigned)Elt->getType().getCategory());
      ListOfValues.push_back(addValueRef(Elt));
    }

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code],
        (unsigned)SI.getKind(),
        BasicBlockMap[BrI->getDestBB()], 0, ListOfValues);
    break;
  }
  case SILInstructionKind::CondBranchInst: {
    // Format: condition, true basic block ID, a list of arguments, false basic
    // block ID, a list of arguments. Use SILOneTypeValuesLayout: the type is
    // for condition, the list has value for condition, true basic block ID,
    // false basic block ID, number of true arguments, and a list of true|false
    // arguments.
    const CondBranchInst *CBI = cast<CondBranchInst>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    ListOfValues.push_back(addValueRef(CBI->getCondition()));
    ListOfValues.push_back(BasicBlockMap[CBI->getTrueBB()]);
    ListOfValues.push_back(BasicBlockMap[CBI->getFalseBB()]);
    ListOfValues.push_back(CBI->getTrueArgs().size());
    for (auto Elt : CBI->getTrueArgs()) {
      ListOfValues.push_back(S.addTypeRef(Elt->getType().getRawASTType()));
      ListOfValues.push_back((unsigned)Elt->getType().getCategory());
      ListOfValues.push_back(addValueRef(Elt));
    }
    for (auto Elt : CBI->getFalseArgs()) {
      ListOfValues.push_back(S.addTypeRef(Elt->getType().getRawASTType()));
      ListOfValues.push_back((unsigned)Elt->getType().getCategory());
      ListOfValues.push_back(addValueRef(Elt));
    }

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code],
        (unsigned)SI.getKind(),
        S.addTypeRef(CBI->getCondition()->getType().getRawASTType()),
        (unsigned)CBI->getCondition()->getType().getCategory(),
        ListOfValues);
    break;
  }
  case SILInstructionKind::AwaitAsyncContinuationInst: {
    const AwaitAsyncContinuationInst *AACI
      = cast<AwaitAsyncContinuationInst>(&SI);

    // Format: continuation, resume block ID, error block ID if given
    SmallVector<ValueID, 3> ListOfValues;
    
    ListOfValues.push_back(addValueRef(AACI->getOperand()));
    ListOfValues.push_back(BasicBlockMap[AACI->getResumeBB()]);
    if (auto errorBB = AACI->getErrorBB()) {
      ListOfValues.push_back(BasicBlockMap[errorBB]);
    }
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
       SILAbbrCodes[SILOneTypeValuesLayout::Code],
       (unsigned)SI.getKind(),
       S.addTypeRef(AACI->getOperand()->getType().getRawASTType()),
       (unsigned)AACI->getOperand()->getType().getCategory(),
       ListOfValues);
    break;
  }
  case SILInstructionKind::SwitchEnumInst:
  case SILInstructionKind::SwitchEnumAddrInst: {
    // Format:
    // - [ownership]
    // - the type of the condition operand,
    // - a list a values: operand, hasDefault, defaultBB,
    //                    [EnumElementDecl,  Basic Block ID]*
    SwitchEnumTermInst SOI(&SI);
    assert(SOI);
    SmallVector<ValueID, 4> ListOfValues;
    ListOfValues.push_back(addValueRef(SOI.getOperand()));
    ListOfValues.push_back((unsigned)SOI.hasDefault());
    if (SOI.hasDefault())
      ListOfValues.push_back(BasicBlockMap[SOI.getDefaultBB()]);
    else
      ListOfValues.push_back(0);

    for (unsigned i = 0, e = SOI.getNumCases(); i < e; ++i) {
      EnumElementDecl *elt;
      SILBasicBlock *dest;
      std::tie(elt, dest) = SOI.getCase(i);
      ListOfValues.push_back(S.addDeclRef(elt));
      ListOfValues.push_back(BasicBlockMap[dest]);
    }
    if (auto *switchEnum = dyn_cast<SwitchEnumInst>(&SI)) {
      unsigned ownershipField =
        encodeValueOwnership(switchEnum->getForwardingOwnershipKind());
      SILOneTypeOwnershipValuesLayout::emitRecord(
          Out, ScratchRecord,
          SILAbbrCodes[SILOneTypeOwnershipValuesLayout::Code],
          (unsigned)SI.getKind(), ownershipField,
          S.addTypeRef(SOI.getOperand()->getType().getRawASTType()),
          (unsigned)SOI.getOperand()->getType().getCategory(), ListOfValues);
    } else {
      SILOneTypeValuesLayout::emitRecord(
          Out, ScratchRecord, SILAbbrCodes[SILOneTypeValuesLayout::Code],
          (unsigned)SI.getKind(),
          S.addTypeRef(SOI.getOperand()->getType().getRawASTType()),
          (unsigned)SOI.getOperand()->getType().getCategory(), ListOfValues);
    }
    break;
  }
  case SILInstructionKind::SelectEnumInst:
  case SILInstructionKind::SelectEnumAddrInst: {
    // Format: condition, a list of cases (EnumElementDecl + Value ID),
    // default value ID. Use SILOneTypeValuesLayout: the type is
    // for condition, the list has value for condition, result type,
    //   hasDefault, default
    // basic block ID, a list of (DeclID, BasicBlock ID).
    auto SEO = SelectEnumOperation(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    ListOfValues.push_back(addValueRef(SEO.getEnumOperand()));
    ListOfValues.push_back(S.addTypeRef(SEO->getType().getRawASTType()));
    ListOfValues.push_back((unsigned)SEO->getType().getCategory());
    ListOfValues.push_back((unsigned)SEO.hasDefault());
    if (SEO.hasDefault()) {
      ListOfValues.push_back(addValueRef(SEO.getDefaultResult()));
    } else {
      ListOfValues.push_back(0);
    }
    for (unsigned i = 0, e = SEO.getNumCases(); i < e; ++i) {
      EnumElementDecl *elt;
      SILValue result;
      std::tie(elt, result) = SEO.getCase(i);
      ListOfValues.push_back(S.addDeclRef(elt));
      ListOfValues.push_back(addValueRef(result));
    }
    SILOneTypeValuesLayout::emitRecord(
        Out, ScratchRecord, SILAbbrCodes[SILOneTypeValuesLayout::Code],
        (unsigned)SI.getKind(),
        S.addTypeRef(SEO.getEnumOperand()->getType().getRawASTType()),
        (unsigned)SEO.getEnumOperand()->getType().getCategory(), ListOfValues);
    break;
  }
  case SILInstructionKind::SwitchValueInst: {
    // Format: condition, a list of cases (Value ID + Basic Block ID),
    // default basic block ID. Use SILOneTypeValuesLayout: the type is
    // for condition, the list contains value for condition, hasDefault, default
    // basic block ID, a list of (Value ID, BasicBlock ID).
    const SwitchValueInst *SII = cast<SwitchValueInst>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    ListOfValues.push_back(addValueRef(SII->getOperand()));
    ListOfValues.push_back((unsigned)SII->hasDefault());
    if (SII->hasDefault())
      ListOfValues.push_back(BasicBlockMap[SII->getDefaultBB()]);
    else
      ListOfValues.push_back(0);

    for (unsigned i = 0, e = SII->getNumCases(); i < e; ++i) {
      SILValue value;
      SILBasicBlock *dest;
      std::tie(value, dest) = SII->getCase(i);
      ListOfValues.push_back(addValueRef(value));
      ListOfValues.push_back(BasicBlockMap[dest]);
    }
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code],
        (unsigned)SI.getKind(),
        S.addTypeRef(SII->getOperand()->getType().getRawASTType()),
        (unsigned)SII->getOperand()->getType().getCategory(),
        ListOfValues);
    break;
  }
  case SILInstructionKind::UnownedCopyValueInst:
  case SILInstructionKind::WeakCopyValueInst:
#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                          \
  case SILInstructionKind::StrongCopy##Name##ValueInst:                        \
  case SILInstructionKind::Load##Name##Inst:
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                         \
  case SILInstructionKind::Name##RetainInst:                                   \
  case SILInstructionKind::Name##ReleaseInst:                                  \
  case SILInstructionKind::StrongRetain##Name##Inst:                           \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)                      \
  case SILInstructionKind::Load##Name##Inst:                                   \
  case SILInstructionKind::Name##RetainInst:                                   \
  case SILInstructionKind::Name##ReleaseInst:                                  \
  case SILInstructionKind::StrongRetain##Name##Inst:                           \
  case SILInstructionKind::StrongCopy##Name##ValueInst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::RetainValueInst:
  case SILInstructionKind::DestructureStructInst:
  case SILInstructionKind::DestructureTupleInst:
  case SILInstructionKind::RetainValueAddrInst:
  case SILInstructionKind::UnmanagedRetainValueInst:
  case SILInstructionKind::EndBorrowInst:
  case SILInstructionKind::CopyValueInst:
  case SILInstructionKind::ExplicitCopyValueInst:
  case SILInstructionKind::MoveValueInst:
  case SILInstructionKind::DropDeinitInst:
  case SILInstructionKind::MarkUnresolvedReferenceBindingInst:
  case SILInstructionKind::MoveOnlyWrapperToCopyableValueInst:
  case SILInstructionKind::CopyableToMoveOnlyWrapperValueInst:
  case SILInstructionKind::DestroyValueInst:
  case SILInstructionKind::ReleaseValueInst:
  case SILInstructionKind::ReleaseValueAddrInst:
  case SILInstructionKind::UnmanagedReleaseValueInst:
  case SILInstructionKind::AutoreleaseValueInst:
  case SILInstructionKind::UnmanagedAutoreleaseValueInst:
  case SILInstructionKind::SetDeallocatingInst:
  case SILInstructionKind::DeallocStackInst:
  case SILInstructionKind::DeallocStackRefInst:
  case SILInstructionKind::DeallocPackInst:
  case SILInstructionKind::DeallocRefInst:
  case SILInstructionKind::DeinitExistentialAddrInst:
  case SILInstructionKind::DeinitExistentialValueInst:
  case SILInstructionKind::DestroyAddrInst:
  case SILInstructionKind::LoadInst:
  case SILInstructionKind::LoadBorrowInst:
  case SILInstructionKind::BeginBorrowInst:
  case SILInstructionKind::ClassifyBridgeObjectInst:
  case SILInstructionKind::ValueToBridgeObjectInst:
  case SILInstructionKind::FixLifetimeInst:
  case SILInstructionKind::EndLifetimeInst:
  case SILInstructionKind::CopyBlockInst:
  case SILInstructionKind::StrongReleaseInst:
  case SILInstructionKind::StrongRetainInst:
  case SILInstructionKind::IsUniqueInst:
  case SILInstructionKind::BeginCOWMutationInst:
  case SILInstructionKind::EndCOWMutationInst:
  case SILInstructionKind::HopToExecutorInst:
  case SILInstructionKind::ExtractExecutorInst:
  case SILInstructionKind::AbortApplyInst:
  case SILInstructionKind::EndApplyInst:
  case SILInstructionKind::ReturnInst:
  case SILInstructionKind::UncheckedOwnershipConversionInst:
  case SILInstructionKind::IsEscapingClosureInst:
  case SILInstructionKind::ThrowInst: {
    unsigned Attr = 0;
    if (auto *LI = dyn_cast<LoadInst>(&SI))
      Attr = unsigned(LI->getOwnershipQualifier());
#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
    else if (auto *LI = dyn_cast<Load##Name##Inst>(&SI)) \
      Attr = LI->isTake();
#include "swift/AST/ReferenceStorage.def"
    else if (auto *RCI = dyn_cast<RefCountingInst>(&SI))
      Attr = RCI->isNonAtomic();
    else if (auto *UOCI = dyn_cast<UncheckedOwnershipConversionInst>(&SI)) {
      Attr = encodeValueOwnership(UOCI->getOwnershipKind());
    } else if (auto *IEC = dyn_cast<IsEscapingClosureInst>(&SI)) {
      Attr = IEC->getVerificationType();
    } else if (auto *HTE = dyn_cast<HopToExecutorInst>(&SI)) {
      Attr = HTE->isMandatory();
    } else if (auto *DVI = dyn_cast<DestroyValueInst>(&SI)) {
      Attr = DVI->poisonRefs();
    } else if (auto *BCMI = dyn_cast<BeginCOWMutationInst>(&SI)) {
      Attr = BCMI->isNative();
    } else if (auto *ECMI = dyn_cast<EndCOWMutationInst>(&SI)) {
      Attr = ECMI->doKeepUnique();
    } else if (auto *BBI = dyn_cast<BeginBorrowInst>(&SI)) {
      Attr =
          unsigned(BBI->isLexical()) | unsigned(BBI->hasPointerEscape() << 1);
    } else if (auto *MVI = dyn_cast<MoveValueInst>(&SI)) {
      Attr = unsigned(MVI->getAllowDiagnostics()) |
             (unsigned(MVI->isLexical() << 1)) |
             (unsigned(MVI->hasPointerEscape() << 2));
    } else if (auto *I = dyn_cast<MarkMustCheckInst>(&SI)) {
      Attr = unsigned(I->getCheckKind());
    } else if (auto *I = dyn_cast<MarkUnresolvedReferenceBindingInst>(&SI)) {
      Attr = unsigned(I->getKind());
    } else if (auto *I = dyn_cast<MoveOnlyWrapperToCopyableValueInst>(&SI)) {
      Attr = I->getForwardingOwnershipKind() == OwnershipKind::Owned ? true
                                                                     : false;
    } else if (auto *I = dyn_cast<CopyableToMoveOnlyWrapperValueInst>(&SI)) {
      Attr = I->getForwardingOwnershipKind() == OwnershipKind::Owned ? true
                                                                     : false;
    }
    writeOneOperandLayout(SI.getKind(), Attr, SI.getOperand(0));
    break;
  }
  case SILInstructionKind::MarkMustCheckInst: {
    unsigned Attr = unsigned(cast<MarkMustCheckInst>(&SI)->getCheckKind());
    writeOneOperandExtraAttributeLayout(SI.getKind(), Attr, SI.getOperand(0));
    break;
  }
  case SILInstructionKind::MarkUninitializedInst: {
    unsigned Attr =
        (unsigned)cast<MarkUninitializedInst>(&SI)->getMarkUninitializedKind();
    writeOneOperandExtraAttributeLayout(SI.getKind(), Attr, SI.getOperand(0));
    break;
  }
  case SILInstructionKind::YieldInst: {
    auto YI = cast<YieldInst>(&SI);
    SmallVector<ValueID, 4> args;
    for (auto arg: YI->getYieldedValues()) {
      args.push_back(S.addTypeRef(arg->getType().getRawASTType()));
      args.push_back((unsigned)arg->getType().getCategory());
      args.push_back(addValueRef(arg));
    }
    args.push_back(BasicBlockMap[YI->getResumeBB()]);
    args.push_back(BasicBlockMap[YI->getUnwindBB()]);
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code],
        (unsigned)YI->getKind(), 0, 0, args);
    break;
  }
  case SILInstructionKind::FunctionRefInst: {
    // Use SILOneOperandLayout to specify the function type and the function
    // name (IdentifierID).
    const FunctionRefInst *FRI = cast<FunctionRefInst>(&SI);
    SILFunction *ReferencedFunction = FRI->getReferencedFunction();
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    SILOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
        (unsigned)SI.getKind(), 0,
        S.addTypeRef(FRI->getType().getRawASTType()),
        (unsigned)FRI->getType().getCategory(),
        addSILFunctionRef(ReferencedFunction));

    break;
  }
  case SILInstructionKind::DynamicFunctionRefInst: {
    // Use SILOneOperandLayout to specify the function type and the function
    // name (IdentifierID).
    const auto *FRI = cast<DynamicFunctionRefInst>(&SI);
    SILFunction *ReferencedFunction = FRI->getInitiallyReferencedFunction();
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    SILOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
        (unsigned)SI.getKind(), 0,
        S.addTypeRef(FRI->getType().getRawASTType()),
        (unsigned)FRI->getType().getCategory(),
        addSILFunctionRef(ReferencedFunction));

    break;
  }
  case SILInstructionKind::PreviousDynamicFunctionRefInst: {
    // Use SILOneOperandLayout to specify the function type and the function
    // name (IdentifierID).
    const auto *FRI = cast<PreviousDynamicFunctionRefInst>(&SI);
    SILFunction *ReferencedFunction = FRI->getInitiallyReferencedFunction();
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    SILOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
        (unsigned)SI.getKind(), 0,
        S.addTypeRef(FRI->getType().getRawASTType()),
        (unsigned)FRI->getType().getCategory(),
        addSILFunctionRef(ReferencedFunction));

    break;
  }
  case SILInstructionKind::CopyBlockWithoutEscapingInst:
  case SILInstructionKind::DeallocPartialRefInst:
  case SILInstructionKind::MarkDependenceInst:
  case SILInstructionKind::IndexAddrInst:
  case SILInstructionKind::IndexRawPointerInst: {
    SILValue operand, operand2;
    unsigned Attr = 0;
    if (SI.getKind() == SILInstructionKind::CopyBlockWithoutEscapingInst) {
      const CopyBlockWithoutEscapingInst *C = cast<CopyBlockWithoutEscapingInst>(&SI);
      operand = C->getBlock();
      operand2 = C->getClosure();
    } else if (SI.getKind() == SILInstructionKind::DeallocPartialRefInst) {
      const DeallocPartialRefInst *DPRI = cast<DeallocPartialRefInst>(&SI);
      operand = DPRI->getInstance();
      operand2 = DPRI->getMetatype();
    } else if (SI.getKind() == SILInstructionKind::IndexRawPointerInst) {
      const IndexRawPointerInst *IRP = cast<IndexRawPointerInst>(&SI);
      operand = IRP->getBase();
      operand2 = IRP->getIndex();
    } else if (SI.getKind() == SILInstructionKind::MarkDependenceInst) {
      const MarkDependenceInst *MDI = cast<MarkDependenceInst>(&SI);
      operand = MDI->getValue();
      operand2 = MDI->getBase();
    } else {
      const IndexAddrInst *IAI = cast<IndexAddrInst>(&SI);
      operand = IAI->getBase();
      operand2 = IAI->getIndex();
      Attr = (IAI->needsStackProtection() ? 1 : 0);
    }
    SILTwoOperandsLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILTwoOperandsLayout::Code],
        (unsigned)SI.getKind(), Attr,
        S.addTypeRef(operand->getType().getRawASTType()),
        (unsigned)operand->getType().getCategory(),
        addValueRef(operand),
        S.addTypeRef(operand2->getType().getRawASTType()),
        (unsigned)operand2->getType().getCategory(),
        addValueRef(operand2));
    break;
  }
  case SILInstructionKind::PackElementGetInst: {
    auto PEGI = cast<PackElementGetInst>(&SI);
    auto elementType = PEGI->getElementType();
    auto elementTypeRef = S.addTypeRef(elementType.getRawASTType());
    auto pack = PEGI->getPack();
    auto packType = pack->getType();
    auto packTypeRef = S.addTypeRef(packType.getRawASTType());
    auto packRef = addValueRef(pack);
    auto indexRef = addValueRef(PEGI->getIndex());
    SILPackElementGetLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILPackElementGetLayout::Code],
        (unsigned)SI.getKind(),
        elementTypeRef,
        (unsigned) elementType.getCategory(),
        packTypeRef,
        (unsigned) packType.getCategory(),
        packRef,
        indexRef);
    break;
  }
  case SILInstructionKind::PackElementSetInst: {
    auto PESI = cast<PackElementSetInst>(&SI);
    auto value = PESI->getValue();
    auto valueType = value->getType();
    auto valueTypeRef = S.addTypeRef(valueType.getRawASTType());
    auto valueRef = addValueRef(value);
    auto pack = PESI->getPack();
    auto packType = pack->getType();
    auto packTypeRef = S.addTypeRef(packType.getRawASTType());
    auto packRef = addValueRef(pack);
    auto indexRef = addValueRef(PESI->getIndex());
    SILPackElementSetLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILPackElementSetLayout::Code],
        valueTypeRef,
        (unsigned) valueType.getCategory(),
        valueRef,
        packTypeRef,
        (unsigned) packType.getCategory(),
        packRef,
        indexRef);
    break;
  }
  case SILInstructionKind::TuplePackElementAddrInst: {
    auto TPEAI = cast<TuplePackElementAddrInst>(&SI);
    auto elementType = TPEAI->getElementType();
    auto elementTypeRef = S.addTypeRef(elementType.getRawASTType());
    auto tuple = TPEAI->getTuple();
    auto tupleType = tuple->getType();
    auto tupleTypeRef = S.addTypeRef(tupleType.getRawASTType());
    auto tupleRef = addValueRef(tuple);
    auto indexRef = addValueRef(TPEAI->getIndex());
    SILPackElementGetLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILPackElementGetLayout::Code],
        (unsigned)SI.getKind(),
        elementTypeRef,
        (unsigned) elementType.getCategory(),
        tupleTypeRef,
        (unsigned) tupleType.getCategory(),
        tupleRef,
        indexRef);
    break;
  }
  case SILInstructionKind::TailAddrInst: {
    const TailAddrInst *TAI = cast<TailAddrInst>(&SI);
    SILTailAddrLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILTailAddrLayout::Code],
        (unsigned)SI.getKind(),
        S.addTypeRef(TAI->getBase()->getType().getRawASTType()),
        addValueRef(TAI->getBase()),
        S.addTypeRef(TAI->getIndex()->getType().getRawASTType()),
        addValueRef(TAI->getIndex()),
        S.addTypeRef(TAI->getTailType().getRawASTType()));
    break;
  }
  case SILInstructionKind::CondFailInst: {
    auto *CFI = cast<CondFailInst>(&SI);
    SILValue operand = CFI->getOperand();
    SILTwoOperandsLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILTwoOperandsLayout::Code],
        (unsigned)SI.getKind(), /*attributes*/ 0,
        S.addTypeRef(operand->getType().getRawASTType()),
        (unsigned)operand->getType().getCategory(),
        addValueRef(operand),
        0, 0, S.addUniquedStringRef(CFI->getMessage()));
    break;
  }
  case SILInstructionKind::IncrementProfilerCounterInst: {
    auto *IPCI = cast<IncrementProfilerCounterInst>(&SI);
    llvm::SmallString<10> HashStr;
    APInt(64, IPCI->getPGOFuncHash()).toStringUnsigned(HashStr);
    SILInstIncrementProfilerCounterLayout::emitRecord(
        Out, ScratchRecord,
        SILAbbrCodes[SILInstIncrementProfilerCounterLayout::Code],
        S.addUniquedStringRef(IPCI->getPGOFuncName()),
        S.addUniquedStringRef(HashStr),
        IPCI->getCounterIndex(),
        IPCI->getNumCounters());
    break;
  }
  case SILInstructionKind::StringLiteralInst: {
    auto SLI = cast<StringLiteralInst>(&SI);
    StringRef Str = SLI->getValue();
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    unsigned encoding = toStableStringEncoding(SLI->getEncoding());
    SILOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
                                    (unsigned)SI.getKind(), encoding, 0, 0,
                                    S.addUniquedStringRef(Str));
    break;
  }
  case SILInstructionKind::FloatLiteralInst:
  case SILInstructionKind::IntegerLiteralInst: {
    // Use SILOneOperandLayout to specify the type and the literal.
    llvm::SmallString<10> Str;
    SILType Ty;
    switch (SI.getKind()) {
    default: llvm_unreachable("Out of sync with parent switch");
    case SILInstructionKind::IntegerLiteralInst:
      cast<IntegerLiteralInst>(&SI)->getValue().toString(Str, 10,
                                                         /*signed*/ true);
      Ty = cast<IntegerLiteralInst>(&SI)->getType();
      break;
    case SILInstructionKind::FloatLiteralInst:
      cast<FloatLiteralInst>(&SI)->getBits().toString(Str, 16,
                                                      /*signed*/ true);
      Ty = cast<FloatLiteralInst>(&SI)->getType();
      break;
    }
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    SILOneOperandLayout::emitRecord(
        Out, ScratchRecord, abbrCode, (unsigned)SI.getKind(), 0,
        S.addTypeRef(Ty.getRawASTType()), (unsigned)Ty.getCategory(),
        S.addUniquedStringRef(Str.str()));
    break;
  }
  case SILInstructionKind::MarkFunctionEscapeInst: {
    // Format: a list of typed values. A typed value is expressed by 4 IDs:
    // TypeID, TypeCategory, ValueID, ValueResultNumber.
    const MarkFunctionEscapeInst *MFE = cast<MarkFunctionEscapeInst>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    for (auto Elt : MFE->getElements()) {
      ListOfValues.push_back(S.addTypeRef(Elt->getType().getRawASTType()));
      ListOfValues.push_back((unsigned)Elt->getType().getCategory());
      ListOfValues.push_back(addValueRef(Elt));
    }

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code],
        (unsigned)SI.getKind(), 0, 0, ListOfValues);
    break;
  }
  case SILInstructionKind::MetatypeInst: {
    auto &MI = cast<MetatypeInst>(SI);
    writeOneTypeLayout(MI.getKind(), 0, MI.getType());
    break;
  }
  case SILInstructionKind::ObjCProtocolInst: {
    const ObjCProtocolInst *PI = cast<ObjCProtocolInst>(&SI);
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    SILOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
                              (unsigned)SI.getKind(), 0,
                              S.addTypeRef(PI->getType().getRawASTType()),
                              (unsigned)PI->getType().getCategory(),
                              S.addDeclRef(PI->getProtocol()));
    break;
  }
  case SILInstructionKind::OpenExistentialAddrInst: {
    auto &open = cast<OpenExistentialAddrInst>(SI);
    assert(open.getNumOperands() - open.getTypeDependentOperands().size() == 1);
    unsigned attrs = open.getAccessKind() == OpenedExistentialAccess::Immutable
                         ? 0 : 1;
    writeOneTypeOneOperandLayout(open.getKind(), attrs, open.getType(),
                                 open.getOperand());
    break;
  }
  case SILInstructionKind::DynamicPackIndexInst: {
    auto &dpii = cast<DynamicPackIndexInst>(SI);
    writeOneTypeOneOperandLayout(dpii.getKind(), 0,
                                 dpii.getIndexedPackType(),
                                 dpii.getOperand());
    break;
  }
  case SILInstructionKind::PackPackIndexInst: {
    auto &ppii = cast<PackPackIndexInst>(SI);
    writeOneTypeOneOperandLayout(ppii.getKind(),
                                 ppii.getComponentStartIndex(),
                                 ppii.getIndexedPackType(),
                                 ppii.getOperand());
    break;
  }
  case SILInstructionKind::ScalarPackIndexInst: {
    auto &spii = cast<ScalarPackIndexInst>(SI);
    writeOneTypeLayout(spii.getKind(),
                       spii.getComponentIndex(),
                       spii.getIndexedPackType());
    break;
  }
  case SILInstructionKind::OpenPackElementInst: {
    auto &opei = cast<OpenPackElementInst>(SI);
    auto envRef =
      S.addGenericEnvironmentRef(opei.getOpenedGenericEnvironment());
    auto operand = opei.getIndexOperand();
    auto operandRef = addValueRef(operand);
    auto operandType = operand->getType();
    auto operandTypeRef = S.addTypeRef(operandType.getRawASTType());

    SILOpenPackElementLayout::emitRecord(Out, ScratchRecord,
          SILAbbrCodes[SILOpenPackElementLayout::Code],
          envRef,
          operandTypeRef, unsigned(operandType.getCategory()),
          operandRef);
    break;
  }
  case SILInstructionKind::GetAsyncContinuationAddrInst: {
    auto &gaca = cast<GetAsyncContinuationAddrInst>(SI);
    writeOneTypeOneOperandLayout(gaca.getKind(), gaca.throws(),
                                 gaca.getFormalResumeType(),
                                 gaca.getOperand());
    break;
  }
  case SILInstructionKind::GetAsyncContinuationInst: {
    auto &gaca = cast<GetAsyncContinuationInst>(SI);
    writeOneTypeLayout(gaca.getKind(), gaca.throws(),
                       gaca.getFormalResumeType());
    break;
  }
  // Conversion instructions (and others of similar form).
#define LOADABLE_REF_STORAGE(Name, ...) \
  case SILInstructionKind::RefTo##Name##Inst: \
  case SILInstructionKind::Name##ToRefInst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::OpenExistentialRefInst:
  case SILInstructionKind::OpenExistentialMetatypeInst:
  case SILInstructionKind::OpenExistentialBoxInst:
  case SILInstructionKind::OpenExistentialValueInst:
  case SILInstructionKind::OpenExistentialBoxValueInst:
  case SILInstructionKind::UncheckedRefCastInst:
  case SILInstructionKind::UncheckedAddrCastInst:
  case SILInstructionKind::UncheckedTrivialBitCastInst:
  case SILInstructionKind::UncheckedBitwiseCastInst:
  case SILInstructionKind::UncheckedValueCastInst:
  case SILInstructionKind::BridgeObjectToRefInst:
  case SILInstructionKind::BridgeObjectToWordInst:
  case SILInstructionKind::UpcastInst:
  case SILInstructionKind::AddressToPointerInst:
  case SILInstructionKind::RefToRawPointerInst:
  case SILInstructionKind::RawPointerToRefInst:
  case SILInstructionKind::ThinToThickFunctionInst:
  case SILInstructionKind::ThickToObjCMetatypeInst:
  case SILInstructionKind::ObjCToThickMetatypeInst:
  case SILInstructionKind::ConvertFunctionInst:
  case SILInstructionKind::ConvertEscapeToNoEscapeInst:
  case SILInstructionKind::ObjCMetatypeToObjectInst:
  case SILInstructionKind::ObjCExistentialMetatypeToObjectInst:
  case SILInstructionKind::ProjectBlockStorageInst: {
    unsigned attrs = 0;
    if (SI.getKind() == SILInstructionKind::ConvertEscapeToNoEscapeInst) {
      if (cast<ConvertEscapeToNoEscapeInst>(SI).isLifetimeGuaranteed())
        attrs |= 0x01;
    }
    if (SI.getKind() == SILInstructionKind::ConvertFunctionInst) {
      if (cast<ConvertFunctionInst>(SI).withoutActuallyEscaping())
        attrs |= 0x01;
    } else if (auto *refCast = dyn_cast<UncheckedRefCastInst>(&SI)) {
      attrs = encodeValueOwnership(refCast->getOwnershipKind());
    } else if (auto *atp = dyn_cast<AddressToPointerInst>(&SI)) {
      attrs = atp->needsStackProtection() ? 1 : 0;
    }
    writeConversionLikeInstruction(cast<SingleValueInstruction>(&SI), attrs);
    break;
  }
  case SILInstructionKind::PointerToAddressInst: {
    auto &PAI = cast<PointerToAddressInst>(SI);
    assert(PAI.getNumOperands() - PAI.getTypeDependentOperands().size() == 1);
    uint8_t encodedAlignment = llvm::encode(PAI.alignment());
    assert(encodedAlignment == llvm::encode(PAI.alignment())
           && "pointer_to_address alignment overflow");
    unsigned attrs = encodedAlignment | (PAI.isStrict() ? 0x100 : 0)
                     | (PAI.isInvariant() ? 0x200 : 0);
    writeOneTypeOneOperandExtraAttributeLayout(
      PAI.getKind(), attrs, PAI.getType(), PAI.getOperand());
    break;
  }
  case SILInstructionKind::RefToBridgeObjectInst: {
    auto RI = cast<RefToBridgeObjectInst>(&SI);
    auto op = RI->getOperand(0);
    SILTwoOperandsLayout::emitRecord(
        Out, ScratchRecord, SILAbbrCodes[SILTwoOperandsLayout::Code],
        (unsigned)SI.getKind(),
        /*attr*/ 0, S.addTypeRef(op->getType().getRawASTType()),
        (unsigned)op->getType().getCategory(), addValueRef(op),
        S.addTypeRef(RI->getBitsOperand()->getType().getRawASTType()),
        (unsigned)RI->getBitsOperand()->getType().getCategory(),
        addValueRef(RI->getBitsOperand()));
    break;
  }
  // Checked Conversion instructions.
  case SILInstructionKind::UnconditionalCheckedCastInst: {
    auto CI = cast<UnconditionalCheckedCastInst>(&SI);
    ValueID listOfValues[] = {
      addValueRef(CI->getOperand()),
      S.addTypeRef(CI->getSourceLoweredType().getRawASTType()),
      (unsigned)CI->getSourceLoweredType().getCategory(),
      S.addTypeRef(CI->getTargetFormalType())
    };

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
             SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
             S.addTypeRef(CI->getTargetLoweredType().getRawASTType()),
             (unsigned)CI->getTargetLoweredType().getCategory(),
             llvm::makeArrayRef(listOfValues));
    break;
  }
  case SILInstructionKind::UnconditionalCheckedCastAddrInst: {
    auto CI = cast<UnconditionalCheckedCastAddrInst>(&SI);
    ValueID listOfValues[] = {
      S.addTypeRef(CI->getSourceFormalType()),
      addValueRef(CI->getSrc()),
      S.addTypeRef(CI->getSourceLoweredType().getRawASTType()),
      (unsigned)CI->getSourceLoweredType().getCategory(),
      S.addTypeRef(CI->getTargetFormalType()),
      addValueRef(CI->getDest())
    };
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
             SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
             S.addTypeRef(CI->getTargetLoweredType().getRawASTType()),
             (unsigned)CI->getTargetLoweredType().getCategory(),
             llvm::makeArrayRef(listOfValues));
    break;
  }
  case SILInstructionKind::UncheckedRefCastAddrInst: {
    auto CI = cast<UncheckedRefCastAddrInst>(&SI);
    ValueID listOfValues[] = {
      S.addTypeRef(CI->getSourceFormalType()),
      addValueRef(CI->getSrc()),
      S.addTypeRef(CI->getSourceLoweredType().getRawASTType()),
      (unsigned)CI->getSourceLoweredType().getCategory(),
      S.addTypeRef(CI->getTargetFormalType()),
      addValueRef(CI->getDest())
    };
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
             SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
             S.addTypeRef(CI->getTargetLoweredType().getRawASTType()),
             (unsigned)CI->getTargetLoweredType().getCategory(),
             llvm::makeArrayRef(listOfValues));
    break;
  }

  case SILInstructionKind::BeginAccessInst: {
    unsigned abbrCode = SILAbbrCodes[SILOneOperandExtraAttributeLayout::Code];
    auto *BAI = cast<BeginAccessInst>(&SI);
    unsigned attr = unsigned(BAI->getAccessKind())
                    + (unsigned(BAI->getEnforcement()) << 2)
                    + (BAI->hasNoNestedConflict() << 4)
                    + (BAI->isFromBuiltin() << 5);
    SILValue operand = BAI->getOperand();

    SILOneOperandExtraAttributeLayout::emitRecord(
        Out, ScratchRecord, abbrCode, (unsigned)SI.getKind(), attr,
        S.addTypeRef(operand->getType().getRawASTType()),
        (unsigned)operand->getType().getCategory(),
        addValueRef(operand));
    break;
  }

  case SILInstructionKind::MoveOnlyWrapperToCopyableAddrInst: {
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    auto *BAI = cast<MoveOnlyWrapperToCopyableAddrInst>(&SI);
    SILValue operand = BAI->getOperand();

    SILOneOperandLayout::emitRecord(
        Out, ScratchRecord, abbrCode, (unsigned)SI.getKind(), 0,
        S.addTypeRef(operand->getType().getRawASTType()),
        (unsigned)operand->getType().getCategory(), addValueRef(operand));
    break;
  }
  case SILInstructionKind::MoveOnlyWrapperToCopyableBoxInst: {
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    auto *BAI = cast<MoveOnlyWrapperToCopyableBoxInst>(&SI);
    SILValue operand = BAI->getOperand();

    SILOneOperandLayout::emitRecord(
        Out, ScratchRecord, abbrCode, (unsigned)SI.getKind(), 0,
        S.addTypeRef(operand->getType().getRawASTType()),
        (unsigned)operand->getType().getCategory(), addValueRef(operand));
    break;
  }
  case SILInstructionKind::CopyableToMoveOnlyWrapperAddrInst: {
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    auto *BAI = cast<CopyableToMoveOnlyWrapperAddrInst>(&SI);
    SILValue operand = BAI->getOperand();

    SILOneOperandLayout::emitRecord(
        Out, ScratchRecord, abbrCode, (unsigned)SI.getKind(), 0,
        S.addTypeRef(operand->getType().getRawASTType()),
        (unsigned)operand->getType().getCategory(), addValueRef(operand));
    break;
  }

  case SILInstructionKind::EndAccessInst: {
    unsigned abbrCode = SILAbbrCodes[SILOneOperandLayout::Code];
    auto *EAI = cast<EndAccessInst>(&SI);
    unsigned attr = unsigned(EAI->isAborting());
    SILValue operand = EAI->getOperand();

    SILOneOperandLayout::emitRecord(
        Out, ScratchRecord, abbrCode, (unsigned)SI.getKind(), attr,
        S.addTypeRef(operand->getType().getRawASTType()),
        (unsigned)operand->getType().getCategory(),
        addValueRef(operand));
    break;
  }

  case SILInstructionKind::BeginUnpairedAccessInst: {
    unsigned abbrCode = SILAbbrCodes[SILTwoOperandsExtraAttributeLayout::Code];
    auto *BAI = cast<BeginUnpairedAccessInst>(&SI);
    unsigned attr = unsigned(BAI->getAccessKind())
                    + (unsigned(BAI->getEnforcement()) << 2)
                    + (unsigned(BAI->hasNoNestedConflict()) << 4)
                    + (unsigned(BAI->isFromBuiltin()) << 5);
    SILValue source = BAI->getSource();
    SILValue buffer = BAI->getBuffer();

    SILTwoOperandsExtraAttributeLayout::emitRecord(
        Out, ScratchRecord, abbrCode, (unsigned)SI.getKind(), attr,
        S.addTypeRef(source->getType().getRawASTType()),
        (unsigned)source->getType().getCategory(),
        addValueRef(source),
        S.addTypeRef(buffer->getType().getRawASTType()),
        (unsigned)buffer->getType().getCategory(),
        addValueRef(buffer));
    break;
  }

  case SILInstructionKind::EndUnpairedAccessInst: {
    unsigned abbrCode = SILAbbrCodes[SILOneOperandExtraAttributeLayout::Code];
    auto *EAI = cast<EndUnpairedAccessInst>(&SI);
    unsigned attr = unsigned(EAI->isAborting())
                    + (unsigned(EAI->getEnforcement()) << 1)
                    + (unsigned(EAI->isFromBuiltin()) << 3);
    SILValue operand = EAI->getOperand();

    SILOneOperandExtraAttributeLayout::emitRecord(
        Out, ScratchRecord, abbrCode, (unsigned)SI.getKind(), attr,
        S.addTypeRef(operand->getType().getRawASTType()),
        (unsigned)operand->getType().getCategory(),
        addValueRef(operand));
    break;
  }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
  case SILInstructionKind::Store##Name##Inst:
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::AssignInst:
  case SILInstructionKind::CopyAddrInst:
  case SILInstructionKind::ExplicitCopyAddrInst:
  case SILInstructionKind::MarkUnresolvedMoveAddrInst:
  case SILInstructionKind::StoreInst:
  case SILInstructionKind::StoreBorrowInst: {
    SILValue operand, value;
    unsigned Attr = 0;
    if (SI.getKind() == SILInstructionKind::StoreInst) {
      Attr = unsigned(cast<StoreInst>(&SI)->getOwnershipQualifier());
      operand = cast<StoreInst>(&SI)->getDest();
      value = cast<StoreInst>(&SI)->getSrc();
#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
    } else if (SI.getKind() == SILInstructionKind::Store##Name##Inst) { \
      Attr = cast<Store##Name##Inst>(&SI)->isInitializationOfDest(); \
      operand = cast<Store##Name##Inst>(&SI)->getDest(); \
      value = cast<Store##Name##Inst>(&SI)->getSrc();
#include "swift/AST/ReferenceStorage.def"
    } else if (SI.getKind() == SILInstructionKind::AssignInst) {
      Attr = unsigned(cast<AssignInst>(&SI)->getOwnershipQualifier());
      operand = cast<AssignInst>(&SI)->getDest();
      value = cast<AssignInst>(&SI)->getSrc();
    } else if (SI.getKind() == SILInstructionKind::CopyAddrInst) {
      const CopyAddrInst *CAI = cast<CopyAddrInst>(&SI);
      Attr = (CAI->isInitializationOfDest() << 1) | CAI->isTakeOfSrc();
      operand = cast<CopyAddrInst>(&SI)->getDest();
      value = cast<CopyAddrInst>(&SI)->getSrc();
    } else if (SI.getKind() == SILInstructionKind::ExplicitCopyAddrInst) {
      const auto *CAI = cast<ExplicitCopyAddrInst>(&SI);
      Attr = (CAI->isInitializationOfDest() << 1) | CAI->isTakeOfSrc();
      operand = cast<ExplicitCopyAddrInst>(&SI)->getDest();
      value = cast<ExplicitCopyAddrInst>(&SI)->getSrc();
    } else if (SI.getKind() == SILInstructionKind::MarkUnresolvedMoveAddrInst) {
      auto *mai = cast<MarkUnresolvedMoveAddrInst>(&SI);
      operand = mai->getDest();
      value = mai->getSrc();
    } else if (auto *SBI = dyn_cast<StoreBorrowInst>(&SI)) {
      operand = SBI->getDest();
      value = SBI->getSrc();
    } else {
      llvm_unreachable("switch out of sync");
    }

    unsigned abbrCode = SILAbbrCodes[SILOneValueOneOperandLayout::Code];
    SILOneValueOneOperandLayout::emitRecord(Out, ScratchRecord, abbrCode,
                  (unsigned)SI.getKind(), Attr, addValueRef(value),
                  S.addTypeRef(operand->getType().getRawASTType()),
                  (unsigned)operand->getType().getCategory(),
                  addValueRef(operand));
    break;
  }
  case SILInstructionKind::AssignByWrapperInst:
  case SILInstructionKind::AssignOrInitInst:
    llvm_unreachable("not supported");
  case SILInstructionKind::BindMemoryInst: {
    auto *BI = cast<BindMemoryInst>(&SI);
    SILValue baseOperand = BI->getBase();
    SILValue indexOperand = BI->getIndex();
    SILType boundType = BI->getBoundType();
    SmallVector<ValueID, 6> ListOfValues;
    ListOfValues.push_back(S.addTypeRef(
                             baseOperand->getType().getRawASTType()));
    ListOfValues.push_back((unsigned)baseOperand->getType().getCategory());
    ListOfValues.push_back(addValueRef(baseOperand));
    ListOfValues.push_back(S.addTypeRef(
                             indexOperand->getType().getRawASTType()));
    ListOfValues.push_back((unsigned)indexOperand->getType().getCategory());
    ListOfValues.push_back(addValueRef(indexOperand));

    SILOneTypeValuesLayout::emitRecord(
      Out,
      ScratchRecord,
      SILAbbrCodes[SILOneTypeValuesLayout::Code],
      (unsigned)SI.getKind(),
      S.addTypeRef(boundType.getRawASTType()),
      (unsigned)boundType.getCategory(),
      ListOfValues);
    break;
  }
  case SILInstructionKind::RebindMemoryInst: {
    auto *RBI = cast<RebindMemoryInst>(&SI);
    SILValue baseOperand = RBI->getBase();
    SILValue inToken = RBI->getInToken();
    SILTwoOperandsLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILTwoOperandsLayout::Code], (unsigned)SI.getKind(),
                                     /*attr*/0,
        S.addTypeRef(baseOperand->getType().getRawASTType()),
        (unsigned)baseOperand->getType().getCategory(),
        addValueRef(baseOperand),
        S.addTypeRef(inToken->getType().getRawASTType()),
        (unsigned)inToken->getType().getCategory(),
        addValueRef(inToken));
    break;
  }
  case SILInstructionKind::RefElementAddrInst:
  case SILInstructionKind::StructElementAddrInst:
  case SILInstructionKind::StructExtractInst:
  case SILInstructionKind::InitEnumDataAddrInst:
  case SILInstructionKind::UncheckedEnumDataInst:
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst:
  case SILInstructionKind::InjectEnumAddrInst: {
    // Has a typed valueref and a field decl. We use SILOneValueOneOperandLayout
    // where the field decl is streamed as a ValueID.
    SILValue operand;
    Decl *tDecl;
    unsigned attr = 0;
    switch (SI.getKind()) {
    default: llvm_unreachable("Out of sync with parent switch");
    case SILInstructionKind::RefElementAddrInst:
      operand = cast<RefElementAddrInst>(&SI)->getOperand();
      tDecl = cast<RefElementAddrInst>(&SI)->getField();
      attr = unsigned(cast<RefElementAddrInst>(&SI)->isImmutable());
      break;
    case SILInstructionKind::StructElementAddrInst:
      operand = cast<StructElementAddrInst>(&SI)->getOperand();
      tDecl = cast<StructElementAddrInst>(&SI)->getField();
      break;
    case SILInstructionKind::StructExtractInst:
      operand = cast<StructExtractInst>(&SI)->getOperand();
      tDecl = cast<StructExtractInst>(&SI)->getField();
      break;
    case SILInstructionKind::InitEnumDataAddrInst:
      operand = cast<InitEnumDataAddrInst>(&SI)->getOperand();
      tDecl = cast<InitEnumDataAddrInst>(&SI)->getElement();
      break;
    case SILInstructionKind::UncheckedEnumDataInst:
      operand = cast<UncheckedEnumDataInst>(&SI)->getOperand();
      tDecl = cast<UncheckedEnumDataInst>(&SI)->getElement();
      break;
    case SILInstructionKind::UncheckedTakeEnumDataAddrInst:
      operand = cast<UncheckedTakeEnumDataAddrInst>(&SI)->getOperand();
      tDecl = cast<UncheckedTakeEnumDataAddrInst>(&SI)->getElement();
      break;
    case SILInstructionKind::InjectEnumAddrInst:
      operand = cast<InjectEnumAddrInst>(&SI)->getOperand();
      tDecl = cast<InjectEnumAddrInst>(&SI)->getElement();
      break;
    }
    SILOneValueOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneValueOneOperandLayout::Code],
        (unsigned)SI.getKind(), attr, S.addDeclRef(tDecl),
        S.addTypeRef(operand->getType().getRawASTType()),
        (unsigned)operand->getType().getCategory(),
        addValueRef(operand));
    break;
  }
  case SILInstructionKind::RefTailAddrInst: {
    auto *RTAI = cast<RefTailAddrInst>(&SI);
    writeOneTypeOneOperandLayout(RTAI->getKind(), unsigned(RTAI->isImmutable()),
                                 RTAI->getType(),
                                 RTAI->getOperand());
    break;
  }
  case SILInstructionKind::StructInst: {
    // Format: a type followed by a list of typed values. A typed value is
    // expressed by 4 IDs: TypeID, TypeCategory, ValueID, ValueResultNumber.
    const StructInst *StrI = cast<StructInst>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    for (auto Elt : StrI->getElements()) {
      ListOfValues.push_back(S.addTypeRef(Elt->getType().getRawASTType()));
      ListOfValues.push_back((unsigned)Elt->getType().getCategory());
      ListOfValues.push_back(addValueRef(Elt));
    }

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code],
        (unsigned)SI.getKind(),
        S.addTypeRef(StrI->getType().getRawASTType()),
        (unsigned)StrI->getType().getCategory(), ListOfValues);
    break;
  }
  case SILInstructionKind::TupleElementAddrInst:
  case SILInstructionKind::TupleExtractInst: {
    SILValue operand;
    unsigned FieldNo;
    switch (SI.getKind()) {
    default: llvm_unreachable("Out of sync with parent switch");
    case SILInstructionKind::TupleElementAddrInst:
      operand = cast<TupleElementAddrInst>(&SI)->getOperand();
      FieldNo = cast<TupleElementAddrInst>(&SI)->getFieldIndex();
      break;
    case SILInstructionKind::TupleExtractInst:
      operand = cast<TupleExtractInst>(&SI)->getOperand();
      FieldNo = cast<TupleExtractInst>(&SI)->getFieldIndex();
      break;
    }

    // Use OneTypeOneOperand layout where the field number is stored in TypeID.
    SILOneTypeOneOperandLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeOneOperandLayout::Code],
        (unsigned)SI.getKind(), 0,
        FieldNo, 0,
        S.addTypeRef(operand->getType().getRawASTType()),
        (unsigned)operand->getType().getCategory(),
        addValueRef(operand));
    break;
  }
  case SILInstructionKind::TupleInst: {
    // Format: a type followed by a list of values. A value is expressed by
    // 2 IDs: ValueID, ValueResultNumber.
    const TupleInst *TI = cast<TupleInst>(&SI);
    SmallVector<ValueID, 4> ListOfValues;
    for (auto Elt : TI->getElements()) {
      ListOfValues.push_back(addValueRef(Elt));
    }

    unsigned abbrCode = SILAbbrCodes[SILOneTypeValuesLayout::Code];
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord, abbrCode,
        (unsigned)SI.getKind(),
        S.addTypeRef(TI->getType().getRawASTType()),
        (unsigned)TI->getType().getCategory(),
        ListOfValues);
    break;
  }
  case SILInstructionKind::EnumInst: {
    // Format: a type, an operand and a decl ID. Use SILTwoOperandsLayout: type,
    // (DeclID + hasOperand), and an operand.
    const EnumInst *UI = cast<EnumInst>(&SI);
    TypeID OperandTy = UI->hasOperand() ?
      S.addTypeRef(UI->getOperand()->getType().getRawASTType()) : TypeID();
    unsigned OperandTyCategory = UI->hasOperand() ?
        (unsigned)UI->getOperand()->getType().getCategory() : 0;
    SILTwoOperandsLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILTwoOperandsLayout::Code], (unsigned)SI.getKind(),
        UI->hasOperand(),
        S.addTypeRef(UI->getType().getRawASTType()),
        (unsigned)UI->getType().getCategory(),
        S.addDeclRef(UI->getElement()),
        OperandTy, OperandTyCategory,
        UI->hasOperand() ? addValueRef(UI->getOperand()) : ValueID());
    break;
  }
  case SILInstructionKind::WitnessMethodInst: {
    // Format: a type, an operand and a SILDeclRef. Use SILOneTypeValuesLayout:
    // type, Attr, SILDeclRef (DeclID, Kind, uncurryLevel, IsObjC), and a type.
    const WitnessMethodInst *WMI = cast<WitnessMethodInst>(&SI);
    CanType Ty = WMI->getLookupType();
    SILType Ty2 = WMI->getType();

    SmallVector<uint64_t, 8> ListOfValues;
    handleSILDeclRef(S, WMI->getMember(), ListOfValues);

    // Add an optional operand.
    TypeID OperandTy = TypeID();
    unsigned OperandTyCategory = 0;
    SILValue OptionalOpenedExistential = SILValue();
    auto OperandValueId = addValueRef(OptionalOpenedExistential);
    auto ConformanceId = S.addConformanceRef(WMI->getConformance());

    SILInstWitnessMethodLayout::emitRecord(
        Out, ScratchRecord, SILAbbrCodes[SILInstWitnessMethodLayout::Code],
        S.addTypeRef(Ty), 0, 0,
        S.addTypeRef(Ty2.getRawASTType()), (unsigned)Ty2.getCategory(),
        OperandTy, OperandTyCategory, OperandValueId, ConformanceId,
        ListOfValues);
    break;
  }
  case SILInstructionKind::ClassMethodInst: {
    // Format: a type, an operand and a SILDeclRef. Use SILOneTypeValuesLayout:
    // type, Attr, SILDeclRef (DeclID, Kind, uncurryLevel),
    // and an operand.
    const ClassMethodInst *CMI = cast<ClassMethodInst>(&SI);
    SILType Ty = CMI->getType();
    SmallVector<uint64_t, 9> ListOfValues;
    handleMethodInst(CMI, CMI->getOperand(), ListOfValues);

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
        S.addTypeRef(Ty.getRawASTType()),
        (unsigned)Ty.getCategory(), ListOfValues);
    break;
  }
  case SILInstructionKind::SuperMethodInst: {
    // Format: a type, an operand and a SILDeclRef. Use SILOneTypeValuesLayout:
    // type, Attr, SILDeclRef (DeclID, Kind, uncurryLevel),
    // and an operand.
    const SuperMethodInst *SMI = cast<SuperMethodInst>(&SI);
    SILType Ty = SMI->getType();
    SmallVector<uint64_t, 9> ListOfValues;
    handleMethodInst(SMI, SMI->getOperand(), ListOfValues);

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
        S.addTypeRef(Ty.getRawASTType()),
        (unsigned)Ty.getCategory(), ListOfValues);
    break;
  }
  case SILInstructionKind::ObjCMethodInst: {
    // Format: a type, an operand and a SILDeclRef. Use SILOneTypeValuesLayout:
    // type, Attr, SILDeclRef (DeclID, Kind, uncurryLevel),
    // and an operand.
    const ObjCMethodInst *OMI = cast<ObjCMethodInst>(&SI);
    SILType Ty = OMI->getType();
    SmallVector<uint64_t, 9> ListOfValues;
    handleMethodInst(OMI, OMI->getOperand(), ListOfValues);

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
        S.addTypeRef(Ty.getRawASTType()),
        (unsigned)Ty.getCategory(), ListOfValues);
    break;
  }
  case SILInstructionKind::ObjCSuperMethodInst: {
    // Format: a type, an operand and a SILDeclRef. Use SILOneTypeValuesLayout:
    // type, Attr, SILDeclRef (DeclID, Kind, uncurryLevel),
    // and an operand.
    const ObjCSuperMethodInst *SMI = cast<ObjCSuperMethodInst>(&SI);
    SILType Ty = SMI->getType();
    SmallVector<uint64_t, 9> ListOfValues;
    handleMethodInst(SMI, SMI->getOperand(), ListOfValues);

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
        S.addTypeRef(Ty.getRawASTType()),
        (unsigned)Ty.getCategory(), ListOfValues);
    break;
  }
  case SILInstructionKind::DynamicMethodBranchInst: {
    // Format: a typed value, a SILDeclRef, a BasicBlock ID for method,
    // a BasicBlock ID for no method. Use SILOneTypeValuesLayout.
    const DynamicMethodBranchInst *DMB = cast<DynamicMethodBranchInst>(&SI);
    SmallVector<uint64_t, 8> ListOfValues;
    ListOfValues.push_back(addValueRef(DMB->getOperand()));
    handleSILDeclRef(S, DMB->getMember(), ListOfValues);
    ListOfValues.push_back(BasicBlockMap[DMB->getHasMethodBB()]);
    ListOfValues.push_back(BasicBlockMap[DMB->getNoMethodBB()]);

    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
        S.addTypeRef(DMB->getOperand()->getType().getRawASTType()),
        (unsigned)DMB->getOperand()->getType().getCategory(), ListOfValues);
    break;
  }
  case SILInstructionKind::CheckedCastBranchInst: {
    const CheckedCastBranchInst *CBI = cast<CheckedCastBranchInst>(&SI);
    ValueID listOfValues[] = {
      CBI->isExact(),
      S.addTypeRef(CBI->getSourceFormalType()),
      addValueRef(CBI->getOperand()),
      S.addTypeRef(CBI->getSourceLoweredType().getRawASTType()),
      (unsigned)CBI->getSourceLoweredType().getCategory(),
      S.addTypeRef(CBI->getTargetFormalType()),
      BasicBlockMap[CBI->getSuccessBB()],
      BasicBlockMap[CBI->getFailureBB()]
    };
    unsigned ownershipField =
      encodeValueOwnership(CBI->getForwardingOwnershipKind());
    SILOneTypeOwnershipValuesLayout::emitRecord(
        Out, ScratchRecord, SILAbbrCodes[SILOneTypeOwnershipValuesLayout::Code],
        (unsigned)SI.getKind(), ownershipField,
        S.addTypeRef(CBI->getTargetLoweredType().getRawASTType()),
        (unsigned)CBI->getTargetLoweredType().getCategory(),
        llvm::makeArrayRef(listOfValues));
    break;
  }
  case SILInstructionKind::CheckedCastAddrBranchInst: {
    auto CBI = cast<CheckedCastAddrBranchInst>(&SI);
    ValueID listOfValues[] = {
      toStableCastConsumptionKind(CBI->getConsumptionKind()),
      S.addTypeRef(CBI->getSourceFormalType()),
      addValueRef(CBI->getSrc()),
      S.addTypeRef(CBI->getSourceLoweredType().getRawASTType()),
      (unsigned)CBI->getSourceLoweredType().getCategory(),
      S.addTypeRef(CBI->getTargetFormalType()),
      addValueRef(CBI->getDest()),
      BasicBlockMap[CBI->getSuccessBB()],
      BasicBlockMap[CBI->getFailureBB()]
    };
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
             SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
             S.addTypeRef(CBI->getTargetLoweredType().getRawASTType()),
             (unsigned)CBI->getTargetLoweredType().getCategory(),
             llvm::makeArrayRef(listOfValues));
    break;
  }
  case SILInstructionKind::InitBlockStorageHeaderInst: {
    auto IBSHI = cast<InitBlockStorageHeaderInst>(&SI);
    SmallVector<ValueID, 6> ListOfValues;
    ListOfValues.push_back(addValueRef(IBSHI->getBlockStorage()));
    ListOfValues.push_back(
         S.addTypeRef(IBSHI->getBlockStorage()->getType().getRawASTType()));
    // Always an address, don't need to save category
    
    ListOfValues.push_back(addValueRef(IBSHI->getInvokeFunction()));
    ListOfValues.push_back(
       S.addTypeRef(IBSHI->getInvokeFunction()->getType().getRawASTType()));
    // Always a value, don't need to save category
    ListOfValues.push_back(S.addSubstitutionMapRef(IBSHI->getSubstitutions()));
    
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
             SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
             S.addTypeRef(IBSHI->getType().getRawASTType()),
             (unsigned)IBSHI->getType().getCategory(),
             ListOfValues);

    break;
  }
  case SILInstructionKind::KeyPathInst: {
    auto KPI = cast<KeyPathInst>(&SI);
    SmallVector<uint64_t, 6> ListOfValues;

    auto pattern = KPI->getPattern();
    ListOfValues.push_back(S.addTypeRef(pattern->getRootType()));
    ListOfValues.push_back(S.addTypeRef(pattern->getValueType()));
    ListOfValues.push_back(pattern->getComponents().size());
    ListOfValues.push_back(pattern->getNumOperands());
    ListOfValues.push_back(S.addSubstitutionMapRef(KPI->getSubstitutions()));

    ListOfValues.push_back(S.addUniquedStringRef(pattern->getObjCString()));

    if (auto sig = pattern->getGenericSignature()) {
      assert(sig.getGenericParams().size() > 0);
      ListOfValues.push_back(sig.getGenericParams().size());
      for (auto param : sig.getGenericParams())
        ListOfValues.push_back(S.addTypeRef(param));
      auto reqts = sig.getRequirements();
      S.serializeGenericRequirements(reqts, ListOfValues);
    } else {
      ListOfValues.push_back(0);
    }

    for (auto &component : pattern->getComponents()) {
      writeKeyPathPatternComponent(component, ListOfValues);
    }
    
    for (auto &operand : KPI->getPatternOperands()) {
      auto value = operand.get();
      ListOfValues.push_back(addValueRef(value));
      ListOfValues.push_back(S.addTypeRef(value->getType().getRawASTType()));
      ListOfValues.push_back((unsigned)value->getType().getCategory());
    }
    
    SILOneTypeValuesLayout::emitRecord(Out, ScratchRecord,
         SILAbbrCodes[SILOneTypeValuesLayout::Code], (unsigned)SI.getKind(),
         S.addTypeRef(KPI->getType().getRawASTType()),
         (unsigned)KPI->getType().getCategory(),
         ListOfValues);

    break;
  }
  case SILInstructionKind::DifferentiableFunctionInst: {
    auto *dfi = cast<DifferentiableFunctionInst>(&SI);
    SmallVector<ValueID, 4> trailingInfo;
    auto *paramIndices = dfi->getParameterIndices();
    for (unsigned i : paramIndices->getIndices())
      trailingInfo.push_back(i);
    auto *resultIndices = dfi->getResultIndices();
    for (unsigned i : resultIndices->getIndices())
      trailingInfo.push_back(i);
    for (auto &op : dfi->getAllOperands()) {
      auto val = op.get();
      trailingInfo.push_back(S.addTypeRef(val->getType().getRawASTType()));
      trailingInfo.push_back((unsigned)val->getType().getCategory());
      trailingInfo.push_back(addValueRef(val));
    }
    SILInstDifferentiableFunctionLayout::emitRecord(
        Out, ScratchRecord,
        SILAbbrCodes[SILInstDifferentiableFunctionLayout::Code],
        paramIndices->getCapacity(), resultIndices->getCapacity(),
        paramIndices->getNumIndices(), dfi->hasDerivativeFunctions(),
        trailingInfo);
    break;
  }
  case SILInstructionKind::LinearFunctionInst: {
    auto *lfi = cast<LinearFunctionInst>(&SI);
    SmallVector<ValueID, 4> trailingInfo;
    auto *paramIndices = lfi->getParameterIndices();
    for (unsigned idx : paramIndices->getIndices())
      trailingInfo.push_back(idx);
    for (auto &op : lfi->getAllOperands()) {
      auto val = op.get();
      trailingInfo.push_back(S.addTypeRef(val->getType().getRawASTType()));
      trailingInfo.push_back((unsigned)val->getType().getCategory());
      trailingInfo.push_back(addValueRef(val));
    }
    SILInstLinearFunctionLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILInstLinearFunctionLayout::Code],
        paramIndices->getCapacity(), lfi->hasTransposeFunction(),
        trailingInfo);
    break;
  }
  case SILInstructionKind::DifferentiableFunctionExtractInst: {
    auto *dfei = cast<DifferentiableFunctionExtractInst>(&SI);
    auto operandRef = addValueRef(dfei->getOperand());
    auto operandType = dfei->getOperand()->getType();
    auto operandTypeRef = S.addTypeRef(operandType.getRawASTType());
    auto rawExtractee = (unsigned)dfei->getExtractee();
    auto extracteeTypeRef = S.addTypeRef(dfei->getType().getRawASTType());
    SILInstDifferentiableFunctionExtractLayout::emitRecord(
        Out, ScratchRecord,
        SILAbbrCodes[SILInstDifferentiableFunctionExtractLayout::Code],
        operandTypeRef, (unsigned)operandType.getCategory(), operandRef,
        rawExtractee, (unsigned)dfei->hasExplicitExtracteeType(),
        extracteeTypeRef);
    break;
  }
  case SILInstructionKind::LinearFunctionExtractInst: {
    auto *lfei = cast<LinearFunctionExtractInst>(&SI);
    auto operandRef = addValueRef(lfei->getOperand());
    auto operandType = lfei->getOperand()->getType();
    auto operandTypeRef = S.addTypeRef(operandType.getRawASTType());
    auto rawExtractee = (unsigned)lfei->getExtractee();
    SILInstLinearFunctionExtractLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[SILInstLinearFunctionExtractLayout::Code],
        operandTypeRef, (unsigned)operandType.getCategory(), operandRef,
        rawExtractee);
    break;
  }
  case SILInstructionKind::DifferentiabilityWitnessFunctionInst: {
    auto *dwfi = cast<DifferentiabilityWitnessFunctionInst>(&SI);
    auto *witness = dwfi->getWitness();
    DifferentiabilityWitnessesToEmit.insert(witness);
    Mangle::ASTMangler mangler;
    auto mangledKey = mangler.mangleSILDifferentiabilityWitness(
        witness->getOriginalFunction()->getName(), witness->getKind(),
        witness->getConfig());
    auto rawWitnessKind = (unsigned)dwfi->getWitnessKind();
    // We only store the type when the instruction has an explicit type.
    bool hasExplicitFnTy = dwfi->getHasExplicitFunctionType();
    SILOneOperandLayout::emitRecord(
        Out, ScratchRecord, SILAbbrCodes[SILOneOperandLayout::Code],
        (unsigned)dwfi->getKind(), rawWitnessKind,
        hasExplicitFnTy ? S.addTypeRef(dwfi->getType().getRawASTType()) : TypeID(),
        hasExplicitFnTy ? (unsigned)dwfi->getType().getCategory() : 0,
        S.addUniquedStringRef(mangledKey));
    break;
  }
  case SILInstructionKind::HasSymbolInst: {
    auto *hsi = cast<HasSymbolInst>(&SI);
    auto *decl = hsi->getDecl();
    // Although the instruction doesn't have them as members, we need to
    // ensure that any SILFunctions that are technically referenced by the
    // instruction get serialized.
    SmallVector<SILFunction *, 4> fns;
    hsi->getReferencedFunctions(fns);
    SmallVector<IdentifierID, 4> functionRefs;
    for (auto fn : fns) {
      functionRefs.push_back(addSILFunctionRef(fn));
    }
    SILInstHasSymbolLayout::emitRecord(
        Out, ScratchRecord, SILAbbrCodes[SILInstHasSymbolLayout::Code],
        S.addDeclRef(decl), functionRefs);
    break;
  }
  }
  // Non-void values get registered in the value table.
  for (auto result : SI.getResults()) {
    addValueRef(result);
    ++InstID;
  }
}

/// Depending on the RecordKind, we write the SILFunction table, the global
/// variable table, the table for SILVTable, or the table for SILWitnessTable.
static void writeIndexTable(Serializer &S,
                            const sil_index_block::ListLayout &List,
                            sil_index_block::RecordKind kind,
                            const SILSerializer::Table &table) {
  assert((kind == sil_index_block::SIL_FUNC_NAMES ||
          kind == sil_index_block::SIL_VTABLE_NAMES ||
          kind == sil_index_block::SIL_MOVEONLYDEINIT_NAMES ||
          kind == sil_index_block::SIL_GLOBALVAR_NAMES ||
          kind == sil_index_block::SIL_WITNESS_TABLE_NAMES ||
          kind == sil_index_block::SIL_DEFAULT_WITNESS_TABLE_NAMES ||
          kind == sil_index_block::SIL_DIFFERENTIABILITY_WITNESS_NAMES) &&
         "SIL function table, global, vtable, (default) witness table, and "
         "differentiability witness table are supported");
  llvm::SmallString<4096> hashTableBlob;
  uint32_t tableOffset;
  {
    llvm::OnDiskChainedHashTableGenerator<FuncTableInfo> generator;
    FuncTableInfo tableInfo(S);
    for (auto &entry : table)
      generator.insert(entry.first, entry.second, tableInfo);

    llvm::raw_svector_ostream blobStream(hashTableBlob);
    // Make sure that no bucket is at offset 0.
    endian::write<uint32_t>(blobStream, 0, little);
    tableOffset = generator.Emit(blobStream, tableInfo);
  }
  SmallVector<uint64_t, 8> scratch;
  List.emit(scratch, kind, tableOffset, hashTableBlob);
}

void SILSerializer::writeIndexTables() {
  BCBlockRAII restoreBlock(Out, SIL_INDEX_BLOCK_ID, 4);

  sil_index_block::ListLayout List(Out);
  sil_index_block::OffsetLayout Offset(Out);
  if (!FuncTable.empty()) {
    writeIndexTable(S, List, sil_index_block::SIL_FUNC_NAMES, FuncTable);
    Offset.emit(ScratchRecord, sil_index_block::SIL_FUNC_OFFSETS, Funcs);
  }

  if (!VTableList.empty()) {
    writeIndexTable(S, List, sil_index_block::SIL_VTABLE_NAMES, VTableList);
    Offset.emit(ScratchRecord, sil_index_block::SIL_VTABLE_OFFSETS,
                VTableOffset);
  }

  if (!MoveOnlyDeinitList.empty()) {
    writeIndexTable(S, List, sil_index_block::SIL_MOVEONLYDEINIT_NAMES,
                    MoveOnlyDeinitList);
    Offset.emit(ScratchRecord, sil_index_block::SIL_MOVEONLYDEINIT_OFFSETS,
                MoveOnlyDeinitOffset);
  }

  if (!GlobalVarList.empty()) {
    writeIndexTable(S, List, sil_index_block::SIL_GLOBALVAR_NAMES,
                    GlobalVarList);
    Offset.emit(ScratchRecord, sil_index_block::SIL_GLOBALVAR_OFFSETS,
                GlobalVarOffset);
  }

  if (!WitnessTableList.empty()) {
    writeIndexTable(S, List, sil_index_block::SIL_WITNESS_TABLE_NAMES,
                    WitnessTableList);
    Offset.emit(ScratchRecord, sil_index_block::SIL_WITNESS_TABLE_OFFSETS,
                WitnessTableOffset);
  }
  
  if (!DefaultWitnessTableList.empty()) {
    writeIndexTable(S, List, sil_index_block::SIL_DEFAULT_WITNESS_TABLE_NAMES,
                    DefaultWitnessTableList);
    Offset.emit(ScratchRecord,
                sil_index_block::SIL_DEFAULT_WITNESS_TABLE_OFFSETS,
                DefaultWitnessTableOffset);
  }

  if (!PropertyOffset.empty()) {
    Offset.emit(ScratchRecord, sil_index_block::SIL_PROPERTY_OFFSETS,
                PropertyOffset);
  }

  if (!DifferentiabilityWitnessList.empty()) {
    writeIndexTable(S, List,
                    sil_index_block::SIL_DIFFERENTIABILITY_WITNESS_NAMES,
                    DifferentiabilityWitnessList);
    Offset.emit(ScratchRecord,
                sil_index_block::SIL_DIFFERENTIABILITY_WITNESS_OFFSETS,
                DifferentiabilityWitnessOffset);
  }

}

void SILSerializer::writeSILGlobalVar(const SILGlobalVariable &g) {
  GlobalVarList[g.getName()] = NextGlobalVarID++;
  GlobalVarOffset.push_back(Out.GetCurrentBitNo());
  TypeID TyID = S.addTypeRef(g.getLoweredType().getRawASTType());
  DeclID dID = S.addDeclRef(g.getDecl());
  SILGlobalVarLayout::emitRecord(Out, ScratchRecord,
                                 SILAbbrCodes[SILGlobalVarLayout::Code],
                                 toStableSILLinkage(g.getLinkage()),
                                 g.isSerialized() ? 1 : 0,
                                 (unsigned)!g.isDefinition(),
                                 (unsigned)g.isLet(),
                                 TyID, dID);

  // Don't emit the initializer instructions if not marked as "serialized".
  if (!g.isSerialized())
    return;

  ValueIDs.clear();
  InstID = 0;
  unsigned ValueID = 2;
  for (const SILInstruction &initInst : g) {
      for (auto result : initInst.getResults()) {
        ValueIDs[result] = ValueID++;
      }
  }

  for (const SILInstruction &initInst : g) {
    writeSILInstruction(initInst);
  }
}

void SILSerializer::writeSILVTable(const SILVTable &vt) {
  // Do not emit vtables for non-public classes unless everything has to be
  // serialized.
  if (!ShouldSerializeAll &&
      vt.getClass()->getEffectiveAccess() < swift::AccessLevel::Public)
    return;

  // Use the mangled name of the class as a key to distinguish between classes
  // which have the same name (but are in different contexts).
  Mangle::ASTMangler mangler;
  std::string mangledClassName = mangler.mangleNominalType(vt.getClass());
  size_t nameLength = mangledClassName.size();
  char *stringStorage = (char *)StringTable.Allocate(nameLength, 1);
  std::memcpy(stringStorage, mangledClassName.data(), nameLength);

  VTableList[StringRef(stringStorage, nameLength)] = NextVTableID++;

  VTableOffset.push_back(Out.GetCurrentBitNo());
  VTableLayout::emitRecord(Out, ScratchRecord, SILAbbrCodes[VTableLayout::Code],
                           S.addDeclRef(vt.getClass()),
                           vt.isSerialized() == IsSerialized ? 1 : 0);

  for (auto &entry : vt.getEntries()) {
    SmallVector<uint64_t, 4> ListOfValues;
    SILFunction *impl = entry.getImplementation();

    if (ShouldSerializeAll || impl->hasValidLinkageForFragileRef()) {
      handleSILDeclRef(S, entry.getMethod(), ListOfValues);
      addReferencedSILFunction(impl, true);
      // Each entry is a pair of SILDeclRef and SILFunction.
      VTableEntryLayout::emitRecord(
          Out, ScratchRecord, SILAbbrCodes[VTableEntryLayout::Code],
          // SILFunction name
          S.addUniquedStringRef(impl->getName()),
          toStableVTableEntryKind(entry.getKind()),
          entry.isNonOverridden(),
          ListOfValues);
    }
  }
}

void SILSerializer::writeSILMoveOnlyDeinit(const SILMoveOnlyDeinit &deinit) {
  // Do not emit deinit for non-public nominal types unless everything has to be
  // serialized.
  if (!ShouldSerializeAll && deinit.getNominalDecl()->getEffectiveAccess() <
                                 swift::AccessLevel::Public)
    return;

  SILFunction *impl = deinit.getImplementation();
  if (!ShouldSerializeAll && !impl->hasValidLinkageForFragileRef())
    return;

  // Use the mangled name of the class as a key to distinguish between classes
  // which have the same name (but are in different contexts).
  Mangle::ASTMangler mangler;
  std::string mangledNominalName =
      mangler.mangleNominalType(deinit.getNominalDecl());
  size_t nameLength = mangledNominalName.size();
  char *stringStorage = (char *)StringTable.Allocate(nameLength, 1);
  std::memcpy(stringStorage, mangledNominalName.data(), nameLength);

  MoveOnlyDeinitList[StringRef(stringStorage, nameLength)] =
      NextMoveOnlyDeinitOffsetID++;
  MoveOnlyDeinitOffset.push_back(Out.GetCurrentBitNo());

  addReferencedSILFunction(impl, true);
  MoveOnlyDeinitLayout::emitRecord(
      Out, ScratchRecord, SILAbbrCodes[MoveOnlyDeinitLayout::Code],
      S.addDeclRef(deinit.getNominalDecl()),
      S.addUniquedStringRef(impl->getName()),
      deinit.isSerialized() == IsSerialized ? 1 : 0);
}

void SILSerializer::writeSILProperty(const SILProperty &prop) {
  PropertyOffset.push_back(Out.GetCurrentBitNo());
  
  SmallVector<uint64_t, 4> componentValues;
  
  if (auto component = prop.getComponent()) {
    writeKeyPathPatternComponent(*component, componentValues);
  } else {
    componentValues.push_back((unsigned)KeyPathComponentKindEncoding::Trivial);
  }
  
  PropertyLayout::emitRecord(
    Out, ScratchRecord,
    SILAbbrCodes[PropertyLayout::Code],
    S.addDeclRef(prop.getDecl()),
    prop.isSerialized(),
    componentValues);
}

void SILSerializer::writeSILWitnessTable(const SILWitnessTable &wt) {
  WitnessTableList[wt.getName()] = NextWitnessTableID++;
  WitnessTableOffset.push_back(Out.GetCurrentBitNo());

  auto conformanceID = S.addConformanceRef(wt.getConformance());
  WitnessTableLayout::emitRecord(
    Out, ScratchRecord,
    SILAbbrCodes[WitnessTableLayout::Code],
    toStableSILLinkage(wt.getLinkage()),
    unsigned(wt.isDeclaration()),
    wt.isSerialized() == IsSerialized ? 1 : 0,
    conformanceID);

  // If we have a declaration, do not attempt to serialize entries.
  if (wt.isDeclaration())
    return;

  for (auto &entry : wt.getEntries()) {
    writeSILWitnessTableEntry(entry);
  }

  for (auto conditional : wt.getConditionalConformances()) {
    auto requirementID = S.addTypeRef(conditional.Requirement);
    auto conformanceID = S.addConformanceRef(conditional.Conformance);
    WitnessConditionalConformanceLayout::emitRecord(
        Out, ScratchRecord,
        SILAbbrCodes[WitnessConditionalConformanceLayout::Code],
        requirementID, conformanceID);
  }
}

void SILSerializer::writeSILWitnessTableEntry(
                                        const SILWitnessTable::Entry &entry) {
  if (entry.getKind() == SILWitnessTable::BaseProtocol) {
    auto &baseWitness = entry.getBaseProtocolWitness();

    auto requirementID = S.addDeclRef(baseWitness.Requirement);
    auto conformanceID = S.addConformanceRef(baseWitness.Witness);

    WitnessBaseEntryLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[WitnessBaseEntryLayout::Code],
        requirementID, conformanceID);
    return;
  }

  if (entry.getKind() == SILWitnessTable::AssociatedTypeProtocol) {
    auto &assoc = entry.getAssociatedTypeProtocolWitness();

    auto requirementID = S.addTypeRef(assoc.Requirement);
    auto protocolID = S.addDeclRef(assoc.Protocol);
    auto conformanceID = S.addConformanceRef(assoc.Witness);

    WitnessAssocProtocolLayout::emitRecord(
      Out, ScratchRecord,
      SILAbbrCodes[WitnessAssocProtocolLayout::Code],
      requirementID, protocolID, conformanceID);
    return;
  }

  if (entry.getKind() == SILWitnessTable::AssociatedType) {
    auto &assoc = entry.getAssociatedTypeWitness();
    WitnessAssocEntryLayout::emitRecord(Out, ScratchRecord,
        SILAbbrCodes[WitnessAssocEntryLayout::Code],
        S.addDeclRef(assoc.Requirement),
        S.addTypeRef(assoc.Witness));
    return;
  }

  auto &methodWitness = entry.getMethodWitness();
  SmallVector<uint64_t, 4> ListOfValues;
  handleSILDeclRef(S, methodWitness.Requirement, ListOfValues);
  IdentifierID witnessID = 0;
  SILFunction *witness = methodWitness.Witness;
  if (witness && witness->hasValidLinkageForFragileRef()) {
    addReferencedSILFunction(witness, true);
    witnessID = S.addUniquedStringRef(witness->getName());
  }
  WitnessMethodEntryLayout::emitRecord(Out, ScratchRecord,
      SILAbbrCodes[WitnessMethodEntryLayout::Code],
      // SILFunction name
      witnessID,
      ListOfValues);
}


void SILSerializer::
writeSILDefaultWitnessTable(const SILDefaultWitnessTable &wt) {
  if (wt.isDeclaration())
    return;

  StringRef name = S.addUniquedString(wt.getUniqueName()).first;
  DefaultWitnessTableList[name] = NextDefaultWitnessTableID++;
  DefaultWitnessTableOffset.push_back(Out.GetCurrentBitNo());

  DefaultWitnessTableLayout::emitRecord(
    Out, ScratchRecord,
    SILAbbrCodes[DefaultWitnessTableLayout::Code],
    S.addDeclRef(wt.getProtocol()),
    toStableSILLinkage(wt.getLinkage()));

  for (auto &entry : wt.getEntries()) {
    if (!entry.isValid()) {
      DefaultWitnessTableNoEntryLayout::emitRecord(Out, ScratchRecord,
          SILAbbrCodes[DefaultWitnessTableNoEntryLayout::Code]);
      continue;
    }

    writeSILWitnessTableEntry(entry);
  }
}

void SILSerializer::writeSILDifferentiabilityWitness(
    const SILDifferentiabilityWitness &dw) {
  Mangle::ASTMangler mangler;
  auto mangledKey = mangler.mangleSILDifferentiabilityWitness(
      dw.getOriginalFunction()->getName(), dw.getKind(), dw.getConfig());
  size_t nameLength = mangledKey.size();
  char *stringStorage =
      static_cast<char *>(StringTable.Allocate(nameLength, 1));
  std::memcpy(stringStorage, mangledKey.data(), nameLength);
  DifferentiabilityWitnessList[StringRef(stringStorage, nameLength)] =
      NextDifferentiabilityWitnessID++;
  DifferentiabilityWitnessOffset.push_back(Out.GetCurrentBitNo());

  auto *original = dw.getOriginalFunction();
  IdentifierID jvpID = 0;
  IdentifierID vjpID = 0;
  if (auto *jvp = dw.getJVP())
    jvpID = addSILFunctionRef(jvp);
  if (auto *vjp = dw.getVJP())
    vjpID = addSILFunctionRef(vjp);
  SmallVector<unsigned, 8> parameterAndResultIndices(
      dw.getParameterIndices()->begin(), dw.getParameterIndices()->end());
  parameterAndResultIndices.append(dw.getResultIndices()->begin(),
                                   dw.getResultIndices()->end());
  auto originalFnType = original->getLoweredFunctionType();
  assert(originalFnType->getNumParameters() ==
             dw.getParameterIndices()->getCapacity() &&
         "Original function parameter count should match differentiability "
         "witness parameter indices capacity");
  unsigned numInoutParameters = llvm::count_if(
      originalFnType->getParameters(), [](SILParameterInfo paramInfo) {
        return paramInfo.isIndirectMutating();
      });
  assert(originalFnType->getNumResults() + numInoutParameters ==
             dw.getResultIndices()->getCapacity() &&
         "Original function result count should match differentiability "
         "witness result indices capacity");

  DifferentiabilityWitnessLayout::emitRecord(
      Out, ScratchRecord, SILAbbrCodes[DifferentiabilityWitnessLayout::Code],
      addSILFunctionRef(original), toStableSILLinkage(dw.getLinkage()),
      dw.isDeclaration(), dw.isSerialized(),
      toStableDifferentiabilityKind(dw.getKind()),
      S.addGenericSignatureRef(dw.getDerivativeGenericSignature()), jvpID,
      vjpID, dw.getParameterIndices()->getNumIndices(),
      dw.getResultIndices()->getNumIndices(), parameterAndResultIndices);
}

/// Helper function for whether to emit a function body.
bool SILSerializer::shouldEmitFunctionBody(const SILFunction *F,
                                           bool isReference) {

  // If F is a declaration, it has no body to emit...
  // The declaration will be serialized anyways if it is referenced anywhere.
  if (F->isExternalDeclaration())
    return false;

  // Never serialize any function definitions available externally.
  if (F->isAvailableExternally())
    return false;

  // If we are asked to serialize everything, go ahead and do it.
  if (ShouldSerializeAll)
    return true;

  // If F is serialized, we should always emit its body.
  // Shared functions are only serialized if they are referenced from another
  // serialized function. This is handled in `addReferencedSILFunction`.
  if (F->isSerialized() && !hasSharedVisibility(F->getLinkage()))
    return true;

  return false;
}

void SILSerializer::writeSILBlock(const SILModule *SILMod) {
  BCBlockRAII subBlock(Out, SIL_BLOCK_ID, 6);

  registerSILAbbr<SILFunctionLayout>();
  registerSILAbbr<SILBasicBlockLayout>();
  registerSILAbbr<SILOneValueOneOperandLayout>();
  registerSILAbbr<SILOneTypeLayout>();
  registerSILAbbr<SILOneOperandLayout>();
  registerSILAbbr<SILOneOperandExtraAttributeLayout>();
  registerSILAbbr<SILOneTypeOneOperandLayout>();
  registerSILAbbr<SILOneTypeOneOperandExtraAttributeLayout>();
  registerSILAbbr<SILInitExistentialLayout>();
  registerSILAbbr<SILOneTypeValuesLayout>();
  registerSILAbbr<SILOneTypeOwnershipValuesLayout>();
  registerSILAbbr<SILTwoOperandsLayout>();
  registerSILAbbr<SILTwoOperandsExtraAttributeLayout>();
  registerSILAbbr<SILTailAddrLayout>();
  registerSILAbbr<SILInstApplyLayout>();
  registerSILAbbr<SILInstNoOperandLayout>();
  registerSILAbbr<SILOneOperandLayout>();
  registerSILAbbr<SILTwoOperandsLayout>();
  registerSILAbbr<SILInstWitnessMethodLayout>();
  registerSILAbbr<SILSpecializeAttrLayout>();
  registerSILAbbr<SILArgEffectsAttrLayout>();
  registerSILAbbr<SILInstDifferentiableFunctionLayout>();
  registerSILAbbr<SILInstLinearFunctionLayout>();
  registerSILAbbr<SILInstDifferentiableFunctionExtractLayout>();
  registerSILAbbr<SILInstLinearFunctionExtractLayout>();
  registerSILAbbr<SILInstIncrementProfilerCounterLayout>();
  registerSILAbbr<SILInstHasSymbolLayout>();
  registerSILAbbr<SILOpenPackElementLayout>();
  registerSILAbbr<SILPackElementGetLayout>();
  registerSILAbbr<SILPackElementSetLayout>();

  registerSILAbbr<VTableLayout>();
  registerSILAbbr<VTableEntryLayout>();
  registerSILAbbr<MoveOnlyDeinitLayout>();
  registerSILAbbr<SILGlobalVarLayout>();
  registerSILAbbr<WitnessTableLayout>();
  registerSILAbbr<WitnessMethodEntryLayout>();
  registerSILAbbr<WitnessBaseEntryLayout>();
  registerSILAbbr<WitnessAssocProtocolLayout>();
  registerSILAbbr<WitnessAssocEntryLayout>();
  registerSILAbbr<WitnessConditionalConformanceLayout>();
  registerSILAbbr<DefaultWitnessTableLayout>();
  registerSILAbbr<DefaultWitnessTableNoEntryLayout>();
  registerSILAbbr<PropertyLayout>();
  registerSILAbbr<DifferentiabilityWitnessLayout>();

  // Write out VTables first because it may require serializations of
  // non-transparent SILFunctions (body is not needed).
  // Go through all SILVTables in SILMod and write them if we should
  // serialize everything.
  // FIXME: Resilience: could write out vtable for fragile classes.
  for (const auto &vt : SILMod->getVTables()) {
    if ((ShouldSerializeAll || vt->isSerialized()) &&
        SILMod->shouldSerializeEntitiesAssociatedWithDeclContext(vt->getClass()))
      writeSILVTable(*vt);
  }

  for (const auto &deinit : SILMod->getMoveOnlyDeinits()) {
    if ((ShouldSerializeAll || deinit->isSerialized()) &&
        SILMod->shouldSerializeEntitiesAssociatedWithDeclContext(
            deinit->getNominalDecl()))
      writeSILMoveOnlyDeinit(*deinit);
  }

  // Write out property descriptors.
  for (const SILProperty &prop : SILMod->getPropertyList()) {
    if ((ShouldSerializeAll || prop.isSerialized()) &&
        SILMod->shouldSerializeEntitiesAssociatedWithDeclContext(
                                     prop.getDecl()->getInnermostDeclContext()))
      writeSILProperty(prop);
  }

  // Write out fragile WitnessTables.
  for (const SILWitnessTable &wt : SILMod->getWitnessTables()) {
    if ((ShouldSerializeAll || wt.isSerialized()) &&
        SILMod->shouldSerializeEntitiesAssociatedWithDeclContext(
                                         wt.getConformance()->getDeclContext()))
      writeSILWitnessTable(wt);
  }

  // Write out DefaultWitnessTables.
  for (const SILDefaultWitnessTable &wt : SILMod->getDefaultWitnessTables()) {
    // FIXME: Don't need to serialize private and internal default witness
    // tables.
    if (SILMod->shouldSerializeEntitiesAssociatedWithDeclContext(
                                                              wt.getProtocol()))
      writeSILDefaultWitnessTable(wt);
  }

  // Add global variables that must be emitted to the list.
  for (const SILGlobalVariable &g : SILMod->getSILGlobals()) {
    if (g.isSerialized() || ShouldSerializeAll)
      addReferencedGlobalVariable(&g);
  }

  // Emit only declarations if it is a module with pre-specializations.
  // And only do it in optimized builds.
  bool emitDeclarationsForOnoneSupport =
      SILMod->isOptimizedOnoneSupportModule();

  // Go through all the SILFunctions in SILMod and write out any
  // mandatory function bodies.
  for (const SILFunction &F : *SILMod) {
    if (emitDeclarationsForOnoneSupport) {
      // Only declarations of hardcoded pre-specializations with
      // public linkage need to be serialized as they will be used
      // by the UsePrespecializations pass during -Onone compilation to
      // check for availability of concrete pre-specializations.
      if (!hasPublicVisibility(F.getLinkage()) ||
          !isKnownPrespecialization(F.getName()))
        continue;
    }

    addMandatorySILFunction(&F, emitDeclarationsForOnoneSupport);
    processWorklists();
  }

  // Write out differentiability witnesses.
  // Note: this must be done after visiting SIL functions above so that
  // differentiability witness references (`differentiability_witness_function`
  // instructions) have been tracked.
  for (const auto &diffWitness : SILMod->getDifferentiabilityWitnessList()) {
    // TODO(TF-893): Consider checking
    // `SILMod->shouldSerializeEntitiesAssociatedWithDeclContext` on the JVP/VJP
    // functions.
    if ((ShouldSerializeAll || diffWitness.isSerialized()))
      DifferentiabilityWitnessesToEmit.insert(&diffWitness);
  }
  for (auto *diffWitness : DifferentiabilityWitnessesToEmit)
    writeSILDifferentiabilityWitness(*diffWitness);
  // Process SIL functions referenced by differentiability witnesses.
  // Note: this is necessary despite processing `FuncsToEmit` below because
  // `Worklist` is processed separately.
  processWorklists();

  // Now write function declarations for every function we've
  // emitted a reference to without emitting a function body for.
  auto resilience = SILMod->getSwiftModule()->getResilienceStrategy();
  for (const SILFunction &F : *SILMod) {
    auto iter = FuncsToEmit.find(&F);
    if (iter != FuncsToEmit.end()) {
      if (iter->second) {
        assert((emitDeclarationsForOnoneSupport ||
                !shouldEmitFunctionBody(&F)) &&
               "Should have emitted function body earlier");
        writeSILFunction(F, true);
      }
    } else if (F.getLinkage() == SILLinkage::Public &&
               resilience != ResilienceStrategy::Resilient &&
               F.hasArgumentEffects()) {
      writeSILFunction(F, true);
    }
  }

  assert(functionWorklist.empty() && globalWorklist.empty() &&
         "Did not emit everything in worklists");
}

void SILSerializer::writeSILModule(const SILModule *SILMod) {
  writeSILBlock(SILMod);
  writeIndexTables();
}

void Serializer::writeSIL(const SILModule *SILMod, bool serializeAllSIL) {
  if (!SILMod)
    return;

  SILSerializer SILSer(*this, Out, serializeAllSIL);
  SILSer.writeSILModule(SILMod);
}
