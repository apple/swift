//===---- FineGrainedDependencyFormat.cpp - reading and writing swiftdeps -===//
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

#include "swift/AST/FileSystem.h"
#include "swift/AST/FineGrainedDependencies.h"
#include "swift/AST/FineGrainedDependencyFormat.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/PrettyStackTrace.h"
#include "swift/Basic/Version.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Bitstream/BitstreamReader.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualOutputBackend.h"

using namespace swift;
using namespace fine_grained_dependencies;

namespace {

class Deserializer {
  std::vector<std::string> Identifiers;

  llvm::BitstreamCursor Cursor;

  SmallVector<uint64_t, 64> Scratch;
  StringRef BlobData;

  // These all return true if there was an error.
  bool readSignature();
  bool enterTopLevelBlock();
  bool readMetadata();

  std::optional<std::string> getIdentifier(unsigned n);

public:
  Deserializer(llvm::MemoryBufferRef Data) : Cursor(Data) {}
  bool readFineGrainedDependencyGraph(SourceFileDepGraph &g, Purpose purpose);
  bool readFineGrainedDependencyGraphFromSwiftModule(SourceFileDepGraph &g);
};

} // end namespace

bool Deserializer::readSignature() {
  for (unsigned char byte : FINE_GRAINED_DEPENDENCY_FORMAT_SIGNATURE) {
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

  // Enters our subblock, which contains the actual dependency information.
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

bool Deserializer::readMetadata() {
  using namespace record_block;

  auto entry = Cursor.advance();
  if (!entry) {
    consumeError(entry.takeError());
    return true;
  }

  if (entry->Kind != llvm::BitstreamEntry::Record)
    return true;

  auto recordID = Cursor.readRecord(entry->ID, Scratch, &BlobData);
  if (!recordID) {
    consumeError(recordID.takeError());
    return true;
  }

  if (*recordID != METADATA)
    return true;

  unsigned majorVersion, minorVersion;

  MetadataLayout::readRecord(Scratch, majorVersion, minorVersion);
  if (majorVersion != FINE_GRAINED_DEPENDENCY_FORMAT_VERSION_MAJOR ||
      minorVersion != FINE_GRAINED_DEPENDENCY_FORMAT_VERSION_MINOR) {
    return true;
  }

  return false;
}

static std::optional<NodeKind> getNodeKind(unsigned nodeKind) {
  if (nodeKind < unsigned(NodeKind::kindCount))
    return NodeKind(nodeKind);
  return std::nullopt;
}

static std::optional<DeclAspect> getDeclAspect(unsigned declAspect) {
  if (declAspect < unsigned(DeclAspect::aspectCount))
    return DeclAspect(declAspect);
  return std::nullopt;
}

bool Deserializer::readFineGrainedDependencyGraph(SourceFileDepGraph &g,
                                                  Purpose purpose) {
  using namespace record_block;

  switch (purpose) {
  case Purpose::ForSwiftDeps:
    if (readSignature())
      return true;

    if (enterTopLevelBlock())
      return true;
    LLVM_FALLTHROUGH;
  case Purpose::ForSwiftModule:
    // N.B. Incremental metadata embedded in swiftmodule files does not have
    // a leading signature, and its top-level block has already been
    // consumed by the time we get here.
    break;
  }

  if (readMetadata())
    return true;

  SourceFileDepGraphNode *node = nullptr;
  size_t sequenceNumber = 0;

  while (!Cursor.AtEndOfStream()) {
    auto entry = cantFail(Cursor.advance(), "Advance bitstream cursor");

    if (entry.Kind == llvm::BitstreamEntry::EndBlock) {
      Cursor.ReadBlockEnd();
      assert(Cursor.GetCurrentBitNo() % CHAR_BIT == 0);
      break;
    }

    if (entry.Kind != llvm::BitstreamEntry::Record)
      llvm::report_fatal_error("Bad bitstream entry kind");

    Scratch.clear();
    unsigned recordID = cantFail(
      Cursor.readRecord(entry.ID, Scratch, &BlobData),
      "Read bitstream record");

    switch (recordID) {
    case METADATA: {
      // METADATA must appear at the beginning and is handled by readMetadata().
      llvm::report_fatal_error("Unexpected METADATA record");
      break;
    }

    case SOURCE_FILE_DEP_GRAPH_NODE: {
      unsigned nodeKindID, declAspectID, contextID, nameID, isProvides;
      SourceFileDepGraphNodeLayout::readRecord(Scratch, nodeKindID, declAspectID,
                                               contextID, nameID, isProvides);
      node = new SourceFileDepGraphNode();
      node->setSequenceNumber(sequenceNumber++);
      g.addNode(node);

      auto nodeKind = getNodeKind(nodeKindID);
      if (!nodeKind)
        llvm::report_fatal_error("Bad node kind");
      auto declAspect = getDeclAspect(declAspectID);
      if (!declAspect)
        llvm::report_fatal_error("Bad decl aspect");
      auto context = getIdentifier(contextID);
      if (!context)
        llvm::report_fatal_error("Bad context");
      auto name = getIdentifier(nameID);
      if (!name)
        llvm::report_fatal_error("Bad identifier");

      node->setKey(DependencyKey(*nodeKind, *declAspect, *context, *name));
      if (isProvides)
        node->setIsProvides();
      break;
    }

    case FINGERPRINT_NODE: {
      // FINGERPRINT_NODE must follow a SOURCE_FILE_DEP_GRAPH_NODE.
      if (node == nullptr)
        llvm::report_fatal_error("Unexpected FINGERPRINT_NODE record");
      if (auto fingerprint = Fingerprint::fromString(BlobData))
        node->setFingerprint(fingerprint.value());
      else
        llvm::report_fatal_error(Twine("Unconvertable FINGERPRINT_NODE record: '") + BlobData + "'" );
      break;
    }

    case DEPENDS_ON_DEFINITION_NODE: {
      // DEPENDS_ON_DEFINITION_NODE must follow a SOURCE_FILE_DEP_GRAPH_NODE.
      if (node == nullptr)
        llvm::report_fatal_error("Unexpected DEPENDS_ON_DEFINITION_NODE record");

      unsigned dependsOnDefID;
      DependsOnDefNodeLayout::readRecord(Scratch, dependsOnDefID);

      node->addDefIDependUpon(dependsOnDefID);
      break;
    }

    case IDENTIFIER_NODE: {
      // IDENTIFIER_NODE must come before SOURCE_FILE_DEP_GRAPH_NODE.
      if (node != nullptr)
        llvm::report_fatal_error("Unexpected IDENTIFIER_NODE record");

      IdentifierNodeLayout::readRecord(Scratch);
      Identifiers.push_back(BlobData.str());
      break;
    }

    default: {
      llvm::report_fatal_error("Unknown record ID");
    }
    }
  }

  return false;
}

bool swift::fine_grained_dependencies::readFineGrainedDependencyGraph(
    llvm::MemoryBuffer &buffer, SourceFileDepGraph &g) {
  Deserializer deserializer(buffer.getMemBufferRef());
  return deserializer.readFineGrainedDependencyGraph(g, Purpose::ForSwiftDeps);
}

bool swift::fine_grained_dependencies::readFineGrainedDependencyGraph(
     StringRef path, SourceFileDepGraph &g) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer)
    return false;

  return readFineGrainedDependencyGraph(*buffer.get(), g);
}

std::optional<std::string> Deserializer::getIdentifier(unsigned n) {
  if (n == 0)
    return std::string();

  --n;
  if (n >= Identifiers.size())
    return std::nullopt;

  return Identifiers[n];
}

namespace {

class Serializer {
  llvm::StringMap<unsigned, llvm::BumpPtrAllocator> IdentifierIDs;
  unsigned LastIdentifierID = 0;
  std::vector<StringRef> IdentifiersToWrite;

  llvm::BitstreamWriter &Out;

  /// A reusable buffer for emitting records.
  SmallVector<uint64_t, 64> ScratchRecord;

  std::array<unsigned, 256> AbbrCodes;

  void addIdentifier(StringRef str);
  unsigned getIdentifier(StringRef str);

  template <typename Layout>
  void registerRecordAbbr() {
    using AbbrArrayTy = decltype(AbbrCodes);
    static_assert(Layout::Code <= std::tuple_size<AbbrArrayTy>::value,
                  "layout has invalid record code");
    AbbrCodes[Layout::Code] = Layout::emitAbbrev(Out);
  }

  void emitBlockID(unsigned ID, StringRef name,
                   SmallVectorImpl<unsigned char> &nameBuffer);

  void emitRecordID(unsigned ID, StringRef name,
                    SmallVectorImpl<unsigned char> &nameBuffer);

  void writeSignature();
  void writeBlockInfoBlock();
  void writeMetadata();

public:
  Serializer(llvm::BitstreamWriter &ExistingOut) : Out(ExistingOut) {}

public:
  void writeFineGrainedDependencyGraph(const SourceFileDepGraph &g,
                                       Purpose purpose);
};

} // end namespace

/// Record the name of a block.
void Serializer::emitBlockID(unsigned ID, StringRef name,
                             SmallVectorImpl<unsigned char> &nameBuffer) {
  SmallVector<unsigned, 1> idBuffer;
  idBuffer.push_back(ID);
  Out.EmitRecord(llvm::bitc::BLOCKINFO_CODE_SETBID, idBuffer);

  // Emit the block name if present.
  if (name.empty())
    return;
  nameBuffer.resize(name.size());
  memcpy(nameBuffer.data(), name.data(), name.size());
  Out.EmitRecord(llvm::bitc::BLOCKINFO_CODE_BLOCKNAME, nameBuffer);
}

void Serializer::emitRecordID(unsigned ID, StringRef name,
                              SmallVectorImpl<unsigned char> &nameBuffer) {
  assert(ID < 256 && "can't fit record ID in next to name");
  nameBuffer.resize(name.size()+1);
  nameBuffer[0] = ID;
  memcpy(nameBuffer.data()+1, name.data(), name.size());
  Out.EmitRecord(llvm::bitc::BLOCKINFO_CODE_SETRECORDNAME, nameBuffer);
}

void Serializer::writeSignature() {
  for (auto c : FINE_GRAINED_DEPENDENCY_FORMAT_SIGNATURE)
    Out.Emit((unsigned) c, 8);
}

void Serializer::writeBlockInfoBlock() {
  llvm::BCBlockRAII restoreBlock(Out, llvm::bitc::BLOCKINFO_BLOCK_ID, 2);

  SmallVector<unsigned char, 64> nameBuffer;
#define BLOCK(X) emitBlockID(X ## _ID, #X, nameBuffer)
#define BLOCK_RECORD(K, X) emitRecordID(K::X, #X, nameBuffer)

  BLOCK(RECORD_BLOCK);
  BLOCK_RECORD(record_block, METADATA);
  BLOCK_RECORD(record_block, SOURCE_FILE_DEP_GRAPH_NODE);
  BLOCK_RECORD(record_block, FINGERPRINT_NODE);
  BLOCK_RECORD(record_block, DEPENDS_ON_DEFINITION_NODE);
  BLOCK_RECORD(record_block, IDENTIFIER_NODE);
}

void Serializer::writeMetadata() {
  using namespace record_block;

  MetadataLayout::emitRecord(Out, ScratchRecord,
                             AbbrCodes[MetadataLayout::Code],
                             FINE_GRAINED_DEPENDENCY_FORMAT_VERSION_MAJOR,
                             FINE_GRAINED_DEPENDENCY_FORMAT_VERSION_MINOR,
                             version::getSwiftFullVersion());
}

void
Serializer::writeFineGrainedDependencyGraph(const SourceFileDepGraph &g,
                                            Purpose purpose) {
  unsigned blockID = 0;
  switch (purpose) {
  case Purpose::ForSwiftDeps:
    writeSignature();
    writeBlockInfoBlock();
    blockID = RECORD_BLOCK_ID;
    break;
  case Purpose::ForSwiftModule:
    blockID = INCREMENTAL_INFORMATION_BLOCK_ID;
    break;
  }

  llvm::BCBlockRAII restoreBlock(Out, blockID, 8);

  using namespace record_block;

  registerRecordAbbr<MetadataLayout>();
  registerRecordAbbr<SourceFileDepGraphNodeLayout>();
  registerRecordAbbr<FingerprintNodeLayout>();
  registerRecordAbbr<DependsOnDefNodeLayout>();
  registerRecordAbbr<IdentifierNodeLayout>();

  writeMetadata();

  // Make a pass to collect all unique strings
  g.forEachNode([&](SourceFileDepGraphNode *node) {
    addIdentifier(node->getKey().getContext());
    addIdentifier(node->getKey().getName());
  });

  // Write the strings
  for (auto str : IdentifiersToWrite) {
    IdentifierNodeLayout::emitRecord(Out, ScratchRecord,
                                     AbbrCodes[IdentifierNodeLayout::Code],
                                     str);
  }

  size_t sequenceNumber = 0;

  // Now write each graph node
  g.forEachNode([&](SourceFileDepGraphNode *node) {
    SourceFileDepGraphNodeLayout::emitRecord(Out, ScratchRecord,
                                             AbbrCodes[SourceFileDepGraphNodeLayout::Code],
                                             unsigned(node->getKey().getKind()),
                                             unsigned(node->getKey().getAspect()),
                                             getIdentifier(node->getKey().getContext()),
                                             getIdentifier(node->getKey().getName()),
                                             node->getIsProvides() ? 1 : 0);
    assert(sequenceNumber == node->getSequenceNumber());
    sequenceNumber++;
    (void) sequenceNumber;

    if (auto fingerprint = node->getFingerprint()) {
      FingerprintNodeLayout::emitRecord(Out, ScratchRecord,
                                        AbbrCodes[FingerprintNodeLayout::Code],
                                        fingerprint->getRawValue());
    }

    node->forEachDefIDependUpon([&](size_t defIDependOn) {
      DependsOnDefNodeLayout::emitRecord(Out, ScratchRecord,
                                         AbbrCodes[DependsOnDefNodeLayout::Code],
                                         defIDependOn);
    });
  });
}

void Serializer::addIdentifier(StringRef str) {
  if (str.empty())
    return;

  decltype(IdentifierIDs)::iterator iter;
  bool isNew;
  std::tie(iter, isNew) =
      IdentifierIDs.insert({str, LastIdentifierID + 1});

  if (!isNew)
    return;

  ++LastIdentifierID;
  // Note that we use the string data stored in the StringMap.
  IdentifiersToWrite.push_back(iter->getKey());
}

unsigned Serializer::getIdentifier(StringRef str) {
  if (str.empty())
    return 0;

  auto iter = IdentifierIDs.find(str);
  assert(iter != IdentifierIDs.end());
  assert(iter->second != 0);
  return iter->second;
}

void swift::fine_grained_dependencies::writeFineGrainedDependencyGraph(
    llvm::BitstreamWriter &Out, const SourceFileDepGraph &g,
    Purpose purpose) {
  Serializer serializer{Out};
  serializer.writeFineGrainedDependencyGraph(g, purpose);
}

bool swift::fine_grained_dependencies::writeFineGrainedDependencyGraphToPath(
    DiagnosticEngine &diags, llvm::vfs::OutputBackend &backend, StringRef path,
    const SourceFileDepGraph &g) {
  PrettyStackTraceStringAction stackTrace("saving fine-grained dependency graph", path);
  return withOutputPath(diags, backend, path, [&](llvm::raw_ostream &out) {
    SmallVector<char, 0> Buffer;
    llvm::BitstreamWriter Writer{Buffer};
    writeFineGrainedDependencyGraph(Writer, g, Purpose::ForSwiftDeps);
    out.write(Buffer.data(), Buffer.size());
    out.flush();
    return false;
  });
}

static bool checkModuleSignature(llvm::BitstreamCursor &cursor,
                                 ArrayRef<unsigned char> signature) {
  for (unsigned char byte : signature) {
    if (cursor.AtEndOfStream())
      return false;
    if (llvm::Expected<llvm::SimpleBitstreamCursor::word_t> maybeRead =
            cursor.Read(8)) {
      if (maybeRead.get() != byte)
        return false;
    } else {
      consumeError(maybeRead.takeError());
      return false;
    }
  }
  return true;
}

static bool enterTopLevelModuleBlock(llvm::BitstreamCursor &cursor, unsigned ID,
                                     bool shouldReadBlockInfo = true) {
  llvm::Expected<llvm::BitstreamEntry> maybeNext = cursor.advance();
  if (!maybeNext) {
    consumeError(maybeNext.takeError());
    return false;
  }
  llvm::BitstreamEntry next = maybeNext.get();

  if (next.Kind != llvm::BitstreamEntry::SubBlock)
    return false;

  if (next.ID == llvm::bitc::BLOCKINFO_BLOCK_ID) {
    if (shouldReadBlockInfo) {
      if (!cursor.ReadBlockInfoBlock())
        return false;
    } else {
      if (cursor.SkipBlock())
        return false;
    }
    return enterTopLevelModuleBlock(cursor, ID, false);
  }

  if (next.ID != ID)
    return false;

  if (llvm::Error Err = cursor.EnterSubBlock(ID)) {
    consumeError(std::move(Err));
    return false;
  }

  return true;
}

bool swift::fine_grained_dependencies::
    readFineGrainedDependencyGraphFromSwiftModule(llvm::MemoryBuffer &buffer,
                                                  SourceFileDepGraph &g) {
  Deserializer deserializer(buffer.getMemBufferRef());
  return deserializer.readFineGrainedDependencyGraphFromSwiftModule(g);
}

bool Deserializer::readFineGrainedDependencyGraphFromSwiftModule(
    SourceFileDepGraph &g) {
  if (!checkModuleSignature(Cursor, {0xE2, 0x9C, 0xA8, 0x0E}) ||
      !enterTopLevelModuleBlock(Cursor, llvm::bitc::FIRST_APPLICATION_BLOCKID, false)) {
    return true;
  }

  llvm::BitstreamEntry topLevelEntry;
  bool DidNotReadFineGrainedDependencies = true;
  while (!Cursor.AtEndOfStream()) {
    llvm::Expected<llvm::BitstreamEntry> maybeEntry =
        Cursor.advance(llvm::BitstreamCursor::AF_DontPopBlockAtEnd);
    if (!maybeEntry) {
      consumeError(maybeEntry.takeError());
      return true;
    }
    topLevelEntry = maybeEntry.get();
    if (topLevelEntry.Kind != llvm::BitstreamEntry::SubBlock)
      break;

    switch (topLevelEntry.ID) {
    case INCREMENTAL_INFORMATION_BLOCK_ID: {
      if (llvm::Error Err =
              Cursor.EnterSubBlock(INCREMENTAL_INFORMATION_BLOCK_ID)) {
        consumeError(std::move(Err));
        return true;
      }
      if (readFineGrainedDependencyGraph(g, Purpose::ForSwiftModule)) {
        break;
      }

      DidNotReadFineGrainedDependencies = false;
      break;
    }

    default:
      // Unknown top-level block, possibly for use by a future version of the
      // module format.
      if (Cursor.SkipBlock()) {
        return true;
      }
      break;
    }
  }

  return DidNotReadFineGrainedDependencies;
}
