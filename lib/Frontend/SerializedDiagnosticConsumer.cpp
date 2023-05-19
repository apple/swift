//===--- SerializedDiagnosticConsumer.cpp - Serialize Diagnostics ---------===//
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
//
//  This file implements the SerializedDiagnosticConsumer class.
//
//===----------------------------------------------------------------------===//

#include "clang/Frontend/SerializedDiagnostics.h"

#include "swift/Frontend/SerializedDiagnosticConsumer.h"
#include "swift/AST/DiagnosticConsumer.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Parse/Lexer.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Bitstream/BitstreamWriter.h"

using namespace swift;
using namespace clang::serialized_diags;

namespace {
class AbbreviationMap {
  llvm::DenseMap<unsigned, unsigned> Abbrevs;
public:
  AbbreviationMap() {}

  void set(unsigned recordID, unsigned abbrevID) {
    assert(Abbrevs.find(recordID) == Abbrevs.end()
           && "Abbreviation already set.");
    Abbrevs[recordID] = abbrevID;
  }

  unsigned get(unsigned recordID) {
    assert(Abbrevs.find(recordID) != Abbrevs.end() &&
           "Abbreviation not set.");
    return Abbrevs[recordID];
  }
};

using RecordData = SmallVector<uint64_t, 64>;
using RecordDataImpl = SmallVectorImpl<uint64_t>;

struct SharedState : llvm::RefCountedBase<SharedState> {
  SharedState(StringRef serializedDiagnosticsPath)
      : Stream(Buffer),
        SerializedDiagnosticsPath(serializedDiagnosticsPath),
        EmittedAnyDiagBlocks(false) {}

  /// The byte buffer for the serialized content.
  llvm::SmallString<1024> Buffer;

  /// The BitStreamWriter for the serialized diagnostics.
  llvm::BitstreamWriter Stream;

  /// The path of the diagnostics file.
  std::string SerializedDiagnosticsPath;

  /// The set of constructed record abbreviations.
  AbbreviationMap Abbrevs;

  /// A utility buffer for constructing record content.
  RecordData Record;

  /// A text buffer for rendering diagnostic text.
  llvm::SmallString<256> diagBuf;

  /// The collection of files used.
  llvm::DenseMap<const char *, unsigned> Files;

  /// The collection of categories used.
  llvm::DenseMap<const char *, unsigned> Categories;

  using DiagFlagsTy =
      llvm::DenseMap<const void *, std::pair<unsigned, StringRef>>;

  /// Map for uniquing strings.
  DiagFlagsTy DiagFlags;

  /// Whether we have already started emission of any DIAG blocks. Once
  /// this becomes \c true, we never close a DIAG block until we know that we're
  /// starting another one or we're done.
  bool EmittedAnyDiagBlocks;
};

/// Diagnostic consumer that serializes diagnostics to a stream.
class SerializedDiagnosticConsumer : public DiagnosticConsumer {
  /// State shared among the various clones of this diagnostic consumer.
  llvm::IntrusiveRefCntPtr<SharedState> State;
  bool EmitMacroExpansionFiles = false;
  bool CalledFinishProcessing = false;
  bool CompilationWasComplete = true;

public:
  SerializedDiagnosticConsumer(StringRef serializedDiagnosticsPath,
                               bool emitMacroExpansionFiles)
      : State(new SharedState(serializedDiagnosticsPath)),
        EmitMacroExpansionFiles(emitMacroExpansionFiles) {
    emitPreamble();
  }

  ~SerializedDiagnosticConsumer() {
    assert(CalledFinishProcessing && "did not call finishProcessing()");
  }

  bool finishProcessing() override {
    assert(!CalledFinishProcessing &&
           "called finishProcessing() multiple times");
    CalledFinishProcessing = true;

    // NOTE: clang also does check for shared instances.  We don't
    // have these yet in Swift, but if we do we need to add an extra
    // check here.

    // Finish off any diagnostic we were in the process of emitting.
    if (State->EmittedAnyDiagBlocks)
      exitDiagBlock();

    // Write the generated bitstream to the file.
    std::error_code EC;
    std::unique_ptr<llvm::raw_fd_ostream> OS;
    OS.reset(new llvm::raw_fd_ostream(State->SerializedDiagnosticsPath, EC,
                                      llvm::sys::fs::OF_None));
    if (EC) {
      // Create a temporary diagnostics engine to print the error to stderr.
      SourceManager dummyMgr;
      DiagnosticEngine DE(dummyMgr);
      PrintingDiagnosticConsumer PDC;
      DE.addConsumer(PDC);
      DE.diagnose(SourceLoc(), diag::cannot_open_serialized_file,
                  State->SerializedDiagnosticsPath, EC.message());
      return true;
    }

    if (CompilationWasComplete) {
      OS->write((char *)&State->Buffer.front(), State->Buffer.size());
      OS->flush();
    }
    return false;
  }

  /// In batch mode, if any error occurs, no primaries can be compiled.
  /// Some primaries will have errors in their diagnostics files and so
  /// a client (such as Xcode) can see that those primaries failed.
  /// Other primaries will have no errors in their diagnostics files.
  /// In order for the driver to distinguish the two cases without parsing
  /// the diagnostics, the frontend emits a truncated diagnostics file
  /// for the latter case.
  /// The unfortunate aspect is that the truncation discards warnings, etc.
  
  void informDriverOfIncompleteBatchModeCompilation() override {
    CompilationWasComplete = false;
  }

  void handleDiagnostic(SourceManager &SM, const DiagnosticInfo &Info) override;

private:
  /// Emit bitcode for the preamble.
  void emitPreamble();

  /// Emit bitcode for the BlockInfoBlock (part of the preamble).
  void emitBlockInfoBlock();

  /// Emit bitcode for metadata block (part of preamble).
  void emitMetaBlock();

  /// Emit bitcode to enter a block for a diagnostic.
  void enterDiagBlock() {
    State->Stream.EnterSubblock(BLOCK_DIAG, 4);
  }

  /// Emit bitcode to exit a block for a diagnostic.
  void exitDiagBlock() {
    State->Stream.ExitBlock();
  }

  // Record identifier for the file.
  unsigned getEmitFile(
      SourceManager &SM, StringRef Filename, unsigned bufferID
  );

  // Record identifier for the category.
  unsigned getEmitCategory(StringRef Category);

  /// Add a source location to a record.
  void addLocToRecord(SourceLoc Loc,
                      SourceManager &SM,
                      StringRef Filename,
                      RecordDataImpl &Record);

  void addRangeToRecord(CharSourceRange Range, SourceManager &SM,
                        StringRef Filename, RecordDataImpl &Record);

  /// Emit the message payload of a diagnostic to bitcode.
  void emitDiagnosticMessage(SourceManager &SM, SourceLoc Loc,
                             DiagnosticKind Kind,
                             StringRef Text, const DiagnosticInfo &Info);
};
} // end anonymous namespace

namespace swift {
namespace serialized_diagnostics {
  std::unique_ptr<DiagnosticConsumer> createConsumer(
      StringRef outputPath, bool emitMacroExpansionFiles
  ) {
    return std::make_unique<SerializedDiagnosticConsumer>(
        outputPath, emitMacroExpansionFiles);
  }
} // namespace serialized_diagnostics
} // namespace swift

unsigned SerializedDiagnosticConsumer::getEmitFile(
    SourceManager &SM, StringRef Filename, unsigned bufferID
) {
  // NOTE: Using Filename.data() here relies on SourceMgr using
  // const char* as buffer identifiers.  This is fast, but may
  // be brittle.  We can always switch over to using a StringMap.
  // Note that the logic in EditorDiagConsumer::getBufferInfo
  // will also need changing.
  unsigned &existingEntry = State->Files[Filename.data()];
  if (existingEntry)
    return existingEntry;

  // Lazily generate the record for the file.  Note that in
  // practice we only expect there to be one file, but this is
  // general and is what the diagnostic file expects.
  unsigned entry = State->Files.size();
  existingEntry = entry;
  RecordData Record;
  Record.push_back(RECORD_FILENAME);
  Record.push_back(entry);
  Record.push_back(0); // For legacy.
  Record.push_back(0); // For legacy.
  Record.push_back(Filename.size());
  State->Stream.EmitRecordWithBlob(State->Abbrevs.get(RECORD_FILENAME),
                                   Record, Filename.data());

  // If the buffer contains code that was synthesized by the compiler,
  // emit the contents of the buffer.
  auto generatedInfo = SM.getGeneratedSourceInfo(bufferID);
  if (!generatedInfo)
    return entry;

  Record.clear();
  Record.push_back(RECORD_SOURCE_FILE_CONTENTS);
  Record.push_back(entry);

  // The source range that this buffer was generated from, expressed as
  // offsets into the original buffer.
  if (generatedInfo->originalSourceRange.isValid()) {
    auto originalFilename = SM.getDisplayNameForLoc(generatedInfo->originalSourceRange.getStart(),
                                EmitMacroExpansionFiles);
    addRangeToRecord(
        generatedInfo->originalSourceRange,
        SM, originalFilename, Record
    );
  } else {
    addLocToRecord(SourceLoc(), SM, "", Record); // Start
    addLocToRecord(SourceLoc(), SM, "", Record); // End
  }

  // Contents of the buffer.
  auto sourceText = SM.getEntireTextForBuffer(bufferID);
  Record.push_back(sourceText.size());
  State->Stream.EmitRecordWithBlob(
      State->Abbrevs.get(RECORD_SOURCE_FILE_CONTENTS),
      Record, sourceText);

  return entry;
}

unsigned SerializedDiagnosticConsumer::getEmitCategory(StringRef Category) {
  unsigned &entry = State->Categories[Category.data()];
  if (entry)
    return entry;

  // Lazily generate the record for the category.
  entry = State->Categories.size();
  RecordData Record;
  Record.push_back(RECORD_CATEGORY);
  Record.push_back(entry);
  Record.push_back(Category.size());
  State->Stream.EmitRecordWithBlob(State->Abbrevs.get(RECORD_CATEGORY), Record,
                                   Category.data());

  return entry;
}

void SerializedDiagnosticConsumer::addLocToRecord(SourceLoc Loc,
                                                  SourceManager &SM,
                                                  StringRef Filename,
                                                  RecordDataImpl &Record) {
  if (!Loc.isValid()) {
    // Emit a "sentinel" location.
    Record.push_back((unsigned)0); // File.
    Record.push_back((unsigned)0); // Line.
    Record.push_back((unsigned)0); // Column.
    Record.push_back((unsigned)0); // Offset.
    return;
  }

  auto bufferId = SM.findBufferContainingLoc(Loc);
  unsigned line, col;
  std::tie(line, col) = SM.getPresumedLineAndColumnForLoc(Loc);

  Record.push_back(getEmitFile(SM, Filename, bufferId));
  Record.push_back(line);
  Record.push_back(col);
  Record.push_back(SM.getLocOffsetInBuffer(Loc, bufferId));
}

void SerializedDiagnosticConsumer::addRangeToRecord(CharSourceRange Range,
                                                    SourceManager &SM,
                                                    StringRef Filename,
                                                    RecordDataImpl &Record) {
  assert(Range.isValid());
  addLocToRecord(Range.getStart(), SM, Filename, Record);
  addLocToRecord(Range.getEnd(), SM, Filename, Record);
}

/// Map a Swift DiagnosticKind to the diagnostic level expected
/// for serialized diagnostics.
static clang::serialized_diags::Level getDiagnosticLevel(DiagnosticKind Kind) {
  switch (Kind) {
  case DiagnosticKind::Error:
    return clang::serialized_diags::Error;
  case DiagnosticKind::Note:
    return clang::serialized_diags::Note;
  case DiagnosticKind::Warning:
    return clang::serialized_diags::Warning;
  case DiagnosticKind::Remark:
    return clang::serialized_diags::Remark;
  }

  llvm_unreachable("Unhandled DiagnosticKind in switch.");
}

void SerializedDiagnosticConsumer::emitPreamble() {
  State->Stream.Emit((unsigned)'D', 8);
  State->Stream.Emit((unsigned)'I', 8);
  State->Stream.Emit((unsigned)'A', 8);
  State->Stream.Emit((unsigned)'G', 8);
  emitBlockInfoBlock();
  emitMetaBlock();
}


void SerializedDiagnosticConsumer::emitMetaBlock() {
  llvm::BitstreamWriter &Stream = State->Stream;
  RecordData &Record = State->Record;
  AbbreviationMap &Abbrevs = State->Abbrevs;

  Stream.EnterSubblock(BLOCK_META, 3);
  Record.clear();
  Record.push_back(RECORD_VERSION);
  Record.push_back(clang::serialized_diags::VersionNumber);
  Stream.EmitRecordWithAbbrev(Abbrevs.get(RECORD_VERSION), Record);
  Stream.ExitBlock();
}


/// Emits a block ID in the BLOCKINFO block.
static void emitBlockID(unsigned ID, const char *Name,
                        llvm::BitstreamWriter &Stream,
                        RecordDataImpl &Record) {
  Record.clear();
  Record.push_back(ID);
  Stream.EmitRecord(llvm::bitc::BLOCKINFO_CODE_SETBID, Record);

  // Emit the block name if present.
  if (Name == nullptr || Name[0] == 0)
    return;

  Record.clear();

  while (*Name)
    Record.push_back(*Name++);

  Stream.EmitRecord(llvm::bitc::BLOCKINFO_CODE_BLOCKNAME, Record);
}

/// Emits a record ID in the BLOCKINFO block.
static void emitRecordID(unsigned ID, const char *Name,
                         llvm::BitstreamWriter &Stream,
                         RecordDataImpl &Record) {
  Record.clear();
  Record.push_back(ID);

  while (*Name)
    Record.push_back(*Name++);

  Stream.EmitRecord(llvm::bitc::BLOCKINFO_CODE_SETRECORDNAME, Record);
}

/// Emit bitcode for abbreviation for source locations.
static void
addSourceLocationAbbrev(std::shared_ptr<llvm::BitCodeAbbrev> Abbrev) {
  using namespace llvm;
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 5));    // File ID.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Line.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Column.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Offset;
}

/// Emit bitcode for abbreviation for source ranges.
static void
addRangeLocationAbbrev(std::shared_ptr<llvm::BitCodeAbbrev> Abbrev) {
  addSourceLocationAbbrev(Abbrev);
  addSourceLocationAbbrev(Abbrev);
}

void SerializedDiagnosticConsumer::emitBlockInfoBlock() {
  State->Stream.EnterBlockInfoBlock();

  using namespace llvm;
  llvm::BitstreamWriter &Stream = State->Stream;
  RecordData &Record = State->Record;
  AbbreviationMap &Abbrevs = State->Abbrevs;

  // ==---------------------------------------------------------------------==//
  // The subsequent records and Abbrevs are for the "Meta" block.
  // ==---------------------------------------------------------------------==//

  emitBlockID(BLOCK_META, "Meta", Stream, Record);
  emitRecordID(RECORD_VERSION, "Version", Stream, Record);
  auto Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_VERSION));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32));
  Abbrevs.set(RECORD_VERSION, Stream.EmitBlockInfoAbbrev(BLOCK_META, Abbrev));

  // ==---------------------------------------------------------------------==//
  // The subsequent records and Abbrevs are for the "Diagnostic" block.
  // ==---------------------------------------------------------------------==//

  emitBlockID(BLOCK_DIAG, "Diag", Stream, Record);
  emitRecordID(RECORD_DIAG, "DiagInfo", Stream, Record);
  emitRecordID(RECORD_SOURCE_RANGE, "SrcRange", Stream, Record);
  emitRecordID(RECORD_CATEGORY, "CatName", Stream, Record);
  emitRecordID(RECORD_DIAG_FLAG, "DiagFlag", Stream, Record);
  emitRecordID(RECORD_FILENAME, "FileName", Stream, Record);
  emitRecordID(RECORD_FIXIT, "FixIt", Stream, Record);
  emitRecordID(
      RECORD_SOURCE_FILE_CONTENTS, "SourceFileContents", Stream, Record);

  // Emit abbreviation for RECORD_DIAG.
  Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_DIAG));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 3));  // Diag level.
  addSourceLocationAbbrev(Abbrev);
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 10)); // Category.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 10)); // Mapped Diag ID.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 16)); // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob)); // Diagnostic text.
  Abbrevs.set(RECORD_DIAG, Stream.EmitBlockInfoAbbrev(BLOCK_DIAG, Abbrev));

  // Emit abbreviation for RECORD_CATEGORY.
  Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_CATEGORY));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 16)); // Category ID.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 8));  // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob));      // Category text.
  Abbrevs.set(RECORD_CATEGORY, Stream.EmitBlockInfoAbbrev(BLOCK_DIAG, Abbrev));

  // Emit abbreviation for RECORD_SOURCE_RANGE.
  Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_SOURCE_RANGE));
  addRangeLocationAbbrev(Abbrev);
  Abbrevs.set(RECORD_SOURCE_RANGE,
              Stream.EmitBlockInfoAbbrev(BLOCK_DIAG, Abbrev));

  // Emit the abbreviation for RECORD_DIAG_FLAG.
  Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_DIAG_FLAG));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 10)); // Mapped Diag ID.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 16)); // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob)); // Flag name text.
  Abbrevs.set(RECORD_DIAG_FLAG, Stream.EmitBlockInfoAbbrev(BLOCK_DIAG,
                                                           Abbrev));

  // Emit the abbreviation for RECORD_FILENAME.
  Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_FILENAME));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 5)); // Mapped file ID.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 32)); // Modification time.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 16)); // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob)); // File name text.
  Abbrevs.set(RECORD_FILENAME, Stream.EmitBlockInfoAbbrev(BLOCK_DIAG,
                                                          Abbrev));

  // Emit the abbreviation for RECORD_FIXIT.
  Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_FIXIT));
  addRangeLocationAbbrev(Abbrev);
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 16)); // Text size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob));      // FixIt text.
  Abbrevs.set(RECORD_FIXIT, Stream.EmitBlockInfoAbbrev(BLOCK_DIAG,
                                                       Abbrev));

  // Emit the abbreviation for RECORD_SOURCE_FILE_CONTENTS.
  Abbrev = std::make_shared<BitCodeAbbrev>();
  Abbrev->Add(BitCodeAbbrevOp(RECORD_SOURCE_FILE_CONTENTS));
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 5)); // File ID.
  addRangeLocationAbbrev(Abbrev);
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 16)); // File size.
  Abbrev->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob)); // File contents.
  Abbrevs.set(RECORD_SOURCE_FILE_CONTENTS,
              Stream.EmitBlockInfoAbbrev(BLOCK_DIAG, Abbrev));

  Stream.ExitBlock();
}

void SerializedDiagnosticConsumer::
emitDiagnosticMessage(SourceManager &SM,
                      SourceLoc Loc,
                      DiagnosticKind Kind,
                      StringRef Text,
                      const DiagnosticInfo &Info) {

  // Emit the diagnostic to bitcode.
  llvm::BitstreamWriter &Stream = State->Stream;
  RecordData &Record = State->Record;
  AbbreviationMap &Abbrevs = State->Abbrevs;

  StringRef filename = "";
  if (Loc.isValid())
    filename = SM.getDisplayNameForLoc(Loc, EmitMacroExpansionFiles);

  // Emit the RECORD_DIAG record.
  Record.clear();
  Record.push_back(RECORD_DIAG);
  Record.push_back(getDiagnosticLevel(Kind));
  addLocToRecord(Loc, SM, filename, Record);

  // Emit the category.
  if (!Info.Category.empty()) {
    Record.push_back(getEmitCategory(Info.Category));
  } else {
    Record.push_back(0);
  }

  // FIXME: Swift diagnostics currently have no flags.
  Record.push_back(0);

  // Emit the message.
  Record.push_back(Text.size());
  Stream.EmitRecordWithBlob(Abbrevs.get(RECORD_DIAG), Record, Text);

  // If the location is invalid, do not emit source ranges or fixits.
  if (Loc.isInvalid())
    return;

  // Emit source ranges.
  auto RangeAbbrev = State->Abbrevs.get(RECORD_SOURCE_RANGE);
  for (const auto &R : Info.Ranges) {
    if (R.isInvalid())
      continue;
    State->Record.clear();
    State->Record.push_back(RECORD_SOURCE_RANGE);
    addRangeToRecord(R, SM, filename, State->Record);
    State->Stream.EmitRecordWithAbbrev(RangeAbbrev, State->Record);
  }

  // Emit FixIts.
  auto FixItAbbrev = State->Abbrevs.get(RECORD_FIXIT);
  for (const auto &F : Info.FixIts) {
    if (F.getRange().isValid()) {
      State->Record.clear();
      State->Record.push_back(RECORD_FIXIT);
      addRangeToRecord(F.getRange(), SM, filename, State->Record);
      State->Record.push_back(F.getText().size());
      Stream.EmitRecordWithBlob(FixItAbbrev, Record, F.getText());
    }
  }
}

void SerializedDiagnosticConsumer::handleDiagnostic(
    SourceManager &SM, const DiagnosticInfo &Info) {

  // Enter the block for a non-note diagnostic immediately, rather
  // than waiting for beginDiagnostic, in case associated notes
  // are emitted before we get there.
  if (Info.Kind != DiagnosticKind::Note) {
    if (State->EmittedAnyDiagBlocks)
      exitDiagBlock();

    enterDiagBlock();
    State->EmittedAnyDiagBlocks = true;
  }

  // Special-case diagnostics with no location.
  // Make sure we bracket all notes as "sub-diagnostics".
  bool bracketDiagnostic = (Info.Kind == DiagnosticKind::Note);

  if (bracketDiagnostic)
    enterDiagBlock();

  // Actually substitute the diagnostic arguments into the diagnostic text.
  llvm::SmallString<256> Text;
  {
    llvm::raw_svector_ostream Out(Text);
    DiagnosticEngine::formatDiagnosticText(Out, Info.FormatString,
                                           Info.FormatArgs);
  }

  emitDiagnosticMessage(SM, Info.Loc, Info.Kind, Text, Info);

  if (bracketDiagnostic)
    exitDiagBlock();
}
