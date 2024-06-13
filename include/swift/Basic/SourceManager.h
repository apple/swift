//===--- SourceManager.h - Manager for Source Buffers -----------*- C++ -*-===//
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

#ifndef SWIFT_BASIC_SOURCEMANAGER_H
#define SWIFT_BASIC_SOURCEMANAGER_H

#include "swift/Basic/FileSystem.h"
#include "swift/Basic/SourceLoc.h"
#include "clang/Basic/FileManager.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/SourceMgr.h"
#include <map>
#include <optional>
#include <vector>

namespace swift {

class CustomAttr;
class DeclContext;

/// Augments a buffer that was created specifically to hold generated source
/// code with the reasons for it being generated.
class GeneratedSourceInfo {
public:
  /// The kind of generated source code.
  enum Kind {
#define MACRO_ROLE(Name, Description) Name##MacroExpansion,
#include "swift/Basic/MacroRoles.def"

    /// A new function body that is replacing an existing function body.
    ReplacedFunctionBody,

    /// Pretty-printed declarations that have no source location.
    PrettyPrinted,

    /// The expansion of default argument at caller side
    DefaultArgument,
  } kind;

  /// The source range in the enclosing buffer where this source was generated.
  ///
  /// This could point at a macro expansion or at the implicit location at
  /// which source code was generated. Conceptually, one can think of the
  /// buffer described by a \c GeneratedSource instance as replacing the
  /// code in the \c originalSourceRange.
  CharSourceRange originalSourceRange;

  /// The source range in the generated-source buffer where the generated
  /// code exists. This might be a subrange of the buffer containing the
  /// generated source, but it will never be from a different buffer.
  CharSourceRange generatedSourceRange;

  /// The opaque pointer for an ASTNode for which this buffer was generated.
  void *astNode = nullptr;

  /// The declaration context in which this buffer logically resides.
  DeclContext *declContext = nullptr;

  /// The custom attribute for an attached macro.
  CustomAttr *attachedMacroCustomAttr = nullptr;

  /// MacroDecl name if the generated source is coming from macro expansion,
  /// otherwise empty string.
  std::string macroName = "";

  /// The name of the source file on disk that was created to hold the
  /// contents of this file for external clients.
  StringRef onDiskBufferCopyFileName = StringRef();

  /// Contains the ancestors of this source buffer, starting with the root source
  /// buffer and ending at this source buffer.
  mutable llvm::ArrayRef<unsigned> ancestors = llvm::ArrayRef<unsigned>();
};

/// This class manages and owns source buffers.
class SourceManager {
public:
  /// A \c #sourceLocation-defined virtual file region, representing the source
  /// source after a \c #sourceLocation (or between two). It provides a
  /// filename and line offset to be applied to \c SourceLoc's within its
  /// \c Range.
  struct VirtualFile {
    CharSourceRange Range;
    std::string Name;
    int LineOffset;
  };

private:
  llvm::SourceMgr LLVMSourceMgr;
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem;
  unsigned IDEInspectionTargetBufferID = 0U;
  unsigned IDEInspectionTargetOffset;

  /// Associates buffer identifiers to buffer IDs.
  llvm::DenseMap<StringRef, unsigned> BufIdentIDMap;

  /// A cache mapping buffer identifiers to vfs Status entries.
  ///
  /// This is as much a hack to prolong the lifetime of status objects as it is
  /// to speed up stats.
  mutable llvm::DenseMap<StringRef, llvm::vfs::Status> StatusCache;

  /// Holds generated source information, indexed by the buffer ID.
  llvm::DenseMap<unsigned, GeneratedSourceInfo> GeneratedSourceInfos;

  /// Holds replaced ranges. Keys are original ranges, and values are new ranges
  /// in different buffers. This is used for code completion and ASTContext
  /// reusing compilation.
  llvm::DenseMap<SourceRange, SourceRange> ReplacedRanges;

  /// The starting source locations of regex literals written in source. This
  /// is an unfortunate hack needed to allow for correct re-lexing.
  llvm::DenseSet<SourceLoc> RegexLiteralStartLocs;

  std::map<const char *, VirtualFile> VirtualFiles;
  mutable std::pair<const char *, const VirtualFile*> CachedVFile = {nullptr, nullptr};

  /// A cache that improves the speed of location -> buffer lookups.
  struct BufferLocCache {
    /// The set of memory buffers IDs, sorted by the start of their source range.
    std::vector<unsigned> sortedBuffers;

    /// The number of buffers that were present when sortedBuffers was formed.
    ///
    /// There can be multiple buffers that refer to the same source range,
    /// and we remove duplicates as part of the processing of forming the
    /// vector of sorted buffers. This number is the number of original buffers,
    /// used to determine when the sorted buffers are out of date.
    unsigned numBuffersOriginal = 0;

    /// The last buffer we looked in. This acts as a one-element MRU cache for
    /// lookups based on source locations.
    std::optional<unsigned> lastBufferID;
  };

  /// The cache that's used to quickly map a source location to a particular
  /// buffer ID.
  mutable BufferLocCache LocCache;

  std::optional<unsigned> findBufferContainingLocInternal(SourceLoc Loc) const;

public:
  SourceManager(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS =
                    llvm::vfs::getRealFileSystem())
    : FileSystem(FS) {}
  ~SourceManager();

  llvm::SourceMgr &getLLVMSourceMgr() {
    return LLVMSourceMgr;
  }
  const llvm::SourceMgr &getLLVMSourceMgr() const {
    return LLVMSourceMgr;
  }

  void setFileSystem(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS) {
    FileSystem = FS;
  }

  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> getFileSystem() const {
    return FileSystem;
  }

  void setIDEInspectionTarget(unsigned BufferID, unsigned Offset) {
    assert(BufferID != 0U && "Buffer should be valid");

    IDEInspectionTargetBufferID = BufferID;
    IDEInspectionTargetOffset = Offset;
  }

  bool hasIDEInspectionTargetBuffer() const {
    return IDEInspectionTargetBufferID != 0U;
  }

  unsigned getIDEInspectionTargetBufferID() const {
    return IDEInspectionTargetBufferID;
  }

  unsigned getIDEInspectionTargetOffset() const {
    return IDEInspectionTargetOffset;
  }

  SourceLoc getIDEInspectionTargetLoc() const;

  /// Returns whether `Range` contains `Loc`. This also respects the
  /// `ReplacedRanges`, i.e. if `Loc` is in a range that replaces a range which
  /// overlaps `Range`, this also returns `true`.
  bool containsRespectingReplacedRanges(SourceRange Range, SourceLoc Loc) const;

  /// Returns whether `Enclosing` contains `Inner`. This also respects the
  /// `ReplacedRanges`, i.e. if `Inner` is contained in a range that replaces a
  /// range which overlaps `Range`, this also returns `true`.
  bool rangeContainsRespectingReplacedRanges(SourceRange Enclosing,
                                             SourceRange Inner) const;

  const llvm::DenseMap<SourceRange, SourceRange> &getReplacedRanges() const {
    return ReplacedRanges;
  }

  /// Set the generated source information associated with a given buffer.
  void setGeneratedSourceInfo(unsigned bufferID, GeneratedSourceInfo);

  /// Checks whether the given buffer has generated source information.
  bool hasGeneratedSourceInfo(unsigned bufferID);

  /// Retrieve the generated source information for the given buffer.
  std::optional<GeneratedSourceInfo>
  getGeneratedSourceInfo(unsigned bufferID) const;

  /// Retrieve the list of ancestors of the given source buffer, starting with
  /// the root buffer and proceding to the given buffer ID at the end.
  ///
  /// The scratch parameter will be used to avoid allocation in the case where
  /// the given buffer ID is the top-level buffer, in which case bufferID will
  /// be written into scratch at the returned array will contain that one
  /// element.
  ArrayRef<unsigned> getAncestors(unsigned bufferID, unsigned &scratch) const;

  /// Record the starting source location of a regex literal.
  void recordRegexLiteralStartLoc(SourceLoc loc) {
    RegexLiteralStartLocs.insert(loc);
  }

  /// Checks whether a given source location is for the start of a regex
  /// literal.
  bool isRegexLiteralStart(SourceLoc loc) const {
    return RegexLiteralStartLocs.contains(loc);
  }

  /// Returns true if \c first is before \c second in the same source file,
  /// accounting for the possibility that first and second are in different
  /// source buffers that are conceptually within the same source file,
  /// due to macro expansion.
  bool isBefore(SourceLoc first, SourceLoc second) const;

  /// Returns true if \c first is at or before \c second in the same source
  /// file, accounting for the possibility that first and second are in
  /// different source buffers that are conceptually within the same source
  /// file, due to macro expansion.
  bool isAtOrBefore(SourceLoc first, SourceLoc second) const;

  /// Returns true if \c LHS is before \c RHS in the same source buffer.
  bool isBeforeInBuffer(SourceLoc LHS, SourceLoc RHS) const {
    return LHS.Value.getPointer() < RHS.Value.getPointer();
  }

  /// Returns true if \c range contains the location \c loc.  The location
  /// \c loc should point at the beginning of the token.
  ///
  /// This function accounts for the possibility that the source locations
  /// provided might come from different source buffers that are conceptually
  /// part of the same source file, for example due to macro expansion.
  bool containsTokenLoc(SourceRange range, SourceLoc loc) const;

  /// Returns true if \c range contains the location \c loc.
  ///
  /// This function accounts for the possibility that the source locations
  /// provided might come from different source buffers that are conceptually
  /// part of the same source file, for example due to macro expansion.
  bool containsLoc(SourceRange range, SourceLoc loc) const;

  /// Returns true if \c enclosing contains the whole range \c inner.
  ///
  /// This function accounts for the possibility that the source locations
  /// provided might come from different source buffers that are conceptually
  /// part of the same source file, for example due to macro expansion.
  bool encloses(SourceRange enclosing, SourceRange inner) const;

  /// Returns true if range \c R contains the location \c Loc, where all
  /// locations are known to be in the same source buffer.
  ///
  /// The location \c Loc should point at the beginning of the token.
  bool rangeContainsTokenLoc(SourceRange R, SourceLoc Loc) const {
    return Loc == R.Start || Loc == R.End ||
           (isBeforeInBuffer(R.Start, Loc) && isBeforeInBuffer(Loc, R.End));
  }

  /// Returns true if range \c R contains the location \c Loc.  The location
  /// \c Loc should point at the beginning of the token.
  bool rangeContainsTokenLoc(CharSourceRange R, SourceLoc Loc) const {
    return Loc == R.getStart() || (isBeforeInBuffer(R.getStart(), Loc) &&
                                   isBeforeInBuffer(Loc, R.getEnd()));
  }

  /// Returns true if range \c Enclosing contains the range \c Inner.
  bool rangeContains(SourceRange Enclosing, SourceRange Inner) const {
    return rangeContainsTokenLoc(Enclosing, Inner.Start) &&
           rangeContainsTokenLoc(Enclosing, Inner.End);
  }

  /// Returns true if range \p R contains the code-completion location, if any.
  bool rangeContainsIDEInspectionTarget(CharSourceRange R) const {
    if (!IDEInspectionTargetBufferID) {
      return false;
    }
    return rangeContainsTokenLoc(R, getIDEInspectionTargetLoc());
  }

  /// Returns the buffer ID for the specified *valid* location.
  ///
  /// Because a valid source location always corresponds to a source buffer,
  /// this routine always returns a valid buffer ID.
  unsigned findBufferContainingLoc(SourceLoc Loc) const;

  /// Whether the source location is pointing to any buffer owned by the SourceManager.
  bool isOwning(SourceLoc Loc) const;

  /// Adds a memory buffer to the SourceManager, taking ownership of it.
  unsigned addNewSourceBuffer(std::unique_ptr<llvm::MemoryBuffer> Buffer);

  /// Add a \c #sourceLocation-defined virtual file region of \p Length.
  void createVirtualFile(SourceLoc Loc, StringRef Name, int LineOffset,
                         unsigned Length);

  /// Add a \c #sourceLocation-defined virtual file region.
  ///
  /// By default, this region continues to the end of the buffer.
  ///
  /// \returns True if the new file was added, false if the file already exists.
  /// The name and line offset must match exactly in that case.
  ///
  /// \sa closeVirtualFile
  bool openVirtualFile(SourceLoc loc, StringRef name, int lineOffset);

  /// Close a \c #sourceLocation-defined virtual file region.
  void closeVirtualFile(SourceLoc end);

  /// Creates a copy of a \c MemoryBuffer and adds it to the \c SourceManager,
  /// taking ownership of the copy.
  unsigned addMemBufferCopy(llvm::MemoryBuffer *Buffer);

  /// Creates and adds a memory buffer to the \c SourceManager, taking
  /// ownership of the newly created copy.
  ///
  /// \p InputData and \p BufIdentifier are copied, so that this memory can go
  /// away as soon as this function returns.
  unsigned addMemBufferCopy(StringRef InputData, StringRef BufIdentifier = "");

  /// Returns a buffer ID for a previously added buffer with the given
  /// buffer identifier, or None if there is no such buffer.
  std::optional<unsigned>
  getIDForBufferIdentifier(StringRef BufIdentifier) const;

  /// Returns the identifier for the buffer with the given ID.
  ///
  /// \p BufferID must be a valid buffer ID.
  ///
  /// \p ForceGeneratedSourceToDisk can be set to true to create a temporary
  /// file on-disk for buffers containing generated source code, returning the
  /// name of that temporary file.
  ///
  /// This should not be used for displaying information about the \e contents
  /// of a buffer, since lines within the buffer may be marked as coming from
  /// other files using \c #sourceLocation. Use #getDisplayNameForLoc instead
  /// in that case.
  StringRef getIdentifierForBuffer(
      unsigned BufferID,
      bool ForceGeneratedSourceToDisk = false
  ) const;

  /// Returns a SourceRange covering the entire specified buffer.
  ///
  /// Note that the start location might not point at the first token: it
  /// might point at whitespace or a comment.
  CharSourceRange getRangeForBuffer(unsigned BufferID) const;

  /// Returns the SourceLoc for the beginning of the specified buffer
  /// (at offset zero).
  ///
  /// Note that the resulting location might not point at the first token: it
  /// might point at whitespace or a comment.
  SourceLoc getLocForBufferStart(unsigned BufferID) const {
    return getRangeForBuffer(BufferID).getStart();
  }

  /// Returns the offset in bytes for the given valid source location.
  unsigned getLocOffsetInBuffer(SourceLoc Loc, unsigned BufferID) const;

  /// Returns the distance in bytes between the given valid source
  /// locations.
  unsigned getByteDistance(SourceLoc Start, SourceLoc End) const;

  /// Returns the SourceLoc for the byte offset in the specified buffer.
  SourceLoc getLocForOffset(unsigned BufferID, unsigned Offset) const {
    return getLocForBufferStart(BufferID).getAdvancedLoc(Offset);
  }

  /// Returns a buffer identifier suitable for display to the user containing
  /// the given source location.
  ///
  /// \p ForceGeneratedSourceToDisk can be set to true to create a temporary
  /// file on-disk for buffers containing generated source code, returning the
  /// name of that temporary file.
  ///
  /// This respects \c #sourceLocation directives and the 'use-external-names'
  /// directive in VFS overlay files. If you need an on-disk file name, use
  /// #getIdentifierForBuffer instead.
  StringRef getDisplayNameForLoc(
      SourceLoc Loc, bool ForceGeneratedSourceToDisk = false) const;

  /// Returns the line and column represented by the given source location.
  ///
  /// If \p BufferID is provided, \p Loc must come from that source buffer.
  ///
  /// This respects \c #sourceLocation directives.
  std::pair<unsigned, unsigned>
  getPresumedLineAndColumnForLoc(SourceLoc Loc, unsigned BufferID = 0) const {
    assert(Loc.isValid());
    int LineOffset = getLineOffset(Loc);
    int l, c;
    std::tie(l, c) = LLVMSourceMgr.getLineAndColumn(Loc.Value, BufferID);
    assert(LineOffset+l > 0 && "bogus line offset");
    return { LineOffset + l, c };
  }

  /// Returns the real line and column for a source location.
  ///
  /// If \p BufferID is provided, \p Loc must come from that source buffer.
  ///
  /// This does not respect \c #sourceLocation directives.
  std::pair<unsigned, unsigned>
  getLineAndColumnInBuffer(SourceLoc Loc, unsigned BufferID = 0) const {
    assert(Loc.isValid());
    return LLVMSourceMgr.getLineAndColumn(Loc.Value, BufferID);
  }

  /// Returns the column for the given source location in the given buffer.
  unsigned getColumnInBuffer(SourceLoc Loc, unsigned BufferID) const;

  StringRef getEntireTextForBuffer(unsigned BufferID) const;

  StringRef extractText(CharSourceRange Range,
                        std::optional<unsigned> BufferID = std::nullopt) const;

  llvm::SMDiagnostic GetMessage(SourceLoc Loc, llvm::SourceMgr::DiagKind Kind,
                                const Twine &Msg,
                                ArrayRef<llvm::SMRange> Ranges,
                                ArrayRef<llvm::SMFixIt> FixIts,
                                bool EmitMacroExpansionFiles = false) const;

  /// Verifies that all buffers are still valid.
  void verifyAllBuffers() const;

  /// Translate line and column pair to the offset.
  /// If the column number is the maximum unsinged int, return the offset of the end of the line.
  std::optional<unsigned> resolveFromLineCol(unsigned BufferId, unsigned Line,
                                             unsigned Col) const;

  /// Translate the end position of the given line to the offset.
  std::optional<unsigned> resolveOffsetForEndOfLine(unsigned BufferId,
                                                    unsigned Line) const;

  /// Get the length of the line
  std::optional<unsigned> getLineLength(unsigned BufferId, unsigned Line) const;

  SourceLoc getLocForLineCol(unsigned BufferId, unsigned Line, unsigned Col) const {
    auto Offset = resolveFromLineCol(BufferId, Line, Col);
    return Offset.has_value() ? getLocForOffset(BufferId, Offset.value()) :
                               SourceLoc();
  }

  /// Retrieve the buffer ID for \p Path, loading if necessary.
  unsigned getExternalSourceBufferID(StringRef Path);

  SourceLoc getLocFromExternalSource(StringRef Path, unsigned Line, unsigned Col);

  /// Retrieve the virtual file for the given \p Loc, or nullptr if none exists.
  const VirtualFile *getVirtualFile(SourceLoc Loc) const;

  /// Whether or not \p Loc is after a \c #sourceLocation directive, ie. its
  /// file, line, and column should be reported using the information in the
  /// directive.
  bool isLocInVirtualFile(SourceLoc Loc) const {
    return getVirtualFile(Loc) != nullptr;
  }

  /// Return a SourceLoc in \c this corresponding to \p otherLoc, which must be
  /// owned by \p otherManager. Returns an invalid SourceLoc if it cannot be
  /// translated.
  SourceLoc getLocForForeignLoc(SourceLoc otherLoc, SourceManager &otherMgr);

private:
  int getLineOffset(SourceLoc Loc) const {
    if (auto VFile = getVirtualFile(Loc))
      return VFile->LineOffset;
    else
      return 0;
  }
};

} // end namespace swift

#endif // SWIFT_BASIC_SOURCEMANAGER_H

