//===--- DiagnosticConsumer.h - Diagnostic Consumer Interface ---*- C++ -*-===//
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
//
//  This file declares the DiagnosticConsumer class, which receives callbacks
//  whenever the front end emits a diagnostic and is responsible for presenting
//  or storing that diagnostic (whatever is appropriate).
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_DIAGNOSTICCONSUMER_H
#define SWIFT_BASIC_DIAGNOSTICCONSUMER_H

#include "swift/Basic/LLVM.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"
#include "llvm/Support/SourceMgr.h"

namespace swift {
  class DiagnosticArgument;
  class DiagnosticEngine;
  class SourceManager;
  enum class DiagID : uint32_t;

/// Describes the kind of diagnostic.
///
enum class DiagnosticKind : uint8_t {
  Error,
  Warning,
  Remark,
  Note
};

/// Information about a diagnostic passed to DiagnosticConsumers.
struct DiagnosticInfo {
  DiagID ID = DiagID(0);
  SourceLoc Loc;
  DiagnosticKind Kind;
  StringRef FormatString;
  ArrayRef<DiagnosticArgument> FormatArgs;
  StringRef Category;

  /// Only used when directing diagnostics to different outputs.
  /// In batch mode a diagnostic may be
  /// located in a non-primary file, but there will be no .dia file for a
  /// non-primary. If valid, this argument contains a location within a buffer
  /// that corresponds to a primary input. The .dia file for that primary can be
  /// used for the diagnostic, as if it had occurred at this location.
  SourceLoc BufferIndirectlyCausingDiagnostic;

  /// DiagnosticInfo of notes which are children of this diagnostic, if any
  ArrayRef<DiagnosticInfo *> ChildDiagnosticInfo;

  /// Paths to "educational note" diagnostic documentation in the toolchain.
  ArrayRef<std::string> EducationalNotePaths;

  /// Represents a fix-it, a replacement of one range of text with another.
  class FixIt {
    CharSourceRange Range;
    std::string Text;

  public:
    FixIt(CharSourceRange R, StringRef Str, ArrayRef<DiagnosticArgument> Args);

    CharSourceRange &getRange() { return Range; }
    const CharSourceRange &getRange() const { return Range; }

    StringRef getText() const { return Text; }
  };

  /// Extra source ranges that are attached to the diagnostic.
  ArrayRef<CharSourceRange> Ranges;

  /// Extra source ranges that are attached to the diagnostic.
  ArrayRef<FixIt> FixIts;

  /// This is a note which has a parent error or warning
  bool IsChildNote = false;

  DiagnosticInfo() {}

  DiagnosticInfo(DiagID ID, SourceLoc Loc, DiagnosticKind Kind,
                 StringRef FormatString,
                 ArrayRef<DiagnosticArgument> FormatArgs, StringRef Category,
                 SourceLoc BufferIndirectlyCausingDiagnostic,
                 ArrayRef<DiagnosticInfo *> ChildDiagnosticInfo,
                 ArrayRef<CharSourceRange> Ranges, ArrayRef<FixIt> FixIts,
                 bool IsChildNote)
      : ID(ID), Loc(Loc), Kind(Kind), FormatString(FormatString),
        FormatArgs(FormatArgs), Category(Category),
        BufferIndirectlyCausingDiagnostic(BufferIndirectlyCausingDiagnostic),
        ChildDiagnosticInfo(ChildDiagnosticInfo), Ranges(Ranges),
        FixIts(FixIts), IsChildNote(IsChildNote) {}
};
  
/// Abstract interface for classes that present diagnostics to the user.
class DiagnosticConsumer {
protected:
  static llvm::SMLoc getRawLoc(SourceLoc Loc);

  static llvm::SMRange getRawRange(SourceManager &SM, CharSourceRange R) {
    return llvm::SMRange(getRawLoc(R.getStart()), getRawLoc(R.getEnd()));
  }

  static llvm::SMFixIt getRawFixIt(SourceManager &SM, DiagnosticInfo::FixIt F) {
    // FIXME: It's unfortunate that we have to copy the replacement text.
    return llvm::SMFixIt(getRawRange(SM, F.getRange()), F.getText());
  }

public:
  virtual ~DiagnosticConsumer();

  /// Invoked whenever the frontend emits a diagnostic.
  ///
  /// \param SM The source manager associated with the source locations in
  /// this diagnostic.
  ///
  /// \param Info Information describing the diagnostic.
  virtual void handleDiagnostic(SourceManager &SM,
                                const DiagnosticInfo &Info) = 0;

  /// \returns true if an error occurred while finishing-up.
  virtual bool finishProcessing() { return false; }

  /// Flush any in-flight diagnostics.
  virtual void flush() {}

  /// In batch mode, any error causes failure for all primary files, but
  /// anyone consulting .dia files will only see an error for a particular
  /// primary in that primary's serialized diagnostics file. For other
  /// primaries' serialized diagnostics files, do something to signal the driver
  /// what happened. This is only meaningful for SerializedDiagnosticConsumers,
  /// so here's a placeholder.

  virtual void informDriverOfIncompleteBatchModeCompilation() {}
};
  
/// DiagnosticConsumer that discards all diagnostics.
class NullDiagnosticConsumer : public DiagnosticConsumer {
public:
  void handleDiagnostic(SourceManager &SM, const DiagnosticInfo &Info) override;
};

/// DiagnosticConsumer that forwards diagnostics to the consumers of
// another DiagnosticEngine.
class ForwardingDiagnosticConsumer : public DiagnosticConsumer {
  DiagnosticEngine &TargetEngine;
public:
  ForwardingDiagnosticConsumer(DiagnosticEngine &Target);
  void handleDiagnostic(SourceManager &SM, const DiagnosticInfo &Info) override;
};

/// DiagnosticConsumer that funnels diagnostics in certain files to
/// particular sub-consumers.
///
/// The intended use case for such a consumer is "batch mode" compilations,
/// where we want to record diagnostics for each file as if they were compiled
/// separately. This is important for incremental builds, so that if a file has
/// warnings but doesn't get recompiled in the next build, the warnings persist.
///
/// Diagnostics that are not in one of the special files are emitted into every
/// sub-consumer. This is necessary to deal with, for example, diagnostics in a
/// bridging header imported from Objective-C, which isn't really about the
/// current file.
class FileSpecificDiagnosticConsumer : public DiagnosticConsumer {
public:
  class Subconsumer;

  /// Given a vector of subconsumers, return the most specific
  /// DiagnosticConsumer for that vector. That will be a
  /// FileSpecificDiagnosticConsumer if the vector has > 1 subconsumer, the
  /// subconsumer itself if the vector has just one, or a null pointer if there
  /// are no subconsumers. Takes ownership of the DiagnosticConsumers specified
  /// in \p subconsumers.
  static std::unique_ptr<DiagnosticConsumer>
  consolidateSubconsumers(SmallVectorImpl<Subconsumer> &subconsumers);

  /// A diagnostic consumer, along with the name of the buffer that it should
  /// be associated with.
  class Subconsumer {
    friend std::unique_ptr<DiagnosticConsumer>
    FileSpecificDiagnosticConsumer::consolidateSubconsumers(
        SmallVectorImpl<Subconsumer> &subconsumers);

    /// The name of the input file that a consumer and diagnostics should
    /// be associated with. An empty string means that a consumer is not
    /// associated with any particular buffer, and should only receive
    /// diagnostics that are not in any of the other consumers' files.
    std::string inputFileName;

    /// The consumer (if any) for diagnostics associated with the inputFileName.
    /// A null pointer for the DiagnosticConsumer means that this file is a
    /// non-primary one in batch mode and we have no .dia file for it.
    /// If there is a responsible primary when the diagnostic is handled
    /// it will be shunted to that primary's .dia file.
    /// Otherwise it will be suppressed, assuming that the diagnostic will
    /// surface in another frontend job that compiles that file as a primary.
    std::unique_ptr<DiagnosticConsumer> consumer;

    // Has this subconsumer ever handled a diagnostic that is an error?
    bool hasAnErrorBeenConsumed = false;

  public:
    std::string getInputFileName() const { return inputFileName; }

    DiagnosticConsumer *getConsumer() const { return consumer.get(); }

    Subconsumer(std::string inputFileName,
                std::unique_ptr<DiagnosticConsumer> consumer)
        : inputFileName(inputFileName), consumer(std::move(consumer)) {}

    void handleDiagnostic(SourceManager &SM, const DiagnosticInfo &Info) {
      if (!getConsumer())
        return;
      hasAnErrorBeenConsumed |= Info.Kind == DiagnosticKind::Error;
      getConsumer()->handleDiagnostic(SM, Info);
    }

    void informDriverOfIncompleteBatchModeCompilation() {
      if (!hasAnErrorBeenConsumed && getConsumer())
        getConsumer()->informDriverOfIncompleteBatchModeCompilation();
    }
  };

private:
  /// All consumers owned by this FileSpecificDiagnosticConsumer.
  SmallVector<Subconsumer, 4> Subconsumers;

public:
  class ConsumerAndRange {
  private:
    /// The range of SourceLoc's for which diagnostics should be directed to
    /// this subconsumer.
    /// Should be const but then the sort won't compile.
    /*const*/ CharSourceRange range;

    /// Index into Subconsumers vector for this subconsumer.
    /// Should be const but then the sort won't compile.
    /*const*/ unsigned subconsumerIndex;

  public:
    unsigned getSubconsumerIndex() const { return subconsumerIndex; }

    ConsumerAndRange(const CharSourceRange range, unsigned subconsumerIndex)
        : range(range), subconsumerIndex(subconsumerIndex) {}

    /// Compare according to range:
    bool operator<(const ConsumerAndRange &right) const {
      auto compare = std::less<const char *>();
      return compare(getRawLoc(range.getEnd()).getPointer(),
                     getRawLoc(right.range.getEnd()).getPointer());
    }

    /// Overlaps by range:
    bool overlaps(const ConsumerAndRange &other) const {
      return range.overlaps(other.range);
    }

    /// Does my range end after \p loc?
    bool endsAfter(const SourceLoc loc) const {
      auto compare = std::less<const char *>();
      return compare(getRawLoc(range.getEnd()).getPointer(),
                     getRawLoc(loc).getPointer());
    }

    bool contains(const SourceLoc loc) const { return range.contains(loc); }
  };

private:
  Subconsumer &operator[](const ConsumerAndRange &consumerAndRange) {
    return Subconsumers[consumerAndRange.getSubconsumerIndex()];
  }
  /// The consumers owned by this FileSpecificDiagnosticConsumer, sorted by
  /// the end locations of each file so that a lookup by position can be done
  /// using binary search.
  ///
  /// Generated and cached when the first diagnostic with a location is emitted.
  /// This allows diagnostics to be emitted before files are actually opened,
  /// as long as they don't have source locations.
  ///
  /// \see #subconsumerForLocation
  SmallVector<ConsumerAndRange, 4> ConsumersOrderedByRange;

  /// Indicates which consumer to send Note diagnostics too.
  ///
  /// Notes are always considered attached to the error, warning, or remark
  /// that was most recently emitted.
  ///
  /// If None, Note diagnostics are sent to every consumer.
  /// If null, diagnostics are suppressed.
  llvm::Optional<Subconsumer *> SubconsumerForSubsequentNotes = llvm::None;

  bool HasAnErrorBeenConsumed = false;

  /// Takes ownership of the DiagnosticConsumers specified in \p consumers.
  ///
  /// There must not be two consumers for the same file (i.e., having the same
  /// buffer name).
  explicit FileSpecificDiagnosticConsumer(
      SmallVectorImpl<Subconsumer> &consumers);

public:
  void handleDiagnostic(SourceManager &SM, const DiagnosticInfo &Info) override;

  bool finishProcessing() override;

private:
  /// In batch mode, any error causes failure for all primary files, but
  /// Xcode will only see an error for a particular primary in that primary's
  /// serialized diagnostics file. So, tell the subconsumers to inform the
  /// driver of incomplete batch mode compilation.
  void tellSubconsumersToInformDriverOfIncompleteBatchModeCompilation();

  void computeConsumersOrderedByRange(SourceManager &SM);

  /// Returns nullptr if diagnostic is to be suppressed,
  /// None if diagnostic is to be distributed to every consumer,
  /// a particular consumer if diagnostic goes there.
  llvm::Optional<FileSpecificDiagnosticConsumer::Subconsumer *>
  subconsumerForLocation(SourceManager &SM, SourceLoc loc);

  llvm::Optional<FileSpecificDiagnosticConsumer::Subconsumer *>
  findSubconsumer(SourceManager &SM, const DiagnosticInfo &Info);

  llvm::Optional<FileSpecificDiagnosticConsumer::Subconsumer *>
  findSubconsumerForNonNote(SourceManager &SM, const DiagnosticInfo &Info);
};

} // end namespace swift

#endif // SWIFT_BASIC_DIAGNOSTICCONSUMER_H
