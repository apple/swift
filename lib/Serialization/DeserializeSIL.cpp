//===--- DeserializeSIL.cpp - Read SIL ------------------------------------===//
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

#define DEBUG_TYPE "deserialize"

#include "DeserializeSIL.h"

#include "BCReadingExtras.h"
#include "DeserializationErrors.h"
#include "ModuleFile.h"
#include "SILFormat.h"
#include "SILSerializationFunctionBuilder.h"

#include "swift/AST/GenericSignature.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/PrettyStackTrace.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILMoveOnlyDeinit.h"
#include "swift/SIL/SILProperty.h"
#include "swift/SIL/SILUndef.h"

#include "swift/SIL/OwnershipUtils.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/DJB.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/OnDiskHashTable.h"

#include <type_traits>

using namespace swift;
using namespace swift::serialization;
using namespace swift::serialization::sil_block;
using namespace llvm::support;

const char SILEntityError::ID = '\0';
void SILEntityError::anchor() {}
const char SILFunctionTypeMismatch::ID = '\0';
void SILFunctionTypeMismatch::anchor() {}

STATISTIC(NumDeserializedFunc, "Number of deserialized SIL functions");

static Optional<StringLiteralInst::Encoding>
fromStableStringEncoding(unsigned value) {
  switch (value) {
  case SIL_BYTES: return StringLiteralInst::Encoding::Bytes;
  case SIL_UTF8: return StringLiteralInst::Encoding::UTF8;
  case SIL_OBJC_SELECTOR: return StringLiteralInst::Encoding::ObjCSelector;
  default: return None;
  }
}

static Optional<SILLinkage>
fromStableSILLinkage(unsigned value) {
  switch (value) {
  case SIL_LINKAGE_PUBLIC: return SILLinkage::Public;
  case SIL_LINKAGE_PUBLIC_NON_ABI: return SILLinkage::PublicNonABI;
  case SIL_LINKAGE_HIDDEN: return SILLinkage::Hidden;
  case SIL_LINKAGE_SHARED: return SILLinkage::Shared;
  case SIL_LINKAGE_PRIVATE: return SILLinkage::Private;
  case SIL_LINKAGE_PUBLIC_EXTERNAL: return SILLinkage::PublicExternal;
  case SIL_LINKAGE_HIDDEN_EXTERNAL: return SILLinkage::HiddenExternal;
  default: return None;
  }
}

static Optional<SILVTable::Entry::Kind>
fromStableVTableEntryKind(unsigned value) {
  switch (value) {
  case SIL_VTABLE_ENTRY_NORMAL: return SILVTable::Entry::Kind::Normal;
  case SIL_VTABLE_ENTRY_INHERITED: return SILVTable::Entry::Kind::Inherited;
  case SIL_VTABLE_ENTRY_OVERRIDE: return SILVTable::Entry::Kind::Override;
  default: return None;
  }
}

static Optional<swift::DifferentiabilityKind>
fromStableDifferentiabilityKind(uint8_t diffKind) {
  switch (diffKind) {
#define CASE(THE_DK) \
  case (uint8_t)serialization::DifferentiabilityKind::THE_DK: \
    return swift::DifferentiabilityKind::THE_DK;
  CASE(NonDifferentiable)
  CASE(Forward)
  CASE(Reverse)
  CASE(Normal)
  CASE(Linear)
#undef CASE
  default:
    return None;
  }
}

/// Used to deserialize entries in the on-disk func hash table.
class SILDeserializer::FuncTableInfo {
  ModuleFile &MF;

public:
  using internal_key_type = StringRef;
  using external_key_type = StringRef;
  using data_type = DeclID;
  using hash_value_type = uint32_t;
  using offset_type = unsigned;

  explicit FuncTableInfo(ModuleFile &MF) : MF(MF) {}

  internal_key_type GetInternalKey(external_key_type ID) { return ID; }

  external_key_type GetExternalKey(internal_key_type ID) { return ID; }

  hash_value_type ComputeHash(internal_key_type key) {
    return llvm::djbHash(key, SWIFTMODULE_HASH_SEED);
  }

  static bool EqualKey(internal_key_type lhs, internal_key_type rhs) {
    return lhs == rhs;
  }

  static std::pair<unsigned, unsigned> ReadKeyDataLength(const uint8_t *&data) {
    return { sizeof(uint32_t), sizeof(uint32_t) };
  }

  internal_key_type ReadKey(const uint8_t *data, unsigned length) {
    assert(length == sizeof(uint32_t) && "Expect a single IdentifierID.");
    IdentifierID keyID = endian::readNext<uint32_t, little, unaligned>(data);
    return MF.getIdentifierText(keyID);
  }

  static data_type ReadData(internal_key_type key, const uint8_t *data,
                            unsigned length) {
    assert(length == sizeof(uint32_t) && "Expect a single DeclID.");
    data_type result = endian::readNext<uint32_t, little, unaligned>(data);
    return result;
  }
};

SILDeserializer::SILDeserializer(
    ModuleFile *MF, SILModule &M,
    DeserializationNotificationHandlerSet *callback)
    : MF(MF), SILMod(M), Callback(callback) {

  SILCursor = MF->getSILCursor();
  SILIndexCursor = MF->getSILIndexCursor();
  // Early return if either sil block or sil index block does not exist.
  if (SILCursor.AtEndOfStream() || SILIndexCursor.AtEndOfStream())
    return;

  // Load any abbrev records at the start of the block.
  MF->fatalIfUnexpected(SILCursor.advance());

  llvm::BitstreamCursor cursor = SILIndexCursor;
  // We expect SIL_FUNC_NAMES first, then SIL_VTABLE_NAMES, then
  // SIL_GLOBALVAR_NAMES, then SIL_WITNESS_TABLE_NAMES, and finally
  // SIL_DEFAULT_WITNESS_TABLE_NAMES. But each one can be
  // omitted if no entries exist in the module file.
  unsigned kind = 0;
  while (kind != sil_index_block::SIL_PROPERTY_OFFSETS) {
    llvm::BitstreamEntry next = MF->fatalIfUnexpected(cursor.advance());
    if (next.Kind == llvm::BitstreamEntry::EndBlock)
      return;

    SmallVector<uint64_t, 4> scratch;
    StringRef blobData;
    unsigned prevKind = kind;
    kind =
        MF->fatalIfUnexpected(cursor.readRecord(next.ID, scratch, &blobData));
    assert((next.Kind == llvm::BitstreamEntry::Record && kind > prevKind &&
            (kind == sil_index_block::SIL_FUNC_NAMES ||
             kind == sil_index_block::SIL_VTABLE_NAMES ||
             kind == sil_index_block::SIL_MOVEONLYDEINIT_NAMES ||
             kind == sil_index_block::SIL_GLOBALVAR_NAMES ||
             kind == sil_index_block::SIL_WITNESS_TABLE_NAMES ||
             kind == sil_index_block::SIL_DEFAULT_WITNESS_TABLE_NAMES ||
             kind == sil_index_block::SIL_PROPERTY_OFFSETS ||
             kind == sil_index_block::SIL_DIFFERENTIABILITY_WITNESS_NAMES)) &&
           "Expect SIL_FUNC_NAMES, SIL_VTABLE_NAMES, SIL_GLOBALVAR_NAMES, \
          SIL_WITNESS_TABLE_NAMES, SIL_DEFAULT_WITNESS_TABLE_NAMES, \
          SIL_PROPERTY_OFFSETS, SIL_MOVEONLYDEINIT_NAMES, or SIL_DIFFERENTIABILITY_WITNESS_NAMES.");
    (void)prevKind;

    if (kind == sil_index_block::SIL_FUNC_NAMES)
      FuncTable = readFuncTable(scratch, blobData);
    else if (kind == sil_index_block::SIL_VTABLE_NAMES)
      VTableList = readFuncTable(scratch, blobData);
    else if (kind == sil_index_block::SIL_MOVEONLYDEINIT_NAMES)
      MoveOnlyDeinitList = readFuncTable(scratch, blobData);
    else if (kind == sil_index_block::SIL_GLOBALVAR_NAMES)
      GlobalVarList = readFuncTable(scratch, blobData);
    else if (kind == sil_index_block::SIL_WITNESS_TABLE_NAMES)
      WitnessTableList = readFuncTable(scratch, blobData);
    else if (kind == sil_index_block::SIL_DEFAULT_WITNESS_TABLE_NAMES)
      DefaultWitnessTableList = readFuncTable(scratch, blobData);
    else if (kind == sil_index_block::SIL_DIFFERENTIABILITY_WITNESS_NAMES)
      DifferentiabilityWitnessList = readFuncTable(scratch, blobData);
    else if (kind == sil_index_block::SIL_PROPERTY_OFFSETS) {
      // No matching 'names' block for property descriptors needed yet.
      MF->allocateBuffer(Properties, scratch);
      return;
    }

    // Read SIL_FUNC|VTABLE|GLOBALVAR_OFFSETS record.
    next = MF->fatalIfUnexpected(cursor.advance());
    scratch.clear();
    unsigned offKind =
        MF->fatalIfUnexpected(cursor.readRecord(next.ID, scratch, &blobData));
    (void)offKind;
    if (kind == sil_index_block::SIL_FUNC_NAMES) {
      assert((next.Kind == llvm::BitstreamEntry::Record &&
              offKind == sil_index_block::SIL_FUNC_OFFSETS) &&
             "Expect a SIL_FUNC_OFFSETS record.");
      MF->allocateBuffer(Funcs, scratch);
    } else if (kind == sil_index_block::SIL_VTABLE_NAMES) {
      assert((next.Kind == llvm::BitstreamEntry::Record &&
              offKind == sil_index_block::SIL_VTABLE_OFFSETS) &&
             "Expect a SIL_VTABLE_OFFSETS record.");
      MF->allocateBuffer(VTables, scratch);
    } else if (kind == sil_index_block::SIL_MOVEONLYDEINIT_NAMES) {
      assert((next.Kind == llvm::BitstreamEntry::Record &&
              offKind == sil_index_block::SIL_MOVEONLYDEINIT_OFFSETS) &&
             "Expect a SIL_MOVEONLYDEINIT_OFFSETS record.");
      MF->allocateBuffer(MoveOnlyDeinits, scratch);
    } else if (kind == sil_index_block::SIL_GLOBALVAR_NAMES) {
      assert((next.Kind == llvm::BitstreamEntry::Record &&
              offKind == sil_index_block::SIL_GLOBALVAR_OFFSETS) &&
             "Expect a SIL_GLOBALVAR_OFFSETS record.");
      MF->allocateBuffer(GlobalVars, scratch);
    } else if (kind == sil_index_block::SIL_WITNESS_TABLE_NAMES) {
      assert((next.Kind == llvm::BitstreamEntry::Record &&
              offKind == sil_index_block::SIL_WITNESS_TABLE_OFFSETS) &&
             "Expect a SIL_WITNESS_TABLE_OFFSETS record.");
      MF->allocateBuffer(WitnessTables, scratch);
    } else if (kind == sil_index_block::SIL_DEFAULT_WITNESS_TABLE_NAMES) {
      assert((next.Kind == llvm::BitstreamEntry::Record &&
              offKind == sil_index_block::SIL_DEFAULT_WITNESS_TABLE_OFFSETS) &&
             "Expect a SIL_DEFAULT_WITNESS_TABLE_OFFSETS record.");
      MF->allocateBuffer(DefaultWitnessTables, scratch);
    } else if (kind == sil_index_block::SIL_DIFFERENTIABILITY_WITNESS_NAMES) {
      assert((next.Kind == llvm::BitstreamEntry::Record &&
              offKind ==
                  sil_index_block::SIL_DIFFERENTIABILITY_WITNESS_OFFSETS) &&
             "Expect a SIL_DIFFERENTIABILITY_WITNESS_OFFSETS record.");
      MF->allocateBuffer(DifferentiabilityWitnesses, scratch);
    }
  }
}

std::unique_ptr<SILDeserializer::SerializedFuncTable>
SILDeserializer::readFuncTable(ArrayRef<uint64_t> fields, StringRef blobData) {
  uint32_t tableOffset;
  sil_index_block::ListLayout::readRecord(fields, tableOffset);
  auto base = reinterpret_cast<const uint8_t *>(blobData.data());

  using OwnedTable = std::unique_ptr<SerializedFuncTable>;
  return OwnedTable(SerializedFuncTable::Create(base + tableOffset,
                                                base + sizeof(uint32_t), base,
                                                FuncTableInfo(*MF)));
}

/// A high-level overview of how forward references work in serializer and
/// deserializer:
/// In the serializer, we pre-assign a value ID in order, to each basic block
/// argument and each SILInstruction that has a value.
/// In the deserializer, we create a PlaceholderValue for a forward-referenced
/// value (a value that is used but not yet defined). LocalValues are updated in
/// setLocalValue where the ID passed in assumes the same ordering as in
/// serializer: in-order for each basic block argument and each SILInstruction
/// that has a value.
/// When a forward-referenced value is defined, it replaces the PlaceholderValue
/// in LocalValues.
void SILDeserializer::setLocalValue(ValueBase *Value, ValueID Id) {
  ValueBase *&Entry = LocalValues[Id];

  if (auto *placeholder = dyn_cast_or_null<PlaceholderValue>(Entry)) {
    placeholder->replaceAllUsesWith(Value);
    ::delete placeholder;
  } else {
    assert(!Entry && "We should not redefine the same value.");
  }

  // Store it in our map.
  Entry = Value;
}

SILValue SILDeserializer::getLocalValue(ValueID Id,
                                        SILType Type) {
  // The first two IDs are special undefined values.
  if (Id == 0)
    return SILUndef::get(Type, SILMod);
  assert(Id != 1 && "This used to be for SILUndef with OwnershipKind::Owned... "
                    "but we don't support that anymore. Make sure no one "
                    "changes that without updating this code if needed");

  // Check to see if this is already defined.
  ValueBase *&Entry = LocalValues[Id];
  if (!Entry) {
    // Otherwise, this is a forward reference.  Create a dummy node to represent
    // it until we see a real definition.
    Entry = ::new PlaceholderValue(Type);
  }
  // If this value was already defined, check it to make sure types match.
  assert(Entry->getType() == Type && "Value Type mismatch?");
  return Entry;
}

/// Return the SILBasicBlock of a given ID.
SILBasicBlock *SILDeserializer::getBBForDefinition(SILFunction *Fn,
                                                   SILBasicBlock *Prev,
                                                   unsigned ID) {
  SILBasicBlock *&BB = BlocksByID[ID];
  // If the block has never been named yet, just create it.
  if (BB == nullptr) {
    if (Prev) {
      BB = Fn->createBasicBlockAfter(Prev);      
    } else {
      BB = Fn->createBasicBlock();
    }
    return BB;
  }

  // If it already exists, it was either a forward reference or a redefinition.
  // The latter should never happen.
  bool wasForwardReferenced = UndefinedBlocks.erase(BB);
  assert(wasForwardReferenced);
  (void)wasForwardReferenced;

  if (Prev)
    Fn->moveBlockAfter(BB, Prev);
  return BB;
}

/// Return the SILBasicBlock of a given ID.
SILBasicBlock *SILDeserializer::getBBForReference(SILFunction *Fn,
                                                  unsigned ID) {
  SILBasicBlock *&BB = BlocksByID[ID];
  if (BB != nullptr)
    return BB;

  // Otherwise, create it and remember that this is a forward reference
  BB = Fn->createBasicBlock();
  UndefinedBlocks[BB] = ID;
  return BB;
}

/// Helper function to convert from Type to SILType.
SILType SILDeserializer::getSILType(Type Ty, SILValueCategory Category,
                                    SILFunction *inContext) {
  auto TyLoc = TypeLoc::withoutLoc(Ty);
  if (!inContext) {
    return SILType::getPrimitiveType(TyLoc.getType()->getCanonicalType(),
                                     Category);
  }
  return inContext->getLoweredType(TyLoc.getType()->getCanonicalType())
      .getCategoryType(Category);
}

/// Helper function to find a SILDifferentiabilityWitness, given its mangled
/// key.
SILDifferentiabilityWitness *
SILDeserializer::getSILDifferentiabilityWitnessForReference(
    StringRef mangledKey) {
  // Check to see if we have a witness under this key already.
  auto *witness = SILMod.lookUpDifferentiabilityWitness(mangledKey);
  if (witness)
    return witness;
  // Otherwise, look for a witness under this key in the module.
  if (!DifferentiabilityWitnessList)
    return nullptr;
  auto iter = DifferentiabilityWitnessList->find(mangledKey);
  if (iter == DifferentiabilityWitnessList->end())
    return nullptr;
  return readDifferentiabilityWitness(*iter);
}

/// Helper function to find a SILFunction, given its name and type.
SILFunction *SILDeserializer::getFuncForReference(StringRef name,
                                                  SILType type) {
  // Check to see if we have a function by this name already.
  SILFunction *fn = SILMod.lookUpFunction(name);
  if (!fn) {
    // Otherwise, look for a function with this name in the module.
    auto iter = FuncTable->find(name);
    if (iter != FuncTable->end()) {
      auto maybeFn = readSILFunctionChecked(*iter, nullptr, name,
                                            /*declarationOnly*/ true);
      if (maybeFn) {
        fn = maybeFn.get();
      } else {
        // Ignore the failure; we'll synthesize a bogus function instead.
        llvm::consumeError(maybeFn.takeError());
      }
    }
  }

  // FIXME: check for matching types.

  // At this point, if fn is set, we know that we have a good function to use.
  if (fn)
    return fn;

  // Otherwise, create a function declaration with the right type and a bogus
  // source location. This ensures that we can at least parse the rest of the
  // SIL.
  SourceLoc sourceLoc;
  SILSerializationFunctionBuilder builder(SILMod);
  fn = builder.createDeclaration(name, type,
                                 RegularLocation(sourceLoc));
  // The function is not really de-serialized, but it's important to call
  // `didDeserialize` on every new function. Otherwise some Analysis might miss
  // `notifyAddedOrModifiedFunction` notifications.
  if (Callback)
    Callback->didDeserialize(MF->getAssociatedModule(), fn);
  return fn;
}

/// Helper function to find a SILFunction, given its name and type.
SILFunction *SILDeserializer::getFuncForReference(StringRef name) {
  // Check to see if we have a function by this name already.
  SILFunction *fn = SILMod.lookUpFunction(name);
  if (fn)
    return fn;

  // Otherwise, look for a function with this name in the module.
  auto iter = FuncTable->find(name);
  if (iter == FuncTable->end())
    return nullptr;

  auto maybeFn = readSILFunctionChecked(*iter, nullptr, name,
                                        /*declarationOnly*/ true);
  if (!maybeFn) {
    // Ignore the failure and just pretend the function doesn't exist
    llvm::consumeError(maybeFn.takeError());
    return nullptr;
  }

  return maybeFn.get();
}

/// Helper function to find a SILGlobalVariable given its name. It first checks
/// in the module. If we cannot find it in the module, we attempt to
/// deserialize it.
SILGlobalVariable *SILDeserializer::getGlobalForReference(StringRef name) {
  // Check to see if we have a global by this name already.
  if (SILGlobalVariable *g = SILMod.lookUpGlobalVariable(name))
    return g;

  // Otherwise, look for a global with this name in the module.
  return readGlobalVar(name);
}

/// Deserialize a SILFunction if it is not already deserialized. The input
/// SILFunction can either be an empty declaration or null. If it is an empty
/// declaration, we fill in the contents. If the input SILFunction is
/// null, we create a SILFunction.
SILFunction *SILDeserializer::readSILFunction(DeclID FID,
                                              SILFunction *existingFn,
                                              StringRef name,
                                              bool declarationOnly,
                                              bool errorIfEmptyBody) {
  llvm::Expected<SILFunction *> deserialized =
      readSILFunctionChecked(FID, existingFn, name, declarationOnly,
                             errorIfEmptyBody);
  if (!deserialized) {
    MF->fatal(deserialized.takeError());
  }
  return deserialized.get();
}

llvm::Expected<SILFunction *>
SILDeserializer::readSILFunctionChecked(DeclID FID, SILFunction *existingFn,
                                        StringRef name, bool declarationOnly,
                                        bool errorIfEmptyBody) {
  // We can't deserialize function bodies after IRGen lowering passes have
  // happened since other definitions in the module will no longer be in
  // canonical SIL form.
  switch (SILMod.getStage()) {
  case SILStage::Raw:
  case SILStage::Canonical:
    break;
    
  case SILStage::Lowered:
    llvm_unreachable("cannot deserialize into a module that has entered "
                     "Lowered stage");
  }
  
  if (FID == 0)
    return nullptr;
  assert(FID <= Funcs.size() && "invalid SILFunction ID");

  PrettyStackTraceStringAction trace("deserializing SIL function", name);

  auto &cacheEntry = Funcs[FID-1];
  if (cacheEntry.isFullyDeserialized() ||
      (cacheEntry.isDeserialized() && declarationOnly))
    return cacheEntry.get();

  BCOffsetRAII restoreOffset(SILCursor);
  if (llvm::Error Err = SILCursor.JumpToBit(cacheEntry.getOffset()))
    return std::move(Err);

  llvm::Expected<llvm::BitstreamEntry> maybeEntry =
      SILCursor.advance(AF_DontPopBlockAtEnd);
  if (!maybeEntry)
    return maybeEntry.takeError();
  llvm::BitstreamEntry entry = maybeEntry.get();
  if (entry.Kind == llvm::BitstreamEntry::Error)
    return MF->diagnoseFatal("Cursor advance error in readSILFunction");

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  llvm::Expected<unsigned> maybeKind =
      SILCursor.readRecord(entry.ID, scratch, &blobData);
  if (!maybeKind)
    return MF->diagnoseFatal(maybeKind.takeError());
  unsigned kind = maybeKind.get();
  assert(kind == SIL_FUNCTION && "expect a sil function");
  (void)kind;

  DeclID clangNodeOwnerID;
  ModuleID parentModuleID;
  TypeID funcTyID;
  IdentifierID replacedFunctionID;
  IdentifierID usedAdHocWitnessFunctionID;
  GenericSignatureID genericSigID;
  unsigned rawLinkage, isTransparent, isSerialized, isThunk,
      isWithoutActuallyEscapingThunk, specialPurpose, inlineStrategy,
      optimizationMode, perfConstr, subclassScope, hasCReferences, effect,
      numAttrs, hasQualifiedOwnership, isWeakImported,
      LIST_VER_TUPLE_PIECES(available), isDynamic, isExactSelfClass,
      isDistributed, isRuntimeAccessible, forceEnableLexicalLifetimes;
  ArrayRef<uint64_t> SemanticsIDs;
  SILFunctionLayout::readRecord(
      scratch, rawLinkage, isTransparent, isSerialized, isThunk,
      isWithoutActuallyEscapingThunk, specialPurpose, inlineStrategy,
      optimizationMode, perfConstr, subclassScope, hasCReferences, effect,
      numAttrs, hasQualifiedOwnership, isWeakImported,
      LIST_VER_TUPLE_PIECES(available), isDynamic, isExactSelfClass,
      isDistributed, isRuntimeAccessible, forceEnableLexicalLifetimes, funcTyID,
      replacedFunctionID, usedAdHocWitnessFunctionID, genericSigID,
      clangNodeOwnerID, parentModuleID, SemanticsIDs);

  if (funcTyID == 0)
    return MF->diagnoseFatal("SILFunction typeID is 0");
  auto astType = MF->getTypeChecked(funcTyID);
  if (!astType) {
    if (!existingFn || errorIfEmptyBody) {
      return llvm::make_error<SILEntityError>(
          name, takeErrorInfo(astType.takeError()));
    }
    consumeError(astType.takeError());
    return existingFn;
  }
  auto ty = getSILType(astType.get(), SILValueCategory::Object, nullptr);
  if (!ty.is<SILFunctionType>())
    return MF->diagnoseFatal("not a function type for SILFunction");

  SILFunction *replacedFunction = nullptr;
  Identifier replacedObjectiveCFunc;
  if (replacedFunctionID &&
      ty.getAs<SILFunctionType>()->getExtInfo().getRepresentation() !=
          SILFunctionTypeRepresentation::ObjCMethod) {
    replacedFunction =
        getFuncForReference(MF->getIdentifier(replacedFunctionID).str());
  } else if (replacedFunctionID) {
    replacedObjectiveCFunc = MF->getIdentifier(replacedFunctionID);
  }

  SILFunction *usedAdHocWitnessFunction = nullptr;
  if (usedAdHocWitnessFunctionID) {
    auto usedAdHocWitnessFunctionStr =
        MF->getIdentifier(usedAdHocWitnessFunctionID).str();
    usedAdHocWitnessFunction = getFuncForReference(usedAdHocWitnessFunctionStr);
  }

  auto linkageOpt = fromStableSILLinkage(rawLinkage);
  if (!linkageOpt) {
    LLVM_DEBUG(llvm::dbgs() << "invalid linkage code " << rawLinkage
                            << " for SILFunction\n");
    return MF->diagnoseFatal("invalid linkage code");
  }
  SILLinkage linkage = linkageOpt.value();

  ValueDecl *clangNodeOwner = nullptr;
  if (clangNodeOwnerID != 0) {
    clangNodeOwner = dyn_cast_or_null<ValueDecl>(MF->getDecl(clangNodeOwnerID));
    if (!clangNodeOwner)
      return MF->diagnoseFatal("invalid clang node owner for SILFunction");
  }

  // If we weren't handed a function, check for an existing
  // declaration in the output module.
  if (!existingFn) existingFn = SILMod.lookUpFunction(name);
  auto fn = existingFn;

  // TODO: use the correct SILLocation from module.
  SILLocation loc = RegularLocation::getAutoGeneratedLocation();

  // If we've already serialized the module, don't mark the function
  // as serialized, since we no longer need to enforce resilience
  // boundaries.
  if (SILMod.isSerialized())
    isSerialized = IsNotSerialized;

  SILSerializationFunctionBuilder builder(SILMod);

  // If we have an existing function, verify that the types match up.
  if (fn) {
    if (fn->getLoweredType() != ty) {
      auto error = llvm::make_error<SILFunctionTypeMismatch>(
                     name,
                     fn->getLoweredType().getDebugDescription(),
                     ty.getDebugDescription());
      return MF->diagnoseFatal(std::move(error));
    }

    fn->setSerialized(IsSerialized_t(isSerialized));

    // If the serialized function comes from the same module, we're merging
    // modules, and can update the linkage directly. This is needed to
    // correctly update the linkage for forward declarations to entities defined
    // in another file of the same module – we want to ensure the linkage
    // reflects the fact that the entity isn't really external and shouldn't be
    // dropped from the resulting merged module.
    if (getFile()->getParentModule() == SILMod.getSwiftModule())
      fn->setLinkage(linkage);

    if (getFile()->getParentModule()->isStaticLibrary() ||
        getFile()->getParentModule() == SILMod.getSwiftModule())
      fn->setIsStaticallyLinked(true);

    // Don't override the transparency or linkage of a function with
    // an existing declaration, except if we deserialized a
    // PublicNonABI function, which has HiddenExternal when
    // referenced as a declaration, and Shared when it has
    // a deserialized body.
    if (isAvailableExternally(fn->getLinkage())) {
      if (linkage == SILLinkage::PublicNonABI || linkage == SILLinkage::Shared) {
        fn->setLinkage(SILLinkage::Shared);
      } else if (hasPublicVisibility(linkage)) {
        // Cross-module-optimization can change the linkage to public. In this
        // case we need to update the linkage of the function (which is
        // originally just derived from the AST).
        fn->setLinkage(SILLinkage::PublicExternal);
      }
    }

    if (fn->isDynamicallyReplaceable() != isDynamic)
      return MF->diagnoseFatal("SILFunction dynamic replaceable mismatch");

  } else {
    // Otherwise, create a new function.
    fn = builder.createDeclaration(name, ty, loc);
    fn->setLinkage(linkage);
    fn->setTransparent(IsTransparent_t(isTransparent == 1));
    fn->setSerialized(IsSerialized_t(isSerialized));
    fn->setThunk(IsThunk_t(isThunk));
    fn->setWithoutActuallyEscapingThunk(bool(isWithoutActuallyEscapingThunk));
    fn->setInlineStrategy(Inline_t(inlineStrategy));
    fn->setSpecialPurpose(SILFunction::Purpose(specialPurpose));
    fn->setEffectsKind(EffectsKind(effect));
    fn->setOptimizationMode(OptimizationMode(optimizationMode));
    fn->setPerfConstraints((PerformanceConstraints)perfConstr);
    fn->setIsAlwaysWeakImported(isWeakImported);
    fn->setClassSubclassScope(SubclassScope(subclassScope));
    fn->setHasCReferences(bool(hasCReferences));

    llvm::VersionTuple available;
    DECODE_VER_TUPLE(available);
    fn->setAvailabilityForLinkage(
      available.empty()
      ? AvailabilityContext::alwaysAvailable()
      : AvailabilityContext(VersionRange::allGTE(available)));

    fn->setIsDynamic(IsDynamicallyReplaceable_t(isDynamic));
    fn->setIsExactSelfClass(IsExactSelfClass_t(isExactSelfClass));
    fn->setIsDistributed(IsDistributed_t(isDistributed));
    fn->setIsRuntimeAccessible(IsRuntimeAccessible_t(isRuntimeAccessible));
    fn->setForceEnableLexicalLifetimes(
        ForceEnableLexicalLifetimes_t(forceEnableLexicalLifetimes));
    if (replacedFunction)
      fn->setDynamicallyReplacedFunction(replacedFunction);
    if (!replacedObjectiveCFunc.empty())
      fn->setObjCReplacement(replacedObjectiveCFunc);
    if (usedAdHocWitnessFunction)
      fn->setReferencedAdHocRequirementWitnessFunction(usedAdHocWitnessFunction);
    if (clangNodeOwner)
      fn->setClangNodeOwner(clangNodeOwner);
    for (auto ID : SemanticsIDs) {
      fn->addSemanticsAttr(MF->getIdentifierText(ID));
    }
    if (Callback) Callback->didDeserialize(MF->getAssociatedModule(), fn);
  }

  // First before we do /anything/ validate that our function is truly empty.
  assert(fn->empty() && "SILFunction to be deserialized starts being empty.");

  // Given that our original function was empty, just match the deserialized
  // function. Ownership doesn't really have a meaning without a body.
  builder.setHasOwnership(fn, hasQualifiedOwnership);

  // Mark this function as deserialized. This avoids rerunning diagnostic
  // passes. Certain passes in the mandatory pipeline may not work as expected
  // after arbitrary optimization and lowering.
  if (!MF->isSIB())
    fn->setWasDeserializedCanonical();

  fn->setBare(IsBare);
  if (!fn->getDebugScope()) {
    const SILDebugScope *DS = new (SILMod) SILDebugScope(loc, fn);
    fn->setDebugScope(DS);
  }

  // If we don't already have a DeclContext to use to find a parent module,
  // attempt to deserialize a parent module reference directly.
  if (!fn->getDeclContext() && parentModuleID)
    fn->setParentModule(MF->getModule(parentModuleID));

  // Read and instantiate the specialize attributes.
  bool shouldAddSpecAttrs = fn->getSpecializeAttrs().empty();
  bool shouldAddEffectAttrs = !fn->hasArgumentEffects();
  for (unsigned attrIdx = 0; attrIdx < numAttrs; ++attrIdx) {
    llvm::Expected<llvm::BitstreamEntry> maybeNext =
        SILCursor.advance(AF_DontPopBlockAtEnd);
    if (!maybeNext)
      return maybeNext.takeError();
    llvm::BitstreamEntry next = maybeNext.get();
    assert(next.Kind == llvm::BitstreamEntry::Record);

    scratch.clear();
    llvm::Expected<unsigned> maybeKind = SILCursor.readRecord(next.ID, scratch);
    if (!maybeKind)
      return maybeKind.takeError();
    unsigned kind = maybeKind.get();
    
    if (kind == SIL_ARG_EFFECTS_ATTR) {
      IdentifierID effectID;
      unsigned isDerived;
      unsigned isGlobalSideEffects;
      unsigned argumentIndex;
      SILArgEffectsAttrLayout::readRecord(scratch, effectID, argumentIndex,
                                          isGlobalSideEffects, isDerived);
      if (shouldAddEffectAttrs) {
        StringRef effectStr = MF->getIdentifierText(effectID);
        std::pair<const char *, int> error;
        if (isGlobalSideEffects) {
          error = fn->parseGlobalEffectsFromSIL(effectStr);
        } else {
          error = fn->parseArgumentEffectsFromSIL(effectStr, (int)argumentIndex);
        }
        (void)error;
        assert(!error.first && "effects deserialization error");
      }
      continue;
    }

    assert(kind == SIL_SPECIALIZE_ATTR && "Missing specialization attribute");

    unsigned exported;
    unsigned specializationKindVal;
    GenericSignatureID specializedSigID;
    IdentifierID targetFunctionID;
    IdentifierID spiGroupID;
    ModuleID spiModuleID;
    ArrayRef<uint64_t> typeErasedParamsIDs;
    unsigned LIST_VER_TUPLE_PIECES(available);
    SILSpecializeAttrLayout::readRecord(
        scratch, exported, specializationKindVal, specializedSigID,
        targetFunctionID, spiGroupID, spiModuleID,
        LIST_VER_TUPLE_PIECES(available), typeErasedParamsIDs);

    SILFunction *target = nullptr;
    if (targetFunctionID) {
      target = getFuncForReference(MF->getIdentifier(targetFunctionID).str());
    }

    Identifier spiGroup;
    const ModuleDecl *spiModule = nullptr;
    if (spiGroupID) {
      spiGroup = MF->getIdentifier(spiGroupID);
      spiModule = MF->getModule(spiModuleID);
    }

    SILSpecializeAttr::SpecializationKind specializationKind =
        specializationKindVal ? SILSpecializeAttr::SpecializationKind::Partial
                              : SILSpecializeAttr::SpecializationKind::Full;

    llvm::VersionTuple available;
    DECODE_VER_TUPLE(available);
    auto availability = available.empty()
      ? AvailabilityContext::alwaysAvailable()
      : AvailabilityContext(VersionRange::allGTE(available));

    llvm::SmallVector<Type, 4> typeErasedParams;
    for (auto id : typeErasedParamsIDs) {
      typeErasedParams.push_back(MF->getType(id));
    }

    auto specializedSig = MF->getGenericSignature(specializedSigID);
    // Only add the specialize attributes once.
    if (shouldAddSpecAttrs) {
      // Read the substitution list and construct a SILSpecializeAttr.
      fn->addSpecializeAttr(SILSpecializeAttr::create(
          SILMod, specializedSig, typeErasedParams,
          exported != 0, specializationKind, target,
          spiGroup, spiModule, availability));
    }
  }

  GenericEnvironment *genericEnv = nullptr;
  if (!declarationOnly)
    genericEnv = MF->getGenericSignature(genericSigID).getGenericEnvironment();

  // If the next entry is the end of the block, then this function has
  // no contents.
  maybeEntry = SILCursor.advance(AF_DontPopBlockAtEnd);
  if (!maybeEntry)
    return maybeEntry.takeError();
  entry = maybeEntry.get();
  bool isEmptyFunction = (entry.Kind == llvm::BitstreamEntry::EndBlock);
  assert((!isEmptyFunction || !genericEnv) &&
         "generic environment without body?!");

  // Remember this in our cache in case it's a recursive function.
  // Increase the reference count to keep it alive.
  bool isFullyDeserialized = (isEmptyFunction || !declarationOnly);
  if (cacheEntry.isDeserialized()) {
    assert(fn == cacheEntry.get() && "changing SIL function during deserialization!");
  } else {
    fn->incrementRefCount();
  }
  cacheEntry.set(fn, isFullyDeserialized);

  // Stop here if we have nothing else to do.
  if (isEmptyFunction || declarationOnly) {
    return fn;
  }

  ++NumDeserializedFunc;

  assert(!(fn->getGenericEnvironment() && !fn->empty())
         && "function already has context generic params?!");
  if (genericEnv)
    fn->setGenericEnvironment(genericEnv);

  scratch.clear();
  maybeKind = SILCursor.readRecord(entry.ID, scratch);
  if (!maybeKind)
    return maybeKind.takeError();
  kind = maybeKind.get();

  SILBasicBlock *CurrentBB = nullptr;

  // Clear up at the beginning of each SILFunction.
  BasicBlockID = 0;
  BlocksByID.clear();
  UndefinedBlocks.clear();

  // The first two IDs are reserved for SILUndef.
  LastValueID = 1;
  LocalValues.clear();

  SILBuilder Builder(*fn);

  // Another SIL_FUNCTION record means the end of this SILFunction.
  // SIL_VTABLE or SIL_GLOBALVAR or SIL_WITNESS_TABLE record also means the end
  // of this SILFunction.
  while (kind != SIL_FUNCTION && kind != SIL_VTABLE && kind != SIL_GLOBALVAR &&
         kind != SIL_MOVEONLY_DEINIT && kind != SIL_WITNESS_TABLE &&
         kind != SIL_DIFFERENTIABILITY_WITNESS) {
    if (kind == SIL_BASIC_BLOCK)
      // Handle a SILBasicBlock record.
      CurrentBB = readSILBasicBlock(fn, CurrentBB, scratch);
    else {
      // If CurrentBB is empty, just return fn. The code in readSILInstruction
      // assumes that such a situation means that fn is a declaration. Thus it
      // is using return false to mean two different things, error a failure
      // occurred and this is a declaration. Work around that for now.
      if (!CurrentBB)
        return fn;
      Builder.setInsertionPoint(CurrentBB);

      // Handle a SILInstruction record.
      if (readSILInstruction(fn, Builder, kind, scratch))
        return MF->diagnoseFatal("readSILInstruction returns error");
    }

    // Fetch the next record.
    scratch.clear();
    llvm::Expected<llvm::BitstreamEntry> maybeEntry =
        SILCursor.advance(AF_DontPopBlockAtEnd);
    if (!maybeEntry)
      return maybeEntry.takeError();
    llvm::BitstreamEntry entry = maybeEntry.get();

    // EndBlock means the end of this SILFunction.
    if (entry.Kind == llvm::BitstreamEntry::EndBlock)
      break;
    maybeKind = SILCursor.readRecord(entry.ID, scratch);
    if (!maybeKind)
      return maybeKind.takeError();
    kind = maybeKind.get();
  }

  // If fn is empty, we failed to deserialize its body. Return nullptr to signal
  // error.
  if (fn->empty() && errorIfEmptyBody)
    return nullptr;

  // Check that there are no unresolved forward definitions of local
  // archetypes.
  if (SILMod.hasUnresolvedLocalArchetypeDefinitions())
    llvm_unreachable(
        "All forward definitions of local archetypes should be resolved");

  if (Callback)
    Callback->didDeserializeFunctionBody(MF->getAssociatedModule(), fn);

  if (!MF->isSIB() && !SILMod.isSerialized()) {
    assert((fn->isSerialized() || fn->empty()) &&
           "deserialized function must have the IsSerialized flag set");
  }
  return fn;
}

// We put these static asserts here to formalize our assumption that both
// SILValueCategory and ValueOwnershipKind have uint8_t as their underlying
// pointer values.
static_assert(
    std::is_same<std::underlying_type<SILValueCategory>::type, uint8_t>::value,
    "Expected an underlying uint8_t type");
// We put these static asserts here to formalize our assumption that both
// SILValueCategory and ValueOwnershipKind have uint8_t as their underlying
// pointer values.
static_assert(std::is_same<std::underlying_type<OwnershipKind::innerty>::type,
                           uint8_t>::value,
              "Expected an underlying uint8_t type");
SILBasicBlock *SILDeserializer::readSILBasicBlock(SILFunction *Fn,
                                                  SILBasicBlock *Prev,
                                    SmallVectorImpl<uint64_t> &scratch) {
  ArrayRef<uint64_t> Args;
  SILBasicBlockLayout::readRecord(scratch, Args);

  // Args should be a list of triples of the following form:
  //
  // 1. A TypeID.
  // 2. A flag of metadata. This currently includes the SILValueCategory and
  //    ValueOwnershipKind. We enforce size constraints of these types above.
  // 3. A ValueID.
  SILBasicBlock *CurrentBB = getBBForDefinition(Fn, Prev, BasicBlockID++);
  bool IsEntry = CurrentBB->isEntry();
  for (unsigned I = 0, E = Args.size(); I < E; I += 3) {
    TypeID TyID = Args[I];
    if (!TyID) return nullptr;
    ValueID ValId = Args[I+2];
    if (!ValId) return nullptr;

    auto ArgTy = MF->getType(TyID);
    SILArgument *Arg;
    auto ValueCategory = SILValueCategory(Args[I + 1] & 0xF);
    SILType SILArgTy = getSILType(ArgTy, ValueCategory, Fn);
    auto OwnershipKind = ValueOwnershipKind((Args[I + 1] >> 8) & 0x7);
    auto reborrow = (Args[I + 1] >> 11) & 0x1;
    auto escaping = (Args[I + 1] >> 12) & 0x1;
    if (IsEntry) {
      auto *fArg = CurrentBB->createFunctionArgument(SILArgTy);
      bool isNoImplicitCopy = (Args[I + 1] >> 13) & 0x1;
      fArg->setNoImplicitCopy(isNoImplicitCopy);
      auto lifetime = (LifetimeAnnotation::Case)((Args[I + 1] >> 14) & 0x3);
      fArg->setLifetimeAnnotation(lifetime);
      bool isClosureCapture = (Args[I + 1] >> 16) & 0x1;
      fArg->setClosureCapture(isClosureCapture);
      bool isFormalParameterPack = (Args[I + 1] >> 17) & 0x1;
      fArg->setFormalParameterPack(isFormalParameterPack);
      Arg = fArg;
    } else {
      Arg = CurrentBB->createPhiArgument(SILArgTy, OwnershipKind,
                                         /*decl*/ nullptr, reborrow, escaping);
    }
    LastValueID = LastValueID + 1;
    setLocalValue(Arg, LastValueID);
  }
  return CurrentBB;
}

static CastConsumptionKind getCastConsumptionKind(unsigned attr) {
  switch (attr) {
  case SIL_CAST_CONSUMPTION_TAKE_ALWAYS:
    return CastConsumptionKind::TakeAlways;
  case SIL_CAST_CONSUMPTION_TAKE_ON_SUCCESS:
    return CastConsumptionKind::TakeOnSuccess;
  case SIL_CAST_CONSUMPTION_COPY_ON_SUCCESS:
    return CastConsumptionKind::CopyOnSuccess;
  case SIL_CAST_CONSUMPTION_BORROW_ALWAYS:
    return CastConsumptionKind::BorrowAlways;
  default:
    llvm_unreachable("not a valid CastConsumptionKind for SIL");
  }
}

/// Construct a SILDeclRef from ListOfValues.
static SILDeclRef getSILDeclRef(ModuleFile *MF,
                                ArrayRef<uint64_t> ListOfValues,
                                unsigned &NextIdx) {
  assert(ListOfValues.size() >= NextIdx+3 &&
         "Expect 3 numbers for SILDeclRef");
  SILDeclRef DRef(cast<ValueDecl>(MF->getDecl(ListOfValues[NextIdx])),
                  (SILDeclRef::Kind)ListOfValues[NextIdx+1],
                  /*isForeign=*/ListOfValues[NextIdx+2] > 0);
  NextIdx += 3;
  return DRef;
}

Optional<KeyPathPatternComponent>
SILDeserializer::readKeyPathComponent(ArrayRef<uint64_t> ListOfValues,
                                      unsigned &nextValue) {
  auto kind =
    (KeyPathComponentKindEncoding)ListOfValues[nextValue++];
  
  if (kind == KeyPathComponentKindEncoding::Trivial)
    return None;
  
  auto type = MF->getType(ListOfValues[nextValue++])
    ->getCanonicalType();

  auto handleComputedId =
  [&]() -> KeyPathPatternComponent::ComputedPropertyId {
    auto kind =
      (KeyPathComputedComponentIdKindEncoding)ListOfValues[nextValue++];
    switch (kind) {
    case KeyPathComputedComponentIdKindEncoding::Property:
      return cast<VarDecl>(MF->getDecl(ListOfValues[nextValue++]));
    case KeyPathComputedComponentIdKindEncoding::Function: {
      auto name = MF->getIdentifierText(ListOfValues[nextValue++]);
      return getFuncForReference(name);
    }
    case KeyPathComputedComponentIdKindEncoding::DeclRef: {
      // read SILDeclRef
      return getSILDeclRef(MF, ListOfValues, nextValue);
    }
    }
    llvm_unreachable("unhandled kind");
  };

  ArrayRef<KeyPathPatternComponent::Index> indices;
  SILFunction *indicesEquals = nullptr;
  SILFunction *indicesHash = nullptr;
  AbstractStorageDecl *externalDecl = nullptr;
  SubstitutionMap externalSubs;

  auto handleComputedExternalReferenceAndIndices = [&] {
    auto externalDeclID = ListOfValues[nextValue++];
    externalDecl =
      cast_or_null<AbstractStorageDecl>(MF->getDecl(externalDeclID));
    externalSubs = MF->getSubstitutionMap(ListOfValues[nextValue++]);
    
    SmallVector<KeyPathPatternComponent::Index, 4> indicesBuf;
    auto numIndexes = ListOfValues[nextValue++];
    indicesBuf.reserve(numIndexes);
    while (numIndexes-- > 0) {
      unsigned operand = ListOfValues[nextValue++];
      auto formalType = MF->getType(ListOfValues[nextValue++]);
      auto loweredType = MF->getType(ListOfValues[nextValue++]);
      auto loweredCategory = (SILValueCategory)ListOfValues[nextValue++];
      auto conformance = MF->getConformance(ListOfValues[nextValue++]);
      indicesBuf.push_back({
        operand, formalType->getCanonicalType(),
        SILType::getPrimitiveType(loweredType->getCanonicalType(),
                                  loweredCategory),
        conformance});
    }
    
    indices = MF->getContext().AllocateCopy(indicesBuf);
    if (!indices.empty()) {
      auto indicesEqualsName = MF->getIdentifierText(ListOfValues[nextValue++]);
      auto indicesHashName = MF->getIdentifierText(ListOfValues[nextValue++]);
      indicesEquals = getFuncForReference(indicesEqualsName);
      indicesHash = getFuncForReference(indicesHashName);
    }
  };

  switch (kind) {
  case KeyPathComponentKindEncoding::StoredProperty: {
    auto decl = cast<VarDecl>(MF->getDecl(ListOfValues[nextValue++]));
    return KeyPathPatternComponent::forStoredProperty(decl, type);
  }
  case KeyPathComponentKindEncoding::GettableProperty: {
    auto id = handleComputedId();
    auto getterName = MF->getIdentifierText(ListOfValues[nextValue++]);
    auto getter = getFuncForReference(getterName);
    handleComputedExternalReferenceAndIndices();
    return KeyPathPatternComponent::forComputedGettableProperty(
        id, getter, indices, indicesEquals, indicesHash,
        externalDecl, externalSubs, type);
  }
  case KeyPathComponentKindEncoding::SettableProperty: {
    auto id = handleComputedId();
    auto getterName = MF->getIdentifierText(ListOfValues[nextValue++]);
    auto getter = getFuncForReference(getterName);
    auto setterName = MF->getIdentifierText(ListOfValues[nextValue++]);
    auto setter = getFuncForReference(setterName);
    handleComputedExternalReferenceAndIndices();
    return KeyPathPatternComponent::forComputedSettableProperty(
        id, getter, setter, indices, indicesEquals, indicesHash,
        externalDecl, externalSubs, type);
    break;
  }
  case KeyPathComponentKindEncoding::OptionalChain:
    return KeyPathPatternComponent::forOptional(
        KeyPathPatternComponent::Kind::OptionalChain, type);
  case KeyPathComponentKindEncoding::OptionalForce:
    return KeyPathPatternComponent::forOptional(
        KeyPathPatternComponent::Kind::OptionalForce, type);
  case KeyPathComponentKindEncoding::OptionalWrap:
    return KeyPathPatternComponent::forOptional(
        KeyPathPatternComponent::Kind::OptionalWrap, type);
  case KeyPathComponentKindEncoding::TupleElement:
    return KeyPathPatternComponent::forTupleElement(
        ListOfValues[nextValue++], type);
  case KeyPathComponentKindEncoding::Trivial:
    llvm_unreachable("handled above");
  }
  
  llvm_unreachable("invalid key path component kind encoding");
}

bool SILDeserializer::readSILInstruction(SILFunction *Fn,
                                         SILBuilder &Builder,
                                         unsigned RecordKind,
                                         SmallVectorImpl<uint64_t> &scratch) {
  if (Fn)
    Builder.setCurrentDebugScope(Fn->getDebugScope());
  unsigned RawOpCode = 0, TyCategory = 0, TyCategory2 = 0, TyCategory3 = 0,
           Attr = 0, Attr2 = 0, Attr3 = 0, Attr4 = 0, NumSubs = 0;
  ValueID ValID, ValID2, ValID3;
  TypeID TyID, TyID2, TyID3;
  TypeID ConcreteTyID;
  ProtocolConformanceID ConformanceID;
  SourceLoc SLoc;
  ApplyOptions ApplyOpts;
  ArrayRef<uint64_t> ListOfValues;
  SILLocation Loc = RegularLocation(SLoc);
  ValueOwnershipKind forwardingOwnership(OwnershipKind::Any);
  auto decodeValueOwnership = [](unsigned field){
    // Invalid/Any ownership is never encoded.
    return ValueOwnershipKind(field+1);
  };

  switch (RecordKind) {
  default:
    llvm_unreachable("Record kind for a SIL instruction is not supported.");
  case SIL_ONE_VALUE_ONE_OPERAND:
    SILOneValueOneOperandLayout::readRecord(scratch, RawOpCode, Attr,
                                            ValID, TyID, TyCategory,
                                            ValID2);
    break;
  case SIL_ONE_TYPE:
    SILOneTypeLayout::readRecord(scratch, RawOpCode, Attr, TyID, TyCategory);
    break;
  case SIL_ONE_OPERAND:
    SILOneOperandLayout::readRecord(scratch, RawOpCode, Attr,
                                    TyID, TyCategory, ValID);
    break;
  case SIL_ONE_OPERAND_EXTRA_ATTR:
    SILOneOperandExtraAttributeLayout::readRecord(scratch, RawOpCode, Attr,
                                                  TyID, TyCategory, ValID);
    break;
  case SIL_ONE_TYPE_ONE_OPERAND:
    SILOneTypeOneOperandLayout::readRecord(scratch, RawOpCode, Attr,
                                           TyID, TyCategory,
                                           TyID2, TyCategory2,
                                           ValID);
    break;
  case SIL_ONE_TYPE_ONE_OPERAND_EXTRA_ATTR:
    SILOneTypeOneOperandExtraAttributeLayout::readRecord(scratch, RawOpCode,
                                                         Attr, TyID, TyCategory,
                                                         TyID2, TyCategory2,
                                                         ValID);
    break;
  case SIL_INIT_EXISTENTIAL:
    SILInitExistentialLayout::readRecord(scratch, RawOpCode,
                                         TyID, TyCategory,
                                         TyID2, TyCategory2,
                                         ValID,
                                         ConcreteTyID,
                                         ListOfValues);
    break;
  case SIL_ONE_TYPE_VALUES:
    SILOneTypeValuesLayout::readRecord(scratch, RawOpCode, TyID, TyCategory,
                                       ListOfValues);
    break;
  case SIL_ONE_TYPE_OWNERSHIP_VALUES: {
    unsigned ownershipField;
    SILOneTypeOwnershipValuesLayout::readRecord(scratch, RawOpCode,
                                                ownershipField, TyID,
                                                TyCategory, ListOfValues);
    forwardingOwnership = decodeValueOwnership(ownershipField);
    break;
  }
  case SIL_TWO_OPERANDS:
    SILTwoOperandsLayout::readRecord(scratch, RawOpCode, Attr,
                                     TyID, TyCategory, ValID,
                                     TyID2, TyCategory2, ValID2);
    break;
  case SIL_TWO_OPERANDS_EXTRA_ATTR:
    SILTwoOperandsExtraAttributeLayout::readRecord(scratch, RawOpCode, Attr,
                                                   TyID, TyCategory, ValID,
                                                   TyID2, TyCategory2, ValID2);
    break;
  case SIL_TAIL_ADDR:
    SILTailAddrLayout::readRecord(scratch, RawOpCode,
                                  TyID, ValID,
                                  TyID2, ValID2,
                                  TyID3);
    break;
  case SIL_INST_APPLY: {
    unsigned Kind, RawApplyOpts;
    SILInstApplyLayout::readRecord(scratch, Kind, RawApplyOpts, NumSubs, TyID, TyID2,
                                   ValID, ListOfValues);
    switch (Kind) {
    case SIL_APPLY:
      RawOpCode = (unsigned)SILInstructionKind::ApplyInst;
      break;
    case SIL_PARTIAL_APPLY:
      RawOpCode = (unsigned)SILInstructionKind::PartialApplyInst;
      break;
    case SIL_BUILTIN:
      RawOpCode = (unsigned)SILInstructionKind::BuiltinInst;
      break;
    case SIL_TRY_APPLY:
      RawOpCode = (unsigned)SILInstructionKind::TryApplyInst;
      break;
    case SIL_BEGIN_APPLY:
      RawOpCode = (unsigned)SILInstructionKind::BeginApplyInst;
      break;
        
    default:
      llvm_unreachable("unexpected apply inst kind");
    }

    ApplyOpts = ApplyOptions(ApplyFlags(RawApplyOpts));
    break;
  }
  case SIL_INST_NO_OPERAND:
    SILInstNoOperandLayout::readRecord(scratch, RawOpCode);
    break;
  case SIL_INST_WITNESS_METHOD:
    SILInstWitnessMethodLayout::readRecord(
        scratch, TyID, TyCategory, Attr, TyID2, TyCategory2, TyID3,
        TyCategory3, ValID3, ConformanceID, ListOfValues);
    RawOpCode = (unsigned)SILInstructionKind::WitnessMethodInst;
    break;
  case SIL_INST_DIFFERENTIABLE_FUNCTION:
    SILInstDifferentiableFunctionLayout::readRecord(
        scratch, /*numParams*/ Attr, /*numResults*/ Attr2,
        /*numDiffParams*/ Attr3,
        /*hasDerivativeFunctions*/ Attr4, ListOfValues);
    RawOpCode = (unsigned)SILInstructionKind::DifferentiableFunctionInst;
    break;
  case SIL_INST_LINEAR_FUNCTION:
    SILInstLinearFunctionLayout::readRecord(scratch, /*numDiffParams*/ Attr,
                                            /*hasTransposeFunction*/ Attr2,
                                            ListOfValues);
    RawOpCode = (unsigned)SILInstructionKind::LinearFunctionInst;
    break;
  case SIL_INST_DIFFERENTIABLE_FUNCTION_EXTRACT:
    SILInstDifferentiableFunctionExtractLayout::readRecord(
        scratch, TyID, TyCategory, ValID, /*extractee*/ Attr,
        /*hasExplicitExtracteeType*/ Attr2, /*explicitExtracteeType*/ TyID2);
    RawOpCode = (unsigned)SILInstructionKind::DifferentiableFunctionExtractInst;
    break;
  case SIL_INST_LINEAR_FUNCTION_EXTRACT:
    SILInstLinearFunctionExtractLayout::readRecord(
        scratch, TyID, TyCategory, ValID, /*extractee*/ Attr);
    RawOpCode = (unsigned)SILInstructionKind::LinearFunctionExtractInst;
    break;
  case SIL_INST_INCREMENT_PROFILER_COUNTER:
    SILInstIncrementProfilerCounterLayout::readRecord(scratch, ValID, ValID2,
                                                      Attr, Attr2);
    RawOpCode = (unsigned)SILInstructionKind::IncrementProfilerCounterInst;
    break;
  case SIL_INST_HAS_SYMBOL:
    SILInstHasSymbolLayout::readRecord(scratch, ValID, ListOfValues);
    RawOpCode = (unsigned)SILInstructionKind::HasSymbolInst;
    break;
  case SIL_OPEN_PACK_ELEMENT:
    SILOpenPackElementLayout::readRecord(scratch, Attr,
                                         TyID, TyCategory, ValID);
    RawOpCode = (unsigned)SILInstructionKind::OpenPackElementInst;
    break;
  case SIL_PACK_ELEMENT_GET:
    SILPackElementGetLayout::readRecord(scratch, RawOpCode,
                                        TyID, TyCategory,
                                        TyID2, TyCategory2, ValID2,
                                        ValID3);
    break;
  case SIL_PACK_ELEMENT_SET:
    SILPackElementSetLayout::readRecord(scratch,
                                        TyID, TyCategory, ValID,
                                        TyID2, TyCategory2, ValID2,
                                        ValID3);
    RawOpCode = (unsigned)SILInstructionKind::PackElementSetInst;
    break;
  }

  // FIXME: validate
  SILInstructionKind OpCode = (SILInstructionKind) RawOpCode;

  SILInstruction *ResultInst;
  switch (OpCode) {
  case SILInstructionKind::DebugValueInst:
  case SILInstructionKind::DebugStepInst:
  case SILInstructionKind::TestSpecificationInst:
    llvm_unreachable("not supported");

  case SILInstructionKind::AllocBoxInst: {
    assert(RecordKind == SIL_ONE_TYPE && "Layout should be OneType.");
    bool hasDynamicLifetime = Attr & 0x1;
    bool reflection = (Attr >> 1) & 0x1;
    bool usesMoveableValueDebugInfo = (Attr >> 2) & 0x1;
    ResultInst = Builder.createAllocBox(
        Loc, cast<SILBoxType>(MF->getType(TyID)->getCanonicalType()), None,
        hasDynamicLifetime, reflection, usesMoveableValueDebugInfo);
    break;
  }
  case SILInstructionKind::AllocStackInst: {
    assert(RecordKind == SIL_ONE_TYPE && "Layout should be OneType.");
    bool hasDynamicLifetime = Attr & 0x1;
    bool isLexical = (Attr >> 1) & 0x1;
    bool wasMoved = (Attr >> 2) & 0x1;
    ResultInst = Builder.createAllocStack(
        Loc, getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn),
        None, hasDynamicLifetime, isLexical, wasMoved);
    break;
  }
  case SILInstructionKind::AllocPackInst: {
    assert(RecordKind == SIL_ONE_TYPE && "Layout should be OneType.");
    ResultInst = Builder.createAllocPack(
        Loc, getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));
    break;
  }
  case SILInstructionKind::PackLengthInst: {
    assert(RecordKind == SIL_ONE_TYPE && "Layout should be OneType.");
    ResultInst = Builder.createPackLength(
        Loc, cast<PackType>(MF->getType(TyID)->getCanonicalType()));
    break;
  }
  case SILInstructionKind::MetatypeInst:
    assert(RecordKind == SIL_ONE_TYPE && "Layout should be OneType.");
    ResultInst = Builder.createMetatype(
        Loc, getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));
    break;
      
  case SILInstructionKind::GetAsyncContinuationInst:
    assert(RecordKind == SIL_ONE_TYPE && "Layout should be OneType.");
    ResultInst = Builder.createGetAsyncContinuation(
        Loc, MF->getType(TyID)->getCanonicalType(),
        /*throws*/ Attr != 0);
    break;
  
  case SILInstructionKind::GetAsyncContinuationAddrInst:
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND
           && "Layout should be OneTypeOneOperand.");
    ResultInst = Builder.createGetAsyncContinuationAddr(Loc,
      getLocalValue(ValID, getSILType(MF->getType(TyID2),
                                      (SILValueCategory)TyCategory2, Fn)),
      MF->getType(TyID)->getCanonicalType(),
      /*throws*/ Attr != 0);
    break;

#define ONETYPE_ONEOPERAND_INST(ID)                                            \
  case SILInstructionKind::ID##Inst:                                           \
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND &&                           \
           "Layout should be OneTypeOneOperand.");                             \
    ResultInst = Builder.create##ID(                                           \
        Loc, getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn),  \
        getLocalValue(ValID, getSILType(MF->getType(TyID2),                    \
                                        (SILValueCategory)TyCategory2, Fn)));  \
    break;
    ONETYPE_ONEOPERAND_INST(ValueMetatype)
    ONETYPE_ONEOPERAND_INST(ExistentialMetatype)
    ONETYPE_ONEOPERAND_INST(ProjectExistentialBox)
#undef ONETYPE_ONEOPERAND_INST
  case SILInstructionKind::DeallocBoxInst:
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND &&
           "Layout should be OneTypeOneOperand.");
    ResultInst = Builder.createDeallocBox(
        Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID2),
                                        (SILValueCategory)TyCategory2, Fn)));
    break;
  case SILInstructionKind::OpenExistentialAddrInst:
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND &&
           "Layout should be OneTypeOneOperand.");
    ResultInst = Builder.createOpenExistentialAddr(
        Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID2),
                                        (SILValueCategory)TyCategory2, Fn)),
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn),
        Attr == 0 ? OpenedExistentialAccess::Immutable
                  : OpenedExistentialAccess::Mutable);
    break;
  case SILInstructionKind::DynamicPackIndexInst: {
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND &&
           "Layout should be OneTypeOneOperand.");
    auto indexOperand =
      getLocalValue(ValID,
        getSILType(MF->getType(TyID2), (SILValueCategory)TyCategory2, Fn));
    auto packType = cast<PackType>(MF->getType(TyID)->getCanonicalType());
    ResultInst = Builder.createDynamicPackIndex(Loc, indexOperand, packType);
    break;
  }
  case SILInstructionKind::PackPackIndexInst: {
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND &&
           "Layout should be OneTypeOneOperand.");
    auto indexOperand =
      getLocalValue(ValID,
        getSILType(MF->getType(TyID2), (SILValueCategory)TyCategory2, Fn));
    auto packType = cast<PackType>(MF->getType(TyID)->getCanonicalType());
    ResultInst =
      Builder.createPackPackIndex(Loc, Attr, indexOperand, packType);
    break;
  }
  case SILInstructionKind::ScalarPackIndexInst: {
    assert(RecordKind == SIL_ONE_TYPE && "Layout should be OneType.");
    auto packType = cast<PackType>(MF->getType(TyID)->getCanonicalType());
    ResultInst = Builder.createScalarPackIndex(Loc, Attr, packType);
    break;
  }
  case SILInstructionKind::OpenPackElementInst: {
    assert(RecordKind == SIL_OPEN_PACK_ELEMENT && "Layout should be OpenPackElement");
    auto index = getLocalValue(ValID,
        getSILType(MF->getType(TyID), (SILValueCategory) TyCategory, Fn));
    auto env = MF->getGenericEnvironmentChecked(Attr);
    if (!env) MF->fatal(env.takeError());
    ResultInst = Builder.createOpenPackElement(Loc, index, *env);
    break;
  }
  case SILInstructionKind::PackElementGetInst: {
    assert(RecordKind == SIL_PACK_ELEMENT_GET);
    auto elementType = getSILType(MF->getType(TyID),
                                  (SILValueCategory) TyCategory, Fn);
    auto packType = getSILType(MF->getType(TyID2),
                               (SILValueCategory) TyCategory2, Fn);
    auto pack = getLocalValue(ValID2, packType);
    auto indexType = SILType::getPackIndexType(MF->getContext());
    auto index = getLocalValue(ValID3, indexType);
    ResultInst = Builder.createPackElementGet(Loc, index, pack, elementType);
    break;
  }
  case SILInstructionKind::PackElementSetInst: {
    assert(RecordKind == SIL_PACK_ELEMENT_SET);
    auto elementType = getSILType(MF->getType(TyID),
                                  (SILValueCategory) TyCategory, Fn);
    auto value = getLocalValue(ValID, elementType);
    auto packType = getSILType(MF->getType(TyID2),
                               (SILValueCategory) TyCategory2, Fn);
    auto pack = getLocalValue(ValID2, packType);
    auto indexType = SILType::getPackIndexType(MF->getContext());
    auto index = getLocalValue(ValID3, indexType);
    ResultInst = Builder.createPackElementSet(Loc, value, index, pack);
    break;
  }
  case SILInstructionKind::TuplePackElementAddrInst: {
    assert(RecordKind == SIL_PACK_ELEMENT_GET);
    auto elementType = getSILType(MF->getType(TyID),
                                  (SILValueCategory) TyCategory, Fn);
    auto tupleType = getSILType(MF->getType(TyID2),
                               (SILValueCategory) TyCategory2, Fn);
    auto tuple = getLocalValue(ValID2, tupleType);
    auto indexType = SILType::getPackIndexType(MF->getContext());
    auto index = getLocalValue(ValID3, indexType);
    ResultInst = Builder.createTuplePackElementAddr(Loc, index, tuple,
                                                    elementType);
    break;
  }

#define ONEOPERAND_ONETYPE_INST(ID)                                            \
  case SILInstructionKind::ID##Inst:                                           \
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND &&                           \
           "Layout should be OneTypeOneOperand.");                             \
    ResultInst = Builder.create##ID(                                           \
        Loc,                                                                   \
        getLocalValue(ValID, getSILType(MF->getType(TyID2),                    \
                                        (SILValueCategory)TyCategory2, Fn)),   \
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));      \
    break;
    ONEOPERAND_ONETYPE_INST(OpenExistentialRef)
    ONEOPERAND_ONETYPE_INST(OpenExistentialMetatype)
    ONEOPERAND_ONETYPE_INST(OpenExistentialBox)
    ONEOPERAND_ONETYPE_INST(OpenExistentialValue)
    ONEOPERAND_ONETYPE_INST(OpenExistentialBoxValue)
    // Conversion instructions.
#define LOADABLE_REF_STORAGE(Name, ...) \
  ONEOPERAND_ONETYPE_INST(RefTo##Name) \
  ONEOPERAND_ONETYPE_INST(Name##ToRef)
#include "swift/AST/ReferenceStorage.def"
  ONEOPERAND_ONETYPE_INST(UncheckedAddrCast)
  ONEOPERAND_ONETYPE_INST(UncheckedTrivialBitCast)
  ONEOPERAND_ONETYPE_INST(UncheckedBitwiseCast)
  ONEOPERAND_ONETYPE_INST(UncheckedValueCast)
  ONEOPERAND_ONETYPE_INST(BridgeObjectToRef)
  ONEOPERAND_ONETYPE_INST(BridgeObjectToWord)
  ONEOPERAND_ONETYPE_INST(Upcast)
  ONEOPERAND_ONETYPE_INST(RefToRawPointer)
  ONEOPERAND_ONETYPE_INST(RawPointerToRef)
  ONEOPERAND_ONETYPE_INST(ThinToThickFunction)
  ONEOPERAND_ONETYPE_INST(ThickToObjCMetatype)
  ONEOPERAND_ONETYPE_INST(ObjCToThickMetatype)
  ONEOPERAND_ONETYPE_INST(ObjCMetatypeToObject)
  ONEOPERAND_ONETYPE_INST(ObjCExistentialMetatypeToObject)
  ONEOPERAND_ONETYPE_INST(ProjectBlockStorage)
#undef ONEOPERAND_ONETYPE_INST

  case SILInstructionKind::AddressToPointerInst: {
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND &&
           "Layout should be OneTypeOneOperand.");
    ResultInst = Builder.createAddressToPointer(
        Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID2),
                                        (SILValueCategory)TyCategory2, Fn)),
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn),
        /*needsStackProtection=*/ Attr != 0);
    break;
  }
  case SILInstructionKind::ProjectBoxInst: {
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND &&
           "Layout should be OneTypeOneOperand.");
    ResultInst = Builder.createProjectBox(
        Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID2),
                                        (SILValueCategory)TyCategory2, Fn)),
        TyID);
    break;
  }
  case  SILInstructionKind::ConvertEscapeToNoEscapeInst: {
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND &&
           "Layout should be OneTypeOneOperand.");
    bool isLifetimeGuaranteed = Attr & 0x01;
    ResultInst = Builder.createConvertEscapeToNoEscape(
        Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID2),
                                        (SILValueCategory)TyCategory2, Fn)),
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn),
        isLifetimeGuaranteed);
    break;
  }
  case SILInstructionKind::ConvertFunctionInst: {
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND
           && "Layout should be OneTypeOneOperand.");
    bool withoutActuallyEscaping = Attr & 0x01;
    ResultInst = Builder.createConvertFunction(
        Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID2),
                                        (SILValueCategory)TyCategory2, Fn)),
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn),
        withoutActuallyEscaping);
    break;
  }
  case SILInstructionKind::PointerToAddressInst: {
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND_EXTRA_ATTR &&
           "Layout should be OneTypeOneOperand.");
    auto alignment = llvm::decodeMaybeAlign(Attr & 0xFF);
    bool isStrict = Attr & 0x100;
    bool isInvariant = Attr & 0x200;
    ResultInst = Builder.createPointerToAddress(
        Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID2),
                                        (SILValueCategory)TyCategory2, Fn)),
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn),
        isStrict, isInvariant, alignment);
    break;
  }
  case SILInstructionKind::DeallocExistentialBoxInst: {
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND &&
           "Layout should be OneTypeOneOperand.");
    ResultInst = Builder.createDeallocExistentialBox(
        Loc, MF->getType(TyID)->getCanonicalType(),
        getLocalValue(ValID, getSILType(MF->getType(TyID2),
                                        (SILValueCategory)TyCategory2, Fn)));
    break;

  }
  
  case SILInstructionKind::RefToBridgeObjectInst: {
    auto RefTy =
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);
    auto Ref = getLocalValue(ValID, RefTy);
    auto BitsTy =
        getSILType(MF->getType(TyID2), (SILValueCategory)TyCategory2, Fn);
    auto Bits = getLocalValue(ValID2, BitsTy);

    ResultInst = Builder.createRefToBridgeObject(Loc, Ref, Bits);
    break;
  }

  case SILInstructionKind::ObjCProtocolInst: {
    auto Ty = getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);
    auto Proto = MF->getDecl(ValID);
    ResultInst = Builder.createObjCProtocol(Loc, cast<ProtocolDecl>(Proto), Ty);
    break;
  }

  case SILInstructionKind::InitExistentialAddrInst:
  case SILInstructionKind::InitExistentialValueInst:
  case SILInstructionKind::InitExistentialMetatypeInst:
  case SILInstructionKind::InitExistentialRefInst:
  case SILInstructionKind::AllocExistentialBoxInst: {

    auto Ty = getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);
    auto Ty2 = MF->getType(TyID2);
    CanType ConcreteTy;
    if (OpCode != SILInstructionKind::InitExistentialMetatypeInst)
      ConcreteTy = MF->getType(ConcreteTyID)->getCanonicalType();
    SILValue operand;
    if (OpCode != SILInstructionKind::AllocExistentialBoxInst)
      operand = getLocalValue(
          ValID, getSILType(Ty2, (SILValueCategory)TyCategory2, Fn));

    SmallVector<ProtocolConformanceRef, 2> conformances;
    for (auto conformanceID: ListOfValues) {
      auto conformance = MF->getConformance(conformanceID);
      conformances.push_back(conformance);
    }

    auto ctxConformances = MF->getContext().AllocateCopy(conformances);

    switch (OpCode) {
    default: llvm_unreachable("Out of sync with parent switch");
    case SILInstructionKind::InitExistentialAddrInst:
      ResultInst = Builder.createInitExistentialAddr(Loc, operand, ConcreteTy,
                                                     Ty, ctxConformances);
      break;
    case SILInstructionKind::InitExistentialValueInst:
      ResultInst = Builder.createInitExistentialValue(Loc, Ty, ConcreteTy,
                                                      operand, ctxConformances);
      break;
    case SILInstructionKind::InitExistentialMetatypeInst:
      ResultInst = Builder.createInitExistentialMetatype(Loc, operand, Ty,
                                                         ctxConformances);
      break;
    case SILInstructionKind::InitExistentialRefInst:
      ResultInst = Builder.createInitExistentialRef(Loc, Ty, ConcreteTy,
                                                    operand, ctxConformances);
      break;
    case SILInstructionKind::AllocExistentialBoxInst:
      ResultInst = Builder.createAllocExistentialBox(Loc, Ty, ConcreteTy,
                                                     ctxConformances);
      break;
    }
    break;
  }
  case SILInstructionKind::AllocRefInst:
  case SILInstructionKind::AllocRefDynamicInst: {
    assert(RecordKind == SIL_ONE_TYPE_VALUES &&
           "Layout should be OneTypeValues.");
    unsigned NumVals = ListOfValues.size();
    assert(NumVals >= 1 && "Not enough values");
    unsigned Flags = ListOfValues[0];
    bool isObjC = (bool)(Flags & 1);
    bool canAllocOnStack = (bool)((Flags >> 1) & 1);
    SILType ClassTy =
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);
    SmallVector<SILValue, 4> Counts;
    SmallVector<SILType, 4> TailTypes;
    unsigned i = 1;
    for (; i + 2 < NumVals; i += 3) {
      SILType TailType = getSILType(MF->getType(ListOfValues[i]),
                                    SILValueCategory::Object, Fn);
      TailTypes.push_back(TailType);
      SILType CountType = getSILType(MF->getType(ListOfValues[i + 2]),
                                     SILValueCategory::Object, Fn);
      SILValue CountVal = getLocalValue(ListOfValues[i+1], CountType);
      Counts.push_back(CountVal);
    }
    if (OpCode == SILInstructionKind::AllocRefDynamicInst) {
      assert(i + 2 == NumVals);
      SILType MetadataType = getSILType(MF->getType(ListOfValues[i+1]),
                                        SILValueCategory::Object, Fn);
      SILValue MetadataOp = getLocalValue(ListOfValues[i], MetadataType);
      ResultInst = Builder.createAllocRefDynamic(Loc, MetadataOp, ClassTy,
                                                 isObjC, canAllocOnStack,
                                                 TailTypes, Counts);
    } else {
      assert(i == NumVals);
      ResultInst = Builder.createAllocRef(Loc, ClassTy, isObjC, canAllocOnStack,
                                          TailTypes, Counts);
    }
    break;
  }
  case SILInstructionKind::ApplyInst:
  case SILInstructionKind::BeginApplyInst: {
    // Format: attributes such as transparent, the callee's type, a value for
    // the callee and a list of values for the arguments. Each value in the list
    // is represented with 2 IDs: ValueID and ValueResultNumber.
    auto Ty = MF->getType(TyID);
    auto Ty2 = MF->getType(TyID2);
    SILType FnTy = getSILType(Ty, SILValueCategory::Object, Fn);
    SILType SubstFnTy = getSILType(Ty2, SILValueCategory::Object, Fn);
    SILFunctionConventions substConventions(SubstFnTy.castTo<SILFunctionType>(),
                                            Builder.getModule());
    assert(substConventions.getNumSILArguments() == ListOfValues.size()
           && "Argument number mismatch in ApplyInst.");
    SmallVector<SILValue, 4> Args;
    for (unsigned I = 0, E = ListOfValues.size(); I < E; ++I)
      Args.push_back(getLocalValue(ListOfValues[I],
                                   substConventions.getSILArgumentType(
                                       I, Builder.getTypeExpansionContext())));
    SubstitutionMap Substitutions = MF->getSubstitutionMap(NumSubs);

    if (OpCode == SILInstructionKind::ApplyInst) {
      ResultInst =
          Builder.createApply(Loc, getLocalValue(ValID, FnTy), Substitutions,
                              Args, ApplyOpts);
    } else {
      ResultInst = Builder.createBeginApply(Loc, getLocalValue(ValID, FnTy),
                                            Substitutions, Args, ApplyOpts);
    }
    break;
  }
  case SILInstructionKind::TryApplyInst: {
    // Format: attributes such as transparent, the callee's type, a value for
    // the callee and a list of values for the arguments. Each value in the list
    // is represented with 2 IDs: ValueID and ValueResultNumber.  The final
    // two values in the list are the basic block identifiers.
    auto Ty = MF->getType(TyID);
    auto Ty2 = MF->getType(TyID2);
    SILType FnTy = getSILType(Ty, SILValueCategory::Object, Fn);
    SILType SubstFnTy = getSILType(Ty2, SILValueCategory::Object, Fn);

    SILBasicBlock *errorBB = getBBForReference(Fn, ListOfValues.back());
    ListOfValues = ListOfValues.drop_back();
    SILBasicBlock *normalBB = getBBForReference(Fn, ListOfValues.back());
    ListOfValues = ListOfValues.drop_back();

    SILFunctionConventions substConventions(SubstFnTy.castTo<SILFunctionType>(),
                                            Builder.getModule());
    assert(substConventions.getNumSILArguments() == ListOfValues.size()
           && "Argument number mismatch in ApplyInst.");
    SmallVector<SILValue, 4> Args;
    for (unsigned I = 0, E = ListOfValues.size(); I < E; ++I)
      Args.push_back(getLocalValue(ListOfValues[I],
                                   substConventions.getSILArgumentType(
                                       I, Builder.getTypeExpansionContext())));
    SubstitutionMap Substitutions = MF->getSubstitutionMap(NumSubs);

    ResultInst = Builder.createTryApply(Loc, getLocalValue(ValID, FnTy),
                                        Substitutions, Args, normalBB, errorBB,
                                        ApplyOpts);
    break;
  }
  case SILInstructionKind::PartialApplyInst: {
    auto Ty = MF->getType(TyID);
    auto Ty2 = MF->getType(TyID2);
    SILType FnTy = getSILType(Ty, SILValueCategory::Object, Fn);
    SILType closureTy = getSILType(Ty2, SILValueCategory::Object, Fn);

    SubstitutionMap Substitutions = MF->getSubstitutionMap(NumSubs);

    auto SubstFnTy = SILType::getPrimitiveObjectType(
        FnTy.castTo<SILFunctionType>()->substGenericArgs(
            Builder.getModule(), Substitutions,
            Builder.getTypeExpansionContext()));
    SILFunctionConventions fnConv(SubstFnTy.castTo<SILFunctionType>(),
                                  Builder.getModule());

    unsigned numArgs = fnConv.getNumSILArguments();
    assert(numArgs >= ListOfValues.size()
           && "Argument number mismatch in PartialApplyInst.");

    SILValue FnVal = getLocalValue(ValID, FnTy);
    SmallVector<SILValue, 4> Args;
    unsigned unappliedArgs = numArgs - ListOfValues.size();
    for (unsigned I = 0, E = ListOfValues.size(); I < E; ++I)
      Args.push_back(getLocalValue(
          ListOfValues[I],
          fnConv.getSILArgumentType(I + unappliedArgs,
                                    Builder.getTypeExpansionContext())));
    auto onStack = closureTy.castTo<SILFunctionType>()->isNoEscape()
                       ? PartialApplyInst::OnStackKind::OnStack
                       : PartialApplyInst::OnStackKind::NotOnStack;
    // FIXME: Why the arbitrary order difference in IRBuilder type argument?
    ResultInst = Builder.createPartialApply(
        Loc, FnVal, Substitutions, Args,
        closureTy.castTo<SILFunctionType>()->getCalleeConvention(), onStack);
    break;
  }
  case SILInstructionKind::BuiltinInst: {
    auto ASTTy = MF->getType(TyID);
    auto ResultTy = getSILType(ASTTy, (SILValueCategory)(unsigned)TyID2, Fn);
    SmallVector<SILValue, 4> Args;
    for (unsigned i = 0, e = ListOfValues.size(); i < e; i += 3) {
      auto ArgASTTy = MF->getType(ListOfValues[i+1]);
      auto ArgTy = getSILType(
          ArgASTTy, (SILValueCategory)(unsigned)ListOfValues[i + 2], Fn);
      Args.push_back(getLocalValue(ListOfValues[i], ArgTy));
    }
    SubstitutionMap Substitutions = MF->getSubstitutionMap(NumSubs);
    Identifier Name = MF->getIdentifier(ValID);

    ResultInst =
        Builder.createBuiltin(Loc, Name, ResultTy, Substitutions, Args);
    break;
  }
  case SILInstructionKind::AllocGlobalInst: {
    // Format: Name and type. Use SILOneOperandLayout.
    StringRef Name = MF->getIdentifierText(ValID);

    // Find the global variable.
    SILGlobalVariable *g = getGlobalForReference(Name);
    assert(g && "Can't deserialize global variable");

    ResultInst = Builder.createAllocGlobal(Loc, g);
    break;
  }
  case SILInstructionKind::GlobalAddrInst:
  case SILInstructionKind::GlobalValueInst: {
    // Format: Name and type. Use SILOneOperandLayout.
    auto Ty = MF->getType(TyID);
    StringRef Name = MF->getIdentifierText(ValID);

    // Find the global variable.
    SILGlobalVariable *g = getGlobalForReference(Name);
    assert(g && "Can't deserialize global variable");
    SILType expectedType =
        (OpCode == SILInstructionKind::GlobalAddrInst
             ? g->getLoweredTypeInContext(TypeExpansionContext(*Fn))
                   .getAddressType()
             : g->getLoweredTypeInContext(TypeExpansionContext(*Fn)));
    assert(expectedType == getSILType(Ty, (SILValueCategory)TyCategory, Fn) &&
           "Type of a global variable does not match GlobalAddr.");
    (void)Ty;
    (void)expectedType;
    if (OpCode == SILInstructionKind::GlobalAddrInst) {
      ResultInst = Builder.createGlobalAddr(Loc, g);
    } else {
      ResultInst = Builder.createGlobalValue(Loc, g);
    }
    break;
  }
  case SILInstructionKind::BaseAddrForOffsetInst:
    assert(RecordKind == SIL_ONE_TYPE && "Layout should be OneType.");
    ResultInst = Builder.createBaseAddrForOffset(
        Loc, getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));
    break;
  case SILInstructionKind::DeallocStackInst: {
    auto Ty = MF->getType(TyID);
    ResultInst = Builder.createDeallocStack(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)));
    break;
  }
  case SILInstructionKind::DeallocStackRefInst: {
    auto Ty = MF->getType(TyID);
    ResultInst = Builder.createDeallocStackRef(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)));
    break;
  }
  case SILInstructionKind::DeallocPackInst: {
    auto Ty = MF->getType(TyID);
    ResultInst = Builder.createDeallocPack(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)));
    break;
  }
  case SILInstructionKind::DeallocRefInst: {
    auto Ty = MF->getType(TyID);
    ResultInst = Builder.createDeallocRef(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)));
    break;
  }
  case SILInstructionKind::DeallocPartialRefInst: {
    auto Ty = MF->getType(TyID);
    auto Ty2 = MF->getType(TyID2);
    ResultInst = Builder.createDeallocPartialRef(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)),
        getLocalValue(ValID2,
                      getSILType(Ty2, (SILValueCategory)TyCategory2, Fn)));
    break;
  }
  case SILInstructionKind::FunctionRefInst: {
    auto Ty = MF->getType(TyID);
    StringRef FuncName = MF->getIdentifierText(ValID);
    ResultInst = Builder.createFunctionRef(
        Loc,
        getFuncForReference(
            FuncName, getSILType(Ty, (SILValueCategory)TyCategory, nullptr)));
    break;
  }
  case SILInstructionKind::DynamicFunctionRefInst: {
    auto Ty = MF->getType(TyID);
    StringRef FuncName = MF->getIdentifierText(ValID);
    ResultInst = Builder.createDynamicFunctionRef(
        Loc,
        getFuncForReference(
            FuncName, getSILType(Ty, (SILValueCategory)TyCategory, nullptr)));
    break;
  }
  case SILInstructionKind::PreviousDynamicFunctionRefInst: {
    auto Ty = MF->getType(TyID);
    StringRef FuncName = MF->getIdentifierText(ValID);
    ResultInst = Builder.createPreviousDynamicFunctionRef(
        Loc,
        getFuncForReference(
            FuncName, getSILType(Ty, (SILValueCategory)TyCategory, nullptr)));
    break;
  }
  case SILInstructionKind::MarkDependenceInst: {
    auto Ty = MF->getType(TyID);
    auto Ty2 = MF->getType(TyID2);
    ResultInst = Builder.createMarkDependence(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)),
        getLocalValue(ValID2,
                      getSILType(Ty2, (SILValueCategory)TyCategory2, Fn)));
    break;
  }
  case SILInstructionKind::CopyBlockWithoutEscapingInst: {
    auto Ty = MF->getType(TyID);
    auto Ty2 = MF->getType(TyID2);
    ResultInst = Builder.createCopyBlockWithoutEscaping(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)),
        getLocalValue(ValID2,
                      getSILType(Ty2, (SILValueCategory)TyCategory2, Fn)));
    break;
  }
  case SILInstructionKind::IndexAddrInst: {
    auto Ty = MF->getType(TyID);
    auto Ty2 = MF->getType(TyID2);
    ResultInst = Builder.createIndexAddr(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)),
        getLocalValue(ValID2,
                      getSILType(Ty2, (SILValueCategory)TyCategory2, Fn)),
        /*needsStackProtection=*/ Attr != 0);
    break;
  }
  case SILInstructionKind::TailAddrInst: {
    auto Ty = MF->getType(TyID);
    auto Ty2 = MF->getType(TyID2);
    auto ResultTy = MF->getType(TyID3);
    ResultInst = Builder.createTailAddr(
        Loc,
        getLocalValue(ValID, getSILType(Ty, SILValueCategory::Address, Fn)),
        getLocalValue(ValID2, getSILType(Ty2, SILValueCategory::Object, Fn)),
        getSILType(ResultTy, SILValueCategory::Address, Fn));
    break;
  }
  case SILInstructionKind::IndexRawPointerInst: {
    auto Ty = MF->getType(TyID);
    auto Ty2 = MF->getType(TyID2);
    ResultInst = Builder.createIndexRawPointer(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)),
        getLocalValue(ValID2,
                      getSILType(Ty2, (SILValueCategory)TyCategory2, Fn)));
    break;
  }
  case SILInstructionKind::IncrementProfilerCounterInst: {
    auto PGOFuncName = MF->getIdentifierText(ValID);
    auto PGOHashStr = MF->getIdentifierText(ValID2);

    uint64_t PGOHash;
    auto HadError = PGOHashStr.getAsInteger(/*radix*/ 10, PGOHash);
    assert(!HadError && "Failed to deserialize PGO hash");
    (void)HadError;

    ResultInst = Builder.createIncrementProfilerCounter(
        Loc, /*CounterIdx*/ Attr, PGOFuncName, /*NumCounters*/ Attr2, PGOHash);
    break;
  }
  case SILInstructionKind::IntegerLiteralInst: {
    auto Ty = MF->getType(TyID);
    auto intTy = Ty->castTo<AnyBuiltinIntegerType>();
    StringRef text = MF->getIdentifierText(ValID);
    bool negate = text[0] == '-';
    if (negate) text = text.drop_front();
    APInt value = intTy->getWidth().parse(text, 10, negate);
    ResultInst = Builder.createIntegerLiteral(
        Loc, getSILType(Ty, (SILValueCategory)TyCategory, Fn), value);
    break;
  }
  case SILInstructionKind::FloatLiteralInst: {
    auto Ty = MF->getType(TyID);
    auto floatTy = Ty->castTo<BuiltinFloatType>();
    StringRef StringVal = MF->getIdentifierText(ValID);
    // Build APInt from string.
    APInt bits(floatTy->getBitWidth(), StringVal, 16);
    if (bits.getBitWidth() != floatTy->getBitWidth())
      bits = bits.zextOrTrunc(floatTy->getBitWidth());

    APFloat value(floatTy->getAPFloatSemantics(), bits);

    ResultInst = Builder.createFloatLiteral(
        Loc, getSILType(Ty, (SILValueCategory)TyCategory, Fn), value);
    break;
  }
  case SILInstructionKind::StringLiteralInst: {
    StringRef StringVal = MF->getIdentifierText(ValID);
    auto encoding = fromStableStringEncoding(Attr);
    if (!encoding) return true;
    ResultInst =
        Builder.createStringLiteral(Loc, StringVal, encoding.value());
    break;
  }
  case SILInstructionKind::CondFailInst: {
    SILValue Op = getLocalValue(
        ValID, getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));
    StringRef StringVal = MF->getIdentifierText(ValID2);
    ResultInst = Builder.createCondFail(Loc, Op, StringVal);
    break;
  }
  case SILInstructionKind::MarkFunctionEscapeInst: {
    // Format: a list of typed values. A typed value is expressed by 4 IDs:
    // TypeID, TypeCategory, ValueID, ValueResultNumber.
    SmallVector<SILValue, 4> OpList;
    for (unsigned I = 0, E = ListOfValues.size(); I < E; I += 3) {
      auto EltTy = MF->getType(ListOfValues[I]);
      OpList.push_back(getLocalValue(
          ListOfValues[I + 2],
          getSILType(EltTy, (SILValueCategory)ListOfValues[I + 1], Fn)));
    }
    ResultInst = Builder.createMarkFunctionEscape(Loc, OpList);
    break;
  }
  // Checked Conversion instructions.
  case SILInstructionKind::UnconditionalCheckedCastInst: {
    SILType srcLoweredType = getSILType(MF->getType(ListOfValues[1]),
                                        (SILValueCategory)ListOfValues[2], Fn);
    SILValue src = getLocalValue(ListOfValues[0], srcLoweredType);

    SILType targetLoweredType =
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);
    CanType targetFormalType =
        MF->getType(ListOfValues[3])->getCanonicalType();
    ResultInst = Builder.createUnconditionalCheckedCast(
        Loc, src, targetLoweredType, targetFormalType);
    break;
  }

#define UNARY_INSTRUCTION(ID)                                                  \
  case SILInstructionKind::ID##Inst:                                           \
    assert(RecordKind == SIL_ONE_OPERAND && "Layout should be OneOperand.");   \
    ResultInst = Builder.create##ID(                                           \
        Loc,                                                                   \
        getLocalValue(ValID, getSILType(MF->getType(TyID),                     \
                                        (SILValueCategory)TyCategory, Fn)));   \
    break;

#define REFCOUNTING_INSTRUCTION(ID)                                            \
  case SILInstructionKind::ID##Inst:                                           \
    assert(RecordKind == SIL_ONE_OPERAND && "Layout should be OneOperand.");   \
    ResultInst = Builder.create##ID(                                           \
        Loc,                                                                   \
        getLocalValue(ValID, getSILType(MF->getType(TyID),                     \
                                        (SILValueCategory)TyCategory, Fn)),    \
        (Atomicity)Attr);                                                      \
    break;

#define UNCHECKED_REF_STORAGE(Name, ...)                                       \
  UNARY_INSTRUCTION(StrongCopy##Name##Value)
#define ALWAYS_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)            \
  REFCOUNTING_INSTRUCTION(Name##Retain)                                        \
  REFCOUNTING_INSTRUCTION(Name##Release)                                       \
  REFCOUNTING_INSTRUCTION(StrongRetain##Name)                                  \
  UNARY_INSTRUCTION(StrongCopy##Name##Value)
#include "swift/AST/ReferenceStorage.def"
  REFCOUNTING_INSTRUCTION(RetainValue)
  REFCOUNTING_INSTRUCTION(RetainValueAddr)
  REFCOUNTING_INSTRUCTION(UnmanagedRetainValue)
  UNARY_INSTRUCTION(CopyValue)
  UNARY_INSTRUCTION(ExplicitCopyValue)
  REFCOUNTING_INSTRUCTION(ReleaseValue)
  REFCOUNTING_INSTRUCTION(ReleaseValueAddr)
  REFCOUNTING_INSTRUCTION(UnmanagedReleaseValue)
  REFCOUNTING_INSTRUCTION(AutoreleaseValue)
  REFCOUNTING_INSTRUCTION(UnmanagedAutoreleaseValue)
  REFCOUNTING_INSTRUCTION(SetDeallocating)
  UNARY_INSTRUCTION(DeinitExistentialAddr)
  UNARY_INSTRUCTION(DeinitExistentialValue)
  UNARY_INSTRUCTION(EndBorrow)
  UNARY_INSTRUCTION(DestroyAddr)
  UNARY_INSTRUCTION(Return)
  UNARY_INSTRUCTION(Throw)
  UNARY_INSTRUCTION(ClassifyBridgeObject)
  UNARY_INSTRUCTION(ValueToBridgeObject)
  UNARY_INSTRUCTION(FixLifetime)
  UNARY_INSTRUCTION(EndLifetime)
  UNARY_INSTRUCTION(CopyBlock)
  UNARY_INSTRUCTION(LoadBorrow)
  REFCOUNTING_INSTRUCTION(StrongRetain)
  REFCOUNTING_INSTRUCTION(StrongRelease)
  UNARY_INSTRUCTION(IsUnique)
  UNARY_INSTRUCTION(AbortApply)
  UNARY_INSTRUCTION(EndApply)
  UNARY_INSTRUCTION(ExtractExecutor)
#undef UNARY_INSTRUCTION
#undef REFCOUNTING_INSTRUCTION

  case SILInstructionKind::BeginBorrowInst: {
    assert(RecordKind == SIL_ONE_OPERAND && "Layout should be OneOperand.");
    bool isLexical = Attr & 0x1;
    ResultInst = Builder.createBeginBorrow(
        Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID),
                                        (SILValueCategory)TyCategory, Fn)),
        isLexical);
    break;
  }

  case SILInstructionKind::IsEscapingClosureInst: {
    assert(RecordKind == SIL_ONE_OPERAND && "Layout should be OneOperand.");
    unsigned verificationType = Attr;
    ResultInst = Builder.createIsEscapingClosure(
        Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID),
                                        (SILValueCategory)TyCategory, Fn)),
        verificationType);
    break;
  }

  case SILInstructionKind::HopToExecutorInst: {
    assert(RecordKind == SIL_ONE_OPERAND && "Layout should be OneOperand.");
    unsigned mandatory = Attr;
    ResultInst = Builder.createHopToExecutor(
        Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID),
                                        (SILValueCategory)TyCategory, Fn)),
        mandatory != 0);
    break;
  }
  case SILInstructionKind::DestroyValueInst: {
    assert(RecordKind == SIL_ONE_OPERAND && "Layout should be OneOperand.");
    unsigned poisonRefs = Attr;
    ResultInst = Builder.createDestroyValue(
        Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID),
                                        (SILValueCategory)TyCategory, Fn)),
        poisonRefs != 0);
    break;
  }

  case SILInstructionKind::BeginCOWMutationInst: {
    assert(RecordKind == SIL_ONE_OPERAND && "Layout should be OneOperand.");
    unsigned isNative = Attr;
    ResultInst = Builder.createBeginCOWMutation(
        Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID),
                                        (SILValueCategory)TyCategory, Fn)),
        isNative != 0);
    break;
  }

  case SILInstructionKind::EndCOWMutationInst: {
    assert(RecordKind == SIL_ONE_OPERAND && "Layout should be OneOperand.");
    unsigned keepUnique = Attr;
    ResultInst = Builder.createEndCOWMutation(
        Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID),
                                        (SILValueCategory)TyCategory, Fn)),
        keepUnique != 0);
    break;
  }

  case SILInstructionKind::DestructureTupleInst: {
    assert(RecordKind == SIL_ONE_OPERAND && "Layout should be OneOperand.");
    SILValue Operand = getLocalValue(
        ValID, getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));
    ResultInst = Builder.createDestructureTuple(Loc, Operand);
    break;
  }
  case SILInstructionKind::DestructureStructInst: {
    assert(RecordKind == SIL_ONE_OPERAND && "Layout should be OneOperand.");
    SILValue Operand = getLocalValue(
        ValID, getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));
    ResultInst = Builder.createDestructureStruct(Loc, Operand);
    break;
  }
  case SILInstructionKind::UncheckedOwnershipConversionInst: {
    auto Ty = MF->getType(TyID);
    auto ResultKind = decodeValueOwnership(Attr);
    ResultInst = Builder.createUncheckedOwnershipConversion(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)),
        ResultKind);
    break;
  }

  case SILInstructionKind::MoveValueInst: {
    auto Ty = MF->getType(TyID);
    bool AllowsDiagnostics = Attr & 0x1;
    bool IsLexical = (Attr >> 1) & 0x1;
    auto *MVI = Builder.createMoveValue(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)),
        IsLexical);
    MVI->setAllowsDiagnostics(AllowsDiagnostics);
    ResultInst = MVI;
    break;
  }

  case SILInstructionKind::DropDeinitInst: {
    auto Ty = MF->getType(TyID);
    ResultInst = Builder.createDropDeinit(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)));
    break;
  }

  case SILInstructionKind::MarkUnresolvedReferenceBindingInst: {
    using Kind = MarkUnresolvedReferenceBindingInst::Kind;
    auto ty = MF->getType(TyID);
    auto kind = Kind(Attr);
    ResultInst = Builder.createMarkUnresolvedReferenceBindingInst(
        Loc,
        getLocalValue(ValID, getSILType(ty, (SILValueCategory)TyCategory, Fn)),
        kind);
    break;
  }

  case SILInstructionKind::MoveOnlyWrapperToCopyableValueInst: {
    auto Ty = MF->getType(TyID);
    bool isOwned = bool(Attr);
    if (isOwned)
      ResultInst = Builder.createOwnedMoveOnlyWrapperToCopyableValue(
          Loc, getLocalValue(ValID,
                             getSILType(Ty, (SILValueCategory)TyCategory, Fn)));
    else
      ResultInst = Builder.createGuaranteedMoveOnlyWrapperToCopyableValue(
          Loc, getLocalValue(ValID,
                             getSILType(Ty, (SILValueCategory)TyCategory, Fn)));
    break;
  }

  case SILInstructionKind::CopyableToMoveOnlyWrapperValueInst: {
    auto Ty = MF->getType(TyID);
    bool isOwned = bool(Attr);
    if (isOwned)
      ResultInst = Builder.createOwnedCopyableToMoveOnlyWrapperValue(
          Loc, getLocalValue(ValID,
                             getSILType(Ty, (SILValueCategory)TyCategory, Fn)));
    else
      ResultInst = Builder.createGuaranteedCopyableToMoveOnlyWrapperValue(
          Loc, getLocalValue(ValID,
                             getSILType(Ty, (SILValueCategory)TyCategory, Fn)));
    break;
  }

  case SILInstructionKind::LoadInst: {
    auto Ty = MF->getType(TyID);
    auto Qualifier = LoadOwnershipQualifier(Attr);
    ResultInst = Builder.createLoad(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)),
        Qualifier);
    break;
  }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...)             \
  case SILInstructionKind::Load##Name##Inst: {                                 \
    auto Ty = MF->getType(TyID);                                               \
    bool isTake = (Attr > 0);                                                  \
    auto Val = getLocalValue(                                                  \
        ValID, getSILType(Ty, SILValueCategory(TyCategory), Fn));              \
    ResultInst = Builder.createLoad##Name(Loc, Val, IsTake_t(isTake));         \
    break;                                                                     \
  }                                                                            \
  case SILInstructionKind::Store##Name##Inst: {                                \
    auto Ty = MF->getType(TyID);                                               \
    SILType addrType = getSILType(Ty, (SILValueCategory)TyCategory, Fn);       \
    auto refType = addrType.castTo<Name##StorageType>();                       \
    auto ValType = SILType::getPrimitiveObjectType(refType.getReferentType()); \
    bool isInit = (Attr > 0);                                                  \
    ResultInst = Builder.createStore##Name(Loc, getLocalValue(ValID, ValType), \
                                           getLocalValue(ValID2, addrType),    \
                                           IsInitialization_t(isInit));        \
    break;                                                                     \
  }
#include "swift/AST/ReferenceStorage.def"
  case SILInstructionKind::MarkUninitializedInst: {
    auto Ty = getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);
    auto Kind = (MarkUninitializedInst::Kind)Attr;
    auto Val = getLocalValue(ValID, Ty);
    ResultInst = Builder.createMarkUninitialized(Loc, Val, Kind);
    break;
  }
  case SILInstructionKind::MarkMustCheckInst: {
    using CheckKind = MarkMustCheckInst::CheckKind;
    auto Ty = MF->getType(TyID);
    auto CKind = CheckKind(Attr);
    ResultInst = Builder.createMarkMustCheckInst(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)),
        CKind);
    break;
  }
  case SILInstructionKind::StoreInst: {
    auto Ty = MF->getType(TyID);
    SILType addrType = getSILType(Ty, (SILValueCategory)TyCategory, Fn);
    SILType ValType = addrType.getObjectType();
    auto Qualifier = StoreOwnershipQualifier(Attr);
    ResultInst =
        Builder.createStore(Loc, getLocalValue(ValID, ValType),
                            getLocalValue(ValID2, addrType), Qualifier);
    break;
  }
  case SILInstructionKind::StoreBorrowInst: {
    auto Ty = MF->getType(TyID);
    SILType addrType = getSILType(Ty, (SILValueCategory)TyCategory, Fn);
    SILType ValType = addrType.getObjectType();
    ResultInst = Builder.createStoreBorrow(Loc, getLocalValue(ValID, ValType),
                                           getLocalValue(ValID2, addrType));
    break;
  }
  case SILInstructionKind::BeginAccessInst: {
    SILValue op = getLocalValue(
        ValID, getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));
    auto accessKind = SILAccessKind(Attr & 0x3);
    auto enforcement = SILAccessEnforcement((Attr >> 2) & 0x3);
    bool noNestedConflict = (Attr >> 4) & 0x01;
    bool fromBuiltin = (Attr >> 5) & 0x01;
    ResultInst = Builder.createBeginAccess(Loc, op, accessKind, enforcement,
                                           noNestedConflict, fromBuiltin);
    break;
  }
  case SILInstructionKind::EndAccessInst: {
    SILValue op = getLocalValue(
        ValID, getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));
    bool aborted = Attr & 0x1;
    ResultInst = Builder.createEndAccess(Loc, op, aborted);
    break;
  }
  case SILInstructionKind::BeginUnpairedAccessInst: {
    SILValue source = getLocalValue(
        ValID, getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));
    SILValue buffer =
        getLocalValue(ValID2, getSILType(MF->getType(TyID2),
                                         (SILValueCategory)TyCategory2, Fn));
    auto accessKind = SILAccessKind(Attr & 0x3);
    auto enforcement = SILAccessEnforcement((Attr >> 2) & 0x03);
    bool noNestedConflict = (Attr >> 4) & 0x01;
    bool fromBuiltin = (Attr >> 5) & 0x01;
    ResultInst = Builder.createBeginUnpairedAccess(
        Loc, source, buffer, accessKind, enforcement, noNestedConflict,
        fromBuiltin);
    break;
  }
  case SILInstructionKind::EndUnpairedAccessInst: {
    SILValue op = getLocalValue(
        ValID, getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));
    bool aborted = Attr & 0x1;
    auto enforcement = SILAccessEnforcement((Attr >> 1) & 0x03);
    bool fromBuiltin = (Attr >> 3) & 0x01;
    ResultInst = Builder.createEndUnpairedAccess(Loc, op, enforcement, aborted,
                                                 fromBuiltin);
    break;
  }
  case SILInstructionKind::CopyAddrInst: {
    auto Ty = MF->getType(TyID);
    SILType addrType = getSILType(Ty, (SILValueCategory)TyCategory, Fn);
    bool isInit = (Attr & 0x2) > 0;
    bool isTake = (Attr & 0x1) > 0;
    ResultInst = Builder.createCopyAddr(
        Loc, getLocalValue(ValID, addrType), getLocalValue(ValID2, addrType),
        IsTake_t(isTake), IsInitialization_t(isInit));
    break;
  }
  case SILInstructionKind::ExplicitCopyAddrInst: {
    auto Ty = MF->getType(TyID);
    SILType addrType = getSILType(Ty, (SILValueCategory)TyCategory, Fn);
    bool isInit = (Attr & 0x2) > 0;
    bool isTake = (Attr & 0x1) > 0;
    ResultInst = Builder.createExplicitCopyAddr(
        Loc, getLocalValue(ValID, addrType), getLocalValue(ValID2, addrType),
        IsTake_t(isTake), IsInitialization_t(isInit));
    break;
  }

  case SILInstructionKind::MarkUnresolvedMoveAddrInst: {
    auto Ty = MF->getType(TyID);
    SILType addrType = getSILType(Ty, (SILValueCategory)TyCategory, Fn);
    ResultInst = Builder.createMarkUnresolvedMoveAddr(
        Loc, getLocalValue(ValID, addrType), getLocalValue(ValID2, addrType));
    break;
  }

  case SILInstructionKind::AssignInst: {
    auto Ty = MF->getType(TyID);
    SILType addrType = getSILType(Ty, (SILValueCategory)TyCategory, Fn);
    SILType valType = addrType.getObjectType();
    auto qualifier = AssignOwnershipQualifier(Attr);
    ResultInst =
        Builder.createAssign(Loc, getLocalValue(ValID, valType),
                             getLocalValue(ValID2, addrType), qualifier);
    break;
  }
  case SILInstructionKind::AssignByWrapperInst:
    llvm_unreachable("not supported");
  case SILInstructionKind::BindMemoryInst: {
    assert(RecordKind == SIL_ONE_TYPE_VALUES &&
           "Layout should be OneTypeValues.");
    auto Ty = MF->getType(TyID);   // BoundTy
    ResultInst = Builder.createBindMemory(
        Loc,
        getLocalValue(ListOfValues[2],
                      getSILType(MF->getType(ListOfValues[0]),
                                 (SILValueCategory)ListOfValues[1], Fn)),
        getLocalValue(ListOfValues[5],
                      getSILType(MF->getType(ListOfValues[3]),
                                 (SILValueCategory)ListOfValues[4], Fn)),
        getSILType(Ty, (SILValueCategory)TyCategory, Fn));
    break;
  }
  case SILInstructionKind::RebindMemoryInst: {
    assert(RecordKind == SIL_TWO_OPERANDS && "Layout should be TwoOperands.");
    auto Ty = MF->getType(TyID);
    auto Ty2 = MF->getType(TyID2);
    ResultInst = Builder.createRebindMemory(
        Loc,
        getLocalValue(ValID, getSILType(Ty, (SILValueCategory)TyCategory, Fn)),
        getLocalValue(ValID2,
                      getSILType(Ty2, (SILValueCategory)TyCategory2, Fn)));
    break;
  }
  case SILInstructionKind::StructElementAddrInst:
  case SILInstructionKind::StructExtractInst: {
    // Use SILOneValueOneOperandLayout.
    VarDecl *Field = cast<VarDecl>(MF->getDecl(ValID));
    auto Ty = MF->getType(TyID);
    auto Val =
        getLocalValue(ValID2, getSILType(Ty, (SILValueCategory)TyCategory, Fn));
    auto ResultTy = Val->getType().getFieldType(
        Field, SILMod, Builder.getTypeExpansionContext());
    if (OpCode == SILInstructionKind::StructElementAddrInst)
      ResultInst = Builder.createStructElementAddr(Loc, Val, Field,
                                                   ResultTy.getAddressType());
    else
      ResultInst = Builder.createStructExtract(Loc, Val, Field,
                                               ResultTy.getObjectType());
    break;
  }
  case SILInstructionKind::StructInst: {
    // Format: a type followed by a list of typed values. A typed value is
    // expressed by 4 IDs: TypeID, TypeCategory, ValueID, ValueResultNumber.
    auto Ty = MF->getType(TyID);
    SmallVector<SILValue, 4> OpList;
    for (unsigned I = 0, E = ListOfValues.size(); I < E; I += 3) {
      auto EltTy = MF->getType(ListOfValues[I]);
      OpList.push_back(getLocalValue(
          ListOfValues[I + 2],
          getSILType(EltTy, (SILValueCategory)ListOfValues[I + 1], Fn)));
    }
    ResultInst = Builder.createStruct(
        Loc, getSILType(Ty, (SILValueCategory)TyCategory, Fn), OpList);
    break;
  }
  case SILInstructionKind::TupleElementAddrInst:
  case SILInstructionKind::TupleExtractInst: {
    // Use OneTypeOneOperand layout where the field number is stored in TypeID.
    auto Ty2 = MF->getType(TyID2);
    SILType ST = getSILType(Ty2, (SILValueCategory)TyCategory2, Fn);
    TupleType *TT = ST.castTo<TupleType>();

    auto ResultTy = TT->getElement(TyID).getType();
    switch (OpCode) {
    default: llvm_unreachable("Out of sync with parent switch");
    case SILInstructionKind::TupleElementAddrInst:
      ResultInst = Builder.createTupleElementAddr(
          Loc, getLocalValue(ValID, ST), TyID,
          getSILType(ResultTy, SILValueCategory::Address, Fn));
      break;
    case SILInstructionKind::TupleExtractInst:
      ResultInst = Builder.createTupleExtract(
          Loc, getLocalValue(ValID, ST), TyID,
          getSILType(ResultTy, SILValueCategory::Object, Fn));
      break;
    }
    break;
  }
  case SILInstructionKind::TupleInst: {
    // Format: a type followed by a list of values. A value is expressed by
    // 2 IDs: ValueID, ValueResultNumber.
    auto Ty = MF->getType(TyID);
    TupleType *TT = Ty->castTo<TupleType>();
    assert(TT && "Type of a TupleInst should be TupleType");
    SmallVector<SILValue, 4> OpList;
    for (unsigned I = 0, E = ListOfValues.size(); I < E; ++I) {
      Type EltTy = TT->getElement(I).getType();
      OpList.push_back(
        getLocalValue(ListOfValues[I],
                      getSILType(EltTy, SILValueCategory::Object, Fn)));
    }
    ResultInst = Builder.createTuple(
        Loc, getSILType(Ty, (SILValueCategory)TyCategory, Fn), OpList);
    break;
  }
  case SILInstructionKind::ObjectInst: {
    assert(RecordKind == SIL_ONE_TYPE_VALUES &&
           "Layout should be OneTypeValues.");
    unsigned NumVals = ListOfValues.size();
    assert(NumVals >= 1 && "Not enough values");
    unsigned numBaseElements = ListOfValues[0];
    SILType ClassTy =
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);
    SmallVector<SILValue, 4> elements;
    for (unsigned i = 1; i < NumVals; i += 2) {
      SILType elementType = getSILType(MF->getType(ListOfValues[i + 1]),
                                       SILValueCategory::Object, Fn);
      SILValue elementVal = getLocalValue(ListOfValues[i], elementType);
      elements.push_back(elementVal);
    }
    ResultInst = Builder.createObject(Loc, ClassTy, elements, numBaseElements);
    break;
  }
  case SILInstructionKind::BranchInst: {
    SmallVector<SILValue, 4> Args;
    for (unsigned I = 0, E = ListOfValues.size(); I < E; I += 3)
      Args.push_back(
          getLocalValue(ListOfValues[I + 2],
                        getSILType(MF->getType(ListOfValues[I]),
                                   (SILValueCategory)ListOfValues[I + 1], Fn)));

    ResultInst = Builder.createBranch(Loc, getBBForReference(Fn, TyID), Args);
    break;
  }
  case SILInstructionKind::CondBranchInst: {
    // Format: condition, true basic block ID, a list of arguments, false basic
    // block ID, a list of arguments. Use SILOneTypeValuesLayout: the type is
    // for condition, the list has value for condition, true basic block ID,
    // false basic block ID, number of true arguments, and a list of true|false
    // arguments.
    SILValue Cond = getLocalValue(
        ListOfValues[0],
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));

    unsigned NumTrueArgs = ListOfValues[3];
    unsigned StartOfTrueArg = 4;
    unsigned StartOfFalseArg = StartOfTrueArg + 3*NumTrueArgs;
    SmallVector<SILValue, 4> TrueArgs;
    for (unsigned I = StartOfTrueArg, E = StartOfFalseArg; I < E; I += 3)
      TrueArgs.push_back(
          getLocalValue(ListOfValues[I + 2],
                        getSILType(MF->getType(ListOfValues[I]),
                                   (SILValueCategory)ListOfValues[I + 1], Fn)));

    SmallVector<SILValue, 4> FalseArgs;
    for (unsigned I = StartOfFalseArg, E = ListOfValues.size(); I < E; I += 3)
      FalseArgs.push_back(
          getLocalValue(ListOfValues[I + 2],
                        getSILType(MF->getType(ListOfValues[I]),
                                   (SILValueCategory)ListOfValues[I + 1], Fn)));

    ResultInst = Builder.createCondBranch(
        Loc, Cond, getBBForReference(Fn, ListOfValues[1]), TrueArgs,
        getBBForReference(Fn, ListOfValues[2]), FalseArgs);
    break;
  }
  case SILInstructionKind::AwaitAsyncContinuationInst: {
    // Format: continuation, resume block ID, error block ID if given
    SILValue Cont = getLocalValue(
              ListOfValues[0],
              getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));
    
    SILBasicBlock *resultBB = getBBForReference(Fn, ListOfValues[1]);
    SILBasicBlock *errorBB = nullptr;
    if (ListOfValues.size() >= 3) {
      errorBB = getBBForReference(Fn, ListOfValues[2]);
    }
    
    ResultInst = Builder.createAwaitAsyncContinuation(Loc, Cont, resultBB, errorBB);
    break;
  }
  case SILInstructionKind::SwitchEnumInst:
  case SILInstructionKind::SwitchEnumAddrInst: {
    // Format: condition, a list of cases (EnumElementDecl + Basic Block ID),
    // default basic block ID. Use SILOneTypeValuesLayout: the type is
    // for condition, the list has value for condition, hasDefault, default
    // basic block ID, a list of (DeclID, BasicBlock ID).
    SILValue Cond = getLocalValue(
        ListOfValues[0],
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));

    SILBasicBlock *DefaultBB = nullptr;
    if (ListOfValues[1])
      DefaultBB = getBBForReference(Fn, ListOfValues[2]);

    SmallVector<std::pair<EnumElementDecl*, SILBasicBlock*>, 4> CaseBBs;
    for (unsigned I = 3, E = ListOfValues.size(); I < E; I += 2) {
      CaseBBs.push_back( {cast<EnumElementDecl>(MF->getDecl(ListOfValues[I])),
                            getBBForReference(Fn, ListOfValues[I+1])} );
    }
    if (OpCode == SILInstructionKind::SwitchEnumInst) {
      ResultInst = Builder.createSwitchEnum(Loc, Cond, DefaultBB, CaseBBs,
                                            None, ProfileCounter(),
                                            forwardingOwnership);
    } else {
      ResultInst = Builder.createSwitchEnumAddr(Loc, Cond, DefaultBB, CaseBBs);
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
    SILValue Cond = getLocalValue(ListOfValues[0],
                                  getSILType(MF->getType(TyID),
                                             (SILValueCategory)TyCategory, Fn));

    Type ResultLoweredTy = MF->getType(ListOfValues[1]);
    SILValueCategory ResultCategory = (SILValueCategory)ListOfValues[2];
    SILType ResultTy = getSILType(ResultLoweredTy, ResultCategory, Fn);

    SILValue DefaultVal = nullptr;
    if (ListOfValues[3])
      DefaultVal = getLocalValue(ListOfValues[4], ResultTy);

    SmallVector<std::pair<EnumElementDecl*, SILValue>, 4> CaseVals;
    for (unsigned I = 5, E = ListOfValues.size(); I < E; I += 2) {
      auto Value = getLocalValue(ListOfValues[I+1], ResultTy);
      CaseVals.push_back({cast<EnumElementDecl>(MF->getDecl(ListOfValues[I])),
                         Value});
    }
    if (OpCode == SILInstructionKind::SelectEnumInst)
      ResultInst =
          Builder.createSelectEnum(Loc, Cond, ResultTy, DefaultVal, CaseVals);
    else
      ResultInst = Builder.createSelectEnumAddr(Loc, Cond, ResultTy, DefaultVal,
                                                CaseVals);
    break;
  }
  case SILInstructionKind::SwitchValueInst: {
    // Format: condition, a list of cases (Value ID + Basic Block ID),
    // default basic block ID. Use SILOneTypeValuesLayout: the type is
    // for condition, the list contains value for condition, hasDefault, default
    // basic block ID, a list of (Value ID, BasicBlock ID).
    SILType ResultTy = getSILType(MF->getType(TyID),
                                             (SILValueCategory)TyCategory, Fn);
    SILValue Cond = getLocalValue(
        ListOfValues[0],
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));

    SILBasicBlock *DefaultBB = nullptr;
    if (ListOfValues[1])
      DefaultBB = getBBForReference(Fn, ListOfValues[2]);

    SmallVector<std::pair<SILValue, SILBasicBlock*>, 4> CaseBBs;
    for (unsigned I = 3, E = ListOfValues.size(); I < E; I += 2) {
      auto value = getLocalValue(ListOfValues[I], ResultTy);
      CaseBBs.push_back( {value, getBBForReference(Fn, ListOfValues[I+1])} );
    }
    ResultInst = Builder.createSwitchValue(Loc, Cond, DefaultBB, CaseBBs);
    break;
  }
  case SILInstructionKind::SelectValueInst: {
    // Format: condition, a list of cases (ValueID + Value ID),
    // default value ID. Use SILOneTypeValuesLayout: the type is
    // for condition, the list has value for condition, result type,
    // hasDefault, default,
    // basic block ID, a list of (Value ID, Value ID).
    SILValue Cond = getLocalValue(
        ListOfValues[0],
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));

    Type ResultLoweredTy = MF->getType(ListOfValues[1]);
    SILValueCategory ResultCategory = (SILValueCategory)ListOfValues[2];
    SILType ResultTy = getSILType(ResultLoweredTy, ResultCategory, Fn);

    SILValue DefaultVal = nullptr;
    if (ListOfValues[3])
      DefaultVal = getLocalValue(ListOfValues[4], ResultTy);

    SmallVector<std::pair<SILValue, SILValue>, 4> CaseValuesAndResults;
    for (unsigned I = 5, E = ListOfValues.size(); I < E; I += 2) {
      auto CaseValue = getLocalValue(ListOfValues[I], Cond->getType());
      auto Result = getLocalValue(ListOfValues[I+1], ResultTy);
      CaseValuesAndResults.push_back({CaseValue, Result});
    }

    ResultInst = Builder.createSelectValue(Loc, Cond, ResultTy, DefaultVal,
                                           CaseValuesAndResults);
    break;
  }  
  case SILInstructionKind::EnumInst: {
    // Format: a type, an operand and a decl ID. Use SILTwoOperandsLayout: type,
    // (DeclID + hasOperand), and an operand.
    SILValue Operand;
    if (Attr)
      Operand =
          getLocalValue(ValID2, getSILType(MF->getType(TyID2),
                                           (SILValueCategory)TyCategory2, Fn));
    ResultInst = Builder.createEnum(
        Loc, Operand, cast<EnumElementDecl>(MF->getDecl(ValID)),
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));
    break;
  }
  case SILInstructionKind::InitEnumDataAddrInst: {
    // Use SILOneValueOneOperandLayout.
    EnumElementDecl *Elt = cast<EnumElementDecl>(MF->getDecl(ValID));
    SILType OperandTy = getSILType(MF->getType(TyID),
                                   (SILValueCategory) TyCategory, Fn);
    SILType ResultTy = OperandTy.getEnumElementType(
        Elt, SILMod, Builder.getTypeExpansionContext());
    ResultInst = Builder.createInitEnumDataAddr(
        Loc, getLocalValue(ValID2, OperandTy), Elt, ResultTy);
    break;
  }
  case SILInstructionKind::UncheckedEnumDataInst: {
    // Use SILOneValueOneOperandLayout.
    EnumElementDecl *Elt = cast<EnumElementDecl>(MF->getDecl(ValID));
    SILType OperandTy =
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);
    SILType ResultTy = OperandTy.getEnumElementType(
        Elt, SILMod, Builder.getTypeExpansionContext());
    ResultInst = Builder.createUncheckedEnumData(
        Loc, getLocalValue(ValID2, OperandTy), Elt, ResultTy);
    break;
  }
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst: {
    // Use SILOneValueOneOperandLayout.
    EnumElementDecl *Elt = cast<EnumElementDecl>(MF->getDecl(ValID));
    SILType OperandTy =
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);
    SILType ResultTy = OperandTy.getEnumElementType(
        Elt, SILMod, Builder.getTypeExpansionContext());
    ResultInst = Builder.createUncheckedTakeEnumDataAddr(
        Loc, getLocalValue(ValID2, OperandTy), Elt, ResultTy);
    break;
  }
  case SILInstructionKind::InjectEnumAddrInst: {
    // Use SILOneValueOneOperandLayout.
    EnumElementDecl *Elt = cast<EnumElementDecl>(MF->getDecl(ValID));
    auto Ty = MF->getType(TyID);
    ResultInst = Builder.createInjectEnumAddr(
        Loc,
        getLocalValue(ValID2, getSILType(Ty, (SILValueCategory)TyCategory, Fn)),
        Elt);
    break;
  }
  case SILInstructionKind::RefElementAddrInst: {
    // Use SILOneValueOneOperandLayout.
    VarDecl *Field = cast<VarDecl>(MF->getDecl(ValID));
    auto Ty = MF->getType(TyID);
    auto Val = getLocalValue(ValID2,
                             getSILType(Ty, (SILValueCategory)TyCategory, Fn));
    auto ResultTy = Val->getType().getFieldType(
        Field, SILMod, Builder.getTypeExpansionContext());
    ResultInst = Builder.createRefElementAddr(Loc, Val, Field, ResultTy,
                                              /*Immutable*/ Attr & 0x1);
    break;
  }
  case SILInstructionKind::RefTailAddrInst: {
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND &&
           "Layout should be OneTypeOneOperand.");
    assert((SILValueCategory)TyCategory == SILValueCategory::Address);
    ResultInst = Builder.createRefTailAddr(
        Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID2),
                                        (SILValueCategory)TyCategory2, Fn)),
        getSILType(MF->getType(TyID), SILValueCategory::Address, Fn),
        /*Immutable*/ Attr & 0x1);
    break;
  }
  case SILInstructionKind::ClassMethodInst:
  case SILInstructionKind::SuperMethodInst:
  case SILInstructionKind::ObjCMethodInst:
  case SILInstructionKind::ObjCSuperMethodInst: {
    // Format: a type, an operand and a SILDeclRef. Use SILOneTypeValuesLayout:
    // type, Attr, SILDeclRef (DeclID, Kind, uncurryLevel), and an operand.
    unsigned NextValueIndex = 0;
    SILDeclRef DRef = getSILDeclRef(MF, ListOfValues, NextValueIndex);
    SILType Ty =
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);
    assert(ListOfValues.size() >= NextValueIndex + 2 &&
           "Out of entries for MethodInst");
    SILType operandTy =
        getSILType(MF->getType(ListOfValues[NextValueIndex]),
                   (SILValueCategory)ListOfValues[NextValueIndex + 1], Fn);
    NextValueIndex += 2;

    switch (OpCode) {
    default: llvm_unreachable("Out of sync with parent switch");
    case SILInstructionKind::ClassMethodInst:
      ResultInst = Builder.createClassMethod(
          Loc, getLocalValue(ListOfValues[NextValueIndex], operandTy), DRef,
          Ty);
      break;
    case SILInstructionKind::SuperMethodInst:
      ResultInst = Builder.createSuperMethod(
          Loc, getLocalValue(ListOfValues[NextValueIndex], operandTy), DRef,
          Ty);
      break;
    case SILInstructionKind::ObjCMethodInst:
      ResultInst = Builder.createObjCMethod(
          Loc, getLocalValue(ListOfValues[NextValueIndex], operandTy), DRef,
          Ty);
      break;
    case SILInstructionKind::ObjCSuperMethodInst:
      ResultInst = Builder.createObjCSuperMethod(
          Loc, getLocalValue(ListOfValues[NextValueIndex], operandTy), DRef,
          Ty);
      break;
    }
    break;
  }
  case SILInstructionKind::WitnessMethodInst: {
    unsigned NextValueIndex = 0;
    SILDeclRef DRef = getSILDeclRef(MF, ListOfValues, NextValueIndex);
    assert(ListOfValues.size() >= NextValueIndex &&
           "Out of entries for MethodInst");

    CanType Ty = MF->getType(TyID)->getCanonicalType();
    SILType OperandTy =
        getSILType(MF->getType(TyID2), (SILValueCategory)TyCategory2, Fn);

    auto Conformance = MF->getConformance(ConformanceID);
    // Read the optional opened existential.
    SILValue ExistentialOperand;
    if (TyID3) {
      SILType ExistentialOperandTy =
          getSILType(MF->getType(TyID3), (SILValueCategory)TyCategory3, Fn);
      if (ValID3)
        ExistentialOperand = getLocalValue(ValID3, ExistentialOperandTy);
    }
    ResultInst =
        Builder.createWitnessMethod(Loc, Ty, Conformance, DRef, OperandTy);
    break;
  }
  case SILInstructionKind::DynamicMethodBranchInst: {
    // Format: a typed value, a SILDeclRef, a BasicBlock ID for method,
    // a BasicBlock ID for no method. Use SILOneTypeValuesLayout.
    unsigned NextValueIndex = 1;
    SILDeclRef DRef = getSILDeclRef(MF, ListOfValues, NextValueIndex);
    assert(ListOfValues.size() == NextValueIndex + 2 &&
           "Wrong number of entries for DynamicMethodBranchInst");
    ResultInst = Builder.createDynamicMethodBranch(
        Loc,
        getLocalValue(
            ListOfValues[0],
            getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn)),
        DRef, getBBForReference(Fn, ListOfValues[NextValueIndex]),
        getBBForReference(Fn, ListOfValues[NextValueIndex + 1]));
    break;
  }
  case SILInstructionKind::CheckedCastBranchInst: {
    // Format: the cast kind, a typed value, a BasicBlock ID for success,
    // a BasicBlock ID for failure. Uses SILOneTypeValuesLayout.
    bool isExact = ListOfValues[0] != 0;
    SILType opTy = getSILType(MF->getType(ListOfValues[2]),
                              (SILValueCategory)ListOfValues[3], Fn);
    SILValue op = getLocalValue(ListOfValues[1], opTy);
    SILType targetLoweredType =
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);
    CanType targetFormalType =
        MF->getType(ListOfValues[4])->getCanonicalType();
    auto *successBB = getBBForReference(Fn, ListOfValues[5]);
    auto *failureBB = getBBForReference(Fn, ListOfValues[6]);

    ResultInst =
        Builder.createCheckedCastBranch(Loc, isExact, op, targetLoweredType,
                                        targetFormalType, successBB, failureBB,
                                        forwardingOwnership);
    break;
  }
  case SILInstructionKind::UnconditionalCheckedCastAddrInst: {
    // ignore attr.
    CanType srcFormalType = MF->getType(ListOfValues[0])->getCanonicalType();
    SILType srcLoweredType = getSILType(MF->getType(ListOfValues[2]),
                                       (SILValueCategory)ListOfValues[3], Fn);
    SILValue src = getLocalValue(ListOfValues[1], srcLoweredType);

    CanType targetFormalType = MF->getType(ListOfValues[4])->getCanonicalType();
    SILType targetLoweredType =
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);
    SILValue dest = getLocalValue(ListOfValues[5], targetLoweredType);

    ResultInst = Builder.createUnconditionalCheckedCastAddr(
        Loc, src, srcFormalType, dest, targetFormalType);
    break;
  }
  case SILInstructionKind::CheckedCastAddrBranchInst: {
    CastConsumptionKind consumption = getCastConsumptionKind(ListOfValues[0]);

    CanType srcFormalType = MF->getType(ListOfValues[1])->getCanonicalType();
    SILType srcLoweredType = getSILType(MF->getType(ListOfValues[3]),
                                        (SILValueCategory)ListOfValues[4], Fn);
    SILValue src = getLocalValue(ListOfValues[2], srcLoweredType);

    CanType targetFormalType =
        MF->getType(ListOfValues[5])->getCanonicalType();
    SILType targetLoweredType =
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);
    SILValue dest = getLocalValue(ListOfValues[6], targetLoweredType);

    auto *successBB = getBBForReference(Fn, ListOfValues[7]);
    auto *failureBB = getBBForReference(Fn, ListOfValues[8]);
    ResultInst = Builder.createCheckedCastAddrBranch(
        Loc, consumption, src, srcFormalType, dest, targetFormalType, successBB,
        failureBB);
    break;
  }
  case SILInstructionKind::UncheckedRefCastInst: {
    assert(RecordKind == SIL_ONE_TYPE_ONE_OPERAND &&
           "Layout should be OneTypeOneOperand.");
    auto *urc = Builder.createUncheckedRefCast(Loc,
        getLocalValue(ValID, getSILType(MF->getType(TyID2),
                                        (SILValueCategory)TyCategory2, Fn)),
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn));
    urc->setForwardingOwnershipKind(decodeValueOwnership(Attr));
    ResultInst = urc;
    break;
  }
  case SILInstructionKind::UncheckedRefCastAddrInst: {
    CanType sourceType = MF->getType(ListOfValues[0])->getCanonicalType();
    // ignore attr.
    SILType srcAddrTy = getSILType(MF->getType(ListOfValues[2]),
                                   (SILValueCategory)ListOfValues[3], Fn);
    SILValue src = getLocalValue(ListOfValues[1], srcAddrTy);

    CanType targetType = MF->getType(ListOfValues[4])->getCanonicalType();
    SILType destAddrTy =
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);
    SILValue dest = getLocalValue(ListOfValues[5], destAddrTy);

    ResultInst = Builder.createUncheckedRefCastAddr(Loc, src, sourceType, dest,
                                                    targetType);
    break;
  }
  case SILInstructionKind::InitBlockStorageHeaderInst: {
    assert(ListOfValues.size() == 5 &&
           "expected 5 values for InitBlockStorageHeader");
    SILType blockTy =
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);

    SILType storageTy = getSILType(MF->getType(ListOfValues[1]),
                                   SILValueCategory::Address, Fn);
    SILValue storage
      = getLocalValue(ListOfValues[0], storageTy);

    SILType invokeTy =
        getSILType(MF->getType(ListOfValues[3]), SILValueCategory::Object, Fn);
    SILValue invoke
      = getLocalValue(ListOfValues[2], invokeTy);
    
    auto SubMap = MF->getSubstitutionMap(ListOfValues[4]);

    ResultInst = Builder.createInitBlockStorageHeader(Loc, storage, invoke,
                                                      blockTy, SubMap);
    break;
  }
  case SILInstructionKind::UnreachableInst: {
    ResultInst = Builder.createUnreachable(Loc);
    break;
  }
  case SILInstructionKind::UnwindInst: {
    ResultInst = Builder.createUnwind(Loc);
    break;
  }
  case SILInstructionKind::YieldInst: {
    SILBasicBlock *unwindBB = getBBForReference(Fn, ListOfValues.back());
    ListOfValues = ListOfValues.drop_back();
    SILBasicBlock *resumeBB = getBBForReference(Fn, ListOfValues.back());
    ListOfValues = ListOfValues.drop_back();

    SmallVector<SILValue, 4> yieldedValues;
    for (unsigned I = 0, E = ListOfValues.size(); I < E; I += 3) {
      auto valueTy = MF->getType(ListOfValues[I]);
      auto valueCategory = (SILValueCategory) ListOfValues[I+1];
      yieldedValues.push_back(getLocalValue(
          ListOfValues[I + 2], getSILType(valueTy, valueCategory, Fn)));
    }

    ResultInst = Builder.createYield(Loc, yieldedValues, resumeBB, unwindBB);
    break;
  }
  case SILInstructionKind::KeyPathInst: {
    unsigned nextValue = 0;
    SILType kpTy =
        getSILType(MF->getType(TyID), (SILValueCategory)TyCategory, Fn);

    auto rootTy = MF->getType(ListOfValues[nextValue++]);
    auto valueTy = MF->getType(ListOfValues[nextValue++]);
    auto numComponents = ListOfValues[nextValue++];
    auto numOperands = ListOfValues[nextValue++];
    auto subMap = MF->getSubstitutionMap(ListOfValues[nextValue++]);
    auto objcString = MF->getIdentifierText(ListOfValues[nextValue++]);

    SmallVector<GenericTypeParamType *, 4> genericParams;
    SmallVector<Requirement, 4> requirements;
    auto numGenericParams = ListOfValues[nextValue++];
    for (unsigned i = 0; i != numGenericParams; ++i) {
      genericParams.push_back(MF->getType(ListOfValues[nextValue++])
                                ->castTo<GenericTypeParamType>());
    }
    if (numGenericParams != 0) {
      MF->deserializeGenericRequirements(ListOfValues, nextValue, requirements);
    }
    
    SmallVector<KeyPathPatternComponent, 4> components;
    components.reserve(numComponents);
    while (numComponents-- > 0) {
      components.push_back(*readKeyPathComponent(ListOfValues, nextValue));
    }
    
    CanGenericSignature sig = CanGenericSignature();
    if (!genericParams.empty() || !requirements.empty())
      sig = GenericSignature::get(genericParams, requirements)
                .getCanonicalSignature();

    auto pattern = KeyPathPattern::get(SILMod, sig,
                                       rootTy->getCanonicalType(),
                                       valueTy->getCanonicalType(),
                                       components,
                                       objcString);
    
    SmallVector<SILValue, 4> operands;
    
    operands.reserve(numOperands);
    while (numOperands-- > 0) {
      auto opValue = ListOfValues[nextValue++];
      auto opTy = MF->getType(ListOfValues[nextValue++]);
      auto opCat = (SILValueCategory)ListOfValues[nextValue++];
      operands.push_back(getLocalValue(opValue, getSILType(opTy, opCat, Fn)));
    }

    ResultInst = Builder.createKeyPath(Loc, pattern, subMap, operands, kpTy);
    break;
  }
  case SILInstructionKind::DifferentiableFunctionInst: {
    auto numParams = Attr;
    auto numResults = Attr2;
    auto numParamIndices = Attr3;
    bool hasDerivativeFunctions = (bool)Attr4;
    unsigned numOperands = hasDerivativeFunctions ? 3 : 1;
    auto numResultIndices =
        ListOfValues.size() - numOperands * 3 - numParamIndices;
    assert(ListOfValues.size() ==
           numParamIndices + numResultIndices + numOperands * 3);
    auto rawParamIndices =
        map<SmallVector<unsigned, 8>>(ListOfValues.take_front(numParamIndices),
                                      [](uint64_t i) { return (unsigned)i; });
    auto *paramIndices =
        IndexSubset::get(MF->getContext(), numParams, rawParamIndices);
    auto rawResultIndices = map<SmallVector<unsigned, 8>>(
        ListOfValues.slice(numParamIndices, numResultIndices),
        [](uint64_t i) { return (unsigned)i; });
    auto *resultIndices =
        IndexSubset::get(MF->getContext(), numResults, rawResultIndices);
    SmallVector<SILValue, 3> operands;
    for (auto i = numParamIndices + numResultIndices;
         i < numParamIndices + numOperands * 3; i += 3) {
      auto astTy = MF->getType(ListOfValues[i]);
      auto silTy = getSILType(astTy, (SILValueCategory)ListOfValues[i + 1], Fn);
      operands.push_back(getLocalValue(ListOfValues[i + 2], silTy));
    }
    Optional<std::pair<SILValue, SILValue>> derivativeFunctions = None;
    if (hasDerivativeFunctions)
      derivativeFunctions = std::make_pair(operands[1], operands[2]);
    ResultInst = Builder.createDifferentiableFunction(
        Loc, paramIndices, resultIndices, operands[0], derivativeFunctions);
    break;
  }
  case SILInstructionKind::LinearFunctionInst: {
    auto numDiffParams = Attr;
    bool hasLinearFunction = (bool)Attr2;
    unsigned numOperands = hasLinearFunction ? 2 : 1;
    auto numParamIndices = ListOfValues.size() - numOperands * 3;
    assert(ListOfValues.size() == numParamIndices + numOperands * 3);
    auto rawParamIndices =
       map<SmallVector<unsigned, 8>>(ListOfValues.take_front(numParamIndices),
                                     [](uint64_t i) { return (unsigned)i; });
    auto *paramIndices =
        IndexSubset::get(MF->getContext(), numDiffParams, rawParamIndices);
    SmallVector<SILValue, 3> operands;
    for (auto i = numParamIndices;
         i < numParamIndices + numOperands * 3; i += 3) {
      auto astTy = MF->getType(ListOfValues[i]);
      auto silTy = getSILType(astTy, (SILValueCategory)ListOfValues[i+1], Fn);
      operands.push_back(getLocalValue(ListOfValues[i+2], silTy));
    }
    Optional<SILValue> transposeFunction = None;
    if (hasLinearFunction)
      transposeFunction = operands[1];
    ResultInst = Builder.createLinearFunction(Loc, paramIndices, operands[0],
                                              transposeFunction);
    break;
  }
  case SILInstructionKind::DifferentiableFunctionExtractInst: {
    auto astTy = MF->getType(TyID);
    auto silTy = getSILType(astTy, SILValueCategory::Object, Fn);
    auto val = getLocalValue(ValID, silTy);
    NormalDifferentiableFunctionTypeComponent extractee(Attr);
    Optional<SILType> explicitExtracteeType = None;
    if (Attr2) {
      auto extracteeASTType = MF->getType(TyID2);
      explicitExtracteeType =
          getSILType(extracteeASTType, SILValueCategory::Object, Fn);
    }
    ResultInst = Builder.createDifferentiableFunctionExtract(
        Loc, extractee, val, explicitExtracteeType);
    break;
  }
  case SILInstructionKind::LinearFunctionExtractInst: {
    auto astTy = MF->getType(TyID);
    auto silTy = getSILType(astTy, SILValueCategory::Object, Fn);
    auto val = getLocalValue(ValID, silTy);
    LinearDifferentiableFunctionTypeComponent extractee(Attr);
    ResultInst = Builder.createLinearFunctionExtract(Loc, extractee, val);
    break;
  }
  case SILInstructionKind::DifferentiabilityWitnessFunctionInst: {
    StringRef mangledKey = MF->getIdentifierText(ValID);
    auto *witness = getSILDifferentiabilityWitnessForReference(mangledKey);
    assert(witness && "SILDifferentiabilityWitness not found");
    DifferentiabilityWitnessFunctionKind witnessKind(Attr);
    Optional<SILType> explicitFnTy = None;
    auto astTy = MF->getType(TyID);
    if (TyID)
      explicitFnTy = getSILType(astTy, SILValueCategory::Object, Fn);
    ResultInst = Builder.createDifferentiabilityWitnessFunction(
        Loc, witnessKind, witness, explicitFnTy);
    break;
  }
  case SILInstructionKind::HasSymbolInst: {
    ValueDecl *decl = cast<ValueDecl>(MF->getDecl(ValID));
    ResultInst = Builder.createHasSymbol(Loc, decl);
    // Deserialize the functions that are implicitly referenced by the
    // instruction.
    for (auto fnID : ListOfValues) {
      (void)getFuncForReference(MF->getIdentifierText(fnID));
    }
    break;
  }
  }

  for (auto result : ResultInst->getResults()) {
    LastValueID = LastValueID + 1;
    setLocalValue(result, LastValueID);
  }

  return false;
}

SILFunction *SILDeserializer::lookupSILFunction(SILFunction *InFunc,
                                                bool onlyUpdateLinkage) {
  StringRef name = InFunc->getName();
  if (!FuncTable)
    return nullptr;
  auto iter = FuncTable->find(name);
  if (iter == FuncTable->end())
    return nullptr;

  // Re-reading the function as declaration will update the linkage.
  auto maybeFunc = readSILFunctionChecked(*iter, InFunc, name,
                                        /*declarationOnly*/ onlyUpdateLinkage);
  if (!maybeFunc) {
    // Ignore the error; treat it as if we didn't have a definition.
    consumeError(maybeFunc.takeError());
    return nullptr;
  }

  if (maybeFunc.get()) {
    LLVM_DEBUG(llvm::dbgs() << "Deserialize SIL:\n";
               maybeFunc.get()->dump());
    assert(InFunc->getName() == maybeFunc.get()->getName());
  }

  return maybeFunc.get();
}

/// Check for existence of a function with a given name and required linkage.
/// This function is modeled after readSILFunction. But it does not
/// create a SILFunction object.
bool SILDeserializer::hasSILFunction(StringRef Name,
                                     Optional<SILLinkage> Linkage) {
  if (!FuncTable)
    return false;
  auto iter = FuncTable->find(Name);
  if (iter == FuncTable->end())
    return false;

  // There is a function with the required name.
  // Find out which linkage it has.
  auto FID = *iter;
  auto &cacheEntry = Funcs[FID-1];
  if (cacheEntry.isFullyDeserialized() ||
      (cacheEntry.isDeserialized()))
    return !Linkage || cacheEntry.get()->getLinkage() == *Linkage;

  BCOffsetRAII restoreOffset(SILCursor);
  if (llvm::Error Err = SILCursor.JumpToBit(cacheEntry.getOffset())) {
    MF->diagnoseAndConsumeFatal(std::move(Err));
    return false;
  }

  llvm::Expected<llvm::BitstreamEntry> maybeEntry =
      SILCursor.advance(AF_DontPopBlockAtEnd);
  if (!maybeEntry) {
    MF->diagnoseAndConsumeFatal(maybeEntry.takeError());
    return false;
  }
  llvm::BitstreamEntry entry = maybeEntry.get();
  if (entry.Kind == llvm::BitstreamEntry::Error) {
    MF->diagnoseAndConsumeFatal("Cursor advance error in hasSILFunction");
    return false;
  }

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  llvm::Expected<unsigned> maybeKind =
      SILCursor.readRecord(entry.ID, scratch, &blobData);
  if (!maybeKind)
    MF->fatal(maybeKind.takeError());
  unsigned kind = maybeKind.get();
  assert(kind == SIL_FUNCTION && "expect a sil function");
  (void)kind;

  // Read function properties only, e.g. its linkage and other attributes.
  // TODO: If this results in any noticeable performance problems, Cache the
  // linkage to avoid re-reading it from the bitcode each time?
  DeclID clangOwnerID;
  ModuleID parentModuleID;
  TypeID funcTyID;
  IdentifierID replacedFunctionID;
  IdentifierID usedAdHocWitnessFunctionID;
  GenericSignatureID genericSigID;
  unsigned rawLinkage, isTransparent, isSerialized, isThunk,
      isWithoutActuallyEscapingThunk, isGlobal, inlineStrategy,
      optimizationMode, perfConstr, subclassScope, hasCReferences, effect,
      numSpecAttrs, hasQualifiedOwnership, isWeakImported,
      LIST_VER_TUPLE_PIECES(available), isDynamic, isExactSelfClass,
      isDistributed, isRuntimeAccessible, forceEnableLexicalLifetimes;
  ArrayRef<uint64_t> SemanticsIDs;
  SILFunctionLayout::readRecord(
      scratch, rawLinkage, isTransparent, isSerialized, isThunk,
      isWithoutActuallyEscapingThunk, isGlobal, inlineStrategy,
      optimizationMode, perfConstr, subclassScope, hasCReferences, effect,
      numSpecAttrs, hasQualifiedOwnership, isWeakImported,
      LIST_VER_TUPLE_PIECES(available), isDynamic, isExactSelfClass,
      isDistributed, isRuntimeAccessible, forceEnableLexicalLifetimes, funcTyID,
      replacedFunctionID, usedAdHocWitnessFunctionID, genericSigID,
      clangOwnerID, parentModuleID, SemanticsIDs);
  auto linkage = fromStableSILLinkage(rawLinkage);
  if (!linkage) {
    LLVM_DEBUG(llvm::dbgs() << "invalid linkage code " << rawLinkage
                            << " for SIL function " << Name << "\n");
    return false;
  }

  // Bail if it is not a required linkage.
  if (Linkage && linkage.value() != *Linkage)
    return false;

  LLVM_DEBUG(llvm::dbgs() << "Found SIL Function: " << Name << "\n");
  return true;
}


SILFunction *SILDeserializer::lookupSILFunction(StringRef name,
                                                bool declarationOnly) {
  if (!FuncTable)
    return nullptr;
  auto iter = FuncTable->find(name);
  if (iter == FuncTable->end())
    return nullptr;

  auto maybeFunc = readSILFunctionChecked(*iter, nullptr, name,
                                          declarationOnly);

  if (!maybeFunc) {
    // Ignore the error; treat it as if we didn't have a definition.
    consumeError(maybeFunc.takeError());
    return nullptr;
  }

  if (maybeFunc.get()) {
    LLVM_DEBUG(llvm::dbgs() << "Deserialize SIL:\n";
               maybeFunc.get()->dump());
  }
  return maybeFunc.get();
}

SILGlobalVariable *SILDeserializer::readGlobalVar(StringRef Name) {
  if (!GlobalVarList)
    return nullptr;

  PrettyStackTraceStringAction trace("deserializing SIL global", Name);

  // If we already deserialized this global variable, just return it.
  if (auto *GV = SILMod.lookUpGlobalVariable(Name))
    return GV;

  // Find Id for the given name.
  auto iter = GlobalVarList->find(Name);
  if (iter == GlobalVarList->end())
    return nullptr;
  auto VId = *iter;
  if (VId == 0)
    return nullptr;

  assert(VId <= GlobalVars.size() && "invalid GlobalVar ID");
  auto &globalVarOrOffset = GlobalVars[VId-1];
  if (globalVarOrOffset.isFullyDeserialized())
    return globalVarOrOffset.get();

  BCOffsetRAII restoreOffset(SILCursor);
  if (llvm::Error Err = SILCursor.JumpToBit(globalVarOrOffset.getOffset()))
    MF->fatal(std::move(Err));
  llvm::Expected<llvm::BitstreamEntry> maybeEntry =
      SILCursor.advance(AF_DontPopBlockAtEnd);
  if (!maybeEntry)
    MF->fatal(maybeEntry.takeError());
  llvm::BitstreamEntry entry = maybeEntry.get();
  if (entry.Kind == llvm::BitstreamEntry::Error) {
    LLVM_DEBUG(llvm::dbgs() << "Cursor advance error in readGlobalVar.\n");
    return nullptr;
  }

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  llvm::Expected<unsigned> maybeKind =
      SILCursor.readRecord(entry.ID, scratch, &blobData);
  if (!maybeKind)
    MF->fatal(maybeKind.takeError());
  unsigned kind = maybeKind.get();
  assert(kind == SIL_GLOBALVAR && "expect a sil global var");
  (void)kind;

  TypeID TyID;
  DeclID dID;
  unsigned rawLinkage, isSerialized, IsDeclaration, IsLet;
  SILGlobalVarLayout::readRecord(scratch, rawLinkage, isSerialized,
                                 IsDeclaration, IsLet, TyID, dID);
  if (TyID == 0) {
    LLVM_DEBUG(llvm::dbgs() << "SILGlobalVariable typeID is 0.\n");
    return nullptr;
  }

  auto linkage = fromStableSILLinkage(rawLinkage);
  if (!linkage) {
    LLVM_DEBUG(llvm::dbgs() << "invalid linkage code " << rawLinkage
                            << " for SILGlobalVariable\n");
    return nullptr;
  }

  auto Ty = MF->getType(TyID);
  SILGlobalVariable *v = SILGlobalVariable::create(
      SILMod, linkage.value(),
      isSerialized ? IsSerialized : IsNotSerialized,
      Name.str(), getSILType(Ty, SILValueCategory::Object, nullptr),
      None,
      dID ? cast<VarDecl>(MF->getDecl(dID)): nullptr);
  v->setLet(IsLet);
  globalVarOrOffset.set(v, true /*isFullyDeserialized*/);
  v->setDeclaration(IsDeclaration);

  if (Callback)
    Callback->didDeserialize(MF->getAssociatedModule(), v);

  scratch.clear();
  maybeEntry = SILCursor.advance(AF_DontPopBlockAtEnd);
  if (!maybeEntry)
    MF->fatal(maybeEntry.takeError());
  entry = maybeEntry.get();
  if (entry.Kind == llvm::BitstreamEntry::EndBlock)
    return v;

  maybeKind = SILCursor.readRecord(entry.ID, scratch, &blobData);
  if (!maybeKind)
    MF->fatal(maybeKind.takeError());
  kind = maybeKind.get();

  SILBuilder Builder(v);
  
  llvm::DenseMap<uint32_t, ValueBase*> SavedLocalValues;
  serialization::ValueID SavedLastValueID = 1;
  
  SavedLocalValues.swap(LocalValues);
  std::swap(SavedLastValueID, LastValueID);

  while (kind != SIL_FUNCTION && kind != SIL_VTABLE && kind != SIL_GLOBALVAR &&
         kind != SIL_MOVEONLY_DEINIT && kind != SIL_WITNESS_TABLE &&
         kind != SIL_DIFFERENTIABILITY_WITNESS) {
    if (readSILInstruction(nullptr, Builder, kind, scratch))
      MF->fatal("readSILInstruction returns error");

    // Fetch the next record.
    scratch.clear();
    llvm::Expected<llvm::BitstreamEntry> maybeEntry =
        SILCursor.advance(AF_DontPopBlockAtEnd);
    if (!maybeEntry)
      MF->fatal(maybeEntry.takeError());
    llvm::BitstreamEntry entry = maybeEntry.get();

    // EndBlock means the end of this SILFunction.
    if (entry.Kind == llvm::BitstreamEntry::EndBlock)
      break;
    maybeKind = SILCursor.readRecord(entry.ID, scratch);
    if (!maybeKind)
      MF->fatal(maybeKind.takeError());
    kind = maybeKind.get();
  }

  SavedLocalValues.swap(LocalValues);
  std::swap(SavedLastValueID, LastValueID);

  return v;
}

void SILDeserializer::getAllSILGlobalVariables() {
  if (!GlobalVarList)
    return;

  for (auto Key : GlobalVarList->keys()) {
    readGlobalVar(Key);
  }
}

void SILDeserializer::getAllSILFunctions() {
  if (!FuncTable)
    return;

  for (auto KI = FuncTable->key_begin(), KE = FuncTable->key_end(); KI != KE;
       ++KI) {
    // Attempt to lookup our name from the output module. If we have a
    // definition already, don't do anything.
    if (SILFunction *F = SILMod.lookUpFunction(*KI))
      if (!F->isExternalDeclaration())
        continue;

    auto DI = FuncTable->find(*KI);
    assert(DI != FuncTable->end() && "There should never be a key without data.");

    auto maybeFunc = readSILFunctionChecked(*DI, nullptr, *KI, false,
                                            false/*errorIfEmptyBody*/);
    if (!maybeFunc) {
      // Ignore the error; treat it as if we didn't have a definition.
      consumeError(maybeFunc.takeError());
    }
  }
}

SILVTable *SILDeserializer::readVTable(DeclID VId) {
  if (VId == 0)
    return nullptr;
  assert(VId <= VTables.size() && "invalid VTable ID");
  auto &vTableOrOffset = VTables[VId-1];

  if (vTableOrOffset.isFullyDeserialized())
    return vTableOrOffset.get();

  BCOffsetRAII restoreOffset(SILCursor);
  if (llvm::Error Err = SILCursor.JumpToBit(vTableOrOffset.getOffset()))
    MF->fatal(std::move(Err));
  llvm::Expected<llvm::BitstreamEntry> maybeEntry =
      SILCursor.advance(AF_DontPopBlockAtEnd);
  if (!maybeEntry)
    MF->fatal(maybeEntry.takeError());
  llvm::BitstreamEntry entry = maybeEntry.get();
  if (entry.Kind == llvm::BitstreamEntry::Error) {
    LLVM_DEBUG(llvm::dbgs() << "Cursor advance error in readVTable.\n");
    return nullptr;
  }

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  llvm::Expected<unsigned> maybeKind =
      SILCursor.readRecord(entry.ID, scratch, &blobData);
  if (!maybeKind)
    MF->fatal(maybeKind.takeError());
  unsigned kind = maybeKind.get();
  assert(kind == SIL_VTABLE && "expect a sil vtable");
  (void)kind;

  DeclID ClassID;
  unsigned Serialized;
  VTableLayout::readRecord(scratch, ClassID, Serialized);
  if (ClassID == 0) {
    LLVM_DEBUG(llvm::dbgs() << "VTable classID is 0.\n");
    return nullptr;
  }

  ClassDecl *theClass = cast<ClassDecl>(MF->getDecl(ClassID));

  PrettyStackTraceDecl trace("deserializing SIL vtable for", theClass);

  // Fetch the next record.
  scratch.clear();
  maybeEntry = SILCursor.advance(AF_DontPopBlockAtEnd);
  if (!maybeEntry)
    MF->fatal(maybeEntry.takeError());
  entry = maybeEntry.get();
  if (entry.Kind == llvm::BitstreamEntry::EndBlock)
    // This vtable has no contents.
    return nullptr;
  maybeKind = SILCursor.readRecord(entry.ID, scratch);
  if (!maybeKind)
    MF->fatal(maybeKind.takeError());
  kind = maybeKind.get();

  std::vector<SILVTable::Entry> vtableEntries;
  // Another SIL_VTABLE record means the end of this VTable.
  while (kind != SIL_VTABLE && kind != SIL_WITNESS_TABLE &&
         kind != SIL_DEFAULT_WITNESS_TABLE && kind != SIL_FUNCTION &&
         kind != SIL_PROPERTY && kind != SIL_MOVEONLY_DEINIT) {
    assert(kind == SIL_VTABLE_ENTRY &&
           "Content of Vtable should be in SIL_VTABLE_ENTRY.");
    ArrayRef<uint64_t> ListOfValues;
    DeclID NameID;
    unsigned RawEntryKind, IsNonOverridden;
    VTableEntryLayout::readRecord(scratch, NameID,
                                  RawEntryKind, IsNonOverridden,
                                  ListOfValues);

    auto EntryKind = fromStableVTableEntryKind(RawEntryKind);

    SILFunction *Func = getFuncForReference(MF->getIdentifierText(NameID));
    if (Func) {
      unsigned NextValueIndex = 0;
      vtableEntries.emplace_back(getSILDeclRef(MF, ListOfValues, NextValueIndex),
                                 Func, EntryKind.value(), (bool)IsNonOverridden);
    }

    // Fetch the next record.
    scratch.clear();
    maybeEntry = SILCursor.advance(AF_DontPopBlockAtEnd);
    if (!maybeEntry)
      MF->fatal(maybeEntry.takeError());
    entry = maybeEntry.get();
    if (entry.Kind == llvm::BitstreamEntry::EndBlock)
      // EndBlock means the end of this VTable.
      break;
    maybeKind = SILCursor.readRecord(entry.ID, scratch);
    if (!maybeKind)
      MF->fatal(maybeKind.takeError());
    kind = maybeKind.get();
  }

  // If we've already serialized the module, don't mark the witness table
  // as serialized, since we no longer need to enforce resilience
  // boundaries.
  if (SILMod.isSerialized())
    Serialized = 0;

  SILVTable *vT = SILVTable::create(
      SILMod, theClass,
      Serialized ? IsSerialized : IsNotSerialized,
      vtableEntries);
  vTableOrOffset.set(vT, true /*isFullyDeserialized*/);

  if (Callback) Callback->didDeserialize(MF->getAssociatedModule(), vT);
  return vT;
}

SILVTable *SILDeserializer::lookupVTable(StringRef MangledClassName) {
  if (!VTableList)
    return nullptr;
  auto iter = VTableList->find(MangledClassName);
  if (iter == VTableList->end())
    return nullptr;

  auto VT = readVTable(*iter);
  return VT;
}

/// Deserialize all VTables inside the module and add them to SILMod.
void SILDeserializer::getAllVTables() {
  if (!VTableList)
    return;

  for (unsigned I = 0, E = VTables.size(); I < E; ++I)
    readVTable(I+1);
}

SILMoveOnlyDeinit *SILDeserializer::readMoveOnlyDeinit(DeclID tableID) {
  if (tableID == 0)
    return nullptr;
  assert(tableID <= MoveOnlyDeinits.size() && "invalid VTable ID");
  auto &moveOnlyDeinitOrOffset = MoveOnlyDeinits[tableID - 1];

  if (moveOnlyDeinitOrOffset.isFullyDeserialized())
    return moveOnlyDeinitOrOffset.get();

  BCOffsetRAII restoreOffset(SILCursor);
  if (llvm::Error Err = SILCursor.JumpToBit(moveOnlyDeinitOrOffset.getOffset()))
    MF->fatal(std::move(Err));
  llvm::Expected<llvm::BitstreamEntry> maybeEntry =
      SILCursor.advance(AF_DontPopBlockAtEnd);
  if (!maybeEntry)
    MF->fatal(maybeEntry.takeError());
  llvm::BitstreamEntry entry = maybeEntry.get();
  if (entry.Kind == llvm::BitstreamEntry::Error) {
    LLVM_DEBUG(llvm::dbgs() << "Cursor advance error in readVTable.\n");
    return nullptr;
  }

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  llvm::Expected<unsigned> maybeKind =
      SILCursor.readRecord(entry.ID, scratch, &blobData);
  if (!maybeKind)
    MF->fatal(maybeKind.takeError());
  unsigned kind = maybeKind.get();
  assert(kind == SIL_MOVEONLY_DEINIT && "expect a sil move only deinit table");
  (void)kind;

  DeclID nominalID;
  DeclID funcNameID;
  unsigned rawSerialized;
  MoveOnlyDeinitLayout::readRecord(scratch, nominalID, funcNameID,
                                   rawSerialized);
  if (nominalID == 0) {
    LLVM_DEBUG(llvm::dbgs() << "MoveOnlyDeinit nominalID is 0.\n");
    return nullptr;
  }

  auto *theNomDecl = cast<NominalTypeDecl>(MF->getDecl(nominalID));
  auto *theFunc = getFuncForReference(MF->getIdentifierText(funcNameID));

  // If we've already serialized the module, don't mark the witness table
  // as serialized, since we no longer need to enforce resilience
  // boundaries.
  if (SILMod.isSerialized())
    rawSerialized = 0;

  auto *deinit = SILMoveOnlyDeinit::create(
      SILMod, theNomDecl, rawSerialized ? IsSerialized : IsNotSerialized,
      theFunc);
  moveOnlyDeinitOrOffset.set(deinit, true /*isFullyDeserialized*/);

  if (Callback)
    Callback->didDeserialize(MF->getAssociatedModule(), deinit);
  return deinit;
}

SILMoveOnlyDeinit *
SILDeserializer::lookupMoveOnlyDeinit(StringRef MangledClassName) {
  if (!MoveOnlyDeinitList)
    return nullptr;
  auto iter = MoveOnlyDeinitList->find(MangledClassName);
  if (iter == MoveOnlyDeinitList->end())
    return nullptr;

  auto tbl = readMoveOnlyDeinit(*iter);
  return tbl;
}

/// Deserialize all move only deinit tables inside the module and add them to
/// SILMod.
void SILDeserializer::getAllMoveOnlyDeinits() {
  if (!MoveOnlyDeinitList)
    return;

  for (unsigned i : range(MoveOnlyDeinits.size()))
    readMoveOnlyDeinit(i + 1);
}

SILProperty *SILDeserializer::readProperty(DeclID PId) {
  auto &propOrOffset = Properties[PId-1];
  
  if (propOrOffset.isFullyDeserialized())
    return propOrOffset.get();

  BCOffsetRAII restoreOffset(SILCursor);
  if (llvm::Error Err = SILCursor.JumpToBit(propOrOffset.getOffset()))
    MF->fatal(std::move(Err));
  llvm::Expected<llvm::BitstreamEntry> maybeEntry =
      SILCursor.advance(AF_DontPopBlockAtEnd);
  if (!maybeEntry)
    MF->fatal(maybeEntry.takeError());
  llvm::BitstreamEntry entry = maybeEntry.get();
  if (entry.Kind == llvm::BitstreamEntry::Error) {
    LLVM_DEBUG(llvm::dbgs() << "Cursor advance error in readProperty.\n");
    return nullptr;
  }

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  llvm::Expected<unsigned> maybeKind =
      SILCursor.readRecord(entry.ID, scratch, &blobData);
  if (!maybeKind)
    MF->fatal(maybeKind.takeError());
  unsigned kind = maybeKind.get();
  assert(kind == SIL_PROPERTY && "expect a sil_property");
  (void)kind;

  unsigned Serialized;
  DeclID StorageID;
  ArrayRef<uint64_t> ComponentValues;
  PropertyLayout::readRecord(scratch, StorageID, Serialized, ComponentValues);
  
  auto decl = cast<AbstractStorageDecl>(MF->getDecl(StorageID));
  unsigned ComponentValueIndex = 0;
  auto component = readKeyPathComponent(ComponentValues, ComponentValueIndex);
  
  auto prop = SILProperty::create(SILMod, Serialized, decl, component);
  propOrOffset.set(prop, /*fully deserialized*/ true);
  return prop;
}

void SILDeserializer::getAllProperties() {
  for (unsigned I = 0, E = Properties.size(); I < E; ++I) {
    readProperty(I+1);
  }
}

void SILDeserializer::readWitnessTableEntries(
    llvm::BitstreamEntry &entry,
    std::vector<SILWitnessTable::Entry> &witnessEntries,
    std::vector<SILWitnessTable::ConditionalConformance>
      &conditionalConformances) {
  SmallVector<uint64_t, 64> scratch;
  llvm::Expected<unsigned> maybeKind = SILCursor.readRecord(entry.ID, scratch);
  if (!maybeKind)
    MF->fatal(maybeKind.takeError());
  unsigned kind = maybeKind.get();

  // Another record means the end of this WitnessTable.
  while (kind != SIL_WITNESS_TABLE &&
         kind != SIL_DEFAULT_WITNESS_TABLE &&
         kind != SIL_DIFFERENTIABILITY_WITNESS &&
         kind != SIL_FUNCTION) {
    if (kind == SIL_DEFAULT_WITNESS_TABLE_NO_ENTRY) {
      witnessEntries.push_back(SILDefaultWitnessTable::Entry());
    } else if (kind == SIL_WITNESS_BASE_ENTRY) {
      DeclID protoId;
      ProtocolConformanceID conformanceId;
      WitnessBaseEntryLayout::readRecord(scratch, protoId, conformanceId);
      ProtocolDecl *proto = cast<ProtocolDecl>(MF->getDecl(protoId));
      auto conformance = MF->getConformance(conformanceId);
      witnessEntries.push_back(SILWitnessTable::BaseProtocolWitness{
        proto, conformance.getConcrete()
      });
    } else if (kind == SIL_WITNESS_ASSOC_PROTOCOL) {
      TypeID assocId;
      DeclID protoId;
      ProtocolConformanceID conformanceId;
      WitnessAssocProtocolLayout::readRecord(scratch, assocId, protoId,
                                             conformanceId);
      CanType type = MF->getType(assocId)->getCanonicalType();
      ProtocolDecl *proto = cast<ProtocolDecl>(MF->getDecl(protoId));
      auto conformance = MF->getConformance(conformanceId);
      witnessEntries.push_back(SILWitnessTable::AssociatedTypeProtocolWitness{
        type, proto, conformance
      });
    } else if (kind == SIL_WITNESS_ASSOC_ENTRY) {
      DeclID assocId;
      TypeID tyId;
      WitnessAssocEntryLayout::readRecord(scratch, assocId, tyId);
      AssociatedTypeDecl *assoc = cast<AssociatedTypeDecl>(MF->getDecl(assocId));
      witnessEntries.push_back(SILWitnessTable::AssociatedTypeWitness{
        assoc, MF->getType(tyId)->getCanonicalType()
      });
    } else if (kind == SIL_WITNESS_METHOD_ENTRY) {
      ArrayRef<uint64_t> ListOfValues;
      DeclID NameID;
      WitnessMethodEntryLayout::readRecord(scratch, NameID, ListOfValues);
      SILFunction *Func = nullptr;
      if (NameID != 0) {
        Func = getFuncForReference(MF->getIdentifierText(NameID));
      }
      if (Func || NameID == 0) {
        unsigned NextValueIndex = 0;
        witnessEntries.push_back(SILWitnessTable::MethodWitness{
          getSILDeclRef(MF, ListOfValues, NextValueIndex), Func
        });
      }
    } else {
      assert(kind == SIL_WITNESS_CONDITIONAL_CONFORMANCE &&
             "Content of WitnessTable should be in "
             "SIL_WITNESS_CONDITIONAL_CONFORMANCE.");
      TypeID assocId;
      ProtocolConformanceID conformanceId;
      WitnessConditionalConformanceLayout::readRecord(scratch, assocId,
                                                      conformanceId);
      CanType type = MF->getType(assocId)->getCanonicalType();
      auto conformance = MF->getConformance(conformanceId);
      conditionalConformances.push_back(
          SILWitnessTable::ConditionalConformance{type, conformance});
    }

    // Fetch the next record.
    scratch.clear();
    llvm::Expected<llvm::BitstreamEntry> maybeEntry =
        SILCursor.advance(AF_DontPopBlockAtEnd);
    if (!maybeEntry)
      MF->fatal(maybeEntry.takeError());
    entry = maybeEntry.get();
    if (entry.Kind == llvm::BitstreamEntry::EndBlock)
      // EndBlock means the end of this WitnessTable.
      break;
    maybeKind = SILCursor.readRecord(entry.ID, scratch);
    if (!maybeKind)
      MF->fatal(maybeKind.takeError());
    kind = maybeKind.get();
  }
}

SILWitnessTable *SILDeserializer::readWitnessTable(DeclID WId,
                                                   SILWitnessTable *existingWt) {
  auto deserialized = readWitnessTableChecked(WId, existingWt);
  if (!deserialized) {
    MF->fatal(deserialized.takeError());
  }
  return deserialized.get();
}

llvm::Expected<SILWitnessTable *>
  SILDeserializer::readWitnessTableChecked(DeclID WId,
                                           SILWitnessTable *existingWt) {
  if (WId == 0)
    return nullptr;
  assert(WId <= WitnessTables.size() && "invalid WitnessTable ID");

  auto &wTableOrOffset = WitnessTables[WId-1];

  if (wTableOrOffset.isFullyDeserialized())
    return wTableOrOffset.get();

  BCOffsetRAII restoreOffset(SILCursor);
  if (llvm::Error Err = SILCursor.JumpToBit(wTableOrOffset.getOffset()))
    MF->fatal(std::move(Err));
  llvm::Expected<llvm::BitstreamEntry> maybeEntry =
      SILCursor.advance(AF_DontPopBlockAtEnd);
  if (!maybeEntry)
    MF->fatal(maybeEntry.takeError());
  llvm::BitstreamEntry entry = maybeEntry.get();
  if (entry.Kind == llvm::BitstreamEntry::Error) {
    LLVM_DEBUG(llvm::dbgs() << "Cursor advance error in readWitnessTable.\n");
    return nullptr;
  }

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  llvm::Expected<unsigned> maybeKind =
      SILCursor.readRecord(entry.ID, scratch, &blobData);
  if (!maybeKind)
    MF->fatal(maybeKind.takeError());
  unsigned kind = maybeKind.get();
  assert(kind == SIL_WITNESS_TABLE && "expect a sil witnesstable");
  (void)kind;

  unsigned RawLinkage;
  unsigned IsDeclaration;
  unsigned Serialized;
  ProtocolConformanceID conformance;
  WitnessTableLayout::readRecord(scratch, RawLinkage, IsDeclaration,
                                 Serialized, conformance);

  auto Linkage = fromStableSILLinkage(RawLinkage);
  if (!Linkage) {
    LLVM_DEBUG(llvm::dbgs() << "invalid linkage code " << RawLinkage
                            << " for SILFunction\n");
    MF->fatal("invalid linkage code");
  }

  // Deserialize Conformance.
  auto maybeConformance = MF->getConformanceChecked(conformance);
  if (!maybeConformance)
    return maybeConformance.takeError();

  auto theConformance = cast<RootProtocolConformance>(
                          maybeConformance.get().getConcrete());

  PrettyStackTraceConformance trace("deserializing SIL witness table for",
                                    theConformance);

  if (!existingWt)
    existingWt = SILMod.lookUpWitnessTable(theConformance);
  auto wT = existingWt;

  // If we have an existing witness table, verify that the conformance matches
  // up.
  if (wT) {
    if (wT->getConformance() != theConformance) {
      MF->fatal("Conformance mismatch");
    }

    // Don't override the linkage of a witness table with an existing
    // declaration.

  } else {
    // Otherwise, create a new witness table declaration.
    wT = SILWitnessTable::create(SILMod, *Linkage, theConformance);
    if (Callback)
      Callback->didDeserialize(MF->getAssociatedModule(), wT);
  }
  
  // We may see multiple shared-linkage definitions of the same witness table
  // for the same conformance.
  if (wT->isDefinition() && hasSharedVisibility(*Linkage)
      && hasSharedVisibility(wT->getLinkage())) {
    wTableOrOffset.set(wT, /*fully deserialized*/ true);
    return wT;
  }

  assert(wT->isDeclaration() && "Our witness table at this point must be a "
                                "declaration.");

  // If we are asked to just emit a declaration, return the declaration and say
  // that the witness table is not fully deserialized.
  if (IsDeclaration) {
    wTableOrOffset.set(wT, /*fully deserialized*/ false);
    return wT;
  }

  // Fetch the next record.
  scratch.clear();
  maybeEntry = SILCursor.advance(AF_DontPopBlockAtEnd);
  if (!maybeEntry)
    MF->fatal(maybeEntry.takeError());
  entry = maybeEntry.get();
  if (entry.Kind == llvm::BitstreamEntry::EndBlock)
    return nullptr;

  std::vector<SILWitnessTable::Entry> witnessEntries;
  std::vector<SILWitnessTable::ConditionalConformance> conditionalConformances;
  readWitnessTableEntries(entry, witnessEntries, conditionalConformances);

  // If we've already serialized the module, don't mark the witness table
  // as serialized, since we no longer need to enforce resilience
  // boundaries.
  if (SILMod.isSerialized())
    Serialized = 0;

  wT->convertToDefinition(witnessEntries, conditionalConformances,
                          Serialized ? IsSerialized : IsNotSerialized);
  wTableOrOffset.set(wT, /*fully deserialized*/ true);
  if (Callback)
    Callback->didDeserializeWitnessTableEntries(MF->getAssociatedModule(), wT);
  return wT;
}

/// Deserialize all WitnessTables inside the module and add them to SILMod.
void SILDeserializer::getAllWitnessTables() {
  if (!WitnessTableList)
    return;
  for (unsigned I = 0, E = WitnessTables.size(); I < E; ++I) {
    auto maybeTable = readWitnessTableChecked(I + 1, nullptr);
    if (!maybeTable) {
      if (maybeTable.errorIsA<XRefNonLoadedModuleError>()) {
        // This is most likely caused by decls hidden by an implementation-only
        // import, it is safe to ignore for this function's purpose.
        consumeError(maybeTable.takeError());
      } else {
        MF->diagnoseAndConsumeFatal(maybeTable.takeError());
      }
    }
  }
}

SILWitnessTable *
SILDeserializer::lookupWitnessTable(SILWitnessTable *existingWt) {
  assert(existingWt && "Cannot deserialize a null witness table declaration.");
  assert(existingWt->isDeclaration() && "Cannot deserialize a witness table "
                                        "definition.");

  // If we don't have a witness table list, we can't look anything up.
  if (!WitnessTableList)
    return nullptr;

  // Use the name of the given witness table to lookup the partially
  // deserialized value from the witness table list.
  auto iter = WitnessTableList->find(existingWt->getName());
  if (iter == WitnessTableList->end())
    return nullptr;

  // Attempt to read the witness table.
  auto Wt = readWitnessTable(*iter, existingWt);
  if (Wt)
    LLVM_DEBUG(llvm::dbgs() << "Deserialize SIL:\n"; Wt->dump());

  return Wt;
}

SILDefaultWitnessTable *SILDeserializer::
readDefaultWitnessTable(DeclID WId, SILDefaultWitnessTable *existingWt) {
  if (WId == 0)
    return nullptr;
  assert(WId <= DefaultWitnessTables.size() &&
         "invalid DefaultWitnessTable ID");

  auto &wTableOrOffset = DefaultWitnessTables[WId-1];

  if (wTableOrOffset.isFullyDeserialized())
    return wTableOrOffset.get();

  BCOffsetRAII restoreOffset(SILCursor);
  if (llvm::Error Err = SILCursor.JumpToBit(wTableOrOffset.getOffset()))
    MF->fatal(std::move(Err));
  llvm::Expected<llvm::BitstreamEntry> maybeEntry =
      SILCursor.advance(AF_DontPopBlockAtEnd);
  if (!maybeEntry)
    MF->fatal(maybeEntry.takeError());
  llvm::BitstreamEntry entry = maybeEntry.get();
  if (entry.Kind == llvm::BitstreamEntry::Error) {
    LLVM_DEBUG(llvm::dbgs() << "Cursor advance error in "
                               "readDefaultWitnessTable.\n");
    return nullptr;
  }

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  llvm::Expected<unsigned> maybeKind =
      SILCursor.readRecord(entry.ID, scratch, &blobData);
  if (!maybeKind)
    MF->fatal(maybeKind.takeError());
  unsigned kind = maybeKind.get();
  assert(kind == SIL_DEFAULT_WITNESS_TABLE && "expect a sil default witness table");
  (void)kind;

  unsigned RawLinkage;
  DeclID protoId;
  DefaultWitnessTableLayout::readRecord(scratch, protoId, RawLinkage);

  auto Linkage = fromStableSILLinkage(RawLinkage);
  if (!Linkage) {
    LLVM_DEBUG(llvm::dbgs() << "invalid linkage code " << RawLinkage
                            << " for SILFunction\n");
    MF->fatal("invalid linkage code");
  }

  ProtocolDecl *proto = cast<ProtocolDecl>(MF->getDecl(protoId));
  if (proto == nullptr) {
    LLVM_DEBUG(llvm::dbgs() << "invalid protocol code " << protoId << "\n");
    MF->fatal("invalid protocol code");
  }

  PrettyStackTraceDecl trace("deserializing default witness table for", proto);

  if (!existingWt)
    existingWt = SILMod.lookUpDefaultWitnessTable(proto, /*deserializeLazily=*/ false);
  auto wT = existingWt;

  // If we have an existing default witness table, verify that the protocol
  // matches up.
  if (wT) {
    if (wT->getProtocol() != proto) {
      MF->fatal("Protocol mismatch");
    }

    // Don't override the linkage of a default witness table with an existing
    // declaration.

  } else {
    // Otherwise, create a new witness table declaration.
    wT = SILDefaultWitnessTable::create(SILMod, *Linkage, proto);
    if (Callback)
      Callback->didDeserialize(MF->getAssociatedModule(), wT);
  }

  // Fetch the next record.
  scratch.clear();
  maybeEntry = SILCursor.advance(AF_DontPopBlockAtEnd);
  if (!maybeEntry)
    MF->fatal(maybeEntry.takeError());
  entry = maybeEntry.get();
  if (entry.Kind == llvm::BitstreamEntry::EndBlock)
    return nullptr;

  std::vector<SILWitnessTable::Entry> witnessEntries;
  std::vector<SILWitnessTable::ConditionalConformance> conditionalConformances;
  readWitnessTableEntries(entry, witnessEntries, conditionalConformances);

  wT->convertToDefinition(witnessEntries);
  wTableOrOffset.set(wT, /*fully deserialized*/ true);
  if (Callback)
    Callback->didDeserializeDefaultWitnessTableEntries(MF->getAssociatedModule(), wT);
  return wT;
}

/// Deserialize all DefaultWitnessTables inside the module and add them to SILMod.
void SILDeserializer::getAllDefaultWitnessTables() {
  if (!DefaultWitnessTableList)
    return;
  for (unsigned I = 0, E = DefaultWitnessTables.size(); I < E; ++I)
    readDefaultWitnessTable(I + 1, nullptr);
}

SILDefaultWitnessTable *
SILDeserializer::lookupDefaultWitnessTable(SILDefaultWitnessTable *existingWt) {
  assert(existingWt && "Cannot deserialize a null default witness table declaration.");
  assert(existingWt->isDeclaration() && "Cannot deserialize a default witness table "
                                        "definition.");

  // If we don't have a default witness table list, we can't look anything up.
  if (!DefaultWitnessTableList)
    return nullptr;

  // Use the mangled name of the protocol to lookup the partially
  // deserialized value from the default witness table list.
  auto iter = DefaultWitnessTableList->find(existingWt->getUniqueName());
  if (iter == DefaultWitnessTableList->end())
    return nullptr;

  // Attempt to read the default witness table.
  auto Wt = readDefaultWitnessTable(*iter, existingWt);
  if (Wt)
    LLVM_DEBUG(llvm::dbgs() << "Deserialize SIL:\n"; Wt->dump());

  return Wt;
}

SILDifferentiabilityWitness *
SILDeserializer::readDifferentiabilityWitness(DeclID DId) {
  if (DId == 0)
    return nullptr;
  assert(DId <= DifferentiabilityWitnesses.size() &&
         "Invalid SILDifferentiabilityWitness ID");

  auto &diffWitnessOrOffset = DifferentiabilityWitnesses[DId - 1];
  if (diffWitnessOrOffset.isFullyDeserialized())
    return diffWitnessOrOffset.get();

  BCOffsetRAII restoreOffset(SILCursor);
  if (auto err = SILCursor.JumpToBit(diffWitnessOrOffset.getOffset()))
    MF->fatal(std::move(err));
  llvm::Expected<llvm::BitstreamEntry> maybeEntry =
      SILCursor.advance(AF_DontPopBlockAtEnd);
  if (!maybeEntry)
    MF->fatal(maybeEntry.takeError());
  auto entry = maybeEntry.get();
  if (entry.Kind == llvm::BitstreamEntry::Error) {
    LLVM_DEBUG(llvm::dbgs() << "Cursor advance error in "
                               "readDefaultWitnessTable.\n");
    return nullptr;
  }

  SmallVector<uint64_t, 64> scratch;
  StringRef blobData;
  llvm::Expected<unsigned> maybeKind =
      SILCursor.readRecord(entry.ID, scratch, &blobData);
  if (!maybeKind)
    MF->fatal(maybeKind.takeError());
  unsigned kind = maybeKind.get();
  assert(kind == SIL_DIFFERENTIABILITY_WITNESS &&
         "Expected sil_differentiability_witness");
  (void)kind;

  DeclID originalNameId, jvpNameId, vjpNameId;
  unsigned rawLinkage, isDeclaration, isSerialized, rawDiffKind,
      numParameterIndices, numResultIndices;
  GenericSignatureID derivativeGenSigID;
  ArrayRef<uint64_t> rawParameterAndResultIndices;

  DifferentiabilityWitnessLayout::readRecord(
      scratch, originalNameId, rawLinkage, isDeclaration, isSerialized,
      rawDiffKind, derivativeGenSigID, jvpNameId, vjpNameId,
      numParameterIndices, numResultIndices, rawParameterAndResultIndices);

  if (isDeclaration) {
    assert(!isSerialized && "declaration must not be serialized");
  }

  auto linkageOpt = fromStableSILLinkage(rawLinkage);
  assert(linkageOpt &&
         "Expected value linkage for sil_differentiability_witness");
  auto diffKind = fromStableDifferentiabilityKind(rawDiffKind);
  assert(diffKind &&
         "Expected differentiability kind for sil_differentiability_witness");
  auto originalName = MF->getIdentifierText(originalNameId);
  auto jvpName = MF->getIdentifierText(jvpNameId);
  auto vjpName = MF->getIdentifierText(vjpNameId);
  auto *original = getFuncForReference(originalName);
  assert(original && "Original function must be found");
  auto *jvp = getFuncForReference(jvpName);
  if (!jvpName.empty()) {
    assert(!isDeclaration && "JVP must not be defined in declaration");
    assert(jvp && "JVP function must be found if JVP name is not empty");
  }
  auto *vjp = getFuncForReference(vjpName);
  if (!vjpName.empty()) {
    assert(!isDeclaration && "VJP must not be defined in declaration");
    assert(vjp && "VJP function must be found if VJP name is not empty");
  }
  auto derivativeGenSig = MF->getGenericSignature(derivativeGenSigID);

  auto originalFnType = original->getLoweredFunctionType();
  SmallVector<unsigned, 8> parameterAndResultIndices(
      rawParameterAndResultIndices.begin(), rawParameterAndResultIndices.end());
  assert(parameterAndResultIndices.size() ==
             numParameterIndices + numResultIndices &&
         "Parameter/result indices count mismatch");
  auto *parameterIndices =
      IndexSubset::get(MF->getContext(), originalFnType->getNumParameters(),
                       ArrayRef<unsigned>(parameterAndResultIndices)
                           .take_front(numParameterIndices));
  auto numResults = originalFnType->getNumResults() +
                    originalFnType->getNumIndirectMutatingParameters();
  auto *resultIndices =
      IndexSubset::get(MF->getContext(), numResults,
                       ArrayRef<unsigned>(parameterAndResultIndices)
                           .take_back(numResultIndices));

  AutoDiffConfig config(parameterIndices, resultIndices, derivativeGenSig);
  auto *diffWitness = SILMod.lookUpDifferentiabilityWitness(
      {originalName, *diffKind, config});

  // Witnesses that we deserialize are always available externally; we never
  // want to emit them ourselves.
  auto linkage = swift::addExternalToLinkage(*linkageOpt);

  // If there is no existing differentiability witness, create one.
  if (!diffWitness)
    diffWitness = SILDifferentiabilityWitness::createDeclaration(
        SILMod, linkage, original, *diffKind, parameterIndices, resultIndices,
        derivativeGenSig);

  // If the current differentiability witness is merely a declaration, and the
  // deserialized witness is a definition, upgrade the current differentiability
  // witness to a definition. This can happen in the following situations:
  // 1. The witness was just created above.
  // 2. The witness started out as a declaration (e.g. the differentiation
  //    pass emitted a witness for an external function) and now we're loading
  //    the definition (e.g. an optimization pass asked for the definition and
  //    we found the definition serialized in this module).
  if (diffWitness->isDeclaration() && !isDeclaration)
    diffWitness->convertToDefinition(jvp, vjp, isSerialized);

  diffWitnessOrOffset.set(diffWitness,
                          /*isFullyDeserialized*/ diffWitness->isDefinition());
  return diffWitness;
}

SILDifferentiabilityWitness *SILDeserializer::lookupDifferentiabilityWitness(
    StringRef mangledDiffWitnessKey) {
  if (!DifferentiabilityWitnessList)
    return nullptr;
  auto iter = DifferentiabilityWitnessList->find(mangledDiffWitnessKey);
  if (iter == DifferentiabilityWitnessList->end())
    return nullptr;
  return readDifferentiabilityWitness(*iter);
}

void SILDeserializer::getAllDifferentiabilityWitnesses() {
  if (!DifferentiabilityWitnessList)
    return;
  for (unsigned I = 0, E = DifferentiabilityWitnesses.size(); I < E; ++I)
    readDifferentiabilityWitness(I + 1);
}

SILDeserializer::~SILDeserializer() {
  // Drop our references to anything we've deserialized.
  for (auto &fnEntry : Funcs) {
    if (fnEntry.isDeserialized())
      fnEntry.get()->decrementRefCount();
  }
}

// Invalidate all cached SILFunctions.
void SILDeserializer::invalidateFunctionCache() {
  for (auto &fnEntry : Funcs)
    if (fnEntry.isDeserialized()) {
      fnEntry.get()->decrementRefCount();
      fnEntry.reset();
    }
}

bool SILDeserializer::invalidateFunction(SILFunction *F) {
  for (auto &fnEntry : Funcs) {
    if (fnEntry.isDeserialized() && fnEntry.get() == F) {
      fnEntry.get()->decrementRefCount();
      fnEntry.reset();
      return true;
    }
  }
  return false;
}

// Invalidate all cached SILGlobalVariable.
void SILDeserializer::invalidateGlobalVariableCache() {
  for (auto &entry : GlobalVars) {
    if (entry.isDeserialized()) {
      entry.reset();
    }
  }
}

// Invalidate a specific cached GlobalVariable.
bool SILDeserializer::invalidateGlobalVariable(SILGlobalVariable *gv) {
  for (auto &entry : GlobalVars) {
    if (entry.isDeserialized() && entry.get() == gv) {
      entry.reset();
      return true;
    }
  }

  return false;
}

// Invalidate all cached SILVTable.
void SILDeserializer::invalidateVTableCache() {
  for (auto &entry : VTables) {
    if (entry.isDeserialized()) {
      entry.reset();
    }
  }
}

// Invalidate a specific cached SILVTable.
bool SILDeserializer::invalidateVTable(SILVTable *v) {
  for (auto &entry : VTables) {
    if (entry.isDeserialized() && entry.get() == v) {
      entry.reset();
      return true;
    }
  }

  return false;
}

// Invalidate all cached SILWitnessTable.
void SILDeserializer::invalidateWitnessTableCache() {
  for (auto &entry : WitnessTables) {
    if (entry.isDeserialized()) {
      entry.reset();
    }
  }
}

// Invalidate a specific cached SILWitnessTable.
bool SILDeserializer::invalidateWitnessTable(SILWitnessTable *wt) {
  for (auto &entry : WitnessTables) {
    if (entry.isDeserialized() && entry.get() == wt) {
      entry.reset();
      return true;
    }
  }
  return false;
}

// Invalidate all cached SILDefaultWitnessTable.
void SILDeserializer::invalidateDefaultWitnessTableCache() {
  for (auto &entry : DefaultWitnessTables) {
    if (entry.isDeserialized()) {
      entry.reset();
    }
  }
}

// Invalidate a specific cached SILDefaultWitnessTable.
bool SILDeserializer::invalidateDefaultWitnessTable(
    SILDefaultWitnessTable *wt) {
  for (auto &entry : DefaultWitnessTables) {
    if (entry.isDeserialized() && entry.get() == wt) {
      entry.reset();
      return true;
    }
  }
  return false;
}

// Invalidate all cached SILProperty.
void SILDeserializer::invalidatePropertyCache() {
  for (auto &entry : Properties) {
    if (entry.isDeserialized()) {
      entry.reset();
    }
  }
}

// Invalidate a specific cached SILProperty.
bool SILDeserializer::invalidateProperty(SILProperty *p) {
  for (auto &entry : Properties) {
    if (entry.isDeserialized() && entry.get() == p) {
      entry.reset();
      return true;
    }
  }
  return false;
}

// Invalidate all cached SILDifferentiabilityWitness.
void SILDeserializer::invalidateDifferentiabilityWitnessCache() {
  for (auto &entry : DifferentiabilityWitnesses) {
    if (entry.isDeserialized()) {
      entry.reset();
    }
  }
}

// Invalidate a specific cached SILDifferentiabilityWitness.
bool SILDeserializer::invalidateDifferentiabilityWitness(
    SILDifferentiabilityWitness *w) {
  for (auto &entry : DifferentiabilityWitnesses) {
    if (entry.isDeserialized() && entry.get() == w) {
      entry.reset();
      return true;
    }
  }
  return false;
}
