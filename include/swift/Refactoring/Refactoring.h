//===--- Refactoring.h - APIs for refactoring --------*- C++ -*-===//
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

#ifndef SWIFT_IDE_REFACTORING_H
#define SWIFT_IDE_REFACTORING_H

#include "swift/AST/DiagnosticConsumer.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/StringExtras.h"
#include "swift/IDE/Utils.h"
#include "llvm/ADT/StringRef.h"

namespace swift {
  class ModuleDecl;
  class SourceLoc;
  class SourceManager;

namespace ide {
  struct ResolvedCursorInfo;

enum class RefactoringKind : int8_t {
  None,
#define REFACTORING(KIND, NAME, ID) KIND,
#include "RefactoringKinds.def"
};

enum class RefactorAvailableKind {
  Available,
  Unavailable_system_symbol,
  Unavailable_has_no_location,
  Unavailable_has_no_name,
  Unavailable_has_no_accessibility,
  Unavailable_decl_from_clang,
  Unavailable_decl_in_macro,
};

struct RefactorAvailabilityInfo {
  RefactoringKind Kind;
  RefactorAvailableKind AvailableKind;
  RefactorAvailabilityInfo(RefactoringKind Kind,
                           RefactorAvailableKind AvailableKind)
      : Kind(Kind), AvailableKind(AvailableKind) {}
  RefactorAvailabilityInfo(RefactoringKind Kind)
      : RefactorAvailabilityInfo(Kind, RefactorAvailableKind::Available) {}
};

struct RenameInfo {
  ValueDecl *VD;
  RefactorAvailabilityInfo Availability;
};

llvm::Optional<RenameInfo> getRenameInfo(ResolvedCursorInfoPtr cursorInfo);

enum class NameUsage {
  Unknown,
  Reference,
  Definition,
  Call
};

struct RenameLoc {
  unsigned Line;
  unsigned Column;
  NameUsage Usage;
  StringRef OldName;
  /// The new name that should be given to this symbol.
  ///
  /// This may not be known if the rename locations are specified by the client
  /// using the a rename locations dicationary in syntactic rename.
  ///
  /// May be empty if no new name was specified in `localRenameLocs`.
  StringRef NewName;
  const bool IsFunctionLike;
  const bool IsNonProtocolType;
};

/// An array of \c RenameLoc that also keeps the underlying string storage of
/// the \c StringRef inside the \c RenameLoc alive.
class RenameLocs {
  std::vector<RenameLoc> Locs;
  std::unique_ptr<StringScratchSpace> StringStorage;

public:
  ArrayRef<RenameLoc> getLocations() { return Locs; }

  RenameLocs(std::vector<RenameLoc> Locs,
             std::unique_ptr<StringScratchSpace> StringStorage)
      : Locs(Locs), StringStorage(std::move(StringStorage)) {}

  RenameLocs() {}
};

/// Return the location to rename when renaming the identifier at \p startLoc
/// in \p SF.
///
/// - Parameters:
///   - SF: The source file in which to perform local rename
///   - renameInfo: Information about the symbol to rename. See `getRenameInfo`
///   - newName: The new name that should be assigned to the identifer. Can
///     be empty, in which case the new name of all `RenameLoc`s will also be
///     empty.
RenameLocs localRenameLocs(SourceFile *SF, RenameInfo renameInfo,
                           StringRef newName);

/// Given a list of `RenameLoc`s, get the corresponding `ResolveLoc`s.
///
/// These resolve locations contain more structured information, such as the
/// range of the base name to rename and the ranges of the argument labels.
std::vector<ResolvedLoc> resolveRenameLocations(ArrayRef<RenameLoc> RenameLocs,
                                                SourceFile &SF,
                                                DiagnosticEngine &Diags);

struct RangeConfig {
  unsigned BufferID;
  unsigned Line;
  unsigned Column;
  unsigned Length;
  SourceLoc getStart(SourceManager &SM);
  SourceLoc getEnd(SourceManager &SM);
};

struct RefactoringOptions {
  RefactoringKind Kind;
  RangeConfig Range;
  std::string PreferredName;
  RefactoringOptions(RefactoringKind Kind) : Kind(Kind) {}
};

// TODO: Merge with NoteRegion – range needs to change to start/end line/column
struct RenameRangeDetail {
  CharSourceRange Range;
  RefactoringRangeKind RangeKind;
  llvm::Optional<unsigned> Index;
};

class FindRenameRangesConsumer {
public:
  virtual void accept(SourceManager &SM, RegionType RegionType,
                      ArrayRef<RenameRangeDetail> Ranges) = 0;
  virtual ~FindRenameRangesConsumer() = default;
};

class FindRenameRangesAnnotatingConsumer : public FindRenameRangesConsumer {
  struct Implementation;
  Implementation &Impl;

public:
  FindRenameRangesAnnotatingConsumer(SourceManager &SM, unsigned BufferId,
                                     llvm::raw_ostream &OS);
  ~FindRenameRangesAnnotatingConsumer();
  void accept(SourceManager &SM, RegionType RegionType,
              ArrayRef<RenameRangeDetail> Ranges) override;
};

StringRef getDescriptiveRefactoringKindName(RefactoringKind Kind);

StringRef getDescriptiveRenameUnavailableReason(RefactorAvailableKind Kind);

bool refactorSwiftModule(ModuleDecl *M, RefactoringOptions Opts,
                         SourceEditConsumer &EditConsumer,
                         DiagnosticConsumer &DiagConsumer);

int syntacticRename(SourceFile *SF, llvm::ArrayRef<RenameLoc> RenameLocs,
                    SourceEditConsumer &EditConsumer,
                    DiagnosticConsumer &DiagConsumer);

int findSyntacticRenameRanges(SourceFile *SF,
                              llvm::ArrayRef<RenameLoc> RenameLocs,
                              FindRenameRangesConsumer &RenameConsumer,
                              DiagnosticConsumer &DiagConsumer);

int findLocalRenameRanges(SourceFile *SF, RangeConfig Range,
                          FindRenameRangesConsumer &RenameConsumer,
                          DiagnosticConsumer &DiagConsumer);

SmallVector<RefactorAvailabilityInfo, 0>
collectRefactorings(SourceFile *SF, RangeConfig Range,
                    bool &RangeStartMayNeedRename,
                    llvm::ArrayRef<DiagnosticConsumer *> DiagConsumers);

SmallVector<RefactorAvailabilityInfo, 0>
collectRefactorings(ResolvedCursorInfoPtr CursorInfo, bool ExcludeRename);

} // namespace ide
} // namespace swift

#endif // SWIFT_IDE_REFACTORING_H
