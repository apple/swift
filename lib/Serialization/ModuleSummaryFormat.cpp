//===--- ModuleSummaryFormat.cpp - Read and write module summary files ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "ModuleSummaryFormat.h"
#include "BCReadingExtras.h"
#include "memory"
#include "swift/AST/FileSystem.h"
#include "swift/Serialization/ModuleSummary.h"
#include "llvm/Bitstream/BitstreamReader.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/Support/CommandLine.h"

using namespace swift;
using namespace modulesummary;
using namespace llvm;

static cl::opt<bool> ModuleSummaryEmbedDebugName(
    "module-summary-embed-debug-name", cl::init(false),
    cl::desc("Embed function names for debugging purpose"));

namespace {

class Serializer {
  SmallVector<char, 0> Buffer;
  BitstreamWriter Out{Buffer};

  /// A reusable buffer for emitting records.
  SmallVector<uint64_t, 64> ScratchRecord;

  std::array<unsigned, 256> AbbrCodes;

  template <typename Layout> void registerRecordAbbr() {
    using AbbrArrayTy = decltype(AbbrCodes);
    static_assert(Layout::Code <= std::tuple_size<AbbrArrayTy>::value,
                  "layout has invalid record code");
    AbbrCodes[Layout::Code] = Layout::emitAbbrev(Out);
  }

  void writeSignature();
  void writeBlockInfoBlock();
  void emitBlockID(unsigned ID, StringRef name,
                   SmallVectorImpl<unsigned char> &nameBuffer);

  void emitRecordID(unsigned ID, StringRef name,
                    SmallVectorImpl<unsigned char> &nameBuffer);
  void emitVFuncTable(const VFuncToImplsMapTy T, VFuncSlot::KindTy kind);
  void emitUsedTypeList(const ArrayRef<GUID> L);

public:
  void emitHeader();
  void emitModuleSummary(const ModuleSummaryIndex &index);
  void emitFunctionSummary(const FunctionSummary *summary);
  void write(raw_ostream &os);
};

void Serializer::emitBlockID(unsigned ID, StringRef name,
                             SmallVectorImpl<unsigned char> &nameBuffer) {
  SmallVector<unsigned, 1> idBuffer;
  idBuffer.push_back(ID);
  Out.EmitRecord(bitc::BLOCKINFO_CODE_SETBID, idBuffer);

  // Emit the block name if present.
  if (name.empty())
    return;
  nameBuffer.resize(name.size());
  memcpy(nameBuffer.data(), name.data(), name.size());
  Out.EmitRecord(bitc::BLOCKINFO_CODE_BLOCKNAME, nameBuffer);
}

void Serializer::emitRecordID(unsigned ID, StringRef name,
                              SmallVectorImpl<unsigned char> &nameBuffer) {
  assert(ID < 256 && "can't fit record ID in next to name");
  nameBuffer.resize(name.size() + 1);
  nameBuffer[0] = ID;
  memcpy(nameBuffer.data() + 1, name.data(), name.size());
  Out.EmitRecord(bitc::BLOCKINFO_CODE_SETRECORDNAME, nameBuffer);
}

void Serializer::writeSignature() {
  for (auto c : MODULE_SUMMARY_SIGNATURE)
    Out.Emit((unsigned)c, 8);
}

void Serializer::writeBlockInfoBlock() {
  BCBlockRAII restoreBlock(Out, bitc::BLOCKINFO_BLOCK_ID, 2);

  SmallVector<unsigned char, 64> nameBuffer;
#define BLOCK(X) emitBlockID(X##_ID, #X, nameBuffer)
#define BLOCK_RECORD(K, X) emitRecordID(K::X, #X, nameBuffer)

  BLOCK(RECORD_BLOCK);
  BLOCK_RECORD(record_block, MODULE_METADATA);

  BLOCK_RECORD(record_block, FUNC_METADATA);
  BLOCK_RECORD(record_block, CALL_GRAPH_EDGE);
  BLOCK_RECORD(record_block, TYPE_REF);

  BLOCK_RECORD(record_block, VFUNC_METADATA);
  BLOCK_RECORD(record_block, VFUNC_IMPL);

  BLOCK_RECORD(record_block, USED_TYPE);
}

void Serializer::emitHeader() {
  writeSignature();
  writeBlockInfoBlock();
}

void Serializer::emitFunctionSummary(const FunctionSummary *summary) {
  using namespace record_block;
  std::string debugFuncName =
      ModuleSummaryEmbedDebugName ? summary->getName() : "";
  FunctionMetadataLayout::emitRecord(
      Out, ScratchRecord, AbbrCodes[FunctionMetadataLayout::Code],
      summary->getGUID(), unsigned(summary->isLive()),
      unsigned(summary->isPreserved()), summary->getInstSize(), debugFuncName);

  for (auto call : summary->calls()) {
    std::string debugName = ModuleSummaryEmbedDebugName ? call.getName() : "";
    CallGraphEdgeLayout::emitRecord(
        Out, ScratchRecord, AbbrCodes[CallGraphEdgeLayout::Code],
        unsigned(call.getKind()), call.getCallee(), debugName);
  }

  for (auto typeRef : summary->typeRefs()) {
    std::string debugName = ModuleSummaryEmbedDebugName ? typeRef.Name : "";
    TypeRefLayout::emitRecord(Out, ScratchRecord,
                              AbbrCodes[TypeRefLayout::Code], typeRef.Guid,
                              debugName);
  }
}

void Serializer::emitVFuncTable(const VFuncToImplsMapTy T,
                                VFuncSlot::KindTy kind) {
  for (auto &pair : T) {
    GUID guid = pair.first;
    std::vector<VFuncImpl> impls = pair.second;
    using namespace record_block;

    VFuncMetadataLayout::emitRecord(Out, ScratchRecord,
                                    AbbrCodes[VFuncMetadataLayout::Code],
                                    unsigned(kind), guid);

    for (auto impl : impls) {
      VFuncImplLayout::emitRecord(Out, ScratchRecord,
                                  AbbrCodes[VFuncImplLayout::Code], impl.Guid, impl.TypeGuid);
    }
  }
}

void Serializer::emitUsedTypeList(const ArrayRef<GUID> L) {
  using namespace record_block;
  for (GUID usedType : L) {
    UsedTypeLayout::emitRecord(Out, ScratchRecord,
                               AbbrCodes[UsedTypeLayout::Code], usedType);
  }
}

void Serializer::emitModuleSummary(const ModuleSummaryIndex &index) {
  using namespace record_block;

  BCBlockRAII restoreBlock(Out, RECORD_BLOCK_ID, 8);

  registerRecordAbbr<ModuleMetadataLayout>();
  registerRecordAbbr<FunctionMetadataLayout>();
  registerRecordAbbr<CallGraphEdgeLayout>();
  registerRecordAbbr<TypeRefLayout>();
  registerRecordAbbr<VFuncMetadataLayout>();
  registerRecordAbbr<VFuncImplLayout>();
  registerRecordAbbr<UsedTypeLayout>();

  ModuleMetadataLayout::emitRecord(Out, ScratchRecord,
                                   AbbrCodes[ModuleMetadataLayout::Code],
                                   index.getName());
  for (auto FI = index.functions_begin(), FE = index.functions_end(); FI != FE;
       ++FI) {
    emitFunctionSummary(FI->second.get());
  }

  emitVFuncTable(index.getWitnessTableMethodMap(), VFuncSlot::Witness);
  emitVFuncTable(index.getVTableMethodMap(), VFuncSlot::VTable);

  emitUsedTypeList(index.getUsedTypeList());
}

void Serializer::write(raw_ostream &os) {
  os.write(Buffer.data(), Buffer.size());
  os.flush();
}

class Deserializer {
  BitstreamCursor Cursor;
  SmallVector<uint64_t, 64> Scratch;
  StringRef BlobData;

  ModuleSummaryIndex &moduleSummary;

  // These all return true if there was an error.
  bool readSignature();
  bool enterTopLevelBlock();
  bool readModuleMetadata();

public:
  Deserializer(MemoryBufferRef inputBuffer, ModuleSummaryIndex &moduleSummary)
      : Cursor{inputBuffer}, moduleSummary(moduleSummary) {}
  bool readModuleSummary();
};

bool Deserializer::readSignature() {
  for (unsigned char byte : MODULE_SUMMARY_SIGNATURE) {
    if (Cursor.AtEndOfStream())
      return true;
    if (auto maybeRead = Cursor.Read(8)) {
      if (maybeRead.get() != byte)
        return true;
    } else {
      return true;
    }
  }
  return false;
}

bool Deserializer::enterTopLevelBlock() {
  // Read the BLOCKINFO_BLOCK, which contains metadata used when dumping
  // the binary data with llvm-bcanalyzer.
  {
    auto next = Cursor.advance();
    if (!next) {
      consumeError(next.takeError());
      return true;
    }

    if (next->Kind != llvm::BitstreamEntry::SubBlock)
      return true;

    if (next->ID != llvm::bitc::BLOCKINFO_BLOCK_ID)
      return true;

    if (!Cursor.ReadBlockInfoBlock())
      return true;
  }

  // Enters our subblock, which contains the actual summary information.
  {
    auto next = Cursor.advance();
    if (!next) {
      consumeError(next.takeError());
      return true;
    }

    if (next->Kind != llvm::BitstreamEntry::SubBlock)
      return true;

    if (next->ID != RECORD_BLOCK_ID)
      return true;

    if (auto err = Cursor.EnterSubBlock(RECORD_BLOCK_ID)) {
      consumeError(std::move(err));
      return true;
    }
  }
  return false;
}

bool Deserializer::readModuleMetadata() {
  Expected<BitstreamEntry> maybeEntry = Cursor.advance();
  if (!maybeEntry)
    report_fatal_error("Should have next entry");

  BitstreamEntry entry = maybeEntry.get();

  if (entry.Kind != BitstreamEntry::Record) {
    return true;
  }
  Scratch.clear();
  auto maybeKind = Cursor.readRecord(entry.ID, Scratch, &BlobData);

  if (!maybeKind) {
    consumeError(maybeKind.takeError());
    return true;
  }

  if (maybeKind.get() != record_block::MODULE_METADATA) {
    return true;
  }

  moduleSummary.setName(BlobData.str());

  return false;
}

static Optional<FunctionSummary::Call::KindTy> getCallKind(unsigned kind) {
  if (kind < unsigned(FunctionSummary::Call::KindTy::kindCount))
    return FunctionSummary::Call::KindTy(kind);
  return None;
}

static Optional<VFuncSlot::KindTy> getSlotKind(unsigned kind) {
  if (kind < unsigned(FunctionSummary::Call::KindTy::kindCount))
    return VFuncSlot::KindTy(kind);
  return None;
}

bool Deserializer::readModuleSummary() {
  using namespace record_block;
  if (readSignature()) {
    return true;
  }
  if (enterTopLevelBlock()) {
    return true;
  }

  if (readModuleMetadata()) {
    return true;
  }

  FunctionSummary *CurrentFunc;
  Optional<VFuncSlot> CurrentSlot;

  while (!Cursor.AtEndOfStream()) {
    Scratch.clear();
    Expected<BitstreamEntry> entry = Cursor.advance();

    if (!entry) {
      // Success if there is no content
      consumeError(entry.takeError());
      return false;
    }

    if (entry->Kind == llvm::BitstreamEntry::EndBlock) {
      Cursor.ReadBlockEnd();
      break;
    }

    if (entry->Kind != llvm::BitstreamEntry::Record) {
      llvm::report_fatal_error("Bad bitstream entry kind");
    }

    auto recordID = Cursor.readRecord(entry->ID, Scratch, &BlobData);
    if (!recordID) {
      consumeError(recordID.takeError());
      return true;
    }

    switch (recordID.get()) {
    case MODULE_METADATA:
      // METADATA must appear at the beginning and is handled by
      // readModuleSummaryMetadata().
      llvm::report_fatal_error("Unexpected MODULE_METADATA record");
    case FUNC_METADATA: {
      GUID guid;
      std::string name;
      unsigned isLive, isPreserved;
      bool shouldMerge = false;
      uint32_t instSize;

      FunctionMetadataLayout::readRecord(Scratch, guid, isLive, isPreserved,
                                         instSize);
      name = BlobData.str();
      if (auto summary = moduleSummary.getFunctionSummary(guid)) {
        CurrentFunc = summary;
        shouldMerge = true;
      } else {
        auto NewFS = std::make_unique<FunctionSummary>(guid);
        CurrentFunc = NewFS.get();
        moduleSummary.addFunctionSummary(std::move(NewFS));
      }
      // Overwrite iff flags are true for merging function summaries of same functions.
      if (!shouldMerge || isLive) {
        CurrentFunc->setLive(isLive);
      }
      if (!shouldMerge || isPreserved) {
        CurrentFunc->setPreserved(isPreserved);
      }
      if (!shouldMerge || !name.empty()) {
        CurrentFunc->setName(name);
      }
      CurrentFunc->setInstSize(instSize);
      break;
    }
    case CALL_GRAPH_EDGE: {
      // CALL_GRAPH_EDGE must follow a FUNC_METADATA.
      if (!CurrentFunc) {
        report_fatal_error("Unexpected CALL_GRAPH_EDGE record");
      }
      unsigned callKindID;
      GUID calleeGUID;
      CallGraphEdgeLayout::readRecord(Scratch, callKindID, calleeGUID);

      auto callKind = getCallKind(callKindID);
      if (!callKind) {
        report_fatal_error("Bad call kind");
      }
      FunctionSummary::Call call(calleeGUID, BlobData.str(), callKind.getValue());
      CurrentFunc->addCall(call);
      break;
    }
    case TYPE_REF: {
      // TYPE_REF must follow a FUNC_METADATA.
      if (!CurrentFunc) {
        report_fatal_error("Unexpected TYPE_REF record");
      }
      GUID typeGUID;
      std::string name;
      TypeRefLayout::readRecord(Scratch, typeGUID);
      name = BlobData.str();
      CurrentFunc->addTypeRef({typeGUID, name});
      break;
    }
    case VFUNC_METADATA: {
      unsigned rawVFuncKind;
      GUID vFuncGUID;
      VFuncMetadataLayout::readRecord(Scratch, rawVFuncKind, vFuncGUID);

      auto Kind = getSlotKind(rawVFuncKind);
      if (!Kind) {
        report_fatal_error("Bad vfunc slot kind");
      }
      CurrentSlot = VFuncSlot(Kind.getValue(), vFuncGUID);
      break;
    }
    case VFUNC_IMPL: {
      // VFUNC_IMPL must follow a VFUNC_METADATA.
      if (!CurrentSlot) {
        report_fatal_error("Unexpected METHOD_IMPL record");
      }
      GUID implGUID, typeGUID;
      VFuncImplLayout::readRecord(Scratch, implGUID, typeGUID);
      moduleSummary.addImplementation(CurrentSlot.getValue(), implGUID, typeGUID);
      break;
    }
    case USED_TYPE: {
      GUID typeGUID;
      UsedTypeLayout::readRecord(Scratch, typeGUID);
      moduleSummary.markUsedType(typeGUID);
    }
    }
  }

  return false;
}

}; // namespace

bool modulesummary::writeModuleSummaryIndex(const ModuleSummaryIndex &index,
                                            DiagnosticEngine &diags,
                                            StringRef path) {
  return withOutputFile(diags, path, [&](raw_ostream &out) {
    Serializer serializer;
    serializer.emitHeader();
    serializer.emitModuleSummary(index);
    serializer.write(out);
    return false;
  });
}
bool modulesummary::loadModuleSummaryIndex(MemoryBufferRef inputBuffer,
                                           ModuleSummaryIndex &moduleSummary) {
  Deserializer deserializer(inputBuffer, moduleSummary);
  return deserializer.readModuleSummary();
}
