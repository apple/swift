//===--- SwiftLangSupport.h - -----------------------------------*- C++ -*-===//
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

#ifndef LLVM_SOURCEKIT_LIB_SWIFTLANG_SWIFTLANGSUPPORT_H
#define LLVM_SOURCEKIT_LIB_SWIFTLANG_SWIFTLANGSUPPORT_H

#include "CodeCompletion.h"
#include "SourceKit/Core/Context.h"
#include "SourceKit/Core/LangSupport.h"
#include "SourceKit/Support/Concurrency.h"
#include "SourceKit/Support/Statistic.h"
#include "SourceKit/Support/ThreadSafeRefCntPtr.h"
#include "SourceKit/Support/Tracing.h"
#include "SwiftInterfaceGenContext.h"
#include "swift/AST/DiagnosticConsumer.h"
#include "swift/AST/PluginRegistry.h"
#include "swift/Basic/ThreadSafeRefCounted.h"
#include "swift/IDE/CancellableResult.h"
#include "swift/IDE/Indenting.h"
#include "swift/Refactoring/Refactoring.h"
#include "swift/IDETool/CompileInstance.h"
#include "swift/IDETool/IDEInspectionInstance.h"
#include "swift/Index/IndexSymbol.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Mutex.h"
#include <map>
#include <string>

namespace swift {
  class ASTContext;
  class ClangModuleLoader;
  class CompilerInstance;
  class CompilerInvocation;
  class Decl;
  class Type;
  class AbstractStorageDecl;
  class SourceFile;
  class SILOptions;
  class ValueDecl;
  class GenericSignature;
  enum class AccessorKind;

namespace ide {
  class CodeCompletionCache;
  class IDEInspectionInstance;
  class OnDiskCodeCompletionCache;
  class SourceEditConsumer;
  class SyntacticMacroExpansion;
  enum class CodeCompletionDeclKind : uint8_t;
  enum class SyntaxNodeKind : uint8_t;
  enum class SyntaxStructureKind : uint8_t;
  enum class SyntaxStructureElementKind : uint8_t;
  enum class RangeKind : int8_t;
  class CodeCompletionConsumer;

  enum class NameKind {
    ObjC,
    Swift,
  };
}
}

namespace SourceKit {
  class FileSystemProvider;
  class ImmutableTextSnapshot;
  typedef RefPtr<ImmutableTextSnapshot> ImmutableTextSnapshotRef;
  class SwiftASTManager;
  class SwiftLangSupport;
  class Context;
  class NotificationCenter;

  using TypeContextKind = swift::ide::CodeCompletionContext::TypeContextKind;

class SwiftEditorDocument :
    public ThreadSafeRefCountedBase<SwiftEditorDocument> {

  struct Implementation;
  Implementation &Impl;

public:

  SwiftEditorDocument(StringRef FilePath, SwiftLangSupport &LangSupport,
       llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem,
       swift::ide::CodeFormatOptions Options = swift::ide::CodeFormatOptions());
  ~SwiftEditorDocument();

  ImmutableTextSnapshotRef
  initializeText(llvm::MemoryBuffer *Buf, ArrayRef<const char *> Args,
                 bool ProvideSemanticInfo,
                 llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem);
  ImmutableTextSnapshotRef replaceText(unsigned Offset, unsigned Length,
                                       llvm::MemoryBuffer *Buf,
                                       bool ProvideSemanticInfo,
                                       std::string &error);

  void updateSemaInfo(SourceKitCancellationToken CancellationToken);

  void removeCachedAST();

  ImmutableTextSnapshotRef getLatestSnapshot() const;

  void resetSyntaxInfo(ImmutableTextSnapshotRef Snapshot,
                       SwiftLangSupport &Lang);
  void readSyntaxInfo(EditorConsumer &consumer, bool ReportDiags);
  void readSemanticInfo(ImmutableTextSnapshotRef Snapshot,
                        EditorConsumer& Consumer);

  void applyFormatOptions(OptionsDictionary &FmtOptions);
  void formatText(unsigned Line, unsigned Length, EditorConsumer &Consumer);
  void expandPlaceholder(unsigned Offset, unsigned Length,
                         EditorConsumer &Consumer);
  const swift::ide::CodeFormatOptions &getFormatOptions();

  static void reportDocumentStructure(swift::SourceFile &SrcFile,
                                      EditorConsumer &Consumer);

  std::string getFilePath() const;
      
  /// Returns the virtual filesystem associated with this document.
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> getFileSystem() const;
};

typedef IntrusiveRefCntPtr<SwiftEditorDocument> SwiftEditorDocumentRef;

class SwiftEditorDocumentFileMap {
  WorkQueue Queue{ WorkQueue::Dequeuing::Concurrent,
                   "sourcekit.swift.EditorDocFileMap" };
  struct DocInfo {
    SwiftEditorDocumentRef DocRef;
    std::string ResolvedPath;
  };
  llvm::StringMap<DocInfo> Docs;

public:
  bool getOrUpdate(StringRef FilePath,
                   SwiftLangSupport &LangSupport,
                   SwiftEditorDocumentRef &EditorDoc);
  /// Looks up the document only by the path name that was given initially.
  SwiftEditorDocumentRef getByUnresolvedName(StringRef FilePath);
  /// Looks up the document by resolving symlinks in the paths.
  /// If \p IsRealpath is \c true, then \p FilePath must already be
  /// canonicalized to a realpath.
  SwiftEditorDocumentRef findByPath(StringRef FilePath,
                                    bool IsRealpath = false);
  SwiftEditorDocumentRef remove(StringRef FilePath);
};

namespace CodeCompletion {

/// Provides a thread-safe cache for code completion results that remain valid
/// for the duration of a 'session' - for example, from the point that a user
/// invokes code completion until they accept a completion, or otherwise close
/// the list of completions.
///
/// The contents of the cache can be modified asynchronously during the session,
/// but the contained objects are immutable.
class SessionCache : public ThreadSafeRefCountedBase<SessionCache> {
  std::unique_ptr<llvm::MemoryBuffer> buffer;
  std::vector<std::string> args;
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem;
  CompletionSink sink;
  std::vector<Completion *> sortedCompletions;
  CompletionKind completionKind;
  TypeContextKind typeContextKind;
  bool completionMayUseImplicitMemberExpr;
  FilterRules filterRules;
  llvm::sys::Mutex mtx;

public:
  SessionCache(CompletionSink &&sink,
               std::unique_ptr<llvm::MemoryBuffer> &&buffer,
               std::vector<std::string> &&args,
               llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem,
               CompletionKind completionKind,
               TypeContextKind typeContextKind, bool mayUseImplicitMemberExpr,
               FilterRules filterRules)
      : buffer(std::move(buffer)), args(std::move(args)),
        fileSystem(std::move(fileSystem)), sink(std::move(sink)),
        completionKind(completionKind), typeContextKind(typeContextKind),
        completionMayUseImplicitMemberExpr(mayUseImplicitMemberExpr),
        filterRules(std::move(filterRules)) {}
  void setSortedCompletions(std::vector<Completion *> &&completions);
  ArrayRef<Completion *> getSortedCompletions();
  llvm::MemoryBuffer *getBuffer();
  ArrayRef<std::string> getCompilerArgs();
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> getFileSystem();
  const FilterRules &getFilterRules();
  CompletionKind getCompletionKind();
  TypeContextKind getCompletionTypeContextKind();
  bool getCompletionMayUseImplicitMemberExpr();
};
typedef RefPtr<SessionCache> SessionCacheRef;

/// A thread-safe map from (buffer, code complete offset) to \c SessionCache.
class SessionCacheMap {
  mutable unsigned nextBufferID = 0;
  mutable llvm::StringMap<unsigned> nameToBufferMap;
  typedef std::pair<unsigned, unsigned> Key;
  llvm::DenseMap<Key, SessionCacheRef> sessions;
  mutable llvm::sys::Mutex mtx;

  // Should only be called with Mtx locked.
  unsigned getBufferID(StringRef name) const;

public:
  SessionCacheRef get(StringRef name, unsigned offset) const;
  bool set(StringRef name, unsigned offset, SessionCacheRef session);
  bool remove(StringRef name, unsigned offset);
};
} // end namespace CodeCompletion

namespace TypeContextInfo {
struct Options {
  // TypeContextInfo doesn't receive any options at this point.
};
} // namespace TypeContextInfo

namespace ConformingMethodList {
struct Options {
  // ConformingMethodList doesn't receive any options at this point.
};
} // namespace ConformingMethodList

class SwiftInterfaceGenMap {
  llvm::StringMap<SwiftInterfaceGenContextRef> IFaceGens;
  mutable llvm::sys::Mutex Mtx;

public:
  SwiftInterfaceGenContextRef get(StringRef Name) const;
  void set(StringRef Name, SwiftInterfaceGenContextRef IFaceGen);
  bool remove(StringRef Name);
  SwiftInterfaceGenContextRef find(StringRef ModuleName,
                                   const swift::CompilerInvocation &Invok);
};

struct SwiftCompletionCache
    : public ThreadSafeRefCountedBase<SwiftCompletionCache> {
  std::unique_ptr<swift::ide::CodeCompletionCache> inMemory;
  std::unique_ptr<swift::ide::OnDiskCodeCompletionCache> onDisk;
  swift::ide::CodeCompletionCache &getCache();
  SwiftCompletionCache() = default;
  ~SwiftCompletionCache();
};

struct SwiftPopularAPI : public ThreadSafeRefCountedBase<SwiftPopularAPI> {
  llvm::StringMap<CodeCompletion::PopularityFactor> nameToFactor;
};

struct SwiftCustomCompletions
    : public ThreadSafeRefCountedBase<SwiftCustomCompletions> {
  std::vector<CustomCompletionInfo> customCompletions;
};

class RequestRefactoringEditConsumer: public swift::ide::SourceEditConsumer,
                                      public swift::DiagnosticConsumer {
  class Implementation;
  Implementation &Impl;
public:
  RequestRefactoringEditConsumer(CategorizedEditsReceiver Receiver);
  ~RequestRefactoringEditConsumer();
  void accept(swift::SourceManager &SM, swift::ide::RegionType RegionType,
              ArrayRef<swift::ide::Replacement> Replacements) override;
  void handleDiagnostic(swift::SourceManager &SM,
                        const swift::DiagnosticInfo &Info) override;
};

class RequestRenameRangeConsumer : public swift::ide::FindRenameRangesConsumer,
                                   public swift::DiagnosticConsumer {
  class Implementation;
  Implementation &Impl;

public:
  RequestRenameRangeConsumer(CategorizedRenameRangesReceiver Receiver);
  ~RequestRenameRangeConsumer();
  void accept(swift::SourceManager &SM, swift::ide::RegionType RegionType,
              ArrayRef<swift::ide::RenameRangeDetail> Ranges) override;
  void handleDiagnostic(swift::SourceManager &SM,
                        const swift::DiagnosticInfo &Info) override;
};

namespace compile {
class Session {
  swift::ide::CompileInstance Compiler;

public:
  Session(const std::string &SwiftExecutablePath,
          const std::string &RuntimeResourcePath,
          const std::string &DiagnosticDocumentationPath,
          std::shared_ptr<swift::PluginRegistry> Plugins)
      : Compiler(SwiftExecutablePath, RuntimeResourcePath,
                 DiagnosticDocumentationPath, Plugins) {}

  bool
  performCompile(llvm::ArrayRef<const char *> Args,
                 llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
                 swift::DiagnosticConsumer *DiagC,
                 std::shared_ptr<std::atomic<bool>> CancellationFlag) {
    return Compiler.performCompile(Args, FileSystem, DiagC, CancellationFlag);
  }
};

class SessionManager {
  const std::string &SwiftExecutablePath;
  const std::string &RuntimeResourcePath;
  const std::string &DiagnosticDocumentationPath;
  const std::shared_ptr<swift::PluginRegistry> Plugins;

  llvm::StringMap<std::shared_ptr<Session>> sessions;
  WorkQueue compileQueue{WorkQueue::Dequeuing::Concurrent,
                         "sourcekit.swift.Compile"};
  mutable llvm::sys::Mutex mtx;

public:
  SessionManager(const std::string &SwiftExecutablePath,
                 const std::string &RuntimeResourcePath,
                 const std::string &DiagnosticDocumentationPath,
                 const std::shared_ptr<swift::PluginRegistry> Plugins)
      : SwiftExecutablePath(SwiftExecutablePath),
        RuntimeResourcePath(RuntimeResourcePath),
        DiagnosticDocumentationPath(DiagnosticDocumentationPath),
        Plugins(Plugins) {}

  std::shared_ptr<Session> getSession(StringRef name);

  void clearSession(StringRef name);

  void performCompileAsync(
      StringRef Name, ArrayRef<const char *> Args,
      llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem,
      std::shared_ptr<std::atomic<bool>> CancellationFlag,
      std::function<void(const RequestResult<CompilationResult> &)> Receiver);
};
} // namespace compile

struct SwiftStatistics {
#define SWIFT_STATISTIC(VAR, UID, DESC)                                        \
  Statistic VAR{UIdent{"source.statistic." #UID}, DESC};
#include "SwiftStatistics.def"
};

class SwiftLangSupport : public LangSupport {
  std::shared_ptr<NotificationCenter> NotificationCtr;
  /// The path of the swift-frontend executable.
  /// Used to find clang relative to it.
  std::string SwiftExecutablePath;
  std::string RuntimeResourcePath;
  std::string DiagnosticDocumentationPath;
  std::shared_ptr<SwiftASTManager> ASTMgr;
  std::shared_ptr<SwiftEditorDocumentFileMap> EditorDocuments;
  std::shared_ptr<RequestTracker> ReqTracker;
  SwiftInterfaceGenMap IFaceGenContexts;
  ThreadSafeRefCntPtr<SwiftCompletionCache> CCCache;
  ThreadSafeRefCntPtr<SwiftPopularAPI> PopularAPI;
  CodeCompletion::SessionCacheMap CCSessions;
  ThreadSafeRefCntPtr<SwiftCustomCompletions> CustomCompletions;
  std::shared_ptr<SwiftStatistics> Stats;
  llvm::StringMap<std::unique_ptr<FileSystemProvider>> FileSystemProviders;
  std::shared_ptr<swift::ide::IDEInspectionInstance> IDEInspectionInst;
  std::shared_ptr<compile::SessionManager> CompileManager;
  std::shared_ptr<swift::ide::SyntacticMacroExpansion> SyntacticMacroExpansions;

public:
  explicit SwiftLangSupport(SourceKit::Context &SKCtx);
  ~SwiftLangSupport();

  std::shared_ptr<NotificationCenter> getNotificationCenter() const {
    return NotificationCtr;
  }

  StringRef getRuntimeResourcePath() const { return RuntimeResourcePath; }
  StringRef getDiagnosticDocumentationPath() const {
    return DiagnosticDocumentationPath;
  }

  std::shared_ptr<SwiftASTManager> getASTManager() { return ASTMgr; }

  std::shared_ptr<SwiftEditorDocumentFileMap> getEditorDocuments() { return EditorDocuments; }
  SwiftInterfaceGenMap &getIFaceGenContexts() { return IFaceGenContexts; }
  IntrusiveRefCntPtr<SwiftCompletionCache> getCodeCompletionCache() {
    return CCCache;
  }

  std::shared_ptr<swift::ide::IDEInspectionInstance>
  getIDEInspectionInstance() {
    return IDEInspectionInst;
  }

  /// Returns the FileSystemProvider registered under Name, or nullptr if not
  /// found.
  FileSystemProvider *getFileSystemProvider(StringRef Name);

  /// Registers the given FileSystemProvider under Name. The caller is
  /// responsible for keeping FileSystemProvider alive at least as long as
  /// this Context.
  /// This should only be called during setup because it is not synchronized.
  /// \param FileSystemProvider must be non-null
  void setFileSystemProvider(StringRef Name, std::unique_ptr<FileSystemProvider> FileSystemProvider);

  /// Returns the filesystem appropriate to use in a request with the given
  /// \p vfsOptions and \p primaryFile.
  ///
  /// If \p vfsOptions is provided, returns a virtual filesystem created from
  /// a registered FileSystemProvider, or nullptr if there is an error.
  ///
  /// If \p vfsOptions is None and \p primaryFile is provided, returns the
  /// virtual filesystem associated with that editor document, if any.
  ///
  /// Otherwise, returns the real filesystem.
  ///
  /// \param vfsOptions Options to select and initialize the VFS, or None to
  ///                   get the real file system.
  /// \param error Set to a description of the error, if appropriate.
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>
  getFileSystem(const Optional<VFSOptions> &vfsOptions,
                Optional<StringRef> primaryFile, std::string &error);

  static SourceKit::UIdent getUIDForDeclLanguage(const swift::Decl *D);
  static SourceKit::UIdent getUIDForDecl(const swift::Decl *D,
                                         bool IsRef = false);
  static SourceKit::UIdent getUIDForExtensionOfDecl(const swift::Decl *D);
  static SourceKit::UIdent getUIDForLocalVar(bool IsRef = false);
  static SourceKit::UIdent getUIDForRefactoringKind(
      swift::ide::RefactoringKind Kind);
  static SourceKit::UIdent getUIDForCodeCompletionDeclKind(
      swift::ide::CodeCompletionDeclKind Kind, bool IsRef = false);
  static SourceKit::UIdent getUIDForAccessor(const swift::ValueDecl *D,
                                             swift::AccessorKind AccKind,
                                             bool IsRef = false);
  static SourceKit::UIdent getUIDForModuleRef();
  static SourceKit::UIdent getUIDForObjCAttr();
  static SourceKit::UIdent getUIDForSyntaxNodeKind(
      swift::ide::SyntaxNodeKind Kind);
  static SourceKit::UIdent getUIDForSyntaxStructureKind(
      swift::ide::SyntaxStructureKind Kind);
  static SourceKit::UIdent getUIDForSyntaxStructureElementKind(
      swift::ide::SyntaxStructureElementKind Kind);

  static SourceKit::UIdent getUIDForSymbol(swift::index::SymbolInfo sym,
                                           bool isRef);

  static SourceKit::UIdent getUIDForRangeKind(swift::ide::RangeKind Kind);

  static SourceKit::UIdent getUIDForRegionType(swift::ide::RegionType Type);

  static SourceKit::UIdent getUIDForRefactoringRangeKind(swift::ide::RefactoringRangeKind Kind);

  static Optional<UIdent> getUIDForDeclAttribute(const swift::DeclAttribute *Attr);

  static SourceKit::UIdent getUIDForFormalAccessScope(const swift::AccessScope Scope);

  static std::vector<UIdent> UIDsFromDeclAttributes(const swift::DeclAttributes &Attrs);

  static SourceKit::UIdent getUIDForNameKind(swift::ide::NameKind Kind);

  static swift::ide::NameKind getNameKindForUID(SourceKit::UIdent Id);

  static bool printDisplayName(const swift::ValueDecl *D, llvm::raw_ostream &OS);

  /// Generate a USR for a Decl, including the prefix.
  /// \returns true if the results should be ignored, false otherwise.
  static bool printUSR(const swift::ValueDecl *D, llvm::raw_ostream &OS);

  /// Generate a USR for the Type of a given decl.
  /// \returns true if the results should be ignored, false otherwise.
  static bool printDeclTypeUSR(const swift::ValueDecl *D, llvm::raw_ostream &OS);

  /// Generate a USR for of a given type.
  /// \returns true if the results should be ignored, false otherwise.
  static bool printTypeUSR(swift::Type Ty, llvm::raw_ostream &OS);

  /// Generate a USR for an accessor, including the prefix.
  /// \returns true if the results should be ignored, false otherwise.
  static bool printAccessorUSR(const swift::AbstractStorageDecl *D,
                               swift::AccessorKind AccKind,
                               llvm::raw_ostream &OS);

  /// Annotates a declaration with XML tags that describe the key substructure
  /// of the declaration for CursorInfo/DocInfo.
  ///
  /// Prints declarations with decl- and type-specific tags derived from the
  /// UIDs used for decl/refs.
  ///
  /// FIXME: This move to libIDE, but currently depends on the UIdentVisitor.
  static void printFullyAnnotatedDeclaration(const swift::ValueDecl *VD,
                                             swift::Type BaseTy,
                                             llvm::raw_ostream &OS);

  static void printFullyAnnotatedDeclaration(const swift::ExtensionDecl *VD,
                                             llvm::raw_ostream &OS);

  static void
  printFullyAnnotatedSynthesizedDeclaration(const swift::ValueDecl *VD,
                                            swift::TypeOrExtensionDecl Target,
                                            llvm::raw_ostream &OS);

  static void
  printFullyAnnotatedSynthesizedDeclaration(const swift::ExtensionDecl *ED,
                                            swift::TypeOrExtensionDecl Target,
                                            llvm::raw_ostream &OS);

  /// Print 'description' or 'sourcetext' the given \p VD to \p OS. If
  /// \p usePlaceholder is \c true, call argument positions are substituted with
  /// a typed editor placeholders which is suitable for 'sourcetext'.
  static void
  printMemberDeclDescription(const swift::ValueDecl *VD, swift::Type baseTy,
                             bool usePlaceholder, llvm::raw_ostream &OS);

  /// Tries to resolve the path to the real file-system path. If it fails it
  /// returns the original path;
  static std::string resolvePathSymlinks(StringRef FilePath);

  /// The result returned from \c performWithParamsToCompletionLikeOperation.
  struct CompletionLikeOperationParams {
    swift::CompilerInvocation &Invocation;
    llvm::MemoryBuffer *completionBuffer;
    swift::DiagnosticConsumer *DiagC;
    std::shared_ptr<std::atomic<bool>> CancellationFlag;
  };

  /// Execute \p PerformOperation synchronously with the parameters necessary to
  /// invoke a completion-like operation on \c IDEInspectionInstance.
  /// If \p InsertCodeCompletionToken is \c true, a code completion token will
  /// be inserted into the source buffer, if \p InsertCodeCompletionToken is \c
  /// false, the buffer is left as-is.
  void performWithParamsToCompletionLikeOperation(
      llvm::MemoryBuffer *UnresolvedInputFile, unsigned Offset,
      bool InsertCodeCompletionToken, ArrayRef<const char *> Args,
      llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
      SourceKitCancellationToken CancellationToken,
      llvm::function_ref<
          void(swift::ide::CancellableResult<CompletionLikeOperationParams>)>
          PerformOperation);

  //==========================================================================//
  // LangSupport Interface
  //==========================================================================//

  void globalConfigurationUpdated(std::shared_ptr<GlobalConfig> Config) override;

  void dependencyUpdated() override;

  void demangleNames(
      ArrayRef<const char *> MangledNames, bool Simplified,
      std::function<void(const RequestResult<ArrayRef<std::string>> &)>
          Receiver) override;

  void mangleSimpleClassNames(
      ArrayRef<std::pair<StringRef, StringRef>> ModuleClassPairs,
      std::function<void(const RequestResult<ArrayRef<std::string>> &)>
          Receiver) override;

  void indexSource(StringRef Filename, IndexingConsumer &Consumer,
                   ArrayRef<const char *> Args) override;

  void codeComplete(llvm::MemoryBuffer *InputBuf, unsigned Offset,
                    OptionsDictionary *options,
                    SourceKit::CodeCompletionConsumer &Consumer,
                    ArrayRef<const char *> Args,
                    Optional<VFSOptions> vfsOptions,
                    SourceKitCancellationToken CancellationToken) override;

  void codeCompleteOpen(StringRef name, llvm::MemoryBuffer *inputBuf,
                        unsigned offset, OptionsDictionary *options,
                        ArrayRef<FilterRule> rawFilterRules,
                        GroupedCodeCompletionConsumer &consumer,
                        ArrayRef<const char *> args,
                        Optional<VFSOptions> vfsOptions,
                        SourceKitCancellationToken CancellationToken) override;

  void codeCompleteClose(StringRef name, unsigned offset,
                         GroupedCodeCompletionConsumer &consumer) override;

  void codeCompleteUpdate(StringRef name, unsigned offset,
                          OptionsDictionary *options,
                          SourceKitCancellationToken CancellationToken,
                          GroupedCodeCompletionConsumer &consumer) override;

  void codeCompleteCacheOnDisk(StringRef path) override;

  void codeCompleteSetPopularAPI(ArrayRef<const char *> popularAPI,
                                 ArrayRef<const char *> unpopularAPI) override;

  void
  codeCompleteSetCustom(ArrayRef<CustomCompletionInfo> completions) override;

  void editorOpen(
      StringRef Name, llvm::MemoryBuffer *Buf, EditorConsumer &Consumer,
      ArrayRef<const char *> Args, Optional<VFSOptions> vfsOptions) override;

  void editorOpenInterface(EditorConsumer &Consumer,
                           StringRef Name,
                           StringRef ModuleName,
                           Optional<StringRef> Group,
                           ArrayRef<const char *> Args,
                           bool SynthesizedExtensions,
                           Optional<StringRef> InterestedUSR) override;

  void editorOpenTypeInterface(EditorConsumer &Consumer,
                               ArrayRef<const char *> Args,
                               StringRef TypeUSR) override;

  void editorOpenHeaderInterface(EditorConsumer &Consumer,
                                 StringRef Name,
                                 StringRef HeaderName,
                                 ArrayRef<const char *> Args,
                                 bool UsingSwiftArgs,
                                 bool SynthesizedExtensions,
                                 StringRef swiftVersion) override;

  void editorOpenSwiftSourceInterface(
      StringRef Name, StringRef SourceName, ArrayRef<const char *> Args,
      SourceKitCancellationToken CancellationToken,
      std::shared_ptr<EditorConsumer> Consumer) override;

  void editorClose(StringRef Name, bool RemoveCache) override;

  void editorReplaceText(StringRef Name, llvm::MemoryBuffer *Buf,
                         unsigned Offset, unsigned Length,
                         EditorConsumer &Consumer) override;

  void editorApplyFormatOptions(StringRef Name,
                                OptionsDictionary &FmtOptions) override;

  void editorFormatText(StringRef Name, unsigned Line, unsigned Length,
                        EditorConsumer &Consumer) override;

  void editorExtractTextFromComment(StringRef Source,
                                    EditorConsumer &Consumer) override;

  void editorConvertMarkupToXML(StringRef Source,
                                EditorConsumer &Consumer) override;

  void editorExpandPlaceholder(StringRef Name, unsigned Offset, unsigned Length,
                               EditorConsumer &Consumer) override;

  void getCursorInfo(StringRef PrimaryFilePath, StringRef InputBufferName,
                     unsigned Offset, unsigned Length, bool Actionables,
                     bool SymbolGraph, bool CancelOnSubsequentRequest,
                     ArrayRef<const char *> Args,
                     Optional<VFSOptions> vfsOptions,
                     SourceKitCancellationToken CancellationToken,
                     std::function<void(const RequestResult<CursorInfoData> &)>
                         Receiver) override;

  void
  getDiagnostics(StringRef PrimaryFilePath, ArrayRef<const char *> Args,
                 Optional<VFSOptions> VfsOptions,
                 SourceKitCancellationToken CancellationToken,
                 std::function<void(const RequestResult<DiagnosticsResult> &)>
                     Receiver) override;

  void getNameInfo(
      StringRef Filename, unsigned Offset, NameTranslatingInfo &Input,
      ArrayRef<const char *> Args, SourceKitCancellationToken CancellationToken,
      std::function<void(const RequestResult<NameTranslatingInfo> &)> Receiver)
      override;

  void getRangeInfo(
      StringRef PrimaryFilePath, StringRef InputBufferName, unsigned Offset,
      unsigned Length, bool CancelOnSubsequentRequest,
      ArrayRef<const char *> Args, SourceKitCancellationToken CancellationToken,
      std::function<void(const RequestResult<RangeInfo> &)> Receiver) override;

  void getCursorInfoFromUSR(
      StringRef PrimaryFilePath, StringRef InputBufferName, StringRef USR,
      bool CancelOnSubsequentRequest, ArrayRef<const char *> Args,
      Optional<VFSOptions> vfsOptions,
      SourceKitCancellationToken CancellationToken,
      std::function<void(const RequestResult<CursorInfoData> &)> Receiver)
      override;

  void findRelatedIdentifiersInFile(
      StringRef PrimaryFilePath, StringRef InputBufferName, unsigned Offset,
      bool CancelOnSubsequentRequest, ArrayRef<const char *> Args,
      SourceKitCancellationToken CancellationToken,
      std::function<void(const RequestResult<RelatedIdentsInfo> &)> Receiver)
      override;

  void findActiveRegionsInFile(
      StringRef PrimaryFilePath, StringRef InputBufferName,
      ArrayRef<const char *> Args, SourceKitCancellationToken CancellationToken,
      std::function<void(const RequestResult<ActiveRegionsInfo> &)> Receiver)
      override;

  void syntacticRename(llvm::MemoryBuffer *InputBuf,
                       ArrayRef<RenameLocations> RenameLocations,
                       ArrayRef<const char*> Args,
                       CategorizedEditsReceiver Receiver) override;

  void findRenameRanges(llvm::MemoryBuffer *InputBuf,
                        ArrayRef<RenameLocations> RenameLocations,
                        ArrayRef<const char *> Args,
                        CategorizedRenameRangesReceiver Receiver) override;

  void findLocalRenameRanges(StringRef Filename, unsigned Line, unsigned Column,
                             unsigned Length, ArrayRef<const char *> Args,
                             SourceKitCancellationToken CancellationToken,
                             CategorizedRenameRangesReceiver Receiver) override;

  void collectExpressionTypes(
      StringRef PrimaryFilePath, StringRef InputBufferName,
      ArrayRef<const char *> Args, ArrayRef<const char *> ExpectedProtocols,
      bool FullyQualified, bool CanonicalType,
      SourceKitCancellationToken CancellationToken,
      std::function<void(const RequestResult<ExpressionTypesInFile> &)>
          Receiver) override;

  void collectVariableTypes(
      StringRef PrimaryFilePath, StringRef InputBufferName,
      ArrayRef<const char *> Args, Optional<unsigned> Offset,
      Optional<unsigned> Length, bool FullyQualified,
      SourceKitCancellationToken CancellationToken,
      std::function<void(const RequestResult<VariableTypesInFile> &)> Receiver)
      override;

  void semanticRefactoring(StringRef PrimaryFilePath,
                           SemanticRefactoringInfo Info,
                           ArrayRef<const char *> Args,
                           SourceKitCancellationToken CancellationToken,
                           CategorizedEditsReceiver Receiver) override;

  void getDocInfo(llvm::MemoryBuffer *InputBuf,
                  StringRef ModuleName,
                  ArrayRef<const char *> Args,
                  DocInfoConsumer &Consumer) override;

  llvm::Optional<std::pair<unsigned, unsigned>>
      findUSRRange(StringRef DocumentName, StringRef USR) override;

  void findInterfaceDocument(StringRef ModuleName, ArrayRef<const char *> Args,
               std::function<void(const RequestResult<InterfaceDocInfo> &)> Receiver) override;

  void findModuleGroups(StringRef ModuleName, ArrayRef<const char *> Args,
               std::function<void(const RequestResult<ArrayRef<StringRef>> &)> Receiver) override;

  void getExpressionContextInfo(llvm::MemoryBuffer *inputBuf, unsigned Offset,
                                OptionsDictionary *options,
                                ArrayRef<const char *> Args,
                                SourceKitCancellationToken CancellationToken,
                                TypeContextInfoConsumer &Consumer,
                                Optional<VFSOptions> vfsOptions) override;

  void getConformingMethodList(llvm::MemoryBuffer *inputBuf, unsigned Offset,
                               OptionsDictionary *options,
                               ArrayRef<const char *> Args,
                               ArrayRef<const char *> ExpectedTypes,
                               SourceKitCancellationToken CancellationToken,
                               ConformingMethodListConsumer &Consumer,
                               Optional<VFSOptions> vfsOptions) override;

  void expandMacroSyntactically(llvm::MemoryBuffer *inputBuf,
                                ArrayRef<const char *> args,
                                ArrayRef<MacroExpansionInfo> expansions,
                                CategorizedEditsReceiver receiver) override;

  void
  performCompile(StringRef Name, ArrayRef<const char *> Args,
                 Optional<VFSOptions> vfsOptions,
                 SourceKitCancellationToken CancellationToken,
                 std::function<void(const RequestResult<CompilationResult> &)>
                     Receiver) override;

  void closeCompile(StringRef Name) override;

  void getStatistics(StatisticsReceiver) override;

private:
  swift::SourceFile *getSyntacticSourceFile(llvm::MemoryBuffer *InputBuf,
                                            ArrayRef<const char *> Args,
                                            swift::CompilerInstance &ParseCI,
                                            std::string &Error);
};

namespace trace {
  void initTraceInfo(trace::SwiftInvocation &SwiftArgs,
                     StringRef InputFile,
                     ArrayRef<const char *> Args);
  void initTraceInfo(trace::SwiftInvocation &SwiftArgs,
                     StringRef InputFile,
                     ArrayRef<std::string> Args);
}

/// When we cannot build any more clang modules, close the .pcm / files to
/// prevent fd leaks in clients that cache the AST.
// FIXME: Remove this once rdar://problem/19720334 is complete.
class CloseClangModuleFiles {
  swift::ClangModuleLoader &loader;

public:
  CloseClangModuleFiles(swift::ClangModuleLoader &loader) : loader(loader) {}
  ~CloseClangModuleFiles();
};


/// Disable expensive SIL options which do not affect indexing or diagnostics.
void disableExpensiveSILOptions(swift::SILOptions &Opts);

} // namespace SourceKit

#endif
