//===--- SwiftEditor.cpp --------------------------------------------------===//
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

#include "SwiftASTManager.h"
#include "SwiftEditorDiagConsumer.h"
#include "SwiftLangSupport.h"
#include "SourceKit/Core/Context.h"
#include "SourceKit/Core/NotificationCenter.h"
#include "SourceKit/Support/FileSystemProvider.h"
#include "SourceKit/Support/ImmutableTextBuffer.h"
#include "SourceKit/Support/Logging.h"
#include "SourceKit/Support/Tracing.h"
#include "SourceKit/Support/UIdent.h"

#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/DiagnosticsClangImporter.h"
#include "swift/AST/DiagnosticsParse.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Demangling/ManglingUtils.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/IDE/CommentConversion.h"
#include "swift/IDE/Indenting.h"
#include "swift/IDE/SourceEntityWalker.h"
#include "swift/IDE/SyntaxModel.h"
#include "swift/Subsystems.h"

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Mutex.h"

using namespace SourceKit;
using namespace swift;
using namespace ide;

static std::vector<unsigned> getSortedBufferIDs(
    const llvm::DenseMap<unsigned, std::vector<DiagnosticEntryInfo>> &Map) {
  std::vector<unsigned> bufferIDs;
  bufferIDs.reserve(Map.size());
  for (auto I = Map.begin(), E = Map.end(); I != E; ++I) {
    bufferIDs.push_back(I->getFirst());
  }
  llvm::array_pod_sort(bufferIDs.begin(), bufferIDs.end());
  return bufferIDs;
}

void EditorDiagConsumer::getAllDiagnostics(
    SmallVectorImpl<DiagnosticEntryInfo> &Result) {

  Result.append(InvalidLocDiagnostics.begin(), InvalidLocDiagnostics.end());

  // Note: We cannot use the input buffer IDs because there may be diagnostics
  // outside the inputs. Instead, sort the extant buffers.
  auto bufferIDs = getSortedBufferIDs(BufferDiagnostics);
  for (unsigned bufferID : bufferIDs) {
    const auto &diags = BufferDiagnostics[bufferID];
    Result.append(diags.begin(), diags.end());
  }
}

/// Retrieve the raw range from a range, if it exists in the provided buffer.
/// Otherwise returns \c None.
static Optional<RawCharSourceRange> getRawRangeInBuffer(CharSourceRange range,
                                                        unsigned bufferID,
                                                        SourceManager &SM) {
  if (!range.isValid() ||
      SM.findBufferContainingLoc(range.getStart()) != bufferID) {
    return None;
  }
  unsigned offset = SM.getLocOffsetInBuffer(range.getStart(), bufferID);
  unsigned length = range.getByteLength();
  return {{offset, length}};
}

BufferInfoSharedPtr EditorDiagConsumer::getBufferInfo(
    StringRef FileName, Optional<unsigned> BufferID, swift::SourceManager &SM) {
  // NOTE: Using StringRef as a key here relies on SourceMgr using const char*
  // as buffer identifiers. This is fast, but may be brittle.  We can always
  // switch over to using a StringMap. Note that the logic in
  // SerializedDiagnosticConsumer::getEmitFile will also need changing.
  auto Result = BufferInfos.find(FileName);
  if (Result != BufferInfos.end())
    return Result->second;

  Optional<std::string> GeneratedFileText;
  Optional<BufferInfo::OriginalLocation> OriginalLocInfo;

  // If we have a generated buffer, we need to include the source text, as
  // clients otherwise won't be able to access to it.
  if (BufferID) {
    if (auto Info = SM.getGeneratedSourceInfo(*BufferID)) {
      // Don't include this information for replaced function body buffers, as
      // they'll just be referencing the original file.
      // FIXME: Does this mean that the location information we'll be giving
      // will be wrong? Seems like we may need to adjust it to correspond to the
      // original file.
      if (Info->kind != GeneratedSourceInfo::ReplacedFunctionBody) {
        GeneratedFileText.emplace(SM.getEntireTextForBuffer(*BufferID));

        auto OrigRange = Info->originalSourceRange;
        if (OrigRange.isValid()) {
          auto OrigLoc = OrigRange.getStart();
          auto OrigBuffer = SM.findBufferContainingLoc(OrigLoc);

          auto OrigRawRange = getRawRangeInBuffer(OrigRange, OrigBuffer, SM);
          assert(OrigRawRange);

          auto OrigFileName = SM.getDisplayNameForLoc(OrigLoc);
          auto OrigInfo = getBufferInfo(OrigFileName, OrigBuffer, SM);
          OriginalLocInfo.emplace(OrigInfo, OrigBuffer, *OrigRawRange);
        }
      }
    }
  }
  auto Info = std::make_shared<BufferInfo>(FileName.str(), GeneratedFileText,
                                           OriginalLocInfo);
  BufferInfos.insert({FileName, Info});
  return Info;
}

void EditorDiagConsumer::handleDiagnostic(SourceManager &SM,
                                          const DiagnosticInfo &Info) {

  if (Info.Kind == DiagnosticKind::Error) {
    HadAnyError = true;
  }

  // Filter out benign diagnostics for editing.
  // oslog_invalid_log_message is spuriously output for live issues as modules
  // in the index build are built without function bodies (including inline
  // functions). OSLogOptimization expects SIL for bodies and hence errors
  // when there isn't any. Ignore in live issues for now and re-evaluate if
  // this (not having SIL for inline functions) becomes a more widespread issue.
  if (Info.ID == diag::lex_editor_placeholder.ID ||
      Info.ID == diag::oslog_invalid_log_message.ID)
    return;

  bool IsNote = (Info.Kind == DiagnosticKind::Note);

  if (IsNote && !haveLastDiag())
    // Is this possible?
    return;

  if (Info.Kind == DiagnosticKind::Remark) {
    // FIXME: we may want to handle optimization remarks in sourcekitd.
    LOG_WARN_FUNC("unhandled optimization remark");
    return;
  }

  DiagnosticEntryInfo SKInfo;

  SKInfo.ID = DiagnosticEngine::diagnosticIDStringFor(Info.ID).str();

  if (Info.Category == "deprecation") {
    SKInfo.Categories.push_back(DiagnosticCategory::Deprecation);
  } else if (Info.Category == "no-usage") {
    SKInfo.Categories.push_back(DiagnosticCategory::NoUsage);
  }

  // Actually substitute the diagnostic arguments into the diagnostic text.
  llvm::SmallString<256> Text;
  {
    llvm::raw_svector_ostream Out(Text);
    DiagnosticEngine::formatDiagnosticText(Out, Info.FormatString,
                                           Info.FormatArgs);
  }
  SKInfo.Description = Text.str();

  for (auto notePath : Info.EducationalNotePaths)
    SKInfo.EducationalNotePaths.push_back(notePath);

  Optional<unsigned> BufferIDOpt;
  if (Info.Loc.isValid()) {
    BufferIDOpt = SM.findBufferContainingLoc(Info.Loc);
  }

  if (BufferIDOpt.has_value()) {
    unsigned BufferID = *BufferIDOpt;

    if (auto info = SM.getGeneratedSourceInfo(BufferID)) {
      if (IsNote && info->kind == GeneratedSourceInfo::PrettyPrinted) {
        // FIXME: This is a note pointing to a synthesized declaration buffer
        // for a declaration coming from a module. We should be able to remove
        // this check once clients have been updated to deal with the buffer contents
        // that we'll include in the response once this check is removed.
        //
        // For now instead of ignoring it, pick up the declaration name from the
        // buffer identifier and append it to the diagnostic message.
        auto &LastDiag = getLastDiag();
        SKInfo.Description += " (";
        SKInfo.Description += SM.getIdentifierForBuffer(*BufferIDOpt);
        SKInfo.Description += ")";
        SKInfo.Offset = LastDiag.Offset;
        SKInfo.Line = LastDiag.Line;
        SKInfo.Column = LastDiag.Column;
        SKInfo.FileInfo = LastDiag.FileInfo;
        LastDiag.Notes.push_back(std::move(SKInfo));
        return;
      }
    }

    SKInfo.Offset = SM.getLocOffsetInBuffer(Info.Loc, BufferID);
    std::tie(SKInfo.Line, SKInfo.Column) =
        SM.getPresumedLineAndColumnForLoc(Info.Loc, BufferID);

    auto Filename = SM.getDisplayNameForLoc(Info.Loc);
    SKInfo.FileInfo = getBufferInfo(Filename, BufferID, SM);

    for (auto R : Info.Ranges) {
      if (auto Raw = getRawRangeInBuffer(R, BufferID, SM))
        SKInfo.Ranges.push_back(*Raw);
    }

    for (auto F : Info.FixIts) {
      if (auto Range = getRawRangeInBuffer(F.getRange(), BufferID, SM))
        SKInfo.Fixits.emplace_back(*Range, F.getText().str());
    }
  } else {
    SKInfo.FileInfo = getBufferInfo("<unknown>", /*BufferID*/ None, SM);
  }

  if (IsNote) {
    getLastDiag().Notes.push_back(std::move(SKInfo));
    return;
  }

  switch (Info.Kind) {
  case DiagnosticKind::Error:
    SKInfo.Severity = DiagnosticSeverityKind::Error;
    break;
  case DiagnosticKind::Warning:
    SKInfo.Severity = DiagnosticSeverityKind::Warning;
    break;
  case DiagnosticKind::Note:
  case DiagnosticKind::Remark:
    llvm_unreachable("already covered");
  }

  if (!BufferIDOpt) {
    InvalidLocDiagnostics.push_back(std::move(SKInfo));
    clearLastDiag();
    return;
  }

  // Look through to the original buffer, so we produce diagnostics that are
  // present in generated buffers that originated there.
  unsigned OrigBufferID = *BufferIDOpt;
  {
    auto BufferInfo = SKInfo.FileInfo;
    while (auto &OrigLocation = BufferInfo->OrigLocation) {
      OrigBufferID = OrigLocation->OrigBufferID;
      BufferInfo = OrigLocation->OrigBufferInfo;
    }
  }

  DiagnosticsTy &Diagnostics = BufferDiagnostics[OrigBufferID];

  if (Diagnostics.empty() || Diagnostics.back().Offset <= SKInfo.Offset) {
    Diagnostics.push_back(std::move(SKInfo));
    LastDiagBufferID = OrigBufferID;
    LastDiagIndex = Diagnostics.size() - 1;
    return;
  }

  // Keep the diagnostics array in source order.
  auto Pos = std::lower_bound(Diagnostics.begin(), Diagnostics.end(), SKInfo.Offset,
    [&](const DiagnosticEntryInfo &LHS, unsigned Offset) -> bool {
      return LHS.Offset < Offset;
    });
  LastDiagBufferID = OrigBufferID;
  LastDiagIndex = Pos - Diagnostics.begin();
  Diagnostics.insert(Pos, std::move(SKInfo));
}

SwiftEditorDocumentRef
SwiftEditorDocumentFileMap::getByUnresolvedName(StringRef FilePath) {
  SwiftEditorDocumentRef EditorDoc;

  Queue.dispatchSync([&]{
    auto It = Docs.find(FilePath);
    if (It != Docs.end())
      EditorDoc = It->second.DocRef;
   });

  return EditorDoc;
}

SwiftEditorDocumentRef
SwiftEditorDocumentFileMap::findByPath(StringRef FilePath, bool IsRealpath) {
  SwiftEditorDocumentRef EditorDoc;

  std::string Scratch;
  if (!IsRealpath) {
    Scratch = SwiftLangSupport::resolvePathSymlinks(FilePath);
    FilePath = Scratch;
  }
  Queue.dispatchSync([&]{
    for (auto &Entry : Docs) {
      if (Entry.getKey() == FilePath ||
          Entry.getValue().ResolvedPath == FilePath) {
        EditorDoc = Entry.getValue().DocRef;
        break;
      }
    }
  });

  return EditorDoc;
}

bool SwiftEditorDocumentFileMap::getOrUpdate(
    StringRef FilePath, SwiftLangSupport &LangSupport,
    SwiftEditorDocumentRef &EditorDoc) {

  bool found = false;

  std::string ResolvedPath = SwiftLangSupport::resolvePathSymlinks(FilePath);
  Queue.dispatchBarrierSync([&]{
    DocInfo &Doc = Docs[FilePath];
    if (!Doc.DocRef) {
      Doc.DocRef = EditorDoc;
      Doc.ResolvedPath = ResolvedPath;
    } else {
      EditorDoc = Doc.DocRef;
      found = true;
    }
  });

  return found;
}

SwiftEditorDocumentRef SwiftEditorDocumentFileMap::remove(StringRef FilePath) {
  SwiftEditorDocumentRef Removed;
  Queue.dispatchBarrierSync([&]{
    auto I = Docs.find(FilePath);
    if (I != Docs.end()) {
      Removed = I->second.DocRef;
      Docs.erase(I);
    }
  });
  return Removed;
}

namespace {

/// Merges two overlapping ranges and splits the first range into two
/// ranges before and after the overlapping range.
void mergeSplitRanges(unsigned Off1, unsigned Len1, unsigned Off2, unsigned Len2,
                      std::function<void(unsigned BeforeOff, unsigned BeforeLen,
                                         unsigned AfterOff,
                                         unsigned AfterLen)> applier) {
  unsigned End1 = Off1 + Len1;
  unsigned End2 = Off2 + Len2;
  if (End1 > Off2) {
    // Overlapping. Split into before and after ranges.
    unsigned BeforeOff = Off1;
    unsigned BeforeLen = Off2 > Off1 ? Off2 - Off1 : 0;
    unsigned AfterOff = End2;
    unsigned AfterLen = End1 > End2 ? End1 - End2 : 0;
    applier(BeforeOff, BeforeLen, AfterOff, AfterLen);
  }
  else {
    // Not overlapping.
    applier(Off1, Len1, 0, 0);
  }
}

struct SwiftSyntaxToken {
  unsigned Offset;
  unsigned Length:24;
  SyntaxNodeKind Kind:8;

  static SwiftSyntaxToken createInvalid() {
    return {0, 0, SyntaxNodeKind::AttributeBuiltin};
  }

  SwiftSyntaxToken(unsigned Offset, unsigned Length, SyntaxNodeKind Kind)
    : Offset(Offset), Length(Length), Kind(Kind) {}

  unsigned endOffset() const { return Offset + Length; }

  bool isInvalid() const { return Length == 0; }

  bool operator==(const SwiftSyntaxToken &Other) const {
    return Offset == Other.Offset && Length == Other.Length &&
      Kind == Other.Kind;
  }

  bool operator!=(const SwiftSyntaxToken &Other) const {
    return Offset != Other.Offset || Length != Other.Length ||
      Kind != Other.Kind;
  }
};

struct SwiftEditorCharRange {
  unsigned Offset;
  unsigned EndOffset;

  SwiftEditorCharRange(unsigned Offset, unsigned EndOffset) :
    Offset(Offset), EndOffset(EndOffset) {}

  SwiftEditorCharRange(SwiftSyntaxToken Token) :
    Offset(Token.Offset), EndOffset(Token.endOffset()) {}

  size_t length() const { return EndOffset - Offset; }
  bool isEmpty() const { return Offset == EndOffset; }
  bool intersects(const SwiftSyntaxToken &Token) const {
    return this->Offset < (Token.endOffset()) && this->EndOffset > Token.Offset;
  }
  void extendToInclude(const SwiftEditorCharRange &Range) {
    if (Range.Offset < Offset)
      Offset = Range.Offset;
    if (Range.EndOffset > EndOffset)
      EndOffset = Range.EndOffset;
  }
  void extendToInclude(unsigned OtherOffset) {
    extendToInclude({OtherOffset, OtherOffset});
  }
};

/// Finds and represents the first mismatching tokens in two syntax maps,
/// ignoring invalidated tokens.
template <class Iter>
struct TokenMismatch {
  /// The begin and end iterators of the previous syntax map
  Iter PrevTok, PrevEnd;
  /// The begin and end iterators of the current syntax map
  Iter CurrTok, CurrEnd;

  TokenMismatch(Iter CurrTok, Iter CurrEnd, Iter PrevTok, Iter PrevEnd) :
  PrevTok(PrevTok), PrevEnd(PrevEnd), CurrTok(CurrTok), CurrEnd(CurrEnd) {
    skipInvalid();
    while(advance());
  }

  /// Returns true if a mismatch was found
  bool foundMismatch() const {
    return CurrTok != CurrEnd || PrevTok != PrevEnd;
  }

  /// Returns the smallest start offset of the mismatched token ranges
  unsigned mismatchStart() const {
    assert(foundMismatch());
    if (CurrTok != CurrEnd) {
      if (PrevTok != PrevEnd)
        return std::min(CurrTok->Offset, PrevTok->Offset);
      return CurrTok->Offset;
    }
    return PrevTok->Offset;
  }

  /// Returns the largest end offset of the mismatched token ranges
  unsigned mismatchEnd() const {
    assert(foundMismatch());
    if (CurrTok != CurrEnd) {
      if (PrevTok != PrevEnd)
        return std::max(CurrTok->endOffset(), PrevTok->endOffset());
      return CurrTok->endOffset();
    }
    return PrevTok->endOffset();
  }

private:
  void skipInvalid() {
    while (PrevTok != PrevEnd && PrevTok->isInvalid())
      ++PrevTok;
  }

  bool advance() {
    if (CurrTok == CurrEnd || PrevTok == PrevEnd || *CurrTok != *PrevTok)
      return false;
    ++CurrTok;
    ++PrevTok;
    skipInvalid();
    return true;
  }
};

/// Represents a the syntax highlighted token ranges in a source file
struct SwiftSyntaxMap {
  std::vector<SwiftSyntaxToken> Tokens;

  explicit SwiftSyntaxMap(unsigned Capacity = 0) {
    if (Capacity)
      Tokens.reserve(Capacity);
  }

  void addToken(const SwiftSyntaxToken &Token) {
    assert(Tokens.empty() || Token.Offset >= Tokens.back().Offset);
    Tokens.push_back(Token);
  }

  /// Merge this nested token into the last token that was added
  void mergeToken(const SwiftSyntaxToken &Token) {
    if (Tokens.empty()) {
      Tokens.push_back(Token);
      return;
    }
    auto &LastTok = Tokens.back();
    assert(LastTok.Offset <= Token.Offset);
    mergeSplitRanges(LastTok.Offset, LastTok.Length, Token.Offset, Token.Length,
                     [&](unsigned BeforeOff, unsigned BeforeLen,
                         unsigned AfterOff, unsigned AfterLen) {
                       auto LastKind = LastTok.Kind;
                       Tokens.pop_back();
                       if (BeforeLen)
                         Tokens.emplace_back(BeforeOff, BeforeLen, LastKind);
                       Tokens.push_back(Token);
                       if (AfterLen)
                         Tokens.emplace_back(AfterOff, AfterLen, LastKind);
                     });
  }

  /// Adjusts the token offsets and lengths in this syntax map to account for
  /// replacing \p Len bytes at the given \p Offset with \p NewLen bytes. Tokens
  /// before the replacement stay the same, tokens after it are shifted, and
  /// tokens that intersect it are 'removed' (really just marked invalid).
  /// Clients are expected to match this behavior.
  ///
  /// Returns the union of the replaced range and the token ranges it
  /// intersected, or nothing if no tokens were intersected.
  llvm::Optional<SwiftEditorCharRange>
  adjustForReplacement(unsigned Offset, unsigned Len, unsigned NewLen) {
    unsigned ReplacedStart = Offset;
    unsigned ReplacedEnd = Offset + Len;
    bool TokenIntersected = false;
    SwiftEditorCharRange Affected = { /*Offset=*/ReplacedStart,
                                      /*EndOffset=*/ReplacedEnd};
    // Adjust the tokens
    auto Token = Tokens.begin();
    while (Token != Tokens.end() && Token->endOffset() <= ReplacedStart) {
      // Completely before the replaced range – no change needed
      ++Token;
    }

    while (Token != Tokens.end() && Token->Offset < ReplacedEnd) {
      // Intersecting the replaced range – extend Affected and invalidate
      TokenIntersected = true;
      Affected.extendToInclude(*Token);
      *Token = SwiftSyntaxToken::createInvalid();
      ++Token;
    }

    while (Token != Tokens.end()) {
      // Completely after the replaced range - shift to account for NewLen
      if (NewLen >= Len)
        Token->Offset += NewLen - Len;
      else
        Token->Offset -= Len - NewLen;
      ++Token;
    }

    // If the replaced range didn't intersect with any existing tokens, there's
    // no need to report an affected range
    if (!TokenIntersected)
      return None;

    // Update the end of the affected range to account for NewLen
    if (NewLen >= Len) {
      Affected.EndOffset += NewLen - Len;
    } else {
      Affected.EndOffset -= Len - NewLen;
    }

    return Affected;
  }

  /// Passes each token in this SwiftSyntaxMap to the given \p Consumer
  void forEach(EditorConsumer &Consumer) {
    for (auto &Token: Tokens) {
      auto Kind = SwiftLangSupport::getUIDForSyntaxNodeKind(Token.Kind);
      Consumer.handleSyntaxMap(Token.Offset, Token.Length, Kind);
    }
  }

  /// Finds the delta between the given SwiftSyntaxMap, \p Prev, and this one.
  /// It passes each token not in \p Prev to the given \p Consumer and, if
  /// needed, also expands or sets the given \p Affected range to cover all
  /// non-matching tokens in the two lists.
  ///
  /// Returns true if this SwiftSyntaxMap is different to \p Prev.
  bool forEachChanged(const SwiftSyntaxMap &Prev,
                      llvm::Optional<SwiftEditorCharRange> &Affected,
                      EditorConsumer &Consumer) const {
    typedef std::vector<SwiftSyntaxToken>::const_iterator ForwardIt;
    typedef std::vector<SwiftSyntaxToken>::const_reverse_iterator ReverseIt;

    // Find the first pair of tokens that don't match
    TokenMismatch<ForwardIt>
    Forward(Tokens.begin(), Tokens.end(), Prev.Tokens.begin(), Prev.Tokens.end());

    // Exit early if there was no mismatch
    if (!Forward.foundMismatch())
      return Affected && !Affected->isEmpty();

    // Find the last pair of tokens that don't match
    TokenMismatch<ReverseIt>
    Backward(Tokens.rbegin(), Tokens.rend(), Prev.Tokens.rbegin(), Prev.Tokens.rend());
    assert(Backward.foundMismatch());

    // Set or extend the affected range to include the  mismatched range
    SwiftEditorCharRange
    MismatchRange = {Forward.mismatchStart(),Backward.mismatchEnd()};
    if (!Affected) {
      Affected = MismatchRange;
    } else {
      Affected->extendToInclude(MismatchRange);
    }

    // Report all tokens in the affected range to the EditorConsumer
    auto From = Forward.CurrTok;
    auto To = Backward.CurrTok;
    while (From != Tokens.begin() && (From-1)->Offset >= Affected->Offset)
      --From;
    while (To != Tokens.rbegin() && (To-1)->endOffset() <= Affected->EndOffset)
      --To;
    for (; From < To.base(); ++From) {
      auto Kind = SwiftLangSupport::getUIDForSyntaxNodeKind(From->Kind);
      Consumer.handleSyntaxMap(From->Offset, From->Length, Kind);
    }

    return true;
  }
};

struct EditorConsumerSyntaxMapEntry {
  unsigned Offset;
  unsigned Length;
  UIdent Kind;
  EditorConsumerSyntaxMapEntry(unsigned Offset, unsigned Length, UIdent Kind)
    :Offset(Offset), Length(Length), Kind(Kind) { }
};

struct SwiftSemanticToken {
  unsigned ByteOffset;
  unsigned Length : 24;
  // The code-completion kinds are a good match for the semantic kinds we want.
  // FIXME: Maybe rename CodeCompletionDeclKind to a more general concept ?
  CodeCompletionDeclKind Kind : 6;
  unsigned IsRef : 1;
  unsigned IsSystem : 1;

  SwiftSemanticToken(CodeCompletionDeclKind Kind,
                     unsigned ByteOffset, unsigned Length,
                     bool IsRef, bool IsSystem)
    : ByteOffset(ByteOffset), Length(Length), Kind(Kind),
      IsRef(IsRef), IsSystem(IsSystem) { }

  bool getIsRef() const { return static_cast<bool>(IsRef); }

  bool getIsSystem() const { return static_cast<bool>(IsSystem); }

  UIdent getUIdentForKind() const {
    return SwiftLangSupport::getUIDForCodeCompletionDeclKind(Kind, getIsRef());
  }
};
#if !defined(_MSC_VER)
static_assert(sizeof(SwiftSemanticToken) == 8, "Too big");
// FIXME: MSVC doesn't pack bitfields with different underlying types.
// Giving up to check this in MSVC for now, because static_assert is only for
// keeping low memory usage.
#endif

class SwiftDocumentSemanticInfo :
    public ThreadSafeRefCountedBase<SwiftDocumentSemanticInfo> {

  const std::string Filename;
  std::weak_ptr<SwiftASTManager> ASTMgr;
  std::shared_ptr<NotificationCenter> NotificationCtr;
  ThreadSafeRefCntPtr<SwiftInvocation> InvokRef;
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem;
  std::string CompilerArgsError;

  uint64_t ASTGeneration = 0;
  ImmutableTextSnapshotRef TokSnapshot;
  std::vector<SwiftSemanticToken> SemaToks;

  ImmutableTextSnapshotRef DiagSnapshot;
  std::vector<DiagnosticEntryInfo> SemaDiags;

  mutable llvm::sys::Mutex Mtx;

public:
  SwiftDocumentSemanticInfo(
      StringRef Filename, std::weak_ptr<SwiftASTManager> ASTMgr,
      std::shared_ptr<NotificationCenter> NotificationCtr,
      llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem)
      : Filename(Filename), ASTMgr(ASTMgr), NotificationCtr(NotificationCtr),
        fileSystem(fileSystem) {}

  SwiftInvocationRef getInvocation() const {
    return InvokRef;
  }

  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> getFileSystem() const {
    return fileSystem;
  }

  uint64_t getASTGeneration() const;

  void setCompilerArgs(ArrayRef<const char *> Args) {
    if (auto ASTMgr = this->ASTMgr.lock()) {
      InvokRef =
          ASTMgr->getTypecheckInvocation(Args, Filename, CompilerArgsError);
    }
  }

  void readSemanticInfo(ImmutableTextSnapshotRef NewSnapshot,
                        std::vector<SwiftSemanticToken> &Tokens,
                        Optional<std::vector<DiagnosticEntryInfo>> &Diags,
                        ArrayRef<DiagnosticEntryInfo> ParserDiags);

  void processLatestSnapshotAsync(EditableTextBufferRef EditableBuffer,
                                  SourceKitCancellationToken CancellationToken);

  void updateSemanticInfo(std::vector<SwiftSemanticToken> Toks,
                          std::vector<DiagnosticEntryInfo> Diags,
                          ImmutableTextSnapshotRef Snapshot,
                          uint64_t ASTGeneration);
  void removeCachedAST() {
    if (InvokRef) {
      if (auto ASTMgr = this->ASTMgr.lock()) {
        ASTMgr->removeCachedAST(InvokRef);
      }
    }
  }

private:
  std::vector<SwiftSemanticToken> takeSemanticTokens(
      ImmutableTextSnapshotRef NewSnapshot);

  Optional<std::vector<DiagnosticEntryInfo>> getSemanticDiagnostics(
      ImmutableTextSnapshotRef NewSnapshot,
      ArrayRef<DiagnosticEntryInfo> ParserDiags);
};

class SwiftDocumentSyntaxInfo {
  SourceManager SM;
  EditorDiagConsumer DiagConsumer;
  std::unique_ptr<ParserUnit> Parser;
  unsigned BufferID;
  std::vector<std::string> Args;
  std::string PrimaryFile;
  bool IsParsed;

public:
  SwiftDocumentSyntaxInfo(const CompilerInvocation &CompInv,
                          ImmutableTextSnapshotRef Snapshot,
                          std::vector<std::string> &Args,
                          StringRef FilePath)
        : Args(Args), PrimaryFile(FilePath) {

    std::unique_ptr<llvm::MemoryBuffer> BufCopy =
      llvm::MemoryBuffer::getMemBufferCopy(
        Snapshot->getBuffer()->getText(), FilePath);

    BufferID = SM.addNewSourceBuffer(std::move(BufCopy));

    Parser.reset(new ParserUnit(
        SM, SourceFileKind::Main, BufferID, CompInv.getLangOptions(),
        CompInv.getTypeCheckerOptions(), CompInv.getSILOptions(),
        CompInv.getModuleName()));

    registerParseRequestFunctions(Parser->getParser().Context.evaluator);
    registerTypeCheckerRequestFunctions(
        Parser->getParser().Context.evaluator);
    registerClangImporterRequestFunctions(Parser->getParser().Context.evaluator);
    Parser->getDiagnosticEngine().addConsumer(DiagConsumer);

    IsParsed = false;
  }

  void parseIfNeeded() {
    if (!IsParsed) {
      // Perform parsing on a deep stack that's the same size as the main thread
      // during normal compilation to avoid stack overflows.
      static WorkQueue BigStackQueue{
          WorkQueue::Dequeuing::Concurrent,
          "SwiftDocumentSyntaxInfo::parseIfNeeded.BigStackQueue"};
      BigStackQueue.dispatchSync(
          [this]() {
            Parser->parse();
            IsParsed = true;
          },
          /*isStackDeep=*/true);
    }
  }

  SourceFile &getSourceFile() {
    return Parser->getSourceFile();
  }

  unsigned getBufferID() {
    return BufferID;
  }

  const LangOptions &getLangOptions() {
    return Parser->getLangOptions();
  }

  SourceManager &getSourceManager() {
    return SM;
  }

  ArrayRef<DiagnosticEntryInfo> getDiagnostics() {
    return DiagConsumer.getDiagnosticsForBuffer(BufferID);
  }
};

} // anonymous namespace

uint64_t SwiftDocumentSemanticInfo::getASTGeneration() const {
  llvm::sys::ScopedLock L(Mtx);
  return ASTGeneration;
}

void SwiftDocumentSemanticInfo::readSemanticInfo(
    ImmutableTextSnapshotRef NewSnapshot,
    std::vector<SwiftSemanticToken> &Tokens,
    Optional<std::vector<DiagnosticEntryInfo>> &Diags,
    ArrayRef<DiagnosticEntryInfo> ParserDiags) {

  llvm::sys::ScopedLock L(Mtx);

  Tokens = takeSemanticTokens(NewSnapshot);
  Diags = getSemanticDiagnostics(NewSnapshot, ParserDiags);
}

std::vector<SwiftSemanticToken>
SwiftDocumentSemanticInfo::takeSemanticTokens(
    ImmutableTextSnapshotRef NewSnapshot) {

  llvm::sys::ScopedLock L(Mtx);

  if (SemaToks.empty())
    return {};

  // Adjust the position of the tokens.
  TokSnapshot->foreachReplaceUntil(NewSnapshot,
    [&](ReplaceImmutableTextUpdateRef Upd) -> bool {
      if (SemaToks.empty())
        return false;

      auto ReplaceBegin = std::lower_bound(SemaToks.begin(), SemaToks.end(),
          Upd->getByteOffset(),
          [&](const SwiftSemanticToken &Tok, unsigned StartOffset) -> bool {
            return Tok.ByteOffset+Tok.Length < StartOffset;
          });

      std::vector<SwiftSemanticToken>::iterator ReplaceEnd;
      if (ReplaceBegin == SemaToks.end()) {
        ReplaceEnd = ReplaceBegin;
      } else if (Upd->getLength() == 0) {
        ReplaceEnd = ReplaceBegin + 1;
      } else {
        ReplaceEnd = std::upper_bound(ReplaceBegin, SemaToks.end(),
            Upd->getByteOffset() + Upd->getLength(),
            [&](unsigned EndOffset, const SwiftSemanticToken &Tok) -> bool {
              return EndOffset < Tok.ByteOffset;
            });
      }

      unsigned InsertLen = Upd->getText().size();
      int Delta = InsertLen - Upd->getLength();
      if (Delta != 0) {
        for (std::vector<SwiftSemanticToken>::iterator
               I = ReplaceEnd, E = SemaToks.end(); I != E; ++I)
          I->ByteOffset += Delta;
      }
      SemaToks.erase(ReplaceBegin, ReplaceEnd);
      return true;
    });

  return std::move(SemaToks);
}

Optional<std::vector<DiagnosticEntryInfo>>
SwiftDocumentSemanticInfo::getSemanticDiagnostics(
    ImmutableTextSnapshotRef NewSnapshot,
    ArrayRef<DiagnosticEntryInfo> ParserDiags) {

  std::vector<DiagnosticEntryInfo> curSemaDiags;
  {
    llvm::sys::ScopedLock L(Mtx);

    if (!DiagSnapshot || DiagSnapshot->getStamp() != NewSnapshot->getStamp()) {
      // The semantic diagnostics are out-of-date, ignore them.
      return llvm::None;
    }

    curSemaDiags = SemaDiags;
  }

  // Diagnostics from the AST and diagnostics from the parser are based on the
  // same source text snapshot. But diagnostics from the AST may have excluded
  // the parser diagnostics due to a fatal error, e.g. if the source has a
  // 'so such module' error, which will suppress other diagnostics.
  // We don't want to turn off the suppression to avoid a flood of diagnostics
  // when a module import fails, but we also don't want to lose the parser
  // diagnostics in such a case, so merge the parser diagnostics with the sema
  // ones.

  auto orderDiagnosticEntryInfos = [](const DiagnosticEntryInfo &LHS,
                                      const DiagnosticEntryInfo &RHS) -> bool {
    if (LHS.FileInfo->BufferName != RHS.FileInfo->BufferName)
      return LHS.FileInfo->BufferName < RHS.FileInfo->BufferName;
    if (LHS.Offset != RHS.Offset)
      return LHS.Offset < RHS.Offset;
    return LHS.Description < RHS.Description;
  };

  std::vector<DiagnosticEntryInfo> sortedParserDiags;
  sortedParserDiags.reserve(ParserDiags.size());
  sortedParserDiags.insert(sortedParserDiags.end(), ParserDiags.begin(),
                           ParserDiags.end());
  std::stable_sort(sortedParserDiags.begin(), sortedParserDiags.end(),
                   orderDiagnosticEntryInfos);

  std::vector<DiagnosticEntryInfo> finalDiags;
  finalDiags.reserve(sortedParserDiags.size()+curSemaDiags.size());

  // Add sema diagnostics unless it is an existing parser diagnostic.
  // Note that we want to merge and eliminate diagnostics from the 'sema' set
  // that also show up in the 'parser' set, but we don't want to remove
  // duplicate diagnostics from within the same set (e.g. duplicates existing in
  // the 'sema' set). We want to report the diagnostics as the compiler reported
  // them, even if there's some duplicate one. This is why we don't just do a
  // simple append/sort/keep-uniques step.
  for (const auto &curDE : curSemaDiags) {
    bool existsAsParserDiag = std::binary_search(sortedParserDiags.begin(),
                                                 sortedParserDiags.end(),
                                             curDE, orderDiagnosticEntryInfos);
    if (!existsAsParserDiag) {
      finalDiags.push_back(curDE);
    }
  }

  finalDiags.insert(finalDiags.end(),
                    sortedParserDiags.begin(), sortedParserDiags.end());
  std::stable_sort(finalDiags.begin(), finalDiags.end(),
                   orderDiagnosticEntryInfos);

  return finalDiags;
}

void SwiftDocumentSemanticInfo::updateSemanticInfo(
    std::vector<SwiftSemanticToken> Toks,
    std::vector<DiagnosticEntryInfo> Diags,
    ImmutableTextSnapshotRef Snapshot,
    uint64_t ASTGeneration) {

  {
    llvm::sys::ScopedLock L(Mtx);
    if (ASTGeneration > this->ASTGeneration) {
      SemaToks = std::move(Toks);
      SemaDiags = std::move(Diags);
      TokSnapshot = DiagSnapshot = std::move(Snapshot);
      this->ASTGeneration = ASTGeneration;
    }
  }

  LOG_INFO_FUNC(High, "posted document update notification for: " << Filename);
  NotificationCtr->postDocumentUpdateNotification(Filename);
}

namespace {

class SemanticAnnotator : public SourceEntityWalker {
  SourceManager &SM;
  unsigned BufferID;
public:

  std::vector<SwiftSemanticToken> SemaToks;

  SemanticAnnotator(SourceManager &SM, unsigned BufferID)
    : SM(SM), BufferID(BufferID) {}

  bool visitDeclReference(ValueDecl *D, CharSourceRange Range,
                          TypeDecl *CtorTyRef, ExtensionDecl *ExtTyRef, Type T,
                          ReferenceMetaData Data) override {
    if (Data.isImplicit)
      return true;

    if (isa<VarDecl>(D) && D->hasName() &&
        D->getName() == D->getASTContext().Id_self)
      return true;

    // Do not annotate references to unavailable decls.
    if (AvailableAttr::isUnavailable(D))
      return true;

    auto &SM = D->getASTContext().SourceMgr;
    if (D == D->getASTContext().getOptionalNoneDecl() &&
        SM.extractText(Range, BufferID) == "nil") {
      // If a 'nil' literal occurs in a swift-case statement, it gets replaced
      // by a reference to 'Optional.none' in the AST. We want to continue
      // highlighting 'nil' as a keyword and not as an enum element.
      return true;
    }

    if (CtorTyRef)
      D = CtorTyRef;
    annotate(D, /*IsRef=*/true, Range);
    return true;
  }

  bool visitSubscriptReference(ValueDecl *D, CharSourceRange Range,
                               ReferenceMetaData Data,
                               bool IsOpenBracket) override {
    // We should treat both open and close brackets equally
    return visitDeclReference(D, Range, nullptr, nullptr, Type(), Data);
  }

  void annotate(const Decl *D, bool IsRef, CharSourceRange Range) {
    if (!Range.isValid())
      return;

    unsigned ByteOffset = SM.getLocOffsetInBuffer(Range.getStart(), BufferID);
    unsigned Length = Range.getByteLength();
    auto Kind = ContextFreeCodeCompletionResult::getCodeCompletionDeclKind(D);
    bool IsSystem = D->getModuleContext()->isNonUserModule();
    SemaToks.emplace_back(Kind, ByteOffset, Length, IsRef, IsSystem);
  }
};

} // anonymous namespace

namespace {

class AnnotAndDiagASTConsumer : public SwiftASTConsumer {
  EditableTextBufferRef EditableBuffer;
  RefPtr<SwiftDocumentSemanticInfo> SemaInfoRef;

public:
  std::vector<SwiftSemanticToken> SemaToks;
  SourceKitCancellationToken CancellationToken;

  AnnotAndDiagASTConsumer(EditableTextBufferRef EditableBuffer,
                          RefPtr<SwiftDocumentSemanticInfo> SemaInfoRef,
                          SourceKitCancellationToken CancellationToken)
      : EditableBuffer(std::move(EditableBuffer)),
        SemaInfoRef(std::move(SemaInfoRef)),
        CancellationToken(CancellationToken) {}

  void failed(StringRef Error) override {
    LOG_WARN_FUNC("sema annotations failed: " << Error);
  }

  void handlePrimaryAST(ASTUnitRef AstUnit) override {
    auto Generation = AstUnit->getGeneration();
    auto &CompIns = AstUnit->getCompilerInstance();
    auto &Consumer = AstUnit->getEditorDiagConsumer();
    assert(Generation);

    if (Generation < SemaInfoRef->getASTGeneration()) {
      // It may happen that this request was waiting in async queue for
      // too long so another thread has already updated this sema with
      // ast generation bigger than ASTGeneration
      return;
    }

    ImmutableTextSnapshotRef DocSnapshot;
    for (auto &Snap : AstUnit->getSnapshots()) {
      if (Snap->getEditableBuffer() == EditableBuffer) {
        DocSnapshot = Snap;
        break;
      }
    }

    if (!DocSnapshot) {
      LOG_WARN_FUNC("did not find document snapshot when handling the AST");
      return;
    }

    if (Generation == SemaInfoRef->getASTGeneration()) {
      // Save time if we already know we processed this AST version.
      if (DocSnapshot->getStamp() != EditableBuffer->getSnapshot()->getStamp()){
        // Handle edits that occurred after we processed the AST.
        SemaInfoRef->processLatestSnapshotAsync(EditableBuffer,
                                                CancellationToken);
      }
      return;
    }

    if (!AstUnit->getPrimarySourceFile().getBufferID().has_value()) {
      LOG_WARN_FUNC("Primary SourceFile is expected to have a BufferID");
      return;
    }
    unsigned BufferID = AstUnit->getPrimarySourceFile().getBufferID().value();

    SemanticAnnotator Annotator(CompIns.getSourceMgr(), BufferID);
    Annotator.walk(AstUnit->getPrimarySourceFile());
    SemaToks = std::move(Annotator.SemaToks);

    SemaInfoRef->
      updateSemanticInfo(std::move(SemaToks),
                     std::move(Consumer.getDiagnosticsForBuffer(BufferID)),
                         DocSnapshot,
                         Generation);

    if (DocSnapshot->getStamp() != EditableBuffer->getSnapshot()->getStamp()) {
      // Handle edits that occurred after we processed the AST.
      SemaInfoRef->processLatestSnapshotAsync(EditableBuffer,
                                              CancellationToken);
    }
  }
};

} // anonymous namespace

void SwiftDocumentSemanticInfo::processLatestSnapshotAsync(
    EditableTextBufferRef EditableBuffer,
    SourceKitCancellationToken CancellationToken) {

  SwiftInvocationRef Invok = InvokRef;
  if (!Invok)
    return;

  RefPtr<SwiftDocumentSemanticInfo> SemaInfoRef = this;
  auto Consumer = std::make_shared<AnnotAndDiagASTConsumer>(
      EditableBuffer, SemaInfoRef, CancellationToken);

  // Semantic annotation queries for a particular document should cancel
  // previously queued queries for the same document. Each document has a
  // SwiftDocumentSemanticInfo pointer so use that for the token.
  const void *OncePerASTToken = SemaInfoRef.get();
  if (auto ASTMgr = this->ASTMgr.lock()) {
    ASTMgr->processASTAsync(Invok, std::move(Consumer), OncePerASTToken,
                            CancellationToken, fileSystem);
  }
}

struct SwiftEditorDocument::Implementation {
  std::weak_ptr<SwiftASTManager> ASTMgr;
  std::shared_ptr<NotificationCenter> NotificationCtr;

  const std::string FilePath;
  EditableTextBufferRef EditableBuffer;

  /// The list of syntax highlighted token offsets and ranges in the document
  SwiftSyntaxMap SyntaxMap;
  /// The minimal range of syntax highlighted tokens affected by the last edit
  llvm::Optional<SwiftEditorCharRange> AffectedRange;
  /// Whether the last operation was an edit rather than a document open
  bool Edited;

  std::vector<DiagnosticEntryInfo> ParserDiagnostics;
  RefPtr<SwiftDocumentSemanticInfo> SemanticInfo;
  CodeFormatOptions FormatOptions;

  std::shared_ptr<SwiftDocumentSyntaxInfo> SyntaxInfo;

  std::shared_ptr<SwiftDocumentSyntaxInfo> getSyntaxInfo() {
    llvm::sys::ScopedLock L(AccessMtx);
    SyntaxInfo->parseIfNeeded();
    return SyntaxInfo;
  }

  llvm::sys::Mutex AccessMtx;

  Implementation(StringRef FilePath, SwiftLangSupport &LangSupport,
                 CodeFormatOptions options,
                 llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem)
      : ASTMgr(LangSupport.getASTManager()),
        NotificationCtr(LangSupport.getNotificationCenter()),
        FilePath(FilePath), FormatOptions(options) {
    assert(fileSystem);
    // This instance of semantic info is used if a document is opened with
    // `key.syntactic_only: 1`, but subsequently a semantic request such as
    // cursor_info is made.
    SemanticInfo = new SwiftDocumentSemanticInfo(
        FilePath, ASTMgr, NotificationCtr, fileSystem);
  }
};

namespace  {

static UIdent getAccessLevelUID(AccessLevel Access) {
  static UIdent AccessOpen("source.lang.swift.accessibility.open");
  static UIdent AccessPublic("source.lang.swift.accessibility.public");
  static UIdent AccessPackage("source.lang.swift.accessibility.package");
  static UIdent AccessInternal("source.lang.swift.accessibility.internal");
  static UIdent AccessFilePrivate("source.lang.swift.accessibility.fileprivate");
  static UIdent AccessPrivate("source.lang.swift.accessibility.private");

  switch (Access) {
  case AccessLevel::Private:
    return AccessPrivate;
  case AccessLevel::FilePrivate:
    return AccessFilePrivate;
  case AccessLevel::Internal:
    return AccessInternal;
  case AccessLevel::Package:
    return AccessPackage;
  case AccessLevel::Public:
    return AccessPublic;
  case AccessLevel::Open:
    return AccessOpen;
  }

  llvm_unreachable("Unhandled access level in switch.");
}

static Optional<AccessLevel>
inferDefaultAccessSyntactically(const ExtensionDecl *ED) {
  // Check if the extension has an explicit access control attribute.
  if (auto *AA = ED->getAttrs().getAttribute<AccessControlAttr>())
    return std::min(std::max(AA->getAccess(), AccessLevel::FilePrivate),
                    AccessLevel::Public);
  return None;
}

/// Document structure is a purely syntactic request that shouldn't require name lookup
/// or type-checking, so this is a best-effort computation, particularly where extensions
/// are concerned.
static Optional<AccessLevel> inferAccessSyntactically(const ValueDecl *D) {
  assert(D);

  // Check if the decl has an explicit access control attribute.
  if (auto *AA = D->getAttrs().getAttribute<AccessControlAttr>())
    return AA->getAccess();

  DeclContext *DC = D->getDeclContext();

  if (D->getKind() == DeclKind::Destructor ||
      D->getKind() == DeclKind::EnumElement) {
    if (auto container = dyn_cast<NominalTypeDecl>(D->getDeclContext())) {
      if (auto containerAccess = inferAccessSyntactically(container))
        return std::max(containerAccess.value(), AccessLevel::Internal);
      return None;
    }
    return AccessLevel::Private;
  }

  switch (DC->getContextKind()) {
  case DeclContextKind::TopLevelCodeDecl:
    return AccessLevel::FilePrivate;
  case DeclContextKind::SerializedLocal:
  case DeclContextKind::AbstractClosureExpr:
  case DeclContextKind::EnumElementDecl:
  case DeclContextKind::Initializer:
  case DeclContextKind::AbstractFunctionDecl:
  case DeclContextKind::SubscriptDecl:
    return AccessLevel::Private;
  case DeclContextKind::Package:
    return AccessLevel::Package;
  case DeclContextKind::Module:
  case DeclContextKind::FileUnit:
  case DeclContextKind::MacroDecl:
    return AccessLevel::Internal;
  case DeclContextKind::GenericTypeDecl: {
    auto generic = cast<GenericTypeDecl>(DC);
    AccessLevel access = AccessLevel::Internal;
    if (isa<ProtocolDecl>(generic)) {
      if (auto protoAccess = inferAccessSyntactically(generic))
        access = std::max(AccessLevel::FilePrivate, protoAccess.value());
    }
    return access;
  }
  case DeclContextKind::ExtensionDecl:
    auto *ED = cast<ExtensionDecl>(DC);
    return inferDefaultAccessSyntactically(ED);
  }

  llvm_unreachable("Unhandled DeclContextKind in switch.");
}

/// Document structure is a purely syntactic request that shouldn't require name lookup
/// or type-checking, so this is a best-effort computation.
static bool inferIsSettableSyntactically(const AbstractStorageDecl *D) {
  if (auto *VD = dyn_cast<VarDecl>(D)) {
    if (VD->isLet())
      return false;
  }
  if (D->hasParsedAccessors()) {
    return D->getParsedAccessor(AccessorKind::Set) != nullptr ||
           D->hasObservers();
  } else {
    return true;
  }
}

static Optional<AccessLevel>
inferSetterAccessSyntactically(const AbstractStorageDecl *D) {
  if (!inferIsSettableSyntactically(D))
    return None;
  if (auto *AA = D->getAttrs().getAttribute<SetterAccessAttr>())
    return AA->getAccess();
  return inferAccessSyntactically(D);
}

class SwiftDocumentStructureWalker: public ide::SyntaxModelWalker {
  SourceManager &SrcManager;
  EditorConsumer &Consumer;
  unsigned BufferID;

public:
  SwiftDocumentStructureWalker(SourceManager &SrcManager,
                               unsigned BufferID,
                               EditorConsumer &Consumer)
    : SrcManager(SrcManager), Consumer(Consumer), BufferID(BufferID) { }

  bool walkToSubStructurePre(SyntaxStructureNode Node) override {
    unsigned StartOffset = SrcManager.getLocOffsetInBuffer(Node.Range.getStart(),
                                                           BufferID);
    unsigned EndOffset = SrcManager.getLocOffsetInBuffer(Node.Range.getEnd(),
                                                         BufferID);
    unsigned NameStart;
    unsigned NameEnd;
    if (Node.NameRange.isValid()) {
      NameStart = SrcManager.getLocOffsetInBuffer(Node.NameRange.getStart(),
                                                  BufferID);
      NameEnd = SrcManager.getLocOffsetInBuffer(Node.NameRange.getEnd(),
                                                BufferID);
    }
    else {
      NameStart = NameEnd = 0;
    }

    unsigned BodyOffset;
    unsigned BodyEnd;
    if (Node.BodyRange.isValid()) {
      BodyOffset = SrcManager.getLocOffsetInBuffer(Node.BodyRange.getStart(),
                                                   BufferID);
      BodyEnd = SrcManager.getLocOffsetInBuffer(Node.BodyRange.getEnd(),
                                                BufferID);
    }
    else {
      BodyOffset = BodyEnd = 0;
    }

    unsigned DocOffset = 0;
    unsigned DocEnd = 0;
    if (Node.DocRange.isValid()) {
      DocOffset = SrcManager.getLocOffsetInBuffer(Node.DocRange.getStart(),
                                                  BufferID);
      DocEnd = SrcManager.getLocOffsetInBuffer(Node.DocRange.getEnd(),
                                               BufferID);
    }

    UIdent Kind = SwiftLangSupport::getUIDForSyntaxStructureKind(Node.Kind);
    UIdent AccessLevel;
    UIdent SetterAccessLevel;
    if (Node.Kind != SyntaxStructureKind::Parameter &&
        Node.Kind != SyntaxStructureKind::LocalVariable &&
        Node.Kind != SyntaxStructureKind::GenericTypeParam) {
      if (auto *VD = dyn_cast_or_null<ValueDecl>(Node.Dcl)) {
        if (auto Access = inferAccessSyntactically(VD))
          AccessLevel = getAccessLevelUID(Access.value());
      } else if (auto *ED = dyn_cast_or_null<ExtensionDecl>(Node.Dcl)) {
        if (auto DefaultAccess = inferDefaultAccessSyntactically(ED))
          AccessLevel = getAccessLevelUID(DefaultAccess.value());
      }
      if (auto *ASD = dyn_cast_or_null<AbstractStorageDecl>(Node.Dcl)) {
        if (auto SetAccess = inferSetterAccessSyntactically(ASD))
          SetterAccessLevel = getAccessLevelUID(SetAccess.value());
      }
    }

    SmallVector<StringRef, 4> InheritedNames;
    if (!Node.InheritedTypeRanges.empty()) {
      for (auto &TR : Node.InheritedTypeRanges) {
        InheritedNames.push_back(SrcManager.extractText(TR));
      }
    }

    StringRef TypeName;
    if (Node.TypeRange.isValid()) {
      TypeName = SrcManager.extractText(Node.TypeRange);
    }

    SmallString<64> DisplayNameBuf;
    StringRef DisplayName;
    if (auto ValueD = dyn_cast_or_null<ValueDecl>(Node.Dcl)) {
      llvm::raw_svector_ostream OS(DisplayNameBuf);
      if (!SwiftLangSupport::printDisplayName(ValueD, OS))
        DisplayName = OS.str();
    }
    else if (Node.NameRange.isValid()) {
      DisplayName = SrcManager.extractText(Node.NameRange);
    }

    SmallString<64> RuntimeNameBuf;
    StringRef RuntimeName = getObjCRuntimeName(Node.Dcl, RuntimeNameBuf);

    SmallString<64> SelectorNameBuf;
    StringRef SelectorName = getObjCSelectorName(Node.Dcl, SelectorNameBuf);

    std::vector<std::tuple<UIdent, unsigned, unsigned>> Attrs;

    for (auto Attr : Node.Attrs) {
      if (auto AttrUID = SwiftLangSupport::getUIDForDeclAttribute(Attr)) {
        unsigned AttrOffset = 0;
        unsigned AttrEnd = 0;
        auto AttrRange = Attr->getRangeWithAt();
        if (AttrRange.isValid()) {
          auto CharRange = Lexer::getCharSourceRangeFromSourceRange(SrcManager,
                                                                    AttrRange);
          AttrOffset = SrcManager.getLocOffsetInBuffer(CharRange.getStart(),
                                                       BufferID);
          AttrEnd = SrcManager.getLocOffsetInBuffer(CharRange.getEnd(),
                                                    BufferID);
        }

        auto AttrTuple = std::make_tuple(AttrUID.value(), AttrOffset,
                                         AttrEnd - AttrOffset);
        Attrs.push_back(AttrTuple);
      }
    }

    Consumer.beginDocumentSubStructure(StartOffset, EndOffset - StartOffset,
                                       Kind, AccessLevel, SetterAccessLevel,
                                       NameStart, NameEnd - NameStart,
                                       BodyOffset, BodyEnd - BodyOffset,
                                       DocOffset, DocEnd - DocOffset,
                                       DisplayName,
                                       TypeName, RuntimeName,
                                       SelectorName,
                                       InheritedNames, Attrs);

    for (const auto &Elem : Node.Elements) {
      if (Elem.Range.isInvalid())
        continue;

      UIdent Kind = SwiftLangSupport::getUIDForSyntaxStructureElementKind(Elem.Kind);
      unsigned Offset = SrcManager.getLocOffsetInBuffer(Elem.Range.getStart(),
                                                        BufferID);
      unsigned Length = Elem.Range.getByteLength();
      Consumer.handleDocumentSubStructureElement(Kind, Offset, Length);
    }

    return true;
  }

  StringRef getObjCRuntimeName(const Decl *D, SmallString<64> &Buf) {
    // We only report runtime name for classes and protocols with an explicitly
    // defined ObjC name, i.e. those that have @objc("SomeName")
    if (D && (isa<ClassDecl>(D) || isa<ProtocolDecl>(D))) {
      auto *ObjCNameAttr = D->getAttrs().getAttribute<ObjCAttr>();
      if (ObjCNameAttr && ObjCNameAttr->hasName())
        return ObjCNameAttr->getName()->getString(Buf);
    }
    return StringRef();
  }

  StringRef getObjCSelectorName(const Decl *D, SmallString<64> &Buf) {
    // We only vend the selector name for @IBAction and @IBSegueAction methods.
    if (auto FuncD = dyn_cast_or_null<FuncDecl>(D)) {
      if (FuncD->getAttrs().hasAttribute<IBActionAttr>() ||
          FuncD->getAttrs().hasAttribute<IBSegueActionAttr>()) {
        return FuncD->getObjCSelector(DeclName(), /*skipIsObjCResolution*/true)
          .getString(Buf);
      }
    }
    return StringRef();
  }

  bool walkToSubStructurePost(SyntaxStructureNode Node) override {
    Consumer.endDocumentSubStructure();
    return true;
  }

  bool walkToNodePre(SyntaxNode Node) override {
    if (Node.Kind != SyntaxNodeKind::CommentMarker)
      return false;

    unsigned StartOffset = SrcManager.getLocOffsetInBuffer(Node.Range.getStart(),
                                                           BufferID);
    unsigned EndOffset = SrcManager.getLocOffsetInBuffer(Node.Range.getEnd(),
                                                         BufferID);
    UIdent Kind = SwiftLangSupport::getUIDForSyntaxNodeKind(Node.Kind);
    Consumer.beginDocumentSubStructure(StartOffset, EndOffset - StartOffset,
                                       Kind, UIdent(), UIdent(), 0, 0,
                                       0, 0, 0, 0,
                                       StringRef(),
                                       StringRef(), StringRef(),
                                       StringRef(),
                                       {}, {});
    return true;
  }

  bool walkToNodePost(SyntaxNode Node) override {
    if (Node.Kind != SyntaxNodeKind::CommentMarker)
      return true;

    Consumer.endDocumentSubStructure();
    return true;
  }
};

/// Walks the syntax model to populate a given SwiftSyntaxMap with the token
/// ranges to highlight and pass document structure information to the given
/// EditorConsumer.
class SwiftEditorSyntaxWalker: public ide::SyntaxModelWalker {
  /// The syntax map to populate
  SwiftSyntaxMap &SyntaxMap;
  SourceManager &SrcManager;
  unsigned BufferID;
  SwiftDocumentStructureWalker DocStructureWalker;
  /// The current token nesting level (e.g. for a field in a doc comment)
  unsigned NestingLevel = 0;
public:
  SwiftEditorSyntaxWalker(SwiftSyntaxMap &SyntaxMap,
                          SourceManager &SrcManager, EditorConsumer &Consumer,
                          unsigned BufferID)
    : SyntaxMap(SyntaxMap), SrcManager(SrcManager), BufferID(BufferID),
      DocStructureWalker(SrcManager, BufferID, Consumer) { }

  bool walkToNodePre(SyntaxNode Node) override {
    if (Node.Kind == SyntaxNodeKind::CommentMarker)
      return DocStructureWalker.walkToNodePre(Node);
    ++NestingLevel;

    auto End = SrcManager.getLocOffsetInBuffer(Node.Range.getEnd(), BufferID),
      Start = SrcManager.getLocOffsetInBuffer(Node.Range.getStart(), BufferID);

    if (NestingLevel > 1) {
      // We're nested inside the previously reported token - merge
      SyntaxMap.mergeToken({Start, End - Start, Node.Kind});
    } else {
      // We're a top-level token, add it after the previous one
      SyntaxMap.addToken({Start, End - Start, Node.Kind});
    }

    return true;
  }

  bool walkToNodePost(SyntaxNode Node) override {
    if (Node.Kind == SyntaxNodeKind::CommentMarker)
      return DocStructureWalker.walkToNodePost(Node);
    --NestingLevel;

    return true;
  }

  bool walkToSubStructurePre(SyntaxStructureNode Node) override {
    return DocStructureWalker.walkToSubStructurePre(Node);
  }

  bool walkToSubStructurePost(SyntaxStructureNode Node) override {
    return DocStructureWalker.walkToSubStructurePost(Node);
  }

};

class PlaceholderExpansionScanner {

public:
  struct Param {
    CharSourceRange NameRange;
    CharSourceRange TypeRange;
    Param(CharSourceRange NameRange, CharSourceRange TypeRange)
      :NameRange(NameRange), TypeRange(TypeRange) { }
  };

  struct ClosureInfo {
    std::vector<Param> Params;
    CharSourceRange ReturnTypeRange;
  };

private:
  SourceManager &SM;

  class PlaceholderFinder: public ASTWalker {
    SourceLoc PlaceholderLoc;
    EditorPlaceholderExpr *&Found;

  public:
    PlaceholderFinder(SourceLoc PlaceholderLoc,
                      EditorPlaceholderExpr *&Found)
    : PlaceholderLoc(PlaceholderLoc), Found(Found) {
    }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
      if (isa<EditorPlaceholderExpr>(E) && E->getStartLoc() == PlaceholderLoc) {
        Found = cast<EditorPlaceholderExpr>(E);
        return Action::Stop();
      }
      return Action::Continue(E);
    }

    PreWalkAction walkToDeclPre(Decl *D) override {
      if (auto *ICD = dyn_cast<IfConfigDecl>(D)) {
        // The base walker assumes the content of active IfConfigDecl clauses
        // has been injected into the parent context and will be walked there.
        // This doesn't hold for pre-typechecked ASTs and we need to find
        // placeholders in inactive clauses anyway, so walk them here.
        for (auto Clause: ICD->getClauses()) {
          for (auto Elem: Clause.Elements) {
            Elem.walk(*this);
          }
        }
        return Action::SkipChildren();
      }
      return Action::Continue();
    }
  };

  class ClosureTypeWalker: public ASTWalker {
    SourceManager &SM;
    ClosureInfo &Info;
  public:
    bool FoundFunctionTypeRepr = false;
    explicit ClosureTypeWalker(SourceManager &SM, ClosureInfo &Info) : SM(SM),
      Info(Info) { }

    MacroWalking getMacroWalkingBehavior() const override {
      return MacroWalking::Arguments;
    }

    PreWalkAction walkToTypeReprPre(TypeRepr *T) override {
      if (auto *FTR = dyn_cast<FunctionTypeRepr>(T)) {
        FoundFunctionTypeRepr = true;
        for (auto &ArgElt : FTR->getArgsTypeRepr()->getElements()) {
          CharSourceRange NR;
          CharSourceRange TR;
          auto name = ArgElt.Name;
          if (!name.empty()) {
            NR = CharSourceRange(ArgElt.NameLoc,
                                 name.getLength());
          }
          SourceLoc SRE = Lexer::getLocForEndOfToken(SM,
                                                  ArgElt.Type->getEndLoc());
          TR = CharSourceRange(SM, ArgElt.Type->getStartLoc(), SRE);
          Info.Params.emplace_back(NR, TR);
        }
        if (auto *RTR = FTR->getResultTypeRepr()) {
          SourceLoc SRE = Lexer::getLocForEndOfToken(SM, RTR->getEndLoc());
          Info.ReturnTypeRange = CharSourceRange(SM, RTR->getStartLoc(), SRE);
        }
      }
      return Action::StopIf(FoundFunctionTypeRepr);
    }
  };

  bool containClosure(Expr *E) {
    if (E->getStartLoc().isInvalid())
      return false;
    EditorPlaceholderExpr *Found = nullptr;
    ClosureInfo Info;
    ClosureTypeWalker ClosureWalker(SM, Info);
    PlaceholderFinder Finder(E->getStartLoc(), Found);
    E->walk(Finder);
    if (Found) {
      if (auto TR = Found->getPlaceholderTypeRepr()) {
        TR->walk(ClosureWalker);
        return ClosureWalker.FoundFunctionTypeRepr;
      }
    }
    E->walk(ClosureWalker);
    return ClosureWalker.FoundFunctionTypeRepr;
  }

  bool scanClosureType(EditorPlaceholderExpr *PHE,
                       ClosureInfo &TargetClosureInfo) {
    TargetClosureInfo.Params.clear();
    TargetClosureInfo.ReturnTypeRange = CharSourceRange();
    if (!PHE->getTypeForExpansion())
      return false;
    ClosureTypeWalker PW(SM, TargetClosureInfo);
    PHE->getTypeForExpansion()->walk(PW);
    return PW.FoundFunctionTypeRepr;
  }

  /// Finds the enclosing CallExpr, and indicates whether it should be further
  /// considered a candidate for application of trailing closure.
  /// For example, if the CallExpr is enclosed in another expression or statement
  /// such as "outer(inner(<#closure#>))", or "if inner(<#closure#>)", then trailing
  /// closure should not be applied to the inner call.
  std::pair<ArgumentList *, bool> enclosingCallExprArg(SourceFile &SF,
                                                       SourceLoc SL) {

    class CallExprFinder : public SourceEntityWalker {
    public:
      const SourceManager &SM;
      SourceLoc TargetLoc;
      std::pair<Expr *, ArgumentList *> EnclosingCallAndArg;
      Expr *OuterExpr;
      Stmt *OuterStmt;
      explicit CallExprFinder(const SourceManager &SM)
        :SM(SM) { }

      bool checkCallExpr(Expr *E) {
        ArgumentList *Args = nullptr;
        if (auto *CE = dyn_cast<CallExpr>(E)) {
          // Call expression can have argument.
          Args = CE->getArgs();
        }
        if (!Args)
          return false;
        if (EnclosingCallAndArg.first)
          OuterExpr = EnclosingCallAndArg.first;
        EnclosingCallAndArg = {E, Args};
        return true;
      }

      bool walkToExprPre(Expr *E) override {
        auto SR = E->getSourceRange();
        if (SR.isValid() && SM.rangeContainsTokenLoc(SR, TargetLoc) &&
            !checkCallExpr(E) && !EnclosingCallAndArg.first) {
          if (!isa<TryExpr>(E) && !isa<AwaitExpr>(E) &&
              !isa<PrefixUnaryExpr>(E)) {
            // We don't want to expand to trailing closures if the call is
            // nested inside another expression that has closing characters,
            // like a `)` for a function call. This is not the case for
            // `try`, `await` and prefix operator applications.
            OuterExpr = E;
          }
        }
        return true;
      }

      bool walkToExprPost(Expr *E) override {
        if (E->getStartLoc() == TargetLoc)
          return false; // found what we needed to find, stop walking.
        return true;
      }

      /// Whether this statement body consists of only an implicit "return",
      /// possibly within braces.
      bool isImplicitReturnBody(Stmt *S) {
        if (auto RS = dyn_cast<ReturnStmt>(S))
          return RS->isImplicit() && RS->getSourceRange().Start == TargetLoc;

        if (auto BS = dyn_cast<BraceStmt>(S)) {
          if (BS->getNumElements() == 1) {
            if (auto innerS = BS->getFirstElement().dyn_cast<Stmt *>())
              return isImplicitReturnBody(innerS);
          }
        }

        return false;
      }

      bool walkToStmtPre(Stmt *S) override {
        auto SR = S->getSourceRange();
        if (SR.isValid() && SM.rangeContainsTokenLoc(SR, TargetLoc) &&
            !isImplicitReturnBody(S)) {
          // A statement inside an expression - e.g. `foo({ if ... })` - resets
          // the enclosing context.
          //
          // ... unless it's an implicit return.
          OuterExpr = nullptr;
          EnclosingCallAndArg = {nullptr, nullptr};

          switch (S->getKind()) {
          case StmtKind::Brace:
          case StmtKind::Return:
          case StmtKind::Yield:
          case StmtKind::Throw:
            // A trailing closure is allowed in these statements.
            OuterStmt = nullptr;
            break;
          default:
            OuterStmt = S;
            break;
          }
        }
        return true;
      }

      bool shouldWalkInactiveConfigRegion() override { return true; }

      ArgumentList *findEnclosingCallArg(SourceFile &SF, SourceLoc SL) {
        EnclosingCallAndArg = {nullptr, nullptr};
        OuterExpr = nullptr;
        OuterStmt = nullptr;
        TargetLoc = SL;
        walk(SF);
        return EnclosingCallAndArg.second;
      }
    };

    CallExprFinder CEFinder(SM);
    auto *Args = CEFinder.findEnclosingCallArg(SF, SL);

    if (!Args)
      return std::make_pair(Args, false);
    if (CEFinder.OuterExpr)
      return std::make_pair(Args, false);
    if (CEFinder.OuterStmt)
      return std::make_pair(Args, false);

    return std::make_pair(Args, true);
  }

  struct ParamClosureInfo {
    Optional<ClosureInfo> placeholderClosure;
    bool isNonPlaceholderClosure = false;
    bool isWrappedWithBraces = false;
  };

  /// Scan the given ArgumentList collecting argument closure information and
  /// returning the index of the given target placeholder (if found).
  Optional<unsigned>
  scanArgumentList(ArgumentList *Args, SourceLoc targetPlaceholderLoc,
                   std::vector<ParamClosureInfo> &outParams) {
    if (Args->empty())
      return llvm::None;

    outParams.clear();
    outParams.reserve(Args->size());

    Optional<unsigned> targetPlaceholderIndex;

    for (auto Arg : *Args) {
      auto *E = Arg.getExpr();
      outParams.emplace_back();
      auto &outParam = outParams.back();

      if (auto CE = dyn_cast<ClosureExpr>(E)) {
        if (CE->hasSingleExpressionBody() &&
            CE->getSingleExpressionBody()->getStartLoc() ==
                targetPlaceholderLoc) {
          targetPlaceholderIndex = outParams.size() - 1;
          if (auto *PHE = dyn_cast<EditorPlaceholderExpr>(
                  CE->getSingleExpressionBody())) {
            outParam.isWrappedWithBraces = true;
            ClosureInfo info;
            if (scanClosureType(PHE, info))
              outParam.placeholderClosure = info;
            continue;
          }
        }
        // else...
        outParam.isNonPlaceholderClosure = true;
        continue;
      }

      if (auto *PHE = dyn_cast<EditorPlaceholderExpr>(E)) {
        ClosureInfo info;
        if (scanClosureType(PHE, info))
          outParam.placeholderClosure = info;
      } else if (containClosure(E)) {
        outParam.isNonPlaceholderClosure = true;
      }

      if (E->getStartLoc() == targetPlaceholderLoc) {
        targetPlaceholderIndex = outParams.size() - 1;
      }
    }

    return targetPlaceholderIndex;
  }

public:
  explicit PlaceholderExpansionScanner(SourceManager &SM) : SM(SM) { }

  /// Retrieves the parameter list, return type and context info for
  /// a typed completion placeholder in a function call.
  /// For example: foo.bar(aaa, <#T##(Int, Int) -> Bool#>).
  bool scan(SourceFile &SF, unsigned BufID, unsigned Offset, unsigned Length,
            std::function<void(ArgumentList *Args, bool UseTrailingClosure,
                               bool isWrappedWithBraces, const ClosureInfo &)>
                OneClosureCallback,
            std::function<void(ArgumentList *Args, unsigned FirstTrailingIndex,
                               ArrayRef<ClosureInfo> trailingClosures)>
                MultiClosureCallback,
            std::function<bool(EditorPlaceholderExpr *)> NonClosureCallback) {
    SourceLoc PlaceholderStartLoc = SM.getLocForOffset(BufID, Offset);

    // See if the placeholder is encapsulated with an EditorPlaceholderExpr
    EditorPlaceholderExpr *PHE = nullptr;
    PlaceholderFinder Finder(PlaceholderStartLoc, PHE);
    SF.walk(Finder);
    if (!PHE)
      return NonClosureCallback(PHE);

    // Retrieve parameter and return type ranges.
    ClosureInfo TargetClosureInfo;
    if (!scanClosureType(PHE, TargetClosureInfo))
      return NonClosureCallback(PHE);

    // Now we need to see if we can suggest trailing closure expansion,
    // and if the call parens can be removed in that case.
    // We'll first find the enclosing CallExpr, and then do further analysis.
    std::vector<ParamClosureInfo> params;
    Optional<unsigned> targetPlaceholderIndex;
    auto ECE = enclosingCallExprArg(SF, PlaceholderStartLoc);
    ArgumentList *Args = ECE.first;
    if (Args && ECE.second) {
      targetPlaceholderIndex =
          scanArgumentList(Args, PlaceholderStartLoc, params);
    }

    // If there was no appropriate parent call expression, it's non-trailing.
    if (!targetPlaceholderIndex.has_value()) {
      OneClosureCallback(Args, /*useTrailingClosure=*/false,
                         /*isWrappedWithBraces=*/false, TargetClosureInfo);
      return true;
    }

    const unsigned end = params.size();
    unsigned firstTrailingIndex = end;

    // Find the first parameter eligible to be trailing.
    while (firstTrailingIndex != 0) {
      unsigned i = firstTrailingIndex - 1;
      if (params[i].isNonPlaceholderClosure ||
          !params[i].placeholderClosure.has_value())
        break;
      firstTrailingIndex = i;
    }

    if (firstTrailingIndex > targetPlaceholderIndex) {
      // Target comes before the eligible trailing closures.
      OneClosureCallback(Args, /*isTrailing=*/false,
                         params[*targetPlaceholderIndex].isWrappedWithBraces,
                         TargetClosureInfo);
      return true;
    } else if (targetPlaceholderIndex == end - 1 &&
               firstTrailingIndex == end - 1) {
      // Target is the only eligible trailing closure.
      OneClosureCallback(Args, /*isTrailing=*/true,
                         params[*targetPlaceholderIndex].isWrappedWithBraces,
                         TargetClosureInfo);
      return true;
    }

    // There are multiple trailing closures.
    SmallVector<ClosureInfo, 4> trailingClosures;
    trailingClosures.reserve(params.size() - firstTrailingIndex);
    for (const auto &param :
         llvm::makeArrayRef(params).slice(firstTrailingIndex)) {
      trailingClosures.push_back(*param.placeholderClosure);
    }
    MultiClosureCallback(Args, firstTrailingIndex, trailingClosures);
    return true;
  }
};

} // anonymous namespace

SwiftEditorDocument::SwiftEditorDocument(
    StringRef FilePath, SwiftLangSupport &LangSupport,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem,
    CodeFormatOptions Options)
  :Impl(*new Implementation(FilePath, LangSupport, Options, fileSystem)) { }

SwiftEditorDocument::~SwiftEditorDocument()
{
  delete &Impl;
}

ImmutableTextSnapshotRef SwiftEditorDocument::initializeText(
    llvm::MemoryBuffer *Buf, ArrayRef<const char *> Args,
    bool ProvideSemanticInfo,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem) {
  llvm::sys::ScopedLock L(Impl.AccessMtx);

  Impl.Edited = false;
  Impl.EditableBuffer =
      new EditableTextBuffer(Impl.FilePath, Buf->getBuffer());

  // Reset the syntax map data and affected range
  Impl.SyntaxMap.Tokens.clear();
  Impl.AffectedRange = {0, static_cast<unsigned>(Buf->getBufferSize())};

  // Try to create a compiler invocation object if needing semantic info
  // or it's syntactic-only but with passed-in compiler arguments.
  if (ProvideSemanticInfo || !Args.empty()) {
    Impl.SemanticInfo = new SwiftDocumentSemanticInfo(
        Impl.FilePath, Impl.ASTMgr, Impl.NotificationCtr, fileSystem);
    Impl.SemanticInfo->setCompilerArgs(Args);
  }
  return Impl.EditableBuffer->getSnapshot();
}

static void updateSemaInfo(RefPtr<SwiftDocumentSemanticInfo> SemanticInfo,
                           EditableTextBufferRef EditableBuffer,
                           SourceKitCancellationToken CancellationToken) {
  if (SemanticInfo) {
    SemanticInfo->processLatestSnapshotAsync(EditableBuffer, CancellationToken);
  }
}

ImmutableTextSnapshotRef SwiftEditorDocument::replaceText(
    unsigned Offset, unsigned Length, llvm::MemoryBuffer *Buf,
    bool ProvideSemanticInfo, std::string &error) {

  ImmutableTextSnapshotRef Snapshot;
  EditableTextBufferRef EditableBuffer;
  RefPtr<SwiftDocumentSemanticInfo> SemanticInfo;
  {
    llvm::sys::ScopedLock L(Impl.AccessMtx);

    EditableBuffer = Impl.EditableBuffer;
    SemanticInfo = Impl.SemanticInfo;

    // Validate offset and length.
    if ((Offset + Length) > EditableBuffer->getSize()) {
      error = "'offset' + 'length' is out of range";
      return nullptr;
    }

    Impl.Edited = true;
    llvm::StringRef Str = Buf->getBuffer();

    // Update the buffer itself
    Snapshot = EditableBuffer->replace(Offset, Length, Str);

    // Update the old syntax map offsets to account for the replaced range.
    // Also set the initial AffectedRange to cover any tokens that
    // the replaced range intersected. This allows for clients that split
    // multi-line tokens at line boundaries, and ensure all parts of these tokens
    // will be cleared.
    Impl.AffectedRange =
        Impl.SyntaxMap.adjustForReplacement(Offset, Length, Str.size());

    // We need to release `AccessMtx` before calling into the ASTManager, since
    // it may call back to the editor for document state.
  }

  if (ProvideSemanticInfo) {
    // If this is not a no-op, update semantic info.
    if (Length != 0 || Buf->getBufferSize() != 0) {
      // We implicitly send the semantic info as a notification after the edit.
      // The client thus doesn't have a handle to cancel it.
      ::updateSemaInfo(SemanticInfo, EditableBuffer,
                       /*CancellationToken=*/nullptr);

      // FIXME: we should also update any "interesting" ASTs that depend on this
      // document here, e.g. any ASTs for files visible in an editor. However,
      // because our API conflates this with any file with unsaved changes we do
      // not update all open documents, since there could be too many of them.
    }
  }

  return Snapshot;
}

void SwiftEditorDocument::updateSemaInfo(
    SourceKitCancellationToken CancellationToken) {
  Impl.AccessMtx.lock();
  auto EditableBuffer = Impl.EditableBuffer;
  auto SemanticInfo = Impl.SemanticInfo;
  // We need to release `AccessMtx` before calling into the ASTManager, since it
  // may call back to the editor for document state.
  Impl.AccessMtx.unlock();

  ::updateSemaInfo(SemanticInfo, EditableBuffer, CancellationToken);
}

void SwiftEditorDocument::resetSyntaxInfo(ImmutableTextSnapshotRef Snapshot,
                                          SwiftLangSupport &Lang) {
  llvm::sys::ScopedLock L(Impl.AccessMtx);

  assert(Impl.SemanticInfo && "Impl.SemanticInfo must be set");

  std::vector<std::string> Args;
  std::string PrimaryFile; // Ignored, Impl.FilePath will be used

  CompilerInvocation CompInv;
  if (Impl.SemanticInfo->getInvocation()) {
    Impl.SemanticInfo->getInvocation()->applyTo(CompInv);
    Impl.SemanticInfo->getInvocation()->raw(Args, PrimaryFile);
  } else {
    // Use stdin as a .swift input to satisfy the driver. Note that we don't
    // use Impl.FilePath here because it may be invalid filename for driver
    // like "" or "-foobar".
    SmallVector<const char *, 1> Args;
    Args.push_back("-");
    std::string Error;
    // Ignore possible error(s)
    Lang.getASTManager()->initCompilerInvocation(
        CompInv, Args, FrontendOptions::ActionType::Parse, StringRef(), Error);
  }
  // When reuse parts of the syntax tree from a SyntaxParsingCache, not
  // all tokens are visited and thus token collection is invalid
  CompInv.getLangOptions().CollectParsedToken = true;
  // Access to Impl.SyntaxInfo is guarded by Impl.AccessMtx
  Impl.SyntaxInfo.reset(
    new SwiftDocumentSyntaxInfo(CompInv, Snapshot, Args, Impl.FilePath));
}

static UIdent SemaDiagStage("source.diagnostic.stage.swift.sema");
static UIdent ParseDiagStage("source.diagnostic.stage.swift.parse");

void SwiftEditorDocument::readSyntaxInfo(EditorConsumer &Consumer, bool ReportDiags) {
  llvm::sys::ScopedLock L(Impl.AccessMtx);
  Impl.SyntaxInfo->parseIfNeeded();

  Impl.ParserDiagnostics = Impl.SyntaxInfo->getDiagnostics();
  if (ReportDiags) {
    Consumer.handleDiagnostics(Impl.ParserDiagnostics, ParseDiagStage);
  }

  SwiftSyntaxMap NewMap = SwiftSyntaxMap(Impl.SyntaxMap.Tokens.size() + 16);

  if (Consumer.documentStructureEnabled() || Consumer.syntaxMapEnabled()) {
    ide::SyntaxModelContext ModelContext(Impl.SyntaxInfo->getSourceFile());

    SwiftEditorSyntaxWalker SyntaxWalker(
        NewMap, Impl.SyntaxInfo->getSourceManager(), Consumer,
        Impl.SyntaxInfo->getBufferID());
    ModelContext.walk(SyntaxWalker);

    bool SawChanges = true;
    if (Impl.Edited) {
      // We're answering an edit request. Report all highlighted token ranges not
      // in the previous syntax map to the Consumer and extend the AffectedRange
      // to contain all added/removed token ranges.
      SawChanges =
          NewMap.forEachChanged(Impl.SyntaxMap, Impl.AffectedRange, Consumer);
    } else {
      // The is an open/initialise. Report all highlighted token ranges to the
      // Consumer.
      NewMap.forEach(Consumer);
    }
    Impl.SyntaxMap = std::move(NewMap);

    // Recording an affected length of 0 still results in the client updating
    // its copy of the syntax map (by clearning all tokens on the line of the
    // affected offset). We need to not record it at all to signal a no-op.
    if (SawChanges)
      Consumer.recordAffectedRange(Impl.AffectedRange->Offset,
                                   Impl.AffectedRange->length());
  }
}

void SwiftEditorDocument::readSemanticInfo(ImmutableTextSnapshotRef Snapshot,
                                           EditorConsumer& Consumer) {
  llvm::sys::ScopedLock L(Impl.AccessMtx);
  std::vector<SwiftSemanticToken> SemaToks;
  Optional<std::vector<DiagnosticEntryInfo>> SemaDiags;
  Impl.SemanticInfo->readSemanticInfo(Snapshot, SemaToks, SemaDiags,
                                      Impl.ParserDiagnostics);

  for (auto SemaTok : SemaToks) {
    unsigned Offset = SemaTok.ByteOffset;
    unsigned Length = SemaTok.Length;
    UIdent Kind = SemaTok.getUIdentForKind();
    bool IsSystem = SemaTok.getIsSystem();
    if (Kind.isValid())
      Consumer.handleSemanticAnnotation(Offset, Length, Kind, IsSystem);
  }

  // If there's no value returned for diagnostics it means they are out-of-date
  // (based on a different snapshot).
  if (SemaDiags) {
    Consumer.handleDiagnostics(*SemaDiags, SemaDiagStage);
  } else {
    Consumer.handleDiagnostics(Impl.ParserDiagnostics, ParseDiagStage);
  }
}

void SwiftEditorDocument::removeCachedAST() {
  Impl.SemanticInfo->removeCachedAST();
}

void SwiftEditorDocument::applyFormatOptions(OptionsDictionary &FmtOptions) {
  static UIdent KeyUseTabs("key.editor.format.usetabs");
  static UIdent KeyIndentWidth("key.editor.format.indentwidth");
  static UIdent KeyTabWidth("key.editor.format.tabwidth");
  static UIdent KeyIndentSwitchCase("key.editor.format.indent_switch_case");

  FmtOptions.valueForOption(KeyUseTabs, Impl.FormatOptions.UseTabs);
  FmtOptions.valueForOption(KeyIndentWidth, Impl.FormatOptions.IndentWidth);
  FmtOptions.valueForOption(KeyTabWidth, Impl.FormatOptions.TabWidth);
  FmtOptions.valueForOption(KeyIndentSwitchCase, Impl.FormatOptions.IndentSwitchCase);
}

const CodeFormatOptions &SwiftEditorDocument::getFormatOptions() {
  return Impl.FormatOptions;
}

std::string SwiftEditorDocument::getFilePath() const { return Impl.FilePath; }

llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>
SwiftEditorDocument::getFileSystem() const {
  llvm::sys::ScopedLock L(Impl.AccessMtx);
  return Impl.SemanticInfo ? Impl.SemanticInfo->getFileSystem()
                           : llvm::vfs::getRealFileSystem();
}

void SwiftEditorDocument::formatText(unsigned Line, unsigned Length,
                                     EditorConsumer &Consumer) {
  auto SyntaxInfo = Impl.getSyntaxInfo();
  SourceFile &SF = SyntaxInfo->getSourceFile();
  SourceManager &SM = SyntaxInfo->getSourceManager();

  LineRange inputRange = LineRange(Line, Length);
  CodeFormatOptions Options = getFormatOptions();
  auto indented = reformat(inputRange, Options, SM, SF);

  LineRange LineRange = indented.first;
  StringRef ModifiedText = indented.second;
  Consumer.recordFormattedText(ModifiedText);
  Consumer.recordAffectedLineRange(LineRange.startLine(), LineRange.lineCount());
}

static void
printClosureBody(const PlaceholderExpansionScanner::ClosureInfo &closure,
                 llvm::raw_ostream &OS, const SourceManager &SM) {
  bool FirstParam = true;
  for (auto &Param : closure.Params) {
    if (!FirstParam)
      OS << ", ";
    FirstParam = false;
    if (Param.NameRange.isValid()) {
      // If we have a parameter name, just output the name as is and skip
      // the type. For example:
      // <#(arg1: Int, arg2: Int)#> turns into '{ arg1, arg2 in'.
      OS << SM.extractText(Param.NameRange);
    } else {
      // If we only have the parameter type, output the type as a
      // placeholder. For example:
      // <#(Int, Int)#> turns into '{ <#Int#>, <#Int#> in'.
      OS << "<#";
      OS << SM.extractText(Param.TypeRange);
      OS << "#>";
    }
  }
  if (!FirstParam)
    OS << " in";
  OS << "\n" << getCodePlaceholder() << "\n";
}

void SwiftEditorDocument::expandPlaceholder(unsigned Offset, unsigned Length,
                                            EditorConsumer &Consumer) {
  auto SyntaxInfo = Impl.getSyntaxInfo();
  SourceManager &SM = SyntaxInfo->getSourceManager();
  unsigned BufID = SyntaxInfo->getBufferID();

  const unsigned PlaceholderStartLen = 2;
  const unsigned PlaceholderEndLen = 2;

  if (Length < (PlaceholderStartLen + PlaceholderEndLen)) {
    Consumer.handleRequestError("Invalid Length parameter");
    return;
  }

  PlaceholderExpansionScanner Scanner(SM);
  SourceFile &SF = SyntaxInfo->getSourceFile();

  Scanner.scan(SF, BufID, Offset, Length,
          [&](ArgumentList *Args,
              bool UseTrailingClosure, bool isWrappedWithBraces,
              const PlaceholderExpansionScanner::ClosureInfo &closure) {

      unsigned EffectiveOffset = Offset;
      unsigned EffectiveLength = Length;
      llvm::SmallString<128> ExpansionStr;
      {
        llvm::raw_svector_ostream OS(ExpansionStr);
        if (UseTrailingClosure) {
          assert(Args);

          if (Args->isUnary()) {
            // There appears to be no other parameters in this call, so we'll
            // expand replacement for trailing closure and cover call parens.
            // For example:
            // foo.bar(<#closure#>) turns into foo.bar <#closure#>.
            EffectiveOffset = SM.getLocOffsetInBuffer(Args->getStartLoc(), BufID);
            OS << " ";
          } else {
            // Expand replacement range for trailing closure.
            // For example:
            // foo.bar(a, <#closure#>) turns into foo.bar(a) <#closure#>.

            // If the preceding token in the call is the leading parameter
            // separator, we'll expand replacement to cover that.
            auto *Arg = Args->getExpr(Args->size() - 2);
            auto BeforeLoc = Lexer::getLocForEndOfToken(SM, Arg->getEndLoc());
            EffectiveOffset = SM.getLocOffsetInBuffer(BeforeLoc, BufID);
            OS << ") ";
          }

          unsigned End = SM.getLocOffsetInBuffer(Args->getEndLoc(), BufID);
          EffectiveLength = (End + 1) - EffectiveOffset;
        }
        // Trailing closure syntax handling will replace braces anyway.
        bool printBraces = !isWrappedWithBraces || UseTrailingClosure;

        if (printBraces)
          OS << "{ ";
        printClosureBody(closure, OS, SM);
        if (printBraces)
          OS << "}";
      }
      Consumer.handleSourceText(ExpansionStr);
      Consumer.recordAffectedRange(EffectiveOffset, EffectiveLength);

    },[&](ArgumentList *args, unsigned firstTrailingIndex,
          ArrayRef<PlaceholderExpansionScanner::ClosureInfo> trailingClosures) {
      unsigned EffectiveOffset = Offset;
      unsigned EffectiveLength = Length;
      llvm::SmallString<128> ExpansionStr;
      {
        llvm::raw_svector_ostream OS(ExpansionStr);

        assert(args->size() - firstTrailingIndex == trailingClosures.size());
        if (firstTrailingIndex == 0) {
          // foo(<....>) -> foo { <...> }
          EffectiveOffset = SM.getLocOffsetInBuffer(args->getStartLoc(), BufID);
          OS << " ";
        } else {
          // foo(blah, <....>) -> foo(blah) { <...> }
          SourceLoc beforeTrailingLoc = Lexer::getLocForEndOfToken(SM,
              args->getExpr(firstTrailingIndex - 1)->getEndLoc());
          EffectiveOffset = SM.getLocOffsetInBuffer(beforeTrailingLoc, BufID);
          OS << ") ";
        }

        unsigned End = SM.getLocOffsetInBuffer(args->getEndLoc(), BufID);
        EffectiveLength = (End + 1) - EffectiveOffset;

        unsigned argI = firstTrailingIndex;
        for (unsigned i = 0; argI != args->size(); ++i, ++argI) {
          const auto &closure = trailingClosures[i];
          if (i == 0) {
            OS << "{ ";
          } else {
            auto label = args->getLabel(argI);
            OS << " " << (label.empty() ? "_" : label.str()) << ": { ";
          }
          printClosureBody(closure, OS, SM);
          OS << "}";
        }
        OS << "\n";
      }

      Consumer.handleSourceText(ExpansionStr);
      Consumer.recordAffectedRange(EffectiveOffset, EffectiveLength);

    }, [&](EditorPlaceholderExpr *PHE) {
      if (!PHE)
        return false;
      if (auto Ty = PHE->getTypeForExpansion()) {
        std::string S;
        llvm::raw_string_ostream OS(S);
        Ty->print(OS);
        Consumer.handleSourceText(OS.str());
        Consumer.recordAffectedRange(Offset, Length);
        return true;
      }
      return false;
    });
}

ImmutableTextSnapshotRef SwiftEditorDocument::getLatestSnapshot() const {
  llvm::sys::ScopedLock L(Impl.AccessMtx);
  return Impl.EditableBuffer->getSnapshot();
}

void SwiftEditorDocument::reportDocumentStructure(SourceFile &SrcFile,
                                                  EditorConsumer &Consumer) {
  ide::SyntaxModelContext ModelContext(SrcFile);
  SwiftDocumentStructureWalker Walker(SrcFile.getASTContext().SourceMgr,
                                      *SrcFile.getBufferID(),
                                      Consumer);
  ModelContext.walk(Walker);
}

//===----------------------------------------------------------------------===//
// EditorOpen
//===----------------------------------------------------------------------===//
void SwiftLangSupport::editorOpen(
    StringRef Name, llvm::MemoryBuffer *Buf, EditorConsumer &Consumer,
    ArrayRef<const char *> Args, Optional<VFSOptions> vfsOptions) {

  std::string error;
  // Do not provide primaryFile so that opening an existing document will
  // reinitialize the filesystem instead of keeping the old one.
  auto fileSystem = getFileSystem(vfsOptions, /*primaryFile=*/None, error);
  if (!fileSystem)
    return Consumer.handleRequestError(error.c_str());

  ImmutableTextSnapshotRef Snapshot = nullptr;
  auto EditorDoc = EditorDocuments->getByUnresolvedName(Name);
  if (!EditorDoc) {
    EditorDoc = new SwiftEditorDocument(Name, *this, fileSystem);
    Snapshot = EditorDoc->initializeText(
        Buf, Args, Consumer.needsSemanticInfo(), fileSystem);
    EditorDoc->resetSyntaxInfo(Snapshot, *this);
    if (EditorDocuments->getOrUpdate(Name, *this, EditorDoc)) {
      // Document already exists, re-initialize it. This should only happen
      // if we get OPEN request while the previous document is not closed.
      LOG_WARN_FUNC("Document already exists in editorOpen(..): " << Name);
      Snapshot = nullptr;
    }
    auto numOpen = ++Stats->numOpenDocs;
    Stats->maxOpenDocs.updateMax(numOpen);
  }

  if (!Snapshot) {
    Snapshot = EditorDoc->initializeText(
        Buf, Args, Consumer.needsSemanticInfo(), fileSystem);
    EditorDoc->resetSyntaxInfo(Snapshot, *this);
  }

  if (Consumer.needsSemanticInfo()) {
    // We implicitly send the semantic info as a notification after the
    // document is opened. The client thus doesn't have a handle to cancel it.
    EditorDoc->updateSemaInfo(/*CancellationToken=*/nullptr);
  }

  if (!Consumer.documentStructureEnabled() &&
      !Consumer.syntaxMapEnabled() &&
      !Consumer.diagnosticsEnabled()) {
    return;
  }

  EditorDoc->readSyntaxInfo(Consumer, /*ReportDiags=*/true);
}

//===----------------------------------------------------------------------===//
// EditorClose
//===----------------------------------------------------------------------===//

void SwiftLangSupport::editorClose(StringRef Name, bool RemoveCache) {
  auto Removed = EditorDocuments->remove(Name);
  if (Removed) {
    --Stats->numOpenDocs;
  } else {
    IFaceGenContexts.remove(Name);
  }

  if (Removed && RemoveCache)
    Removed->removeCachedAST();
  // FIXME: Report error if Name did not apply to anything ?
}


//===----------------------------------------------------------------------===//
// EditorReplaceText
//===----------------------------------------------------------------------===//

void SwiftLangSupport::editorReplaceText(StringRef Name,
                                         llvm::MemoryBuffer *Buf,
                                         unsigned Offset, unsigned Length,
                                         EditorConsumer &Consumer) {
  auto EditorDoc = EditorDocuments->getByUnresolvedName(Name);
  if (!EditorDoc) {
    Consumer.handleRequestError("No associated Editor Document");
    return;
  }

  ImmutableTextSnapshotRef Snapshot;
  if (Length != 0 || Buf->getBufferSize() != 0) {
    std::string error;
    Snapshot = EditorDoc->replaceText(Offset, Length, Buf,
                                      Consumer.needsSemanticInfo(), error);
    if (!Snapshot) {
      assert(error.size());
      Consumer.handleRequestError(error.c_str());
      return;
    }

    EditorDoc->resetSyntaxInfo(Snapshot, *this);

    // If client doesn't need any information, we doesn't need to parse it.
    if (!Consumer.documentStructureEnabled() &&
        !Consumer.syntaxMapEnabled() &&
        !Consumer.diagnosticsEnabled()) {
      return;
    }

    // Do not report syntactic diagnostics; will be handled in readSemanticInfo.
    EditorDoc->readSyntaxInfo(Consumer, /*ReportDiags=*/false);
  } else {
    Snapshot = EditorDoc->getLatestSnapshot();
  }

  EditorDoc->readSemanticInfo(Snapshot, Consumer);
}


//===----------------------------------------------------------------------===//
// EditorFormatText
//===----------------------------------------------------------------------===//
void SwiftLangSupport::editorApplyFormatOptions(StringRef Name,
                                                OptionsDictionary &FmtOptions) {
  auto EditorDoc = EditorDocuments->getByUnresolvedName(Name);
  if (EditorDoc)
    EditorDoc->applyFormatOptions(FmtOptions);
}

void SwiftLangSupport::editorFormatText(StringRef Name, unsigned Line,
                                        unsigned Length,
                                        EditorConsumer &Consumer) {
  auto EditorDoc = EditorDocuments->getByUnresolvedName(Name);
  if (!EditorDoc) {
    Consumer.handleRequestError("No associated Editor Document");
    return;
  }

  EditorDoc->formatText(Line, Length, Consumer);
}

void SwiftLangSupport::editorExtractTextFromComment(StringRef Source,
                                                    EditorConsumer &Consumer) {
  Consumer.handleSourceText(extractPlainTextFromComment(Source));
}

void SwiftLangSupport::editorConvertMarkupToXML(StringRef Source,
                                                EditorConsumer &Consumer) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  if (convertMarkupToXML(Source, OS)) {
    Consumer.handleRequestError("Conversion failed.");
    return;
  }
  Consumer.handleSourceText(Result);
}

//===----------------------------------------------------------------------===//
// EditorExpandPlaceholder
//===----------------------------------------------------------------------===//
void SwiftLangSupport::editorExpandPlaceholder(StringRef Name, unsigned Offset,
                                               unsigned Length,
                                               EditorConsumer &Consumer) {
  auto EditorDoc = EditorDocuments->getByUnresolvedName(Name);
  if (!EditorDoc) {
    Consumer.handleRequestError("No associated Editor Document");
    return;
  }
  EditorDoc->expandPlaceholder(Offset, Length, Consumer);
}
