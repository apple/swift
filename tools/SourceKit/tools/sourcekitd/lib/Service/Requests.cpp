//===--- Requests.cpp -----------------------------------------------------===//
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

#include "sourcekitd/Service.h"
#include "sourcekitd/CodeCompletionResultsArray.h"
#include "sourcekitd/DictionaryKeys.h"
#include "sourcekitd/DocStructureArray.h"
#include "sourcekitd/DocSupportAnnotationArray.h"
#include "sourcekitd/TokenAnnotationsArray.h"
#include "sourcekitd/ExpressionTypeArray.h"
#include "sourcekitd/VariableTypeArray.h"

#include "SourceKit/Core/Context.h"
#include "SourceKit/Core/LangSupport.h"
#include "SourceKit/Core/NotificationCenter.h"
#include "SourceKit/Support/CancellationToken.h"
#include "SourceKit/Support/Concurrency.h"
#include "SourceKit/Support/Logging.h"
#include "SourceKit/Support/Statistic.h"
#include "SourceKit/Support/Tracing.h"
#include "SourceKit/Support/UIdent.h"
#include "SourceKit/SwiftLang/Factory.h"

#include "swift/Basic/InitializeSwiftModules.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/Version.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <mutex>

// FIXME: Portability.
#include <dispatch/dispatch.h>

using namespace sourcekitd;
using namespace SourceKit;

namespace {
class LazySKDUID {
  const char *Name;
  mutable std::atomic<sourcekitd_uid_t> UID { nullptr };
public:
  LazySKDUID(const char *Name) : Name(Name) { }

  sourcekitd_uid_t get() const {
    sourcekitd_uid_t UIDValue = UID;
    if (!UIDValue)
      UID = SKDUIDFromUIdent(UIdent(Name));
    return UID;
  }

  operator sourcekitd_uid_t() const {
    return get();
  }

  StringRef str() const {
    return StringRef(Name);
  }
};

struct SKEditorConsumerOptions {
  bool EnableSyntaxMap = false;
  bool EnableStructure = false;
  bool EnableDiagnostics = false;
  bool SyntacticOnly = false;
};

} // anonymous namespace

static Optional<UIdent> getUIDForOperationKind(trace::OperationKind OpKind);
static void fillDiagnosticInfo(ResponseBuilder::Dictionary ParentElem,
                               ArrayRef<DiagnosticEntryInfo> Diags,
                               Optional<UIdent> DiagStage);

#define REQUEST(NAME, CONTENT) static LazySKDUID Request##NAME(CONTENT);
#define KIND(NAME, CONTENT) static LazySKDUID Kind##NAME(CONTENT);
#include "SourceKit/Core/ProtocolUIDs.def"

#define REFACTORING(KIND, NAME, ID) static LazySKDUID Kind##Refactoring##KIND("source.refactoring.kind."#ID);
#include "swift/Refactoring/RefactoringKinds.def"

static SourceKit::Context *GlobalCtx = nullptr;

// NOTE: if we had a connection context, these stats should move into it.
static Statistic numRequests(UIdentFromSKDUID(KindStatNumRequests),
                             "# of requests (total)");
static Statistic numSemaRequests(UIdentFromSKDUID(KindStatNumSemaRequests),
                                 "# of semantic requests");

void sourcekitd::initializeService(
    llvm::StringRef swiftExecutablePath, StringRef runtimeLibPath,
    StringRef diagnosticDocumentationPath,
    std::function<void(sourcekitd_response_t)> postNotification) {
  INITIALIZE_LLVM();
  initializeSwiftModules();
  llvm::EnablePrettyStackTrace();
  GlobalCtx = new SourceKit::Context(swiftExecutablePath, runtimeLibPath,
                                     diagnosticDocumentationPath,
                                     SourceKit::createSwiftLangSupport);
  auto noteCenter = GlobalCtx->getNotificationCenter();

  noteCenter->addDocumentUpdateNotificationReceiver([postNotification](StringRef DocumentName) {
    static UIdent DocumentUpdateNotificationUID(
        "source.notification.editor.documentupdate");
    ResponseBuilder RespBuilder;
    auto Dict = RespBuilder.getDictionary();
    Dict.set(KeyNotification, DocumentUpdateNotificationUID);
    Dict.set(KeyName, DocumentName);
    postNotification(RespBuilder.createResponse());
  });

  noteCenter->addTestNotificationReceiver([postNotification] {
    static UIdent TestNotification("source.notification.test");
    ResponseBuilder RespBuilder;
    auto Dict = RespBuilder.getDictionary();
    Dict.set(KeyNotification, TestNotification);
    postNotification(RespBuilder.createResponse());
  });

  noteCenter->addSemaEnabledNotificationReceiver([postNotification] {
    static UIdent SemaEnabledNotificationUID(
        "source.notification.sema_enabled");
    ResponseBuilder RespBuilder;
    auto Dict = RespBuilder.getDictionary();
    Dict.set(KeyNotification, SemaEnabledNotificationUID);
    postNotification(RespBuilder.createResponse());
  });

  noteCenter->addCompileWillStartNotificationReceiver([postNotification](uint64_t OpId, trace::OperationKind OpKind, const trace::SwiftInvocation &Inv){
    static UIdent CompileWillStartUID("source.notification.compile-will-start");
    ResponseBuilder RespBuilder;
    auto Dict = RespBuilder.getDictionary();
    Dict.set(KeyNotification, CompileWillStartUID);
    Dict.set(KeyCompileID, std::to_string(OpId));
    Dict.set(KeyFilePath, Inv.Args.PrimaryFile);
    if (auto OperationUID = getUIDForOperationKind(OpKind))
      Dict.set(KeyCompileOperation, OperationUID.value());
    Dict.set(KeyCompilerArgsString, Inv.Args.Arguments);
    postNotification(RespBuilder.createResponse());
  });

  noteCenter->addCompileDidFinishNotificationReceiver([postNotification](uint64_t OpId, trace::OperationKind OpKind, ArrayRef<DiagnosticEntryInfo> Diagnostics){
    static UIdent CompileDidFinishUID("source.notification.compile-did-finish");
    ResponseBuilder RespBuilder;
    auto Dict = RespBuilder.getDictionary();
    Dict.set(KeyNotification, CompileDidFinishUID);
    Dict.set(KeyCompileID, std::to_string(OpId));
    if (auto OperationUID = getUIDForOperationKind(OpKind))
      Dict.set(KeyCompileOperation, OperationUID.value());

    fillDiagnosticInfo(Dict, Diagnostics, /*DiagStage*/ None);
    postNotification(RespBuilder.createResponse());
  });
}

void sourcekitd::shutdownService() {
  delete GlobalCtx;
  GlobalCtx = nullptr;
}

static SourceKit::Context &getGlobalContext() {
  assert(GlobalCtx);
  return *GlobalCtx;
}

static sourcekitd_response_t indexSource(StringRef Filename,
                                         ArrayRef<const char *> Args);

static sourcekitd_response_t reportDocInfo(llvm::MemoryBuffer *InputBuf,
                                           StringRef ModuleName,
                                           ArrayRef<const char *> Args);

static void reportCursorInfo(const RequestResult<CursorInfoData> &Result, ResponseReceiver Rec);

static void reportDiagnostics(const RequestResult<DiagnosticsResult> &Result,
                              ResponseReceiver Rec);

static void reportExpressionTypeInfo(const RequestResult<ExpressionTypesInFile> &Result,
                                     ResponseReceiver Rec);

static void
reportVariableTypeInfo(const RequestResult<VariableTypesInFile> &Result,
                       ResponseReceiver Rec);

static void reportRangeInfo(const RequestResult<RangeInfo> &Result, ResponseReceiver Rec);

static void reportNameInfo(const RequestResult<NameTranslatingInfo> &Result, ResponseReceiver Rec);

static void findRelatedIdents(StringRef PrimaryFilePath,
                              StringRef InputBufferName, int64_t Offset,
                              bool CancelOnSubsequentRequest,
                              ArrayRef<const char *> Args,
                              SourceKitCancellationToken CancellationToken,
                              ResponseReceiver Rec);

static void findActiveRegions(StringRef PrimaryFilePath,
                              StringRef InputBufferName,
                              ArrayRef<const char *> Args,
                              SourceKitCancellationToken CancellationToken,
                              ResponseReceiver Rec);

static sourcekitd_response_t
codeComplete(llvm::MemoryBuffer *InputBuf, int64_t Offset,
             Optional<RequestDict> optionsDict, ArrayRef<const char *> Args,
             Optional<VFSOptions> vfsOptions,
             SourceKitCancellationToken CancellationToken);

static sourcekitd_response_t
codeCompleteOpen(StringRef name, llvm::MemoryBuffer *InputBuf, int64_t Offset,
                 Optional<RequestDict> optionsDict, ArrayRef<const char *> Args,
                 Optional<VFSOptions> vfsOptions,
                 SourceKitCancellationToken CancellationToken);

static sourcekitd_response_t
codeCompleteUpdate(StringRef name, int64_t Offset,
                   Optional<RequestDict> optionsDict,
                   SourceKitCancellationToken CancellationToken);

static sourcekitd_response_t codeCompleteClose(StringRef name, int64_t Offset);

static sourcekitd_response_t
typeContextInfo(llvm::MemoryBuffer *InputBuf, int64_t Offset,
                Optional<RequestDict> optionsDict, ArrayRef<const char *> Args,
                Optional<VFSOptions> vfsOptions,
                SourceKitCancellationToken CancellationToken);

static sourcekitd_response_t conformingMethodList(
    llvm::MemoryBuffer *InputBuf, int64_t Offset,
    Optional<RequestDict> optionsDict, ArrayRef<const char *> Args,
    ArrayRef<const char *> ExpectedTypes, Optional<VFSOptions> vfsOptions,
    SourceKitCancellationToken CancellationToken);

static sourcekitd_response_t
editorOpen(StringRef Name, llvm::MemoryBuffer *Buf,
           SKEditorConsumerOptions Opts, ArrayRef<const char *> Args,
           Optional<VFSOptions> vfsOptions);

static sourcekitd_response_t
editorOpenInterface(StringRef Name, StringRef ModuleName,
                    Optional<StringRef> Group, ArrayRef<const char *> Args,
                    bool SynthesizedExtensions,
                    Optional<StringRef> InterestedUSR);

static sourcekitd_response_t
editorOpenHeaderInterface(StringRef Name, StringRef HeaderName,
                          ArrayRef<const char *> Args,
                          bool UsingSwiftArgs,
                          bool SynthesizedExtensions,
                          StringRef swiftVersion);

static void editorOpenSwiftSourceInterface(
    StringRef Name, StringRef SourceName, ArrayRef<const char *> Args,
    SourceKitCancellationToken CancellationToken, ResponseReceiver Rec);

static void
editorOpenSwiftTypeInterface(StringRef TypeUsr, ArrayRef<const char *> Args,
                             ResponseReceiver Rec);

static sourcekitd_response_t editorExtractTextFromComment(StringRef Source);

static sourcekitd_response_t editorConvertMarkupToXML(StringRef Source);

static sourcekitd_response_t
editorClose(StringRef Name, bool RemoveCache);

static sourcekitd_response_t
editorReplaceText(StringRef Name, llvm::MemoryBuffer *Buf, unsigned Offset,
                  unsigned Length, SKEditorConsumerOptions Opts);

static void
editorApplyFormatOptions(StringRef Name, RequestDict &FmtOptions);

static sourcekitd_response_t
editorFormatText(StringRef Name, unsigned Line, unsigned Length);

static sourcekitd_response_t
editorExpandPlaceholder(StringRef Name, unsigned Offset, unsigned Length);

static sourcekitd_response_t
editorFindUSR(StringRef DocumentName, StringRef USR);

static sourcekitd_response_t
editorFindInterfaceDoc(StringRef ModuleName, ArrayRef<const char *> Args);

static sourcekitd_response_t
editorFindModuleGroups(StringRef ModuleName, ArrayRef<const char *> Args);

static bool
buildRenameLocationsFromDict(const RequestDict &Req, bool UseNewName,
                             std::vector<RenameLocations> &RenameLocations,
                             llvm::SmallString<64> &Error);

static sourcekitd_response_t
createCategorizedEditsResponse(
    const RequestResult<ArrayRef<CategorizedEdits>> &Result);

static sourcekitd_response_t
syntacticRename(llvm::MemoryBuffer *InputBuf,
                ArrayRef<RenameLocations> RenameLocations,
                ArrayRef<const char*> Args);

static sourcekitd_response_t
createCategorizedRenameRangesResponse(
    const RequestResult<ArrayRef<CategorizedRenameRanges>> &Result);

static sourcekitd_response_t
findRenameRanges(llvm::MemoryBuffer *InputBuf,
                 ArrayRef<RenameLocations> RenameLocations,
                 ArrayRef<const char *> Args);

static bool isSemanticEditorDisabled();
static void enableCompileNotifications(bool value);

namespace {
class SKOptionsDictionary : public OptionsDictionary {
  RequestDict Options;

public:
  explicit SKOptionsDictionary(RequestDict Options) : Options(Options) {}

  bool valueForOption(UIdent Key, unsigned &Val) override {
    int64_t result;
    if (Options.getInt64(Key, result, false))
      return false;
    Val = static_cast<unsigned>(result);
    return true;
  }

  bool valueForOption(UIdent Key, bool &Val) override {
    int64_t result;
    if (Options.getInt64(Key, result, false))
      return false;
    Val = result ? true : false;
    return true;
  }

  bool valueForOption(UIdent Key, StringRef &Val) override {
    Optional<StringRef> value = Options.getString(Key);
    if (!value)
      return false;
    Val = *value;
    return true;
  }

  bool forEach(UIdent key, llvm::function_ref<bool(OptionsDictionary &)> applier) override {
    return Options.dictionaryArrayApply(key, [=](RequestDict dict) {
      SKOptionsDictionary skDict(dict);
      return applier(skDict);
    });
  }
};
} // anonymous namespace

static void handleRequestImpl(sourcekitd_object_t Req,
                              SourceKitCancellationToken CancellationToken,
                              ResponseReceiver Receiver);

void sourcekitd::handleRequest(sourcekitd_object_t Req,
                               SourceKitCancellationToken CancellationToken,
                               ResponseReceiver Receiver) {
  LOG_SECTION("handleRequest-before", InfoHighPrio) {
    sourcekitd::printRequestObject(Req, Log->getOS());
  }

  handleRequestImpl(
      Req, CancellationToken,
      [Receiver, CancellationToken](sourcekitd_response_t Resp) {
        LOG_SECTION("handleRequest-after", InfoHighPrio) {
          // Responses are big, print them out with info medium priority.
          if (Logger::isLoggingEnabledForLevel(Logger::Level::InfoMediumPrio))
            sourcekitd::printResponse(Resp, Log->getOS());
        }

        sourcekitd::disposeCancellationToken(CancellationToken);

        Receiver(Resp);
      });
}

void sourcekitd::cancelRequest(SourceKitCancellationToken CancellationToken) {
  getGlobalContext().getRequestTracker()->cancel(CancellationToken);
}

void sourcekitd::disposeCancellationToken(
    SourceKitCancellationToken CancellationToken) {
  getGlobalContext().getRequestTracker()->stopTracking(CancellationToken);
}

bool sourcekitd::requestIsBarrier(sourcekitd_object_t ReqObj) {
  RequestDict Req(ReqObj);
  sourcekitd_uid_t ReqUID = Req.getUID(KeyRequest);
  return ReqUID == RequestEditorOpen || ReqUID == RequestEditorReplaceText ||
         ReqUID == RequestEditorClose;
}

bool sourcekitd::requestIsEnableBarriers(sourcekitd_object_t ReqObj) {
  RequestDict Req(ReqObj);
  sourcekitd_uid_t ReqUID = Req.getUID(KeyRequest);
  return ReqUID == RequestEnableRequestBarriers;
}

void sourcekitd::sendBarriersEnabledResponse(ResponseReceiver Receiver) {
  ResponseBuilder RespBuilder;
  auto Elem = RespBuilder.getDictionary();
  Elem.setBool(KeyBarriersEnabled, true);
  Receiver(RespBuilder.createResponse());
}

static std::unique_ptr<llvm::MemoryBuffer> getInputBufForRequest(
    Optional<StringRef> SourceFile, Optional<StringRef> SourceText,
    const Optional<VFSOptions> &vfsOptions, llvm::SmallString<64> &ErrBuf) {
  std::unique_ptr<llvm::MemoryBuffer> InputBuf;

  if (SourceText.has_value()) {
    StringRef BufName;
    if (SourceFile.has_value())
      BufName = *SourceFile;
    else
      BufName = "<input>";
    InputBuf = llvm::MemoryBuffer::getMemBuffer(*SourceText, BufName);

  } else if (vfsOptions.has_value() && SourceFile.has_value()) {
    ErrBuf = "using 'key.sourcefile' to read source text from the filesystem "
             "is not supported when using 'key.vfs.name'";
    return nullptr;

  } else if (SourceFile.has_value()) {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
        llvm::MemoryBuffer::getFile(*SourceFile);
    if (FileBufOrErr) {
      InputBuf = std::move(FileBufOrErr.get());
    } else {
      llvm::raw_svector_ostream OSErr(ErrBuf);
      OSErr << "error opening input file '" << *SourceFile << "' ("
            << FileBufOrErr.getError().message() << ')';
      return nullptr;
    }
  } else {
    InputBuf = llvm::WritableMemoryBuffer::getNewMemBuffer(0, "<input>");
  }

  return InputBuf;
}

/// Get the input buffer from 'key.sourcefile' or 'key.sourcetext' value in
/// \p Req . If buffer cannot be retrieved for some reason, Reply an error to
/// \c Rec and returns \c nullptr .
static std::unique_ptr<llvm::MemoryBuffer>
getInputBufForRequestOrEmitError(const RequestDict &Req,
                                 const Optional<VFSOptions> &vfsOptions,
                                 ResponseReceiver Rec) {
  Optional<StringRef> SourceFile = Req.getString(KeySourceFile);
  Optional<StringRef> SourceText = Req.getString(KeySourceText);
  SmallString<64> ErrBuf;
  auto buf = getInputBufForRequest(SourceFile, SourceText, vfsOptions, ErrBuf);
  if (!buf) {
    Rec(createErrorRequestFailed(ErrBuf));
  }
  return buf;
}

/// Retrieves `key.primary_file` value as a string, or `key.sourcefile` if
/// missing. If both are missing, reply with an error and return \c None.
///
/// The "primary file" is the file to mark as `-primary-file` when building
/// the corresponding AST for this request. The "input file" is the file to
/// resolve any offset or line/column in. These were (prior to
/// \c GeneratedSourceInfo) always the same, but the input file is now able to
/// be a generated buffer name (where that buffer is created during the AST
/// build).
static Optional<StringRef>
getPrimaryFilePathForRequestOrEmitError(const RequestDict &Req,
                                        ResponseReceiver Rec) {
  Optional<StringRef> PrimaryFilePath = Req.getString(KeyPrimaryFile);
  if (!PrimaryFilePath) {
    // Fallback to the old key.sourcefile
    PrimaryFilePath = Req.getString(KeySourceFile);
    if (!PrimaryFilePath) {
      Rec(createErrorRequestInvalid(
          "missing 'key.primary_file' and 'key.sourcefile'"));
    }
  }
  return PrimaryFilePath;
}

/// Retrieves `key.sourcefile` only if it is different to `key.primary_file`.
/// Returns an empty string if it's the same or if it was missing. Callers are
/// expected to pull the primary file from the AST if the input file is
/// empty.
///
/// See \c getPrimaryFilePathForRequestOrEmitError for an explanation of primary
/// vs input file.
static StringRef getInputBufferNameForRequest(const RequestDict &Req,
                                              ResponseReceiver Rec) {
  Optional<StringRef> PrimaryFilePath = Req.getString(KeyPrimaryFile);
  Optional<StringRef> InputBufferName = Req.getString(KeySourceFile);
  if (!PrimaryFilePath || PrimaryFilePath == InputBufferName)
    return "";
  return InputBufferName.value_or("");
}

/// Get compiler arguments from 'key.compilerargs' in \p Req . If the key is
/// missing, reply an error to \c Rec and returns \c true .
static bool
getCompilerArgumentsForRequestOrEmitError(const RequestDict &Req,
                                          SmallVectorImpl<const char *> &Args,
                                          ResponseReceiver Rec) {
  bool Failed = Req.getStringArray(KeyCompilerArgs, Args, /*isOptional=*/true);
  if (Failed) {
    Rec(createErrorRequestInvalid(
        "'key.compilerargs' not an array of strings"));
    return true;
  }
  return false;
}

/// Read optional VFSOptions from a request dictionary. The request dictionary
/// *must* outlive the resulting VFSOptions.
/// \returns true on failure and sets \p error.
static Optional<VFSOptions> getVFSOptions(const RequestDict &Req) {
  auto name = Req.getString(KeyVFSName);
  if (!name)
    return None;

  std::unique_ptr<OptionsDictionary> options;
  if (auto dict = Req.getDictionary(KeyVFSOptions)) {
    options = std::make_unique<SKOptionsDictionary>(*dict);
  }

  return VFSOptions{name->str(), std::move(options)};
}

static bool checkVFSNotSupported(const RequestDict &Req, ResponseReceiver Rec) {
  if (Req.getString(KeyVFSName)) {
    Rec(createErrorRequestInvalid("This request does not support custom filesystems"));
    return true;
  }
  return false;
}

static void handleSemanticRequest(const RequestDict &Req, ResponseReceiver Rec,
                                  std::function<void()> Fn) {
  if (isSemanticEditorDisabled())
    return Rec(createErrorRequestFailed("semantic editor is disabled"));

  // Typechecking arrays can blow up the stack currently.
  // Run them under a malloc'ed stack.

  static WorkQueue SemaQueue{WorkQueue::Dequeuing::Concurrent,
                             "sourcekit.request.semantic"};
  ++numSemaRequests;
  SemaQueue.dispatch([Fn] { Fn(); }, /*isStackDeep=*/true);
}

static void
handleRequestGlobalConfiguration(const RequestDict &Req,
                                 SourceKitCancellationToken CancellationToken,
                                 ResponseReceiver Rec) {
  {
    auto Config = getGlobalContext().getGlobalConfiguration();
    ResponseBuilder RB;
    auto dict = RB.getDictionary();

    Optional<unsigned> CompletionMaxASTContextReuseCount =
        Req.getOptionalInt64(KeyCompletionMaxASTContextReuseCount)
            .transform([](int64_t v) -> unsigned { return v; });
    Optional<unsigned> CompletionCheckDependencyInterval =
        Req.getOptionalInt64(KeyCompletionCheckDependencyInterval)
            .transform([](int64_t v) -> unsigned { return v; });

    GlobalConfig::Settings UpdatedConfig =
        Config->update(CompletionMaxASTContextReuseCount,
                       CompletionCheckDependencyInterval);

    getGlobalContext().getSwiftLangSupport().globalConfigurationUpdated(Config);

    dict.set(KeyCompletionMaxASTContextReuseCount,
             UpdatedConfig.IDEInspectionOpts.MaxASTContextReuseCount);
    dict.set(KeyCompletionCheckDependencyInterval,
             UpdatedConfig.IDEInspectionOpts.CheckDependencyInterval);

    return Rec(RB.createResponse());
  }
}

static void
handleRequestProtocolVersion(const RequestDict &Req,
                             SourceKitCancellationToken CancellationToken,
                             ResponseReceiver Rec) {
  {
    ResponseBuilder RB;
    auto dict = RB.getDictionary();
    dict.set(KeyVersionMajor, ProtocolMajorVersion);
    dict.set(KeyVersionMinor, static_cast<int64_t>(ProtocolMinorVersion));
    return Rec(RB.createResponse());
  }
}

static void
handleRequestCompilerVersion(const RequestDict &Req,
                             SourceKitCancellationToken CancellationToken,
                             ResponseReceiver Rec) {
  {
    ResponseBuilder RB;
    auto dict = RB.getDictionary();
    auto thisVersion = swift::version::Version::getCurrentLanguageVersion();
    dict.set(KeyVersionMajor, static_cast<int64_t>(thisVersion[0]));
    dict.set(KeyVersionMinor, static_cast<int64_t>(thisVersion[1]));
    if (thisVersion.size() > 2)
      dict.set(KeyVersionPatch, static_cast<int64_t>(thisVersion[2]));
    else
      dict.set(KeyVersionPatch, static_cast<int64_t>(0));
    return Rec(RB.createResponse());
  }
}

static void
handleRequestCrashWithExit(const RequestDict &Req,
                           SourceKitCancellationToken CancellationToken,
                           ResponseReceiver Rec) {
  {
    // 'exit' has the same effect as crashing but without the crash log.
    ::exit(1);
  }
}

static void
handleRequestTestNotification(const RequestDict &Req,
                              SourceKitCancellationToken CancellationToken,
                              ResponseReceiver Rec) {
  {
    getGlobalContext().getNotificationCenter()->postTestNotification();
    return Rec(ResponseBuilder().createResponse());
  }
}

static void handleRequestDemangle(const RequestDict &Req,
                                  SourceKitCancellationToken CancellationToken,
                                  ResponseReceiver Rec) {
  {
    SmallVector<const char *, 8> MangledNames;
    bool Failed = Req.getStringArray(KeyNames, MangledNames, /*isOptional=*/true);
    if (Failed) {
      return Rec(createErrorRequestInvalid(
                                        "'key.names' not an array of strings"));
    }
    int64_t Simplified = false;
    Req.getInt64(KeySimplified, Simplified, /*isOptional=*/true);

    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
    Lang.demangleNames(MangledNames, Simplified, [Rec](auto result) {
      if (result.isError())
        return Rec(createErrorRequestFailed(result.getError()));
      if (result.isCancelled())
        return Rec(createErrorRequestFailed(result.getError()));
      ResponseBuilder RespBuilder;
      auto Arr = RespBuilder.getDictionary().setArray(KeyResults);
      for (auto demangldedName : result.value()) {
        auto Entry = Arr.appendDictionary();
        Entry.set(KeyName, demangldedName.c_str());
      }
      Rec(RespBuilder.createResponse());
    });
  }
}

static void
handleRequestMangleSimpleClass(const RequestDict &Req,
                               SourceKitCancellationToken CancellationToken,
                               ResponseReceiver Rec) {
  {
    SmallVector<std::pair<StringRef, StringRef>, 16> ModuleClassPairs;
    sourcekitd_response_t err = nullptr;
    bool failed = Req.dictionaryArrayApply(KeyNames, [&](RequestDict dict) {
      Optional<StringRef> ModuleName = dict.getString(KeyModuleName);
      if (!ModuleName.has_value()) {
        err = createErrorRequestInvalid("missing 'key.modulename'");
        return true;
      }
      Optional<StringRef> ClassName = dict.getString(KeyName);
      if (!ClassName.has_value()) {
        err = createErrorRequestInvalid("missing 'key.name'");
        return true;
      }
      ModuleClassPairs.push_back(std::make_pair(*ModuleName, *ClassName));
      return false;
    });

    if (failed) {
      if (!err)
        err = createErrorRequestInvalid("missing 'key.names'");
      return Rec(err);
    }

    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
    Lang.mangleSimpleClassNames(ModuleClassPairs, [Rec](auto result) {
      if (result.isError())
        return Rec(createErrorRequestFailed(result.getError()));
      if (result.isCancelled())
        return Rec(createErrorRequestFailed(result.getError()));
      ResponseBuilder RespBuilder;
      auto Arr = RespBuilder.getDictionary().setArray(KeyResults);
      for (auto &mangledName : result.value()) {
        auto Entry = Arr.appendDictionary();
        Entry.set(KeyName, mangledName.c_str());
      }
      Rec(RespBuilder.createResponse());
    });
  }
}

static void handleRequestEnableCompileNotifications(
    const RequestDict &Req, SourceKitCancellationToken CancellationToken,
    ResponseReceiver Rec) {
  {
    int64_t value = true;
    if (Req.getInt64(KeyValue, value, /*isOptional=*/false)) {
      return Rec(createErrorRequestInvalid("missing 'key.value'"));
    }
    enableCompileNotifications(value);
    return Rec(ResponseBuilder().createResponse());
  }
}

static void
handleRequestBuildSettingsRegister(const RequestDict &Req,
                                   SourceKitCancellationToken CancellationToken,
                                   ResponseReceiver Rec) {
  // Just accept 'source.request.buildsettings.register' for now, don't do
  // anything else.
  // FIXME: Heavy WIP here.
  { return Rec(ResponseBuilder().createResponse()); }
}

static void
handleRequestDependencyUpdated(const RequestDict &Req,
                               SourceKitCancellationToken CancellationToken,
                               ResponseReceiver Rec) {
  {
    getGlobalContext().getSwiftLangSupport().dependencyUpdated();
    return Rec(ResponseBuilder().createResponse());
  }
}

static void handleRequestDocInfo(const RequestDict &Req,
                                 SourceKitCancellationToken CancellationToken,
                                 ResponseReceiver Rec) {
  {
    Optional<VFSOptions> vfsOptions = getVFSOptions(Req);
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    std::unique_ptr<llvm::MemoryBuffer> InputBuf =
        getInputBufForRequestOrEmitError(Req, vfsOptions, Rec);
    if (!InputBuf)
      return;
    StringRef ModuleName;
    Optional<StringRef> ModuleNameOpt = Req.getString(KeyModuleName);
    if (ModuleNameOpt.has_value()) ModuleName = *ModuleNameOpt;
    return Rec(reportDocInfo(InputBuf.get(), ModuleName, Args));
  }
}

static void
handleRequestEditorOpen(const RequestDict &Req,
                        SourceKitCancellationToken CancellationToken,
                        ResponseReceiver Rec) {
  {
    Optional<VFSOptions> vfsOptions = getVFSOptions(Req);
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    Optional<StringRef> Name = Req.getString(KeyName);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.name'"));
    std::unique_ptr<llvm::MemoryBuffer> InputBuf =
        getInputBufForRequestOrEmitError(Req, vfsOptions, Rec);
    if (!InputBuf)
      return;
    int64_t EnableSyntaxMap = true;
    Req.getInt64(KeyEnableSyntaxMap, EnableSyntaxMap, /*isOptional=*/true);
    int64_t EnableStructure = true;
    Req.getInt64(KeyEnableStructure, EnableStructure, /*isOptional=*/true);
    int64_t EnableDiagnostics = true;
    Req.getInt64(KeyEnableDiagnostics, EnableDiagnostics, /*isOptional=*/true);
    int64_t SyntacticOnly = false;
    Req.getInt64(KeySyntacticOnly, SyntacticOnly, /*isOptional=*/true);

    SKEditorConsumerOptions Opts;
    Opts.EnableSyntaxMap = EnableSyntaxMap;
    Opts.EnableStructure = EnableStructure;
    Opts.EnableDiagnostics = EnableDiagnostics;
    Opts.SyntacticOnly = SyntacticOnly;
    return Rec(editorOpen(*Name, InputBuf.get(), Opts, Args, std::move(vfsOptions)));
  }
}

static void
handleRequestEditorClose(const RequestDict &Req,
                         SourceKitCancellationToken CancellationToken,
                         ResponseReceiver Rec) {
  {
    Optional<StringRef> Name = Req.getString(KeyName);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.name'"));

    // Whether we remove the cached AST from libcache, by default, false.
    int64_t RemoveCache = false;
    Req.getInt64(KeyRemoveCache, RemoveCache, /*isOptional=*/true);
    return Rec(editorClose(*Name, RemoveCache));
  }
}

static void
handleRequestEditorReplaceText(const RequestDict &Req,
                               SourceKitCancellationToken CancellationToken,
                               ResponseReceiver Rec) {
  {
    Optional<VFSOptions> vfsOptions = getVFSOptions(Req);
    Optional<StringRef> Name = Req.getString(KeyName);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.name'"));
    std::unique_ptr<llvm::MemoryBuffer> InputBuf =
        getInputBufForRequestOrEmitError(Req, vfsOptions, Rec);
    if (!InputBuf)
      return;
    int64_t Offset = 0;
    Req.getInt64(KeyOffset, Offset, /*isOptional=*/true);
    int64_t Length = 0;
    Req.getInt64(KeyLength, Length, /*isOptional=*/true);
    int64_t EnableSyntaxMap = true;
    Req.getInt64(KeyEnableSyntaxMap, EnableSyntaxMap, /*isOptional=*/true);
    int64_t EnableStructure = true;
    Req.getInt64(KeyEnableStructure, EnableStructure, /*isOptional=*/true);
    int64_t EnableDiagnostics = true;
    Req.getInt64(KeyEnableDiagnostics, EnableDiagnostics, /*isOptional=*/true);
    int64_t SyntacticOnly = false;
    Req.getInt64(KeySyntacticOnly, SyntacticOnly, /*isOptional=*/true);

    SKEditorConsumerOptions Opts;
    Opts.EnableSyntaxMap = EnableSyntaxMap;
    Opts.EnableStructure = EnableStructure;
    Opts.EnableDiagnostics = EnableDiagnostics;
    Opts.SyntacticOnly = SyntacticOnly;

    return Rec(editorReplaceText(*Name, InputBuf.get(), Offset, Length, Opts));
  }
}

static void
handleRequestEditorFormatText(const RequestDict &Req,
                              SourceKitCancellationToken CancellationToken,
                              ResponseReceiver Rec) {
  {
    Optional<StringRef> Name = Req.getString(KeyName);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.name'"));
    Optional<RequestDict> FmtOptions = Req.getDictionary(KeyFormatOptions);
    if (FmtOptions.has_value())
      editorApplyFormatOptions(*Name, *FmtOptions);
    int64_t Line = 0;
    Req.getInt64(KeyLine, Line, /*isOptional=*/false);
    int64_t Length = 0;
    Req.getInt64(KeyLength, Length, /*isOptional=*/true);
    return Rec(editorFormatText(*Name, Line, Length));
  }
}

static void handleRequestEditorExpandPlaceholder(
    const RequestDict &Req, SourceKitCancellationToken CancellationToken,
    ResponseReceiver Rec) {
  {
    Optional<StringRef> Name = Req.getString(KeyName);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.name'"));
    int64_t Offset = 0;
    Req.getInt64(KeyOffset, Offset, /*isOptional=*/false);
    int64_t Length = 0;
    Req.getInt64(KeyLength, Length, /*isOptional=*/false);
    return Rec(editorExpandPlaceholder(*Name, Offset, Length));
  }
}

static void
handleRequestEditorOpenInterface(const RequestDict &Req,
                                 SourceKitCancellationToken CancellationToken,
                                 ResponseReceiver Rec) {
  {
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    Optional<StringRef> Name = Req.getString(KeyName);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.name'"));
    Optional<StringRef> ModuleName = Req.getString(KeyModuleName);
    if (!ModuleName.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.modulename'"));
    Optional<StringRef> GroupName = Req.getString(KeyGroupName);
    int64_t SynthesizedExtension = false;
    Req.getInt64(KeySynthesizedExtension, SynthesizedExtension,
                 /*isOptional=*/true);
    Optional<StringRef> InterestedUSR = Req.getString(KeyInterestedUSR);
    return Rec(editorOpenInterface(*Name, *ModuleName, GroupName, Args,
                                   SynthesizedExtension, InterestedUSR));
  }
}

static void handleRequestEditorOpenHeaderInterface(
    const RequestDict &Req, SourceKitCancellationToken CancellationToken,
    ResponseReceiver Rec) {
  {
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    Optional<StringRef> Name = Req.getString(KeyName);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.name'"));
    Optional<StringRef> HeaderName = Req.getString(KeyFilePath);
    if (!HeaderName.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.filepath'"));
    int64_t SynthesizedExtension = false;
    Req.getInt64(KeySynthesizedExtension, SynthesizedExtension,
                 /*isOptional=*/true);
    Optional<int64_t> UsingSwiftArgs = Req.getOptionalInt64(KeyUsingSwiftArgs);
    std::string swiftVer;
    Optional<StringRef> swiftVerValStr = Req.getString(KeySwiftVersion);
    if (swiftVerValStr.has_value()) {
      swiftVer = swiftVerValStr.value().str();
    } else {
      Optional<int64_t> swiftVerVal = Req.getOptionalInt64(KeySwiftVersion);
      if (swiftVerVal.has_value())
        swiftVer = std::to_string(*swiftVerVal);
    }
    return Rec(editorOpenHeaderInterface(*Name, *HeaderName, Args,
                                         UsingSwiftArgs.value_or(false),
                                         SynthesizedExtension, swiftVer));
  }
}

static void handleRequestEditorOpenSwiftSourceInterface(
    const RequestDict &Req, SourceKitCancellationToken CancellationToken,
    ResponseReceiver Rec) {
  {
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    Optional<StringRef> Name = Req.getString(KeyName);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.name'"));
    Optional<StringRef> FileName = Req.getString(KeySourceFile);
    if (!FileName.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.sourcefile'"));
    return editorOpenSwiftSourceInterface(*Name, *FileName, Args,
                                          CancellationToken, Rec);
  }
}

static void handleRequestEditorOpenSwiftTypeInterface(
    const RequestDict &Req, SourceKitCancellationToken CancellationToken,
    ResponseReceiver Rec) {
  {
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    Optional<StringRef> Usr = Req.getString(KeyUSR);
    if (!Usr.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.usr'"));
    return editorOpenSwiftTypeInterface(*Usr, Args, Rec);
  }
}

static void handleRequestEditorExtractTextFromComment(
    const RequestDict &Req, SourceKitCancellationToken CancellationToken,
    ResponseReceiver Rec) {
  {
    Optional<StringRef> Source = Req.getString(KeySourceText);
    if (!Source.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.sourcetext'"));
    return Rec(editorExtractTextFromComment(Source.value()));
  }
}

static void
handleRequestMarkupToXML(const RequestDict &Req,
                         SourceKitCancellationToken CancellationToken,
                         ResponseReceiver Rec) {
  {
    Optional<StringRef> Source = Req.getString(KeySourceText);
    if (!Source.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.sourcetext'"));
    return Rec(editorConvertMarkupToXML(Source.value()));
  }
}

static void
handleRequestEditorFindUSR(const RequestDict &Req,
                           SourceKitCancellationToken CancellationToken,
                           ResponseReceiver Rec) {
  {
    Optional<StringRef> Name = Req.getString(KeySourceFile);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.sourcefile'"));
    Optional<StringRef> USR = Req.getString(KeyUSR);
    if (!USR.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.usr'"));
    return Rec(editorFindUSR(*Name, *USR));
  }
}

static void handleRequestEditorFindInterfaceDoc(
    const RequestDict &Req, SourceKitCancellationToken CancellationToken,
    ResponseReceiver Rec) {
  {
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    Optional<StringRef> ModuleName = Req.getString(KeyModuleName);
    if (!ModuleName.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.modulename'"));
    return Rec(editorFindInterfaceDoc(*ModuleName, Args));
  }
}

static void
handleRequestModuleGroups(const RequestDict &Req,
                          SourceKitCancellationToken CancellationToken,
                          ResponseReceiver Rec) {
  {
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    Optional<StringRef> ModuleName = Req.getString(KeyModuleName);
    if (!ModuleName.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.modulename'"));
    return Rec(editorFindModuleGroups(*ModuleName, Args));
  }
}

static void
handleRequestSyntacticRename(const RequestDict &Req,
                             SourceKitCancellationToken CancellationToken,
                             ResponseReceiver Rec) {
  {
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    Optional<VFSOptions> vfsOptions = getVFSOptions(Req);
    std::unique_ptr<llvm::MemoryBuffer> InputBuf =
        getInputBufForRequestOrEmitError(Req, vfsOptions, Rec);
    if (!InputBuf)
      return;

    SmallString<64> ErrBuf;
    std::vector<RenameLocations> RenameLocations;
    if (buildRenameLocationsFromDict(Req, true, RenameLocations, ErrBuf))
      return Rec(createErrorRequestFailed(ErrBuf.c_str()));
    return Rec(syntacticRename(InputBuf.get(), RenameLocations, Args));
  }
}

static void
handleRequestFindRenameRanges(const RequestDict &Req,
                              SourceKitCancellationToken CancellationToken,
                              ResponseReceiver Rec) {
  {
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    Optional<VFSOptions> vfsOptions = getVFSOptions(Req);
    std::unique_ptr<llvm::MemoryBuffer> InputBuf =
        getInputBufForRequestOrEmitError(Req, vfsOptions, Rec);
    if (!InputBuf)
      return;

    SmallString<64> ErrBuf;
    std::vector<RenameLocations> RenameLocations;
    if (buildRenameLocationsFromDict(Req, false, RenameLocations, ErrBuf))
      return Rec(createErrorRequestFailed(ErrBuf.c_str()));
    return Rec(findRenameRanges(InputBuf.get(), RenameLocations, Args));
  }
}

static void
handleRequestCodeCompleteClose(const RequestDict &Req,
                               SourceKitCancellationToken CancellationToken,
                               ResponseReceiver Rec) {
  {
    // Unlike opening code completion, this is not a semantic request.
    Optional<StringRef> Name = Req.getString(KeyName);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.name'"));
    int64_t Offset;
    if (Req.getInt64(KeyOffset, Offset, /*isOptional=*/false))
      return Rec(createErrorRequestInvalid("missing 'key.offset'"));
    return Rec(codeCompleteClose(*Name, Offset));
  }
}

static void handleRequestCodeCompleteCacheOnDisk(
    const RequestDict &Req, SourceKitCancellationToken CancellationToken,
    ResponseReceiver Rec) {
  {
    Optional<StringRef> Name = Req.getString(KeyName);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.name'"));
    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
    Lang.codeCompleteCacheOnDisk(*Name);
    ResponseBuilder b;
    return Rec(b.createResponse());
  }
}

static void handleRequestCodeCompleteSetPopularAPI(
    const RequestDict &Req, SourceKitCancellationToken CancellationToken,
    ResponseReceiver Rec) {
  {
    llvm::SmallVector<const char *, 0> popular;
    llvm::SmallVector<const char *, 0> unpopular;
    Req.getStringArray(KeyPopular, popular, /*isOptional=*/false);
    Req.getStringArray(KeyUnpopular, unpopular, /*isOptional=*/false);
    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
    Lang.codeCompleteSetPopularAPI(popular, unpopular);
    ResponseBuilder b;
    return Rec(b.createResponse());
  }
}

static void
handleRequestCodeCompleteSetCustom(const RequestDict &Req,
                                   SourceKitCancellationToken CancellationToken,
                                   ResponseReceiver Rec) {
  {
    SmallVector<CustomCompletionInfo, 16> customCompletions;
    sourcekitd_response_t err = nullptr;
    bool failed = Req.dictionaryArrayApply(KeyResults, [&](RequestDict dict) {
      CustomCompletionInfo CCInfo;
      Optional<StringRef> Name = dict.getString(KeyName);
      if (!Name.has_value()) {
        err = createErrorRequestInvalid("missing 'key.name'");
        return true;
      }
      CCInfo.Name = (*Name).str();

      sourcekitd_uid_t Kind = dict.getUID(KeyKind);
      if (!Kind) {
        err = createErrorRequestInvalid("missing 'key.kind'");
        return true;
      }
      CCInfo.Kind = Kind;

      SmallVector<sourcekitd_uid_t, 3> contexts;
      if (dict.getUIDArray(KeyContext, contexts, false)) {
        err = createErrorRequestInvalid("missing 'key.context'");
        return true;
      }

      for (auto context : contexts) {
        if (context == KindExpr) {
          CCInfo.Contexts |= CustomCompletionInfo::Expr;
        } else if (context == KindStmt) {
          CCInfo.Contexts |= CustomCompletionInfo::Stmt;
        } else if (context == KindType) {
          CCInfo.Contexts |= CustomCompletionInfo::Type;
        } else if (context == KindForEachSequence) {
          CCInfo.Contexts |= CustomCompletionInfo::ForEachSequence;
        } else {
          err = createErrorRequestInvalid("invalid value for 'key.context'");
          return true;
        }
      }

      customCompletions.push_back(std::move(CCInfo));
      return false;
    });

    if (failed) {
      if (!err)
        err = createErrorRequestInvalid("missing 'key.results'");
      return Rec(err);
    }

    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
    Lang.codeCompleteSetCustom(customCompletions);
    return Rec(ResponseBuilder().createResponse());
  }
}

static void
handleRequestStatistics(const RequestDict &Req,
                        SourceKitCancellationToken CancellationToken,
                        ResponseReceiver Rec) {
  {
    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
    Lang.getStatistics([Rec](ArrayRef<Statistic *> stats) {
      ResponseBuilder builder;
      auto results = builder.getDictionary().setArray(KeyResults);
      auto addStat = [&results](Statistic *stat) {
        auto dict = results.appendDictionary();
        dict.set(KeyKind, stat->name);
        dict.set(KeyDescription, stat->description);
        dict.set(KeyValue, stat->value);
      };

      Statistic instructionCount(
          UIdentFromSKDUID(KindStatInstructionCount),
          "# of instructions executed since the SourceKit process was started");
      instructionCount.value.store(swift::getInstructionsExecuted());
      addStat(&instructionCount);
      addStat(&numRequests);
      addStat(&numSemaRequests);
      std::for_each(stats.begin(), stats.end(), addStat);

      Rec(builder.createResponse());
    });
    return;
  }
}

static void handleRequestCompile(const RequestDict &Req,
                                 SourceKitCancellationToken CancellationToken,
                                 ResponseReceiver Rec) {
  {
    Optional<VFSOptions> vfsOptions = getVFSOptions(Req);
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    Optional<StringRef> Name = Req.getString(KeyName);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.name'"));

    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
    Lang.performCompile(
        *Name, Args, std::move(vfsOptions), CancellationToken,
        [Rec](const RequestResult<CompilationResult> &result) {
          if (result.isCancelled())
            return Rec(createErrorRequestCancelled());
          if (result.isError())
            return Rec(createErrorRequestFailed(result.getError()));

          const CompilationResult &info = result.value();

          ResponseBuilder builder;

          builder.getDictionary().set(KeyValue, info.ResultStatus);
          fillDiagnosticInfo(builder.getDictionary(), info.Diagnostics,
                             /*DiagStage*/ None);
          Rec(builder.createResponse());
        });
    return;
  }
}

static void
handleRequestCompileClose(const RequestDict &Req,
                          SourceKitCancellationToken CancellationToken,
                          ResponseReceiver Rec) {
  {
    Optional<StringRef> Name = Req.getString(KeyName);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.name'"));

    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
    Lang.closeCompile(*Name);

    return Rec(ResponseBuilder().createResponse());
  }
}

static void
handleRequestCodeComplete(const RequestDict &Req,
                          SourceKitCancellationToken CancellationToken,
                          ResponseReceiver Rec) {
  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    Optional<VFSOptions> vfsOptions = getVFSOptions(Req);
    std::unique_ptr<llvm::MemoryBuffer> InputBuf =
        getInputBufForRequestOrEmitError(Req, vfsOptions, Rec);
    if (!InputBuf)
      return;
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;

    int64_t Offset;
    if (Req.getInt64(KeyOffset, Offset, /*isOptional=*/false))
      return Rec(createErrorRequestInvalid("missing 'key.offset'"));
    Optional<RequestDict> options = Req.getDictionary(KeyCodeCompleteOptions);
    return Rec(codeComplete(InputBuf.get(), Offset, options, Args,
                            std::move(vfsOptions), CancellationToken));
  });
}

static void
handleRequestCodeCompleteOpen(const RequestDict &Req,
                              SourceKitCancellationToken CancellationToken,
                              ResponseReceiver Rec) {
  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    Optional<VFSOptions> vfsOptions = getVFSOptions(Req);
    std::unique_ptr<llvm::MemoryBuffer> InputBuf =
        getInputBufForRequestOrEmitError(Req, vfsOptions, Rec);
    if (!InputBuf)
      return;
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    Optional<StringRef> Name = Req.getString(KeyName);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.name'"));
    int64_t Offset;
    if (Req.getInt64(KeyOffset, Offset, /*isOptional=*/false))
      return Rec(createErrorRequestInvalid("missing 'key.offset'"));
    Optional<RequestDict> options = Req.getDictionary(KeyCodeCompleteOptions);
    return Rec(codeCompleteOpen(*Name, InputBuf.get(), Offset, options, Args,
                                std::move(vfsOptions), CancellationToken));
  });
}

static void
handleRequestCodeCompleteUpdate(const RequestDict &Req,
                                SourceKitCancellationToken CancellationToken,
                                ResponseReceiver Rec) {
  if (checkVFSNotSupported(Req, Rec))
    return;

  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    Optional<StringRef> Name = Req.getString(KeyName);
    if (!Name.has_value())
      return Rec(createErrorRequestInvalid("missing 'key.name'"));
    int64_t Offset;
    if (Req.getInt64(KeyOffset, Offset, /*isOptional=*/false))
      return Rec(createErrorRequestInvalid("missing 'key.offset'"));
    Optional<RequestDict> options = Req.getDictionary(KeyCodeCompleteOptions);
    return Rec(codeCompleteUpdate(*Name, Offset, options, CancellationToken));
  });
}

static void
handleRequestTypeContextInfo(const RequestDict &Req,
                             SourceKitCancellationToken CancellationToken,
                             ResponseReceiver Rec) {
  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    Optional<VFSOptions> vfsOptions = getVFSOptions(Req);
    std::unique_ptr<llvm::MemoryBuffer> InputBuf =
        getInputBufForRequestOrEmitError(Req, vfsOptions, Rec);
    if (!InputBuf)
      return;
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    int64_t Offset;
    if (Req.getInt64(KeyOffset, Offset, /*isOptional=*/false))
      return Rec(createErrorRequestInvalid("missing 'key.offset'"));
    Optional<RequestDict> options =
        Req.getDictionary(KeyTypeContextInfoOptions);
    return Rec(typeContextInfo(InputBuf.get(), Offset, options, Args,
                               std::move(vfsOptions), CancellationToken));
  });
}

static void
handleRequestConformingMethodList(const RequestDict &Req,
                                  SourceKitCancellationToken CancellationToken,
                                  ResponseReceiver Rec) {
  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    Optional<VFSOptions> vfsOptions = getVFSOptions(Req);
    std::unique_ptr<llvm::MemoryBuffer> InputBuf =
        getInputBufForRequestOrEmitError(Req, vfsOptions, Rec);
    if (!InputBuf)
      return;
    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    int64_t Offset;
    if (Req.getInt64(KeyOffset, Offset, /*isOptional=*/false))
      return Rec(createErrorRequestInvalid("missing 'key.offset'"));
    SmallVector<const char *, 8> ExpectedTypeNames;
    if (Req.getStringArray(KeyExpectedTypes, ExpectedTypeNames, true))
      return Rec(createErrorRequestInvalid("invalid 'key.expectedtypes'"));
    Optional<RequestDict> options =
        Req.getDictionary(KeyConformingMethodListOptions);
    return Rec(conformingMethodList(InputBuf.get(), Offset, options, Args,
                                    ExpectedTypeNames, std::move(vfsOptions),
                                    CancellationToken));
  });
}

static void handleRequestIndex(const RequestDict &Req,
                               SourceKitCancellationToken CancellationToken,
                               ResponseReceiver Rec) {
  if (checkVFSNotSupported(Req, Rec))
    return;

  handleSemanticRequest(Req, Rec, [Req, Rec]() {
    Optional<StringRef> PrimaryFilePath =
        getPrimaryFilePathForRequestOrEmitError(Req, Rec);
    if (!PrimaryFilePath)
      return;

    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;
    return Rec(indexSource(*PrimaryFilePath, Args));
  });
}

static void
handleRequestCursorInfo(const RequestDict &Req,
                        SourceKitCancellationToken CancellationToken,
                        ResponseReceiver Rec) {
  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();

    Optional<VFSOptions> vfsOptions = getVFSOptions(Req);
    Optional<StringRef> PrimaryFilePath =
        getPrimaryFilePathForRequestOrEmitError(Req, Rec);
    if (!PrimaryFilePath)
      return;

    StringRef InputBufferName = getInputBufferNameForRequest(Req, Rec);

    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;

    // For backwards compatibility, the default is 1.
    int64_t CancelOnSubsequentRequest = 1;
    Req.getInt64(KeyCancelOnSubsequentRequest, CancelOnSubsequentRequest,
                 /*isOptional=*/true);

    int64_t Offset;
    if (!Req.getInt64(KeyOffset, Offset, /*isOptional=*/false)) {
      int64_t Length = 0;
      Req.getInt64(KeyLength, Length, /*isOptional=*/true);
      int64_t Actionables = false;
      Req.getInt64(KeyRetrieveRefactorActions, Actionables, /*isOptional=*/true);
      int64_t SymbolGraph = false;
      Req.getInt64(KeyRetrieveSymbolGraph, SymbolGraph, /*isOptional=*/true);
      return Lang.getCursorInfo(
          *PrimaryFilePath, InputBufferName, Offset, Length, Actionables,
          SymbolGraph, CancelOnSubsequentRequest, Args, std::move(vfsOptions),
          CancellationToken,
          [Rec](const RequestResult<CursorInfoData> &Result) {
            reportCursorInfo(Result, Rec);
          });
    }
    if (auto USR = Req.getString(KeyUSR)) {
      return Lang.getCursorInfoFromUSR(
          *PrimaryFilePath, InputBufferName, *USR, CancelOnSubsequentRequest,
          Args, std::move(vfsOptions), CancellationToken,
          [Rec](const RequestResult<CursorInfoData> &Result) {
            reportCursorInfo(Result, Rec);
          });
    }

    return Rec(createErrorRequestInvalid(
        "either 'key.offset' or 'key.usr' is required"));
  });
}

static void handleRequestRangeInfo(const RequestDict &Req,
                                   SourceKitCancellationToken CancellationToken,
                                   ResponseReceiver Rec) {
  if (checkVFSNotSupported(Req, Rec))
    return;

  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();

    Optional<StringRef> PrimaryFilePath =
        getPrimaryFilePathForRequestOrEmitError(Req, Rec);
    if (!PrimaryFilePath)
      return;

    StringRef InputBufferName = getInputBufferNameForRequest(Req, Rec);

    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;

    int64_t Offset;
    int64_t Length;
    // For backwards compatibility, the default is 1.
    int64_t CancelOnSubsequentRequest = 1;
    Req.getInt64(KeyCancelOnSubsequentRequest, CancelOnSubsequentRequest,
                 /*isOptional=*/true);
    if (!Req.getInt64(KeyOffset, Offset, /*isOptional=*/false)) {
      if (!Req.getInt64(KeyLength, Length, /*isOptional=*/false)) {
        return Lang.getRangeInfo(*PrimaryFilePath, InputBufferName, Offset,
                                 Length, CancelOnSubsequentRequest, Args,
                                 CancellationToken,
                                 [Rec](const RequestResult<RangeInfo> &Result) {
                                   reportRangeInfo(Result, Rec);
                                 });
      }
    }

    return Rec(createErrorRequestInvalid(
      "'key.offset' or 'key.length' is required"));
  });
}

static void
handleRequestSemanticRefactoring(const RequestDict &Req,
                                 SourceKitCancellationToken CancellationToken,
                                 ResponseReceiver Rec) {
  if (checkVFSNotSupported(Req, Rec))
    return;

  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    Optional<StringRef> PrimaryFilePath =
        getPrimaryFilePathForRequestOrEmitError(Req, Rec);
    if (!PrimaryFilePath)
      return;

    StringRef InputBufferName = getInputBufferNameForRequest(Req, Rec);

    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;

    int64_t Line = 0;
    int64_t Column = 0;
    int64_t Length = 0;
    auto KA = Req.getUID(KeyActionUID);
    if (!KA) {
      return Rec(createErrorRequestInvalid("'key.actionuid' is required"));
    }
    SemanticRefactoringInfo Info;
    Info.Kind = SemanticRefactoringKind::None;

#define SEMANTIC_REFACTORING(KIND, NAME, ID)                                   \
  if (KA == KindRefactoring##KIND) Info.Kind = SemanticRefactoringKind::KIND;
#include "swift/Refactoring/RefactoringKinds.def"

    if (Info.Kind == SemanticRefactoringKind::None)
      return Rec(createErrorRequestInvalid("'key.actionuid' isn't recognized"));

    if (!Req.getInt64(KeyLine, Line, /*isOptional=*/false)) {
      if (!Req.getInt64(KeyColumn, Column, /*isOptional=*/false)) {
        Req.getInt64(KeyLength, Length, /*isOptional=*/true);
        if (auto N = Req.getString(KeyName))
          Info.PreferredName = *N;
        LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
        Info.InputBufferName = InputBufferName;
        Info.Line = Line;
        Info.Column = Column;
        Info.Length = Length;
        return Lang.semanticRefactoring(
            *PrimaryFilePath, Info, Args, CancellationToken,
            [Rec](const RequestResult<ArrayRef<CategorizedEdits>> &Result) {
              Rec(createCategorizedEditsResponse(Result));
            });
      }
    }
    return Rec(createErrorRequestInvalid("'key.line' or 'key.column' are required"));
  });
}

static void
handleRequestCollectExpressionType(const RequestDict &Req,
                                   SourceKitCancellationToken CancellationToken,
                                   ResponseReceiver Rec) {
  if (checkVFSNotSupported(Req, Rec))
    return;

  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();

    Optional<StringRef> PrimaryFilePath =
        getPrimaryFilePathForRequestOrEmitError(Req, Rec);
    if (!PrimaryFilePath)
      return;

    StringRef InputBufferName = getInputBufferNameForRequest(Req, Rec);

    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;

    SmallVector<const char *, 8> ExpectedProtocols;
    if (Req.getStringArray(KeyExpectedTypes, ExpectedProtocols, true))
      return Rec(createErrorRequestInvalid("invalid 'key.expectedtypes'"));
    int64_t FullyQualified = false;
    Req.getInt64(KeyFullyQualified, FullyQualified, /*isOptional=*/true);
    int64_t CanonicalTy = false;
    Req.getInt64(KeyCanonicalizeType, CanonicalTy, /*isOptional=*/true);
    return Lang.collectExpressionTypes(
        *PrimaryFilePath, InputBufferName, Args, ExpectedProtocols,
        FullyQualified, CanonicalTy, CancellationToken,
        [Rec](const RequestResult<ExpressionTypesInFile> &Result) {
          reportExpressionTypeInfo(Result, Rec);
        });
  });
}

static void
handleRequestCollectVariableType(const RequestDict &Req,
                                 SourceKitCancellationToken CancellationToken,
                                 ResponseReceiver Rec) {
  if (checkVFSNotSupported(Req, Rec))
    return;

  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();

    Optional<StringRef> PrimaryFilePath =
        getPrimaryFilePathForRequestOrEmitError(Req, Rec);
    if (!PrimaryFilePath)
      return;

    StringRef InputBufferName = getInputBufferNameForRequest(Req, Rec);

    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;

    Optional<unsigned> Offset = Req.getOptionalInt64(KeyOffset).transform(
        [](int64_t v) -> unsigned { return v; });
    Optional<unsigned> Length = Req.getOptionalInt64(KeyLength).transform(
        [](int64_t v) -> unsigned { return v; });
    int64_t FullyQualified = false;
    Req.getInt64(KeyFullyQualified, FullyQualified, /*isOptional=*/true);
    return Lang.collectVariableTypes(
        *PrimaryFilePath, InputBufferName, Args, Offset, Length, FullyQualified,
        CancellationToken,
        [Rec](const RequestResult<VariableTypesInFile> &Result) {
          reportVariableTypeInfo(Result, Rec);
        });
  });
}

static void
handleRequestFindLocalRenameRanges(const RequestDict &Req,
                                   SourceKitCancellationToken CancellationToken,
                                   ResponseReceiver Rec) {
  if (checkVFSNotSupported(Req, Rec))
    return;

  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    auto PrimaryFilePath = getPrimaryFilePathForRequestOrEmitError(Req, Rec);
    if (!PrimaryFilePath)
      return;

    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;

    int64_t Line = 0, Column = 0, Length = 0;
    if (Req.getInt64(KeyLine, Line, /*isOptional=*/false))
      return Rec(createErrorRequestInvalid("'key.line' is required"));
    if (Req.getInt64(KeyColumn, Column, /*isOptional=*/false))
      return Rec(createErrorRequestInvalid("'key.column' is required"));
    Req.getInt64(KeyLength, Length, /*isOptional=*/true);

    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
    return Lang.findLocalRenameRanges(
        *PrimaryFilePath, Line, Column, Length, Args, CancellationToken,
        [Rec](const RequestResult<ArrayRef<CategorizedRenameRanges>> &Result) {
          Rec(createCategorizedRenameRangesResponse(Result));
        });
  });
}

static void
handleRequestNameTranslation(const RequestDict &Req,
                             SourceKitCancellationToken CancellationToken,
                             ResponseReceiver Rec) {
  if (checkVFSNotSupported(Req, Rec))
    return;

  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    auto PrimaryFilePath = getPrimaryFilePathForRequestOrEmitError(Req, Rec);
    if (!PrimaryFilePath)
      return;

    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;

    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
    int64_t Offset;
    if (Req.getInt64(KeyOffset, Offset, /*isOptional=*/false)) {
      return Rec(createErrorRequestInvalid("'key.offset' is required"));
    }
    NameTranslatingInfo Input;
    auto NK = Req.getUID(KeyNameKind);
    if (!NK) {
      return Rec(createErrorRequestInvalid("'key.namekind' is required"));
    }

    static UIdent UIDKindNameSwift(KindNameSwift.str());
    static UIdent UIDKindNameObjc(KindNameObjc.str());

    if (NK == KindNameSwift.get())
      Input.NameKind = UIDKindNameSwift;
    else if (NK == KindNameObjc.get())
      Input.NameKind = UIDKindNameObjc;
    else
      return Rec(createErrorRequestInvalid("'key.namekind' is unrecognizable"));
    if (auto Base = Req.getString(KeyBaseName)) {
      if (Input.NameKind == UIDKindNameSwift) {
        Input.BaseName = Base.value().trim('`');
      } else {
        Input.BaseName = Base.value();
      }
    }
    llvm::SmallVector<const char*, 4> ArgParts;
    llvm::SmallVector<const char*, 4> Selectors;
    Req.getStringArray(KeyArgNames, ArgParts, true);
    Req.getStringArray(KeySelectorPieces, Selectors, true);
    if (!ArgParts.empty() && !Selectors.empty()) {
      return Rec(createErrorRequestInvalid("cannot specify 'key.selectorpieces' "
                                           "and 'key.argnames' at the same time"));
    }
    llvm::transform(ArgParts, std::back_inserter(Input.ArgNames),
                    [](const char *C) { return StringRef(C).trim('`'); });
    llvm::transform(Selectors, std::back_inserter(Input.ArgNames),
                    [](const char *C) { return StringRef(C); });
    return Lang.getNameInfo(
        *PrimaryFilePath, Offset, Input, Args, CancellationToken,
        [Rec](const RequestResult<NameTranslatingInfo> &Result) {
          reportNameInfo(Result, Rec);
        });
  });
}

static void
handleRequestRelatedIdents(const RequestDict &Req,
                           SourceKitCancellationToken CancellationToken,
                           ResponseReceiver Rec) {
  if (checkVFSNotSupported(Req, Rec))
    return;

  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    Optional<StringRef> PrimaryFilePath =
        getPrimaryFilePathForRequestOrEmitError(Req, Rec);
    if (!PrimaryFilePath)
      return;

    StringRef InputBufferName = getInputBufferNameForRequest(Req, Rec);

    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;

    int64_t Offset;
    if (Req.getInt64(KeyOffset, Offset, /*isOptional=*/false))
      return Rec(createErrorRequestInvalid("missing 'key.offset'"));

    // For backwards compatibility, the default is 1.
    int64_t CancelOnSubsequentRequest = 1;
    Req.getInt64(KeyCancelOnSubsequentRequest, CancelOnSubsequentRequest,
                 /*isOptional=*/true);

    return findRelatedIdents(*PrimaryFilePath, InputBufferName, Offset,
                             CancelOnSubsequentRequest, Args, CancellationToken,
                             Rec);
  });
}

static void
handleRequestActiveRegions(const RequestDict &Req,
                           SourceKitCancellationToken CancellationToken,
                           ResponseReceiver Rec) {
  if (checkVFSNotSupported(Req, Rec))
    return;

  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    Optional<StringRef> PrimaryFilePath =
        getPrimaryFilePathForRequestOrEmitError(Req, Rec);
    if (!PrimaryFilePath)
      return;

    StringRef InputBufferName = getInputBufferNameForRequest(Req, Rec);

    SmallVector<const char *> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;

    return findActiveRegions(*PrimaryFilePath, InputBufferName, Args,
                             CancellationToken, Rec);
  });
}

static void
handleRequestDiagnostics(const RequestDict &Req,
                         SourceKitCancellationToken CancellationToken,
                         ResponseReceiver Rec) {
  handleSemanticRequest(Req, Rec, [Req, CancellationToken, Rec]() {
    Optional<VFSOptions> vfsOptions = getVFSOptions(Req);
    auto PrimaryFilePath = getPrimaryFilePathForRequestOrEmitError(Req, Rec);
    if (!PrimaryFilePath)
      return;

    SmallVector<const char *, 8> Args;
    if (getCompilerArgumentsForRequestOrEmitError(Req, Args, Rec))
      return;

    LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
    Lang.getDiagnostics(*PrimaryFilePath, Args, std::move(vfsOptions),
                        CancellationToken,
                        [Rec](const RequestResult<DiagnosticsResult> &Result) {
                          reportDiagnostics(Result, Rec);
                        });
    return;
  });
}

/// Expand macros in the specified source file syntactically.
///
/// Request would look like:
///   {
///     key.compilerargs: []
///     key.sourcefile: <file name>
///     key.sourcetext: <source text> (optional)
///     key.expansions: [<expansion specifier>...]
///   }
/// 'compilerargs' is used for plugin search paths.
/// 'expansion specifier' is
///   {
///     key.offset: <offset>
///     key.modulename: <plugin module name>
///     key.typename: <macro typename>
///     key.macro_roles: [<macro role UID>...]
///   }
///
/// Sends the results as a 'CategorizedEdits'. 
/// Note that, unlike refactoring, each edit doesn't have 'key.buffer_name'.
/// FIXME: Support nested expansion.
static void handleRequestSyntacticMacroExpansion(
    const RequestDict &req, SourceKitCancellationToken cancellationToken,
    ResponseReceiver rec) {

  Optional<VFSOptions> vfsOptions = getVFSOptions(req);
  std::unique_ptr<llvm::MemoryBuffer> inputBuf =
      getInputBufForRequestOrEmitError(req, vfsOptions, rec);
  if (!inputBuf)
    return;

  SmallVector<const char *, 16> args;
  if (getCompilerArgumentsForRequestOrEmitError(req, args, rec))
    return;

  // key.expansions: [
  //   { key.offset: 42,
  //     key.macro_roles: [source.lang.swift.macrorole.conformance,
  //                      source.lang.swift.macrorole.member],
  //     key.modulename: "MyMacroImpl",
  //     key.typename: "StringifyMacro"},
  //   { key.offset: 132,
  //     key.sourceText: "foo(bar, baz)",
  //     key.macro_roles: [source.lang.swift.macrorole.conformance,
  //                      source.lang.swift.macrorole.member],
  //     key.expandedmacro_replacements: [
  //       {key.offset: 4, key.length: 3, key.argindex: 0},
  //       {key.offset: 9, key.length: 3, key.argindex: 1}]}
  // ]
  std::vector<MacroExpansionInfo> expansions;
  bool failed = req.dictionaryArrayApply(KeyExpansions, [&](RequestDict dict) {
    // offset.
    int64_t offset;
    dict.getInt64(KeyOffset, offset, false);

    // macro roles.
    SmallVector<sourcekitd_uid_t, 1> macroRoleUIDs;
    if (dict.getUIDArray(KeyMacroRoles, macroRoleUIDs, false)) {
      rec(createErrorRequestInvalid(
          "missing 'key.macro_roles' for expansion specifier"));
      return true;
    }
    MacroRoles macroRoles;
    for (auto uid : macroRoleUIDs) {
      if (uid == KindMacroRoleExpression)
        macroRoles |= MacroRole::Expression;
      if (uid == KindMacroRoleDeclaration)
        macroRoles |= MacroRole::Declaration;
      if (uid == KindMacroRoleCodeItem)
        macroRoles |= MacroRole::CodeItem;
      if (uid == KindMacroRoleAccessor)
        macroRoles |= MacroRole::Accessor;
      if (uid == KindMacroRoleMemberAttribute)
        macroRoles |= MacroRole::MemberAttribute;
      if (uid == KindMacroRoleMember)
        macroRoles |= MacroRole::Member;
      if (uid == KindMacroRolePeer)
        macroRoles |= MacroRole::Peer;
      if (uid == KindMacroRoleConformance)
        macroRoles |= MacroRole::Conformance;
    }

    // definition.
    if (auto moduleName = dict.getString(KeyModuleName)) {
      auto typeName = dict.getString(KeyTypeName);
      if (!typeName) {
        rec(createErrorRequestInvalid(
            "missing 'key.typename' for external macro definition"));
        return true;
      }
      MacroExpansionInfo::ExternalMacroReference definition(moduleName->str(),
                                                            typeName->str());
      expansions.emplace_back(offset, macroRoles, definition);
    } else if (auto expandedText = dict.getString(KeySourceText)) {
      MacroExpansionInfo::ExpandedMacroDefinition definition(*expandedText);
      bool failed = dict.dictionaryArrayApply(
          KeyExpandedMacroReplacements, [&](RequestDict dict) {
            int64_t offset, length, paramIndex;
            bool failed = false;
            failed |= dict.getInt64(KeyOffset, offset, false);
            failed |= dict.getInt64(KeyLength, length, false);
            failed |= dict.getInt64(KeyArgIndex, paramIndex, false);
            if (failed) {
              rec(createErrorRequestInvalid(
                  "macro replacement should have key.offset, key.length, and "
                  "key.argindex"));
              return true;
            }
            definition.replacements.emplace_back(
                RawCharSourceRange{unsigned(offset), unsigned(length)},
                paramIndex);
            return false;
          });
      if (failed)
        return true;
      expansions.emplace_back(offset, macroRoles, definition);
    }
    return false;
  });
  if (failed)
    return;

  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.expandMacroSyntactically(
      inputBuf.get(), args, expansions,
      [&](const auto &Result) { rec(createCategorizedEditsResponse(Result)); });
}

void handleRequestImpl(sourcekitd_object_t ReqObj,
                       SourceKitCancellationToken CancellationToken,
                       ResponseReceiver Rec) {
  ++numRequests;

  RequestDict Req(ReqObj);

  if (auto SimulateLongRequestDuration =
          Req.getOptionalInt64(KeySimulateLongRequest)) {
    if (!getGlobalContext().getSlowRequestSimulator()->simulateLongRequest(
            *SimulateLongRequestDuration, CancellationToken)) {
      Rec(createErrorRequestCancelled());
      return;
    }
  }

  sourcekitd_uid_t ReqUID = Req.getUID(KeyRequest);
  if (!ReqUID)
    return Rec(
        createErrorRequestInvalid("missing 'key.request' with UID value"));

#define HANDLE_REQUEST(Kind, IMPL_FUNC)                                        \
  if (ReqUID == Kind) {                                                        \
    return IMPL_FUNC(Req, CancellationToken, Rec);                             \
  }

  HANDLE_REQUEST(RequestGlobalConfiguration, handleRequestGlobalConfiguration)
  HANDLE_REQUEST(RequestProtocolVersion, handleRequestProtocolVersion)
  HANDLE_REQUEST(RequestCompilerVersion, handleRequestCompilerVersion)
  HANDLE_REQUEST(RequestCrashWithExit, handleRequestCrashWithExit)
  HANDLE_REQUEST(RequestTestNotification, handleRequestTestNotification)
  HANDLE_REQUEST(RequestStatistics, handleRequestStatistics)
  HANDLE_REQUEST(RequestDemangle, handleRequestDemangle)
  HANDLE_REQUEST(RequestMangleSimpleClass, handleRequestMangleSimpleClass)

  HANDLE_REQUEST(RequestEnableCompileNotifications,
                 handleRequestEnableCompileNotifications)

  HANDLE_REQUEST(RequestBuildSettingsRegister,
                 handleRequestBuildSettingsRegister)
  HANDLE_REQUEST(RequestDependencyUpdated, handleRequestDependencyUpdated)

  HANDLE_REQUEST(RequestDocInfo, handleRequestDocInfo)

  HANDLE_REQUEST(RequestEditorOpen, handleRequestEditorOpen)
  HANDLE_REQUEST(RequestEditorClose, handleRequestEditorClose)
  HANDLE_REQUEST(RequestEditorReplaceText, handleRequestEditorReplaceText)
  HANDLE_REQUEST(RequestEditorFormatText, handleRequestEditorFormatText)
  HANDLE_REQUEST(RequestEditorExpandPlaceholder,
                 handleRequestEditorExpandPlaceholder)
  HANDLE_REQUEST(RequestEditorOpenInterface, handleRequestEditorOpenInterface)
  HANDLE_REQUEST(RequestEditorOpenHeaderInterface,
                 handleRequestEditorOpenHeaderInterface)
  HANDLE_REQUEST(RequestEditorOpenSwiftSourceInterface,
                 handleRequestEditorOpenSwiftSourceInterface)
  HANDLE_REQUEST(RequestEditorOpenSwiftTypeInterface,
                 handleRequestEditorOpenSwiftTypeInterface)
  HANDLE_REQUEST(RequestEditorExtractTextFromComment,
                 handleRequestEditorExtractTextFromComment)
  HANDLE_REQUEST(RequestMarkupToXML, handleRequestMarkupToXML)
  HANDLE_REQUEST(RequestEditorFindUSR, handleRequestEditorFindUSR)
  HANDLE_REQUEST(RequestEditorFindInterfaceDoc,
                 handleRequestEditorFindInterfaceDoc)
  HANDLE_REQUEST(RequestModuleGroups, handleRequestModuleGroups)

  HANDLE_REQUEST(RequestSyntacticRename, handleRequestSyntacticRename)
  HANDLE_REQUEST(RequestFindRenameRanges, handleRequestFindRenameRanges)

  HANDLE_REQUEST(RequestCodeCompleteClose, handleRequestCodeCompleteClose)
  HANDLE_REQUEST(RequestCodeCompleteCacheOnDisk,
                 handleRequestCodeCompleteCacheOnDisk)
  HANDLE_REQUEST(RequestCodeCompleteSetPopularAPI,
                 handleRequestCodeCompleteSetPopularAPI)
  HANDLE_REQUEST(RequestCodeCompleteSetCustom,
                 handleRequestCodeCompleteSetCustom)

  HANDLE_REQUEST(RequestCompile, handleRequestCompile)
  HANDLE_REQUEST(RequestCompileClose, handleRequestCompileClose)

  // Requests that need semantic typechecking.
  // NOTE: checking semantic functionalities enabled is a handler's
  // responsibility.

  HANDLE_REQUEST(RequestCodeComplete, handleRequestCodeComplete)
  HANDLE_REQUEST(RequestCodeCompleteOpen, handleRequestCodeCompleteOpen)
  HANDLE_REQUEST(RequestCodeCompleteUpdate, handleRequestCodeCompleteUpdate)
  HANDLE_REQUEST(RequestTypeContextInfo, handleRequestTypeContextInfo)
  HANDLE_REQUEST(RequestConformingMethodList, handleRequestConformingMethodList)

  HANDLE_REQUEST(RequestIndex, handleRequestIndex)
  HANDLE_REQUEST(RequestCursorInfo, handleRequestCursorInfo)
  HANDLE_REQUEST(RequestRangeInfo, handleRequestRangeInfo)
  HANDLE_REQUEST(RequestSemanticRefactoring, handleRequestSemanticRefactoring)

  HANDLE_REQUEST(RequestCollectExpressionType,
                 handleRequestCollectExpressionType)
  HANDLE_REQUEST(RequestCollectVariableType, handleRequestCollectVariableType)
  HANDLE_REQUEST(RequestFindLocalRenameRanges,
                 handleRequestFindLocalRenameRanges)
  HANDLE_REQUEST(RequestNameTranslation, handleRequestNameTranslation)
  HANDLE_REQUEST(RequestRelatedIdents, handleRequestRelatedIdents)
  HANDLE_REQUEST(RequestActiveRegions, handleRequestActiveRegions)
  HANDLE_REQUEST(RequestDiagnostics, handleRequestDiagnostics)
  HANDLE_REQUEST(RequestSyntacticMacroExpansion,
                 handleRequestSyntacticMacroExpansion)

  {
    SmallString<64> ErrBuf;
    llvm::raw_svector_ostream OSErr(ErrBuf);
    OSErr << "unknown request: " << UIdentFromSKDUID(ReqUID).getName();
    return Rec(createErrorRequestInvalid(ErrBuf.c_str()));
  }
}

//===----------------------------------------------------------------------===//
// Index
//===----------------------------------------------------------------------===//

namespace {
class SKIndexingConsumer : public IndexingConsumer {
  struct Entity {
    UIdent Kind;
    ResponseBuilder::Dictionary Data;
    ResponseBuilder::Array Entities;
    ResponseBuilder::Array Related;
  };
  SmallVector<Entity, 6> EntitiesStack;

  struct Dependency {
    UIdent Kind;
    ResponseBuilder::Dictionary Data;
    ResponseBuilder::Array Dependencies;
  };
  SmallVector<Dependency, 6> DependenciesStack;

  ResponseBuilder::Dictionary TopDict;
  bool Cancelled = false;

public:
  std::string ErrorDescription;

  explicit SKIndexingConsumer(ResponseBuilder &RespBuilder) {
    TopDict = RespBuilder.getDictionary();

    // First in stack is the top-level "key.entities" container.
    EntitiesStack.push_back(
        { UIdent(),
          TopDict,
          ResponseBuilder::Array(),
          ResponseBuilder::Array() });

    DependenciesStack.push_back({UIdent(), TopDict, ResponseBuilder::Array() });
  }
  ~SKIndexingConsumer() override {
    assert(Cancelled ||
           (EntitiesStack.size() == 1 && DependenciesStack.size() == 1));
    (void) Cancelled;
  }

  void failed(StringRef ErrDescription) override;

  bool startDependency(UIdent Kind,
                       StringRef Name,
                       StringRef Path,
                       bool IsSystem) override;

  bool finishDependency(UIdent Kind) override;

  bool startSourceEntity(const EntityInfo &Info) override;

  bool recordRelatedEntity(const EntityInfo &Info) override;

  bool finishSourceEntity(UIdent Kind) override;
};
} // end anonymous namespace

static sourcekitd_response_t indexSource(StringRef Filename,
                                         ArrayRef<const char *> Args) {
  ResponseBuilder RespBuilder;
  SKIndexingConsumer IdxConsumer(RespBuilder);
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.indexSource(Filename, IdxConsumer, Args);

  if (!IdxConsumer.ErrorDescription.empty())
    return createErrorRequestFailed(IdxConsumer.ErrorDescription.c_str());

  return RespBuilder.createResponse();
}

void SKIndexingConsumer::failed(StringRef ErrDescription) {
  ErrorDescription = ErrDescription.str();
}

bool SKIndexingConsumer::startDependency(UIdent Kind,
                                         StringRef Name,
                                         StringRef Path,
                                         bool IsSystem) {
  Dependency &Parent = DependenciesStack.back();
  ResponseBuilder::Array &Arr = Parent.Dependencies;
  if (Arr.isNull())
    Arr = Parent.Data.setArray(KeyDependencies);

  auto Elem = Arr.appendDictionary();
  Elem.set(KeyKind, Kind);
  Elem.set(KeyName, Name);
  Elem.set(KeyFilePath, Path);
  if (IsSystem)
    Elem.setBool(KeyIsSystem, IsSystem);

  DependenciesStack.push_back({ Kind, Elem, ResponseBuilder::Array() });
  return true;
}

bool SKIndexingConsumer::finishDependency(UIdent Kind) {
  assert(DependenciesStack.back().Kind == Kind);
  DependenciesStack.pop_back();
  return true;
}

bool SKIndexingConsumer::startSourceEntity(const EntityInfo &Info) {
  Entity &Parent = EntitiesStack.back();
  ResponseBuilder::Array &Arr = Parent.Entities;
  if (Arr.isNull())
    Arr = Parent.Data.setArray(KeyEntities);

  auto Elem = Arr.appendDictionary();
  Elem.set(KeyKind, Info.Kind);
  if (!Info.Name.empty())
    Elem.set(KeyName, Info.Name);
  if (!Info.USR.empty())
    Elem.set(KeyUSR, Info.USR);
  if (Info.Line != 0) {
    assert(Info.Column != 0);
    Elem.set(KeyLine, Info.Line);
    Elem.set(KeyColumn, Info.Column);
  }
  if (!Info.Group.empty())
    Elem.set(KeyGroupName, Info.Group);

  if (!Info.ReceiverUSR.empty())
    Elem.set(KeyReceiverUSR, Info.ReceiverUSR);
  if (Info.IsDynamic)
    Elem.setBool(KeyIsDynamic, true);
  if (Info.IsImplicit)
    Elem.setBool(KeyIsImplicit, true);
  if (Info.IsTestCandidate)
    Elem.setBool(KeyIsTestCandidate, true);

  if (!Info.Attrs.empty()) {
    auto AttrArray = Elem.setArray(KeyAttributes);
    for (auto Attr : Info.Attrs) {
      auto AttrDict = AttrArray.appendDictionary();
      AttrDict.set(KeyAttribute, Attr);
    }
  }

  if (Info.EffectiveAccess)
    Elem.set(KeyEffectiveAccess, Info.EffectiveAccess.value());

  EntitiesStack.push_back({ Info.Kind, Elem, ResponseBuilder::Array(),
                            ResponseBuilder::Array()});
  return true;
}

bool SKIndexingConsumer::recordRelatedEntity(const EntityInfo &Info) {
  assert(EntitiesStack.size() > 1 && "Related entity at top-level ?");
  Entity &Parent = EntitiesStack.back();
  ResponseBuilder::Array &Arr = Parent.Related;
  if (Arr.isNull())
    Arr = Parent.Data.setArray(KeyRelated);

  auto Elem = Arr.appendDictionary();
  Elem.set(KeyKind, Info.Kind);
  if (!Info.Name.empty())
    Elem.set(KeyName, Info.Name);
  if (!Info.USR.empty())
    Elem.set(KeyUSR, Info.USR);
  if (Info.Line != 0) {
    assert(Info.Column != 0);
    Elem.set(KeyLine, Info.Line);
    Elem.set(KeyColumn, Info.Column);
  }

  return true;
}

bool SKIndexingConsumer::finishSourceEntity(UIdent Kind) {
  Entity &CurrEnt = EntitiesStack.back();
  assert(CurrEnt.Kind == Kind);
  (void) CurrEnt;
  EntitiesStack.pop_back();
  return true;
}

//===----------------------------------------------------------------------===//
// ReportDocInfo
//===----------------------------------------------------------------------===//

namespace {

class SKDocConsumer : public DocInfoConsumer {
  ResponseBuilder &RespBuilder;

  struct Entity {
    UIdent Kind;
    ResponseBuilder::Dictionary Data;
    ResponseBuilder::Array Entities;
    ResponseBuilder::Array Inherits;
    ResponseBuilder::Array Conforms;
    ResponseBuilder::Array Attrs;
  };
  SmallVector<Entity, 6> EntitiesStack;

  ResponseBuilder::Dictionary TopDict;
  ResponseBuilder::Array Diags;

  DocSupportAnnotationArrayBuilder AnnotationsBuilder;

  bool Cancelled = false;

  void addDocEntityInfoToDict(const DocEntityInfo &Info,
                              ResponseBuilder::Dictionary Dict);
public:
  std::string ErrorDescription;

  explicit SKDocConsumer(ResponseBuilder &RespBuilder)
      : RespBuilder(RespBuilder) {
    TopDict = RespBuilder.getDictionary();

    // First in stack is the top-level "key.entities" container.
    EntitiesStack.push_back(
        { UIdent(),
          TopDict,
          ResponseBuilder::Array(),
          ResponseBuilder::Array(),
          ResponseBuilder::Array(),
          ResponseBuilder::Array() });
  }
  ~SKDocConsumer() override {
    assert(Cancelled || EntitiesStack.size() == 1);
    (void) Cancelled;
  }

  sourcekitd_response_t createResponse() {
    TopDict.setCustomBuffer(KeyAnnotations, AnnotationsBuilder.createBuffer());
    return RespBuilder.createResponse();
  }

  void failed(StringRef ErrDescription) override;

  bool handleSourceText(StringRef Text) override;

  bool handleAnnotation(const DocEntityInfo &Info) override;

  bool startSourceEntity(const DocEntityInfo &Info) override;

  bool handleInheritsEntity(const DocEntityInfo &Info) override;
  bool handleConformsToEntity(const DocEntityInfo &Info) override;
  bool handleExtendsEntity(const DocEntityInfo &Info) override;

  bool handleAvailableAttribute(const AvailableAttrInfo &Info) override;

  bool finishSourceEntity(UIdent Kind) override;

  bool handleDiagnostics(ArrayRef<DiagnosticEntryInfo> Diags) override;
};
} // end anonymous namespace

static sourcekitd_response_t reportDocInfo(llvm::MemoryBuffer *InputBuf,
                                           StringRef ModuleName,
                                           ArrayRef<const char *> Args) {
  ResponseBuilder RespBuilder;
  SKDocConsumer DocConsumer(RespBuilder);
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.getDocInfo(InputBuf, ModuleName, Args, DocConsumer);

  if (!DocConsumer.ErrorDescription.empty())
    return createErrorRequestFailed(DocConsumer.ErrorDescription.c_str());

  return DocConsumer.createResponse();
}

void SKDocConsumer::addDocEntityInfoToDict(const DocEntityInfo &Info,
                                           ResponseBuilder::Dictionary Elem) {
  Elem.set(KeyKind, Info.Kind);
  if (!Info.Name.empty())
    Elem.set(KeyName, Info.Name);
  if (!Info.Argument.empty())
    Elem.set(KeyKeyword, Info.Argument);
  if (!Info.SubModuleName.empty())
    Elem.set(KeyModuleName, Info.SubModuleName);
  if (!Info.USR.empty())
    Elem.set(KeyUSR, Info.USR);
  if (!Info.OriginalUSR.empty())
    Elem.set(KeyOriginalUSR, Info.OriginalUSR);
  if (!Info.ProvideImplementationOfUSR.empty())
    Elem.set(KeyDefaultImplementationOf, Info.ProvideImplementationOfUSR);
  if (Info.Length > 0) {
    Elem.set(KeyOffset, Info.Offset);
    Elem.set(KeyLength, Info.Length);
  }
  if (Info.IsUnavailable)
    Elem.set(KeyIsUnavailable, Info.IsUnavailable);
  if (Info.IsDeprecated)
    Elem.set(KeyIsDeprecated, Info.IsDeprecated);
  if (Info.IsOptional)
    Elem.set(KeyIsOptional, Info.IsOptional);
  if (Info.IsAsync)
    Elem.set(KeyIsAsync, Info.IsAsync);
  if (!Info.DocComment.empty())
    Elem.set(KeyDocFullAsXML, Info.DocComment);
  if (!Info.FullyAnnotatedDecl.empty())
    Elem.set(KeyFullyAnnotatedDecl, Info.FullyAnnotatedDecl);
  if (!Info.FullyAnnotatedGenericSig.empty())
    Elem.set(KeyFullyAnnotatedGenericSignature, Info.FullyAnnotatedGenericSig);
  if (!Info.LocalizationKey.empty())
    Elem.set(KeyLocalizationKey, Info.LocalizationKey);

  if (!Info.GenericParams.empty()) {
    auto GPArray = Elem.setArray(KeyGenericParams);
    for (auto &GP : Info.GenericParams) {
      auto GPElem = GPArray.appendDictionary();
      GPElem.set(KeyName, GP.Name);
      if (!GP.Inherits.empty())
        GPElem.set(KeyInherits, GP.Inherits);
    }
  }
  // Note that due to protocol extensions, GenericRequirements may be non-empty
  // while GenericParams is empty.
  if (!Info.GenericRequirements.empty()) {
    auto ReqArray = Elem.setArray(KeyGenericRequirements);

    for (auto &Req : Info.GenericRequirements) {
      auto ReqElem = ReqArray.appendDictionary();
      ReqElem.set(KeyDescription, Req);
    }
  }

  if (!Info.RequiredBystanders.empty())
    Elem.set(KeyRequiredBystanders, Info.RequiredBystanders);
}

void SKDocConsumer::failed(StringRef ErrDescription) {
  ErrorDescription = ErrDescription.str();
}

bool SKDocConsumer::handleSourceText(StringRef Text) {
  TopDict.set(KeySourceText, Text);
  return true;
}

bool SKDocConsumer::handleAnnotation(const DocEntityInfo &Info) {
  AnnotationsBuilder.add(Info);
  return true;
}

bool SKDocConsumer::startSourceEntity(const DocEntityInfo &Info) {
  Entity &Parent = EntitiesStack.back();
  ResponseBuilder::Array &Arr = Parent.Entities;
  if (Arr.isNull())
    Arr = Parent.Data.setArray(KeyEntities);

  auto Elem = Arr.appendDictionary();
  addDocEntityInfoToDict(Info, Elem);

  EntitiesStack.push_back({ Info.Kind, Elem, ResponseBuilder::Array(),
                            ResponseBuilder::Array(),
                            ResponseBuilder::Array(),
                            ResponseBuilder::Array()});
  return true;
}

bool SKDocConsumer::handleInheritsEntity(const DocEntityInfo &Info) {
  assert(EntitiesStack.size() > 1 && "Related entity at top-level ?");
  Entity &Parent = EntitiesStack.back();
  ResponseBuilder::Array &Arr = Parent.Inherits;
  if (Arr.isNull())
    Arr = Parent.Data.setArray(KeyInherits);

  addDocEntityInfoToDict(Info, Arr.appendDictionary());
  return true;
}

bool SKDocConsumer::handleConformsToEntity(const DocEntityInfo &Info) {
  assert(EntitiesStack.size() > 1 && "Related entity at top-level ?");
  Entity &Parent = EntitiesStack.back();
  ResponseBuilder::Array &Arr = Parent.Conforms;
  if (Arr.isNull())
    Arr = Parent.Data.setArray(KeyConforms);

  addDocEntityInfoToDict(Info, Arr.appendDictionary());
  return true;
}

bool SKDocConsumer::handleExtendsEntity(const DocEntityInfo &Info) {
  assert(EntitiesStack.size() > 1 && "Related entity at top-level ?");
  Entity &Parent = EntitiesStack.back();
  addDocEntityInfoToDict(Info, Parent.Data.setDictionary(KeyExtends));
  return true;
}

bool SKDocConsumer::handleAvailableAttribute(const AvailableAttrInfo &Info) {
  Entity &Parent = EntitiesStack.back();
  ResponseBuilder::Array &Arr = Parent.Attrs;
  if (Arr.isNull())
    Arr = Parent.Data.setArray(KeyAttributes);

  auto Elem = Arr.appendDictionary();
  Elem.set(KeyKind, Info.AttrKind);
  if (Info.IsUnavailable)
    Elem.set(KeyIsUnavailable, Info.IsUnavailable);
  if (Info.IsDeprecated)
    Elem.set(KeyIsDeprecated, Info.IsDeprecated);
  if (Info.Platform.isValid())
    Elem.set(KeyPlatform, Info.Platform);
  if (!Info.Message.empty())
    Elem.set(KeyMessage, Info.Message);
  if (Info.Introduced.has_value())
    Elem.set(KeyIntroduced, Info.Introduced.value().getAsString());
  if (Info.Deprecated.has_value())
    Elem.set(KeyDeprecated, Info.Deprecated.value().getAsString());
  if (Info.Obsoleted.has_value())
    Elem.set(KeyObsoleted, Info.Obsoleted.value().getAsString());

  return true;
}

bool SKDocConsumer::finishSourceEntity(UIdent Kind) {
  Entity &CurrEnt = EntitiesStack.back();
  assert(CurrEnt.Kind == Kind);
  (void) CurrEnt;
  EntitiesStack.pop_back();
  return true;
}

bool SKDocConsumer::handleDiagnostics(ArrayRef<DiagnosticEntryInfo> Diags) {
  if (Diags.empty())
    return true;

  fillDiagnosticInfo(TopDict, Diags, /*DiagStage*/ None);
  return true;
}

//===----------------------------------------------------------------------===//
// ReportCursorInfo
//===----------------------------------------------------------------------===//

static void addCursorSymbolInfo(const CursorSymbolInfo &Symbol,
                                ResponseBuilder::Dictionary &Elem) {
  Elem.set(KeyKind, Symbol.Kind);
  if (Symbol.DeclarationLang.isValid())
    Elem.set(KeyDeclarationLang, Symbol.DeclarationLang);
  Elem.set(KeyName, Symbol.Name);
  if (!Symbol.USR.empty())
    Elem.set(KeyUSR, Symbol.USR);
  if (!Symbol.TypeName.empty())
    Elem.set(KeyTypeName, Symbol.TypeName);
  if (!Symbol.TypeUSR.empty())
    Elem.set(KeyTypeUsr, Symbol.TypeUSR);
  if (!Symbol.ContainerTypeUSR.empty())
    Elem.set(KeyContainerTypeUsr, Symbol.ContainerTypeUSR);
  if (!Symbol.DocComment.empty())
    Elem.set(KeyDocFullAsXML, Symbol.DocComment);
  if (!Symbol.GroupName.empty())
    Elem.set(KeyGroupName, Symbol.GroupName);
  if (!Symbol.LocalizationKey.empty())
    Elem.set(KeyLocalizationKey, Symbol.LocalizationKey);
  if (!Symbol.AnnotatedDeclaration.empty())
    Elem.set(KeyAnnotatedDecl, Symbol.AnnotatedDeclaration);
  if (!Symbol.FullyAnnotatedDeclaration.empty())
    Elem.set(KeyFullyAnnotatedDecl, Symbol.FullyAnnotatedDeclaration);
  if (!Symbol.SymbolGraph.empty())
    Elem.set(KeySymbolGraph, Symbol.SymbolGraph);
  if (!Symbol.ModuleName.empty())
    Elem.set(KeyModuleName, Symbol.ModuleName);
  if (!Symbol.ModuleInterfaceName.empty())
    Elem.set(KeyModuleInterfaceName, Symbol.ModuleInterfaceName);
  if (!Symbol.Location.Filename.empty()) {
    Elem.set(KeyFilePath, Symbol.Location.Filename);
    Elem.set(KeyOffset, Symbol.Location.Offset);
    Elem.set(KeyLength, Symbol.Location.Length);
    Elem.set(KeyLine, Symbol.Location.Line);
    Elem.set(KeyColumn, Symbol.Location.Column);
  }

  if (!Symbol.OverrideUSRs.empty()) {
    auto Overrides = Elem.setArray(KeyOverrides);
    for (auto USR : Symbol.OverrideUSRs) {
      auto Override = Overrides.appendDictionary();
      Override.set(KeyUSR, USR);
    }
  }

  if (!Symbol.AnnotatedRelatedDeclarations.empty()) {
    auto RelDecls = Elem.setArray(KeyRelatedDecls);
    for (auto AnnotDecl : Symbol.AnnotatedRelatedDeclarations) {
      auto RelDecl = RelDecls.appendDictionary();
      RelDecl.set(KeyAnnotatedDecl, AnnotDecl);
    }
  }

  if (!Symbol.ModuleGroupArray.empty()) {
    auto Groups = Elem.setArray(KeyModuleGroups);
    for (auto Name : Symbol.ModuleGroupArray) {
      auto Entry = Groups.appendDictionary();
      Entry.set(KeyGroupName, Name);
    }
  }

  if (!Symbol.ParentContexts.empty()) {
    auto Parents = Elem.setArray(KeyParentContexts);
    for (const auto &ParentTy : Symbol.ParentContexts) {
      auto Parent = Parents.appendDictionary();
      Parent.set(KeyName, ParentTy.Title);
      Parent.set(KeyKind, ParentTy.KindName);
      Parent.set(KeyUSR, ParentTy.USR);
    }
  }

  if (!Symbol.ReferencedSymbols.empty()) {
    auto Refs = Elem.setArray(KeyReferencedSymbols);
    for (const auto &Ref: Symbol.ReferencedSymbols) {
      auto Symbol = Refs.appendDictionary();
      Symbol.set(KeyUSR, Ref.USR);
      Symbol.set(KeyAccessLevel, Ref.AccessLevel);
      Symbol.set(KeyFilePath, Ref.FilePath);
      Symbol.set(KeyModuleName, Ref.ModuleName);
      Symbol.set(KeyDeclarationLang, Ref.DeclarationLang);
      Symbol.setBool(KeyIsSystem, Ref.IsSystem);
      Symbol.setBool(KeyIsSPI, Ref.IsSPI);

      auto Parents = Symbol.setArray(KeyParentContexts);
      for (const auto &ParentTy : Ref.ParentContexts) {
        auto Parent = Parents.appendDictionary();
        Parent.set(KeyName, ParentTy.Title);
        Parent.set(KeyKind, ParentTy.KindName);
        Parent.set(KeyUSR, ParentTy.USR);
      }
    }
  }

  if (!Symbol.ReceiverUSRs.empty()) {
    auto Receivers = Elem.setArray(KeyReceivers);
    for (auto USR : Symbol.ReceiverUSRs) {
      auto Receiver = Receivers.appendDictionary();
      Receiver.set(KeyUSR, USR);
    }
  }

  if (Symbol.IsSystem)
    Elem.setBool(KeyIsSystem, true);
  if (Symbol.IsDynamic)
    Elem.setBool(KeyIsDynamic, true);
  if (Symbol.IsSynthesized)
    Elem.setBool(KeyIsSynthesized, true);

  if (Symbol.ParentNameOffset)
    Elem.set(KeyParentLoc, Symbol.ParentNameOffset.value());
}

static void reportCursorInfo(const RequestResult<CursorInfoData> &Result,
                             ResponseReceiver Rec) {
  if (Result.isCancelled())
    return Rec(createErrorRequestCancelled());
  if (Result.isError())
    return Rec(createErrorRequestFailed(Result.getError()));

  const CursorInfoData &Info = Result.value();

  ResponseBuilder RespBuilder;
  auto Elem = RespBuilder.getDictionary();
  if (!Info.InternalDiagnostic.empty()) {
    Elem.set(KeyInternalDiagnostic, Info.InternalDiagnostic);
    return Rec(RespBuilder.createResponse());
  }

  if (!Info.Symbols.empty()) {
    addCursorSymbolInfo(Info.Symbols[0], Elem);
    if (Info.Symbols.size() > 1) {
      auto SecondarySymbols = Elem.setArray(KeySecondarySymbols);
      for (auto Secondary : makeArrayRef(Info.Symbols).drop_front()) {
        auto SecondaryElem = SecondarySymbols.appendDictionary();
        addCursorSymbolInfo(Secondary, SecondaryElem);
      }
    }
  }

  if (!Info.AvailableActions.empty()) {
    // Clients rely on a kind being set to determine whether the cursor
    // has any results or not. Add one if there's no symbols, ie. only actions
    // were requested (even though it's meaningless).
    if (Info.Symbols.empty())
      Elem.set(KeyKind, KindRefModule);
    auto Actions = Elem.setArray(KeyRefactorActions);
    for (auto Info : Info.AvailableActions) {
      auto Entry = Actions.appendDictionary();
      Entry.set(KeyActionUID, Info.Kind);
      Entry.set(KeyActionName, Info.KindName);
      if (!Info.UnavailableReason.empty())
        Entry.set(KeyActionUnavailableReason, Info.UnavailableReason);
    }
  }

  Elem.setBool(KeyReusingASTContext, Info.DidReuseAST);

  return Rec(RespBuilder.createResponse());
}

//===----------------------------------------------------------------------===//
// ReportDiagnostics
//===----------------------------------------------------------------------===//

static void reportDiagnostics(const RequestResult<DiagnosticsResult> &Result,
                              ResponseReceiver Rec) {
  if (Result.isCancelled())
    return Rec(createErrorRequestCancelled());
  if (Result.isError())
    return Rec(createErrorRequestFailed(Result.getError()));

  auto &DiagResults = Result.value();

  ResponseBuilder RespBuilder;
  auto Dict = RespBuilder.getDictionary();
  fillDiagnosticInfo(Dict, DiagResults, /*DiagStage*/ None);
  Rec(RespBuilder.createResponse());
}

//===----------------------------------------------------------------------===//
// ReportRangeInfo
//===----------------------------------------------------------------------===//

static void reportRangeInfo(const RequestResult<RangeInfo> &Result,
                            ResponseReceiver Rec) {
  if (Result.isCancelled())
    return Rec(createErrorRequestCancelled());
  if (Result.isError())
    return Rec(createErrorRequestFailed(Result.getError()));

  const RangeInfo &Info = Result.value();

  ResponseBuilder RespBuilder;
  auto Elem = RespBuilder.getDictionary();
  Elem.set(KeyKind, Info.RangeKind);
  Elem.set(KeyTypeName, Info.ExprType);
  Elem.set(KeyRangeContent, Info.RangeContent);
  Rec(RespBuilder.createResponse());
}

//===----------------------------------------------------------------------===//
// ReportNameInfo
//===----------------------------------------------------------------------===//

static void reportNameInfo(const RequestResult<NameTranslatingInfo> &Result,
                           ResponseReceiver Rec) {
  if (Result.isCancelled())
    return Rec(createErrorRequestCancelled());
  if (Result.isError())
    return Rec(createErrorRequestFailed(Result.getError()));

  const NameTranslatingInfo &Info = Result.value();

  ResponseBuilder RespBuilder;
  if (!Info.InternalDiagnostic.empty()) {
    auto Elem = RespBuilder.getDictionary();
    Elem.set(KeyInternalDiagnostic, Info.InternalDiagnostic);
    return Rec(RespBuilder.createResponse());
  }
  if (Info.NameKind.isInvalid())
    return Rec(RespBuilder.createResponse());
  if (Info.BaseName.empty() && Info.ArgNames.empty())
    return Rec(RespBuilder.createResponse());

  auto Elem = RespBuilder.getDictionary();
  Elem.set(KeyNameKind, Info.NameKind);

  if (!Info.BaseName.empty()) {
    Elem.set(KeyBaseName, Info.BaseName);
  }
  if (!Info.ArgNames.empty()) {
    static UIdent UIDKindNameSwift(KindNameSwift.str());
    auto Arr = Elem.setArray(Info.NameKind == UIDKindNameSwift ?
                             KeyArgNames : KeySelectorPieces);
    for (auto N : Info.ArgNames) {
      auto NameEle = Arr.appendDictionary();
      NameEle.set(KeyName, N);
    }
  }
  if (Info.IsZeroArgSelector) {
    Elem.set(KeyIsZeroArgSelector, Info.IsZeroArgSelector);
  }
  Rec(RespBuilder.createResponse());
}

//===----------------------------------------------------------------------===//
// ReportExpressionTypeInfo
//===----------------------------------------------------------------------===//
static void reportExpressionTypeInfo(const RequestResult<ExpressionTypesInFile> &Result,
                                     ResponseReceiver Rec) {
  if (Result.isCancelled())
    return Rec(createErrorRequestCancelled());
  if (Result.isError())
    return Rec(createErrorRequestFailed(Result.getError()));

  const ExpressionTypesInFile &Info = Result.value();

  ResponseBuilder Builder;
  auto Dict = Builder.getDictionary();
  ExpressionTypeArrayBuilder ArrBuilder(Info.TypeBuffer);
  for (auto &R: Info.Results) {
    ArrBuilder.add(R);
  }
  Dict.setCustomBuffer(KeyExpressionTypeList, ArrBuilder.createBuffer());
  Rec(Builder.createResponse());
}

//===----------------------------------------------------------------------===//
// ReportVariableTypeInfo
//===----------------------------------------------------------------------===//

static void
reportVariableTypeInfo(const RequestResult<VariableTypesInFile> &Result,
                       ResponseReceiver Rec) {
  if (Result.isCancelled())
    return Rec(createErrorRequestCancelled());
  if (Result.isError())
    return Rec(createErrorRequestFailed(Result.getError()));

  const VariableTypesInFile &Info = Result.value();

  ResponseBuilder Builder;
  auto Dict = Builder.getDictionary();
  VariableTypeArrayBuilder ArrBuilder(Info.TypeBuffer);
  for (auto &R : Info.Results) {
    ArrBuilder.add(R);
  }
  Dict.setCustomBuffer(KeyVariableTypeList, ArrBuilder.createBuffer());
  Rec(Builder.createResponse());
}

//===----------------------------------------------------------------------===//
// FindRelatedIdents
//===----------------------------------------------------------------------===//

static void findRelatedIdents(StringRef PrimaryFilePath,
                              StringRef InputBufferName, int64_t Offset,
                              bool CancelOnSubsequentRequest,
                              ArrayRef<const char *> Args,
                              SourceKitCancellationToken CancellationToken,
                              ResponseReceiver Rec) {
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.findRelatedIdentifiersInFile(
      PrimaryFilePath, InputBufferName, Offset, CancelOnSubsequentRequest, Args,
      CancellationToken, [Rec](const RequestResult<RelatedIdentsInfo> &Result) {
        if (Result.isCancelled())
          return Rec(createErrorRequestCancelled());
        if (Result.isError())
          return Rec(createErrorRequestFailed(Result.getError()));

        const RelatedIdentsInfo &Info = Result.value();

        ResponseBuilder RespBuilder;
        auto Arr = RespBuilder.getDictionary().setArray(KeyResults);
        for (auto R : Info.Ranges) {
          auto Elem = Arr.appendDictionary();
          Elem.set(KeyOffset, R.first);
          Elem.set(KeyLength, R.second);
        }

        Rec(RespBuilder.createResponse());
      });
}

//===----------------------------------------------------------------------===//
// FindActiveRegions
//===----------------------------------------------------------------------===//

static void findActiveRegions(StringRef PrimaryFilePath,
                              StringRef InputBufferName,
                              ArrayRef<const char *> Args,
                              SourceKitCancellationToken CancellationToken,
                              ResponseReceiver Rec) {
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();

  Lang.findActiveRegionsInFile(
      PrimaryFilePath, InputBufferName, Args, CancellationToken,
      [Rec](const RequestResult<ActiveRegionsInfo> &Result) {
        if (Result.isCancelled())
          return Rec(createErrorRequestCancelled());
        if (Result.isError())
          return Rec(createErrorRequestFailed(Result.getError()));

        const ActiveRegionsInfo &Info = Result.value();

        ResponseBuilder RespBuilder;
        auto Arr = RespBuilder.getDictionary().setArray(KeyResults);
        for (auto Config : Info.Configs) {
          auto Elem = Arr.appendDictionary();
          Elem.set(KeyOffset, Config.Offset);
          if (Config.IsActive)
            Elem.setBool(KeyIsActive, true);
        }

        Rec(RespBuilder.createResponse());
      });
}

//===----------------------------------------------------------------------===//
// CodeComplete
//===----------------------------------------------------------------------===//

namespace {
class SKCodeCompletionConsumer : public CodeCompletionConsumer {
  ResponseBuilder &RespBuilder;
  CodeCompletionResultsArrayBuilder ResultsBuilder;

  std::string ErrorDescription;
  bool WasCancelled = false;

public:
  explicit SKCodeCompletionConsumer(ResponseBuilder &RespBuilder)
    : RespBuilder(RespBuilder) {
  }

  sourcekitd_response_t createResponse() {
    if (WasCancelled) {
      return createErrorRequestCancelled();
    } else if (!ErrorDescription.empty()) {
      return createErrorRequestFailed(ErrorDescription.c_str());
    } else {
      RespBuilder.getDictionary().setCustomBuffer(
          KeyResults, ResultsBuilder.createBuffer());
      return RespBuilder.createResponse();
    }
  }


  void failed(StringRef ErrDescription) override;
  void cancelled() override;

  void setCompletionKind(UIdent kind) override;
  void setReusingASTContext(bool flag) override;
  void setAnnotatedTypename(bool flag) override;
  bool handleResult(const CodeCompletionInfo &Info) override;
};
} // end anonymous namespace

static sourcekitd_response_t
codeComplete(llvm::MemoryBuffer *InputBuf, int64_t Offset,
             Optional<RequestDict> optionsDict, ArrayRef<const char *> Args,
             Optional<VFSOptions> vfsOptions,
             SourceKitCancellationToken CancellationToken) {
  ResponseBuilder RespBuilder;
  SKCodeCompletionConsumer CCC(RespBuilder);

  std::unique_ptr<SKOptionsDictionary> options;
  if (optionsDict)
    options = std::make_unique<SKOptionsDictionary>(*optionsDict);

  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.codeComplete(InputBuf, Offset, options.get(), CCC, Args,
                    std::move(vfsOptions), CancellationToken);
  return CCC.createResponse();
}

void SKCodeCompletionConsumer::failed(StringRef ErrDescription) {
  ErrorDescription = ErrDescription.str();
}

void SKCodeCompletionConsumer::cancelled() { WasCancelled = true; }

void SKCodeCompletionConsumer::setCompletionKind(UIdent kind) {
  assert(kind.isValid());
  RespBuilder.getDictionary().set(KeyKind, kind);
}

void SKCodeCompletionConsumer::setReusingASTContext(bool flag) {
  if (flag)
    RespBuilder.getDictionary().setBool(KeyReusingASTContext, flag);
}

void SKCodeCompletionConsumer::setAnnotatedTypename(bool flag) {
  if (flag)
    RespBuilder.getDictionary().setBool(KeyAnnotatedTypename, flag);
}

bool SKCodeCompletionConsumer::handleResult(const CodeCompletionInfo &R) {
  Optional<StringRef> ModuleNameOpt;
  if (!R.ModuleName.empty())
    ModuleNameOpt = R.ModuleName;
  Optional<StringRef> DocBriefOpt;
  if (!R.DocBrief.empty())
    DocBriefOpt = R.DocBrief;
  Optional<StringRef> AssocUSRsOpt;
  if (!R.AssocUSRs.empty())
    AssocUSRsOpt = R.AssocUSRs;

  assert(!R.ModuleImportDepth && "not implemented on CompactArray path");

  ResultsBuilder.add(R.Kind,
                     R.Name,
                     R.Description,
                     R.SourceText,
                     R.TypeName,
                     ModuleNameOpt,
                     DocBriefOpt,
                     AssocUSRsOpt,
                     R.SemanticContext,
                     R.TypeRelation,
                     R.NotRecommended,
                     R.IsSystem,
                     R.NumBytesToErase);
  return true;
}


//===----------------------------------------------------------------------===//
// (New) CodeComplete
//===----------------------------------------------------------------------===//

namespace {
class SKGroupedCodeCompletionConsumer : public GroupedCodeCompletionConsumer {
  ResponseBuilder &RespBuilder;
  ResponseBuilder::Dictionary Response;
  SmallVector<ResponseBuilder::Array, 3> GroupContentsStack;
  std::string ErrorDescription;
  bool WasCancelled = false;

public:
  explicit SKGroupedCodeCompletionConsumer(ResponseBuilder &RespBuilder)
      : RespBuilder(RespBuilder) {}

  sourcekitd_response_t createResponse() {
    if (WasCancelled) {
      return createErrorRequestCancelled();
    } else if (!ErrorDescription.empty()) {
      return createErrorRequestFailed(ErrorDescription.c_str());
    } else {
      assert(GroupContentsStack.empty() && "mismatched start/endGroup");
      return RespBuilder.createResponse();
    }
  }

  void failed(StringRef ErrDescription) override;
  void cancelled() override;
  bool handleResult(const CodeCompletionInfo &Info) override;
  void startGroup(UIdent kind, StringRef name) override;
  void endGroup() override;
  void setNextRequestStart(unsigned offset) override;
  void setReusingASTContext(bool flag) override;
  void setAnnotatedTypename(bool flag) override;
};
} // end anonymous namespace

static sourcekitd_response_t
codeCompleteOpen(StringRef Name, llvm::MemoryBuffer *InputBuf, int64_t Offset,
                 Optional<RequestDict> optionsDict, ArrayRef<const char *> Args,
                 Optional<VFSOptions> vfsOptions,
                 SourceKitCancellationToken CancellationToken) {
  ResponseBuilder RespBuilder;
  SKGroupedCodeCompletionConsumer CCC(RespBuilder);
  std::unique_ptr<SKOptionsDictionary> options;
  std::vector<FilterRule> filterRules;
  if (optionsDict) {
    options = std::make_unique<SKOptionsDictionary>(*optionsDict);
    bool failed = false;
    optionsDict->dictionaryArrayApply(KeyFilterRules, [&](RequestDict dict) {
      FilterRule rule;
      auto kind = dict.getUID(KeyKind);
      if (kind == KindCodeCompletionEverything) {
        rule.kind = FilterRule::Everything;
      } else if (kind == KindCodeCompletionModule) {
        rule.kind = FilterRule::Module;
      } else if (kind == KindCodeCompletionKeyword) {
        rule.kind = FilterRule::Keyword;
      } else if (kind == KindCodeCompletionLiteral) {
        rule.kind = FilterRule::Literal;
      } else if (kind == KindCodeCompletionCustom) {
        rule.kind = FilterRule::CustomCompletion;
      } else if (kind == KindCodeCompletionIdentifier) {
        rule.kind = FilterRule::Identifier;
      } else if (kind == KindCodeCompletionDescription) {
        rule.kind = FilterRule::Description;
      } else {
        // Warning: unknown
      }

      int64_t hide;
      if (dict.getInt64(KeyHide, hide, false)) {
        failed = true;
        CCC.failed("filter rule missing required key 'key.hide'");
        return true;
      }

      rule.hide = hide;

      switch (rule.kind) {
      case FilterRule::Everything:
        break;
      case FilterRule::Module:
      case FilterRule::Identifier:
      case FilterRule::Description: {
        SmallVector<const char *, 8> names;
        if (dict.getStringArray(KeyNames, names, false)) {
          failed = true;
          CCC.failed("filter rule missing required key 'key.names'");
          return true;
        }
        rule.names.assign(names.begin(), names.end());
        break;
      }
      case FilterRule::Keyword:
      case FilterRule::Literal:
      case FilterRule::CustomCompletion: {
        SmallVector<sourcekitd_uid_t, 8> uids;
        dict.getUIDArray(KeyUIDs, uids, true);
        for (auto uid : uids)
          rule.uids.push_back(UIdentFromSKDUID(uid));
        break;
      }
      }

      filterRules.push_back(std::move(rule));
      return false; // continue
    });

    if (failed)
      return CCC.createResponse();
  }
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.codeCompleteOpen(Name, InputBuf, Offset, options.get(), filterRules, CCC,
                        Args, std::move(vfsOptions), CancellationToken);
  return CCC.createResponse();
}

static sourcekitd_response_t codeCompleteClose(StringRef Name, int64_t Offset) {
  ResponseBuilder RespBuilder;
  SKGroupedCodeCompletionConsumer CCC(RespBuilder);
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.codeCompleteClose(Name, Offset, CCC);
  return CCC.createResponse();
}

static sourcekitd_response_t
codeCompleteUpdate(StringRef name, int64_t offset,
                   Optional<RequestDict> optionsDict,
                   SourceKitCancellationToken CancellationToken) {
  ResponseBuilder RespBuilder;
  SKGroupedCodeCompletionConsumer CCC(RespBuilder);
  std::unique_ptr<SKOptionsDictionary> options;
  if (optionsDict)
    options = std::make_unique<SKOptionsDictionary>(*optionsDict);
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.codeCompleteUpdate(name, offset, options.get(), CancellationToken, CCC);
  return CCC.createResponse();
}

void SKGroupedCodeCompletionConsumer::failed(StringRef ErrDescription) {
  ErrorDescription = ErrDescription.str();
}

void SKGroupedCodeCompletionConsumer::cancelled() { WasCancelled = true; }

bool SKGroupedCodeCompletionConsumer::handleResult(const CodeCompletionInfo &R) {
  assert(!GroupContentsStack.empty() && "missing root group");

  auto result = GroupContentsStack.back().appendDictionary();
  if (R.CustomKind)
    result.set(KeyKind, sourcekitd_uid_t(R.CustomKind));
  else
    result.set(KeyKind, R.Kind);
  result.set(KeyName, R.Name);
  result.set(KeyDescription, R.Description);
  result.set(KeySourceText, R.SourceText);
  result.set(KeyTypeName, R.TypeName);
  result.set(KeyContext, R.SemanticContext);
  if (!R.ModuleName.empty())
    result.set(KeyModuleName, R.ModuleName);
  if (!R.DocBrief.empty())
    result.set(KeyDocBrief, R.DocBrief);
  if (!R.AssocUSRs.empty())
    result.set(KeyAssociatedUSRs, R.AssocUSRs);
  if (R.ModuleImportDepth)
    result.set(KeyModuleImportDepth, *R.ModuleImportDepth);
  if (R.NotRecommended)
    result.set(KeyNotRecommended, R.NotRecommended);
  if (R.IsSystem)
    result.set(KeyIsSystem, R.IsSystem);
  result.set(KeyNumBytesToErase, R.NumBytesToErase);

  if (R.descriptionStructure) {
    auto addRange = [](ResponseBuilder::Dictionary dict, UIdent offset,
                       UIdent length, CodeCompletionInfo::IndexRange range) {
      if (!range.empty()) {
        dict.set(offset, range.begin);
        dict.set(length, range.length());
      }
    };

    auto structure = result.setDictionary(KeySubStructure);
    addRange(structure, KeyNameOffset, KeyNameLength,
             R.descriptionStructure->baseName);
    addRange(structure, KeyBodyOffset, KeyBodyLength,
             R.descriptionStructure->parameterRange);

    if (R.parametersStructure) {
      auto params = structure.setArray(KeySubStructure);
      for (auto &P : *R.parametersStructure) {
        auto param = params.appendDictionary();
        addRange(param, KeyNameOffset, KeyNameLength, P.name);
        addRange(param, KeyBodyOffset, KeyBodyLength, P.afterColon);
        if (P.isLocalName)
          param.set(KeyIsLocal, true);
      }
    }
  }

  return true;
}

void SKGroupedCodeCompletionConsumer::startGroup(UIdent kind, StringRef name) {
  ResponseBuilder::Dictionary group;
  if (GroupContentsStack.empty()) {
    group = RespBuilder.getDictionary();
    Response = group;
  } else {
    group = GroupContentsStack.back().appendDictionary();
  }
  group.set(KeyKind, kind);
  group.set(KeyName, name);
  auto contents = group.setArray(KeyResults);
  GroupContentsStack.push_back(contents);
}
void SKGroupedCodeCompletionConsumer::endGroup() {
  assert(!GroupContentsStack.empty());
  GroupContentsStack.pop_back();
}
void SKGroupedCodeCompletionConsumer::setNextRequestStart(unsigned offset) {
  assert(!Response.isNull());
  Response.set(KeyNextRequestStart, offset);
}
void SKGroupedCodeCompletionConsumer::setReusingASTContext(bool flag) {
  if (flag)
    RespBuilder.getDictionary().setBool(KeyReusingASTContext, flag);
}
void SKGroupedCodeCompletionConsumer::setAnnotatedTypename(bool flag) {
  if (flag)
    RespBuilder.getDictionary().setBool(KeyAnnotatedTypename, flag);
}

//===----------------------------------------------------------------------===//
// Type Context Info
//===----------------------------------------------------------------------===//

static sourcekitd_response_t
typeContextInfo(llvm::MemoryBuffer *InputBuf, int64_t Offset,
                Optional<RequestDict> optionsDict, ArrayRef<const char *> Args,
                Optional<VFSOptions> vfsOptions,
                SourceKitCancellationToken CancellationToken) {
  ResponseBuilder RespBuilder;

  class Consumer : public TypeContextInfoConsumer {
    ResponseBuilder RespBuilder;
    ResponseBuilder::Array SKResults;
    Optional<std::string> ErrorDescription;
    bool WasCancelled = false;

  public:
    Consumer(ResponseBuilder Builder)
        : RespBuilder(Builder),
          SKResults(Builder.getDictionary().setArray(KeyResults)) {}

    void handleResult(const TypeContextInfoItem &Item) override {
      auto SKElem = SKResults.appendDictionary();
      SKElem.set(KeyTypeName, Item.TypeName);
      SKElem.set(KeyTypeUsr, Item.TypeUSR);
      auto members = SKElem.setArray(KeyImplicitMembers);
      for (auto member : Item.ImplicitMembers) {
        auto memberElem = members.appendDictionary();
        memberElem.set(KeyName, member.Name);
        memberElem.set(KeyDescription, member.Description);
        memberElem.set(KeySourceText, member.SourceText);
        if (!member.DocBrief.empty())
          memberElem.set(KeyDocBrief, member.DocBrief);
      }
    }

    void setReusingASTContext(bool flag) override {
      if (flag)
        RespBuilder.getDictionary().setBool(KeyReusingASTContext, flag);
    }

    void failed(StringRef ErrDescription) override {
      ErrorDescription = ErrDescription.str();
    }

    void cancelled() override { WasCancelled = true; }

    bool wasCancelled() const { return WasCancelled; }
    bool isError() const { return ErrorDescription.has_value(); }
    const char *getErrorDescription() const {
      return ErrorDescription->c_str();
    }
  } Consumer(RespBuilder);

  std::unique_ptr<SKOptionsDictionary> options;
  if (optionsDict)
    options = std::make_unique<SKOptionsDictionary>(*optionsDict);

  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.getExpressionContextInfo(InputBuf, Offset, options.get(), Args,
                                CancellationToken, Consumer,
                                std::move(vfsOptions));

  if (Consumer.wasCancelled()) {
    return createErrorRequestCancelled();
  } else if (Consumer.isError()) {
    return createErrorRequestFailed(Consumer.getErrorDescription());
  } else {
    return RespBuilder.createResponse();
  }
}

//===----------------------------------------------------------------------===//
// Conforming Method List
//===----------------------------------------------------------------------===//

static sourcekitd_response_t conformingMethodList(
    llvm::MemoryBuffer *InputBuf, int64_t Offset,
    Optional<RequestDict> optionsDict, ArrayRef<const char *> Args,
    ArrayRef<const char *> ExpectedTypes, Optional<VFSOptions> vfsOptions,
    SourceKitCancellationToken CancellationToken) {
  ResponseBuilder RespBuilder;

  class Consumer : public ConformingMethodListConsumer {
    ResponseBuilder::Dictionary SKResult;
    Optional<std::string> ErrorDescription;
    bool WasCancelled = false;

  public:
    Consumer(ResponseBuilder Builder) : SKResult(Builder.getDictionary()) {}

    void handleResult(const ConformingMethodListResult &Result) override {
      SKResult.set(KeyTypeName, Result.TypeName);
      SKResult.set(KeyTypeUsr, Result.TypeUSR);
      auto members = SKResult.setArray(KeyMembers);
      for (auto member : Result.Members) {
        auto memberElem = members.appendDictionary();
        memberElem.set(KeyName, member.Name);
        memberElem.set(KeyTypeName, member.TypeName);
        memberElem.set(KeyTypeUsr, member.TypeUSR);
        memberElem.set(KeyDescription, member.Description);
        memberElem.set(KeySourceText, member.SourceText);
        if (!member.DocBrief.empty())
          memberElem.set(KeyDocBrief, member.DocBrief);
      }
    }

    void setReusingASTContext(bool flag) override {
      if (flag)
        SKResult.setBool(KeyReusingASTContext, flag);
    }

    void failed(StringRef ErrDescription) override {
      ErrorDescription = ErrDescription.str();
    }

    void cancelled() override { WasCancelled = true; }

    bool wasCancelled() const { return WasCancelled; }
    bool isError() const { return ErrorDescription.has_value(); }
    const char *getErrorDescription() const {
      return ErrorDescription->c_str();
    }
  } Consumer(RespBuilder);

  std::unique_ptr<SKOptionsDictionary> options;
  if (optionsDict)
    options = std::make_unique<SKOptionsDictionary>(*optionsDict);

  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.getConformingMethodList(InputBuf, Offset, options.get(), Args,
                               ExpectedTypes, CancellationToken, Consumer,
                               std::move(vfsOptions));

  if (Consumer.wasCancelled()) {
    return createErrorRequestCancelled();
  } else if (Consumer.isError()) {
    return createErrorRequestFailed(Consumer.getErrorDescription());
  } else {
    return RespBuilder.createResponse();
  }
}

//===----------------------------------------------------------------------===//
// Editor
//===----------------------------------------------------------------------===//

namespace {
class SKEditorConsumer : public EditorConsumer {
  ResponseReceiver RespReceiver;
  ResponseBuilder RespBuilder;

public:
  ResponseBuilder::Dictionary Dict;
  DocStructureArrayBuilder DocStructure;
  TokenAnnotationsArrayBuilder SyntaxMap;
  TokenAnnotationsArrayBuilder SemanticAnnotations;

  sourcekitd_response_t Error = nullptr;

  SKEditorConsumerOptions Opts;

public:
  SKEditorConsumer(SKEditorConsumerOptions Opts) : Opts(Opts) {
    Dict = RespBuilder.getDictionary();
  }

  SKEditorConsumer(ResponseReceiver RespReceiver, SKEditorConsumerOptions Opts)
      : SKEditorConsumer(Opts) {
    this->RespReceiver = RespReceiver;
  }

  sourcekitd_response_t createResponse();

  bool needsSemanticInfo() override {
    return !Opts.SyntacticOnly && !isSemanticEditorDisabled();
  }

  void handleRequestError(const char *Description) override;

  bool syntaxMapEnabled() override { return Opts.EnableSyntaxMap; }

  void handleSyntaxMap(unsigned Offset, unsigned Length, UIdent Kind) override;

  void handleSemanticAnnotation(unsigned Offset, unsigned Length, UIdent Kind,
                                bool isSystem) override;

  bool documentStructureEnabled() override { return Opts.EnableStructure; }

  void beginDocumentSubStructure(unsigned Offset, unsigned Length, UIdent Kind,
                                 UIdent AccessLevel,
                                 UIdent SetterAccessLevel,
                                 unsigned NameOffset,
                                 unsigned NameLength,
                                 unsigned BodyOffset,
                                 unsigned BodyLength,
                                 unsigned DocOffset,
                                 unsigned DocLength,
                                 StringRef DisplayName,
                                 StringRef TypeName,
                                 StringRef RuntimeName,
                                 StringRef SelectorName,
                                 ArrayRef<StringRef> InheritedTypes,
                                 ArrayRef<std::tuple<UIdent, unsigned, unsigned>> Attrs) override;

  void endDocumentSubStructure() override;

  void handleDocumentSubStructureElement(UIdent Kind, unsigned Offset,
                                         unsigned Length) override;

  void recordAffectedRange(unsigned Offset, unsigned Length) override;

  void recordAffectedLineRange(unsigned Line, unsigned Length) override;

  void recordFormattedText(StringRef Text) override;

  bool diagnosticsEnabled() override { return Opts.EnableDiagnostics; }

  void handleDiagnostics(ArrayRef<DiagnosticEntryInfo> Diags,
                         UIdent DiagStage) override;

  void handleSourceText(StringRef Text) override;

  void finished() override {
    if (RespReceiver)
      RespReceiver(createResponse());
  }
};

} // end anonymous namespace

static sourcekitd_response_t
editorOpen(StringRef Name, llvm::MemoryBuffer *Buf,
           SKEditorConsumerOptions Opts, ArrayRef<const char *> Args,
           Optional<VFSOptions> vfsOptions) {
  SKEditorConsumer EditC(Opts);
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.editorOpen(Name, Buf, EditC, Args, std::move(vfsOptions));
  return EditC.createResponse();
}

static sourcekitd_response_t
editorOpenInterface(StringRef Name, StringRef ModuleName,
                    Optional<StringRef> Group, ArrayRef<const char *> Args,
                    bool SynthesizedExtensions,
                    Optional<StringRef> InterestedUSR) {
  SKEditorConsumerOptions Opts;
  Opts.EnableSyntaxMap = true;
  Opts.EnableStructure = true;
  SKEditorConsumer EditC(Opts);
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.editorOpenInterface(EditC, Name, ModuleName, Group, Args,
                           SynthesizedExtensions, InterestedUSR);
  return EditC.createResponse();
}


/// Getting the interface from a swift source file differs from getting interfaces
/// from headers or modules for its performing asynchronously.
static void editorOpenSwiftSourceInterface(
    StringRef Name, StringRef HeaderName, ArrayRef<const char *> Args,
    SourceKitCancellationToken CancellationToken, ResponseReceiver Rec) {
  SKEditorConsumerOptions Opts;
  Opts.EnableSyntaxMap = true;
  Opts.EnableStructure = true;
  auto EditC = std::make_shared<SKEditorConsumer>(Rec, Opts);
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.editorOpenSwiftSourceInterface(Name, HeaderName, Args, CancellationToken,
                                      EditC);
}

static void
editorOpenSwiftTypeInterface(StringRef TypeUsr, ArrayRef<const char *> Args,
                             ResponseReceiver Rec) {
  SKEditorConsumerOptions Opts;
  Opts.EnableSyntaxMap = true;
  Opts.EnableStructure = true;
  auto EditC = std::make_shared<SKEditorConsumer>(Rec, Opts);
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.editorOpenTypeInterface(*EditC, Args, TypeUsr);
}

static sourcekitd_response_t editorExtractTextFromComment(StringRef Source) {
  SKEditorConsumerOptions Opts;
  Opts.SyntacticOnly = true;
  SKEditorConsumer EditC(Opts);
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.editorExtractTextFromComment(Source, EditC);
  return EditC.createResponse();
}

static sourcekitd_response_t editorConvertMarkupToXML(StringRef Source) {
  SKEditorConsumerOptions Opts;
  Opts.SyntacticOnly = true;
  SKEditorConsumer EditC(Opts);
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.editorConvertMarkupToXML(Source, EditC);
  return EditC.createResponse();
}

static sourcekitd_response_t
editorOpenHeaderInterface(StringRef Name, StringRef HeaderName,
                          ArrayRef<const char *> Args,
                          bool UsingSwiftArgs,
                          bool SynthesizedExtensions,
                          StringRef swiftVersion) {
  SKEditorConsumerOptions Opts;
  Opts.EnableSyntaxMap = true;
  Opts.EnableStructure = true;
  SKEditorConsumer EditC(Opts);
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.editorOpenHeaderInterface(EditC, Name, HeaderName, Args, UsingSwiftArgs,
                                 SynthesizedExtensions, swiftVersion);
  return EditC.createResponse();
}

static sourcekitd_response_t
editorClose(StringRef Name, bool RemoveCache) {
  ResponseBuilder RespBuilder;
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.editorClose(Name, RemoveCache);
  return RespBuilder.createResponse();
}

static sourcekitd_response_t
editorReplaceText(StringRef Name, llvm::MemoryBuffer *Buf, unsigned Offset,
                  unsigned Length, SKEditorConsumerOptions Opts) {
  SKEditorConsumer EditC(Opts);
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.editorReplaceText(Name, Buf, Offset, Length, EditC);
  return EditC.createResponse();
}

static void
editorApplyFormatOptions(StringRef Name, RequestDict &FmtOptions) {
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  SKOptionsDictionary SKFmtOptions(FmtOptions);
  Lang.editorApplyFormatOptions(Name, SKFmtOptions);
}

static sourcekitd_response_t
editorFormatText(StringRef Name, unsigned Line, unsigned Length) {
  SKEditorConsumerOptions Opts;
  Opts.SyntacticOnly = true;
  SKEditorConsumer EditC(Opts);
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.editorFormatText(Name, Line, Length, EditC);
  return EditC.createResponse();
}

static sourcekitd_response_t
editorExpandPlaceholder(StringRef Name, unsigned Offset, unsigned Length) {
  SKEditorConsumerOptions Opts;
  Opts.SyntacticOnly = true;
  SKEditorConsumer EditC(Opts);
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.editorExpandPlaceholder(Name, Offset, Length, EditC);
  return EditC.createResponse();
}

sourcekitd_response_t SKEditorConsumer::createResponse() {
  if (Error)
    return Error;

  if (Opts.EnableSyntaxMap) {
    Dict.setCustomBuffer(KeySyntaxMap, SyntaxMap.createBuffer());
  }
  if (!SemanticAnnotations.empty()) {
    Dict.setCustomBuffer(KeyAnnotations, SemanticAnnotations.createBuffer());
  }
  if (Opts.EnableStructure) {
    Dict.setCustomBuffer(KeySubStructure, DocStructure.createBuffer());
  }


  return RespBuilder.createResponse();
}

void SKEditorConsumer::handleRequestError(const char *Description) {
  if (!Error) {
    Error = createErrorRequestFailed(Description);
  }
  if (RespReceiver) {
    RespReceiver(Error);
    RespReceiver = ResponseReceiver();
  }
}

void SKEditorConsumer::handleSyntaxMap(unsigned Offset, unsigned Length,
                                       UIdent Kind) {
  if (!Opts.EnableSyntaxMap)
    return;

  SyntaxMap.add(Kind, Offset, Length, /*IsSystem=*/false);
}

void SKEditorConsumer::handleSemanticAnnotation(unsigned Offset,
                                                unsigned Length, UIdent Kind,
                                                bool isSystem) {
  assert(Kind.isValid());
  SemanticAnnotations.add(Kind, Offset, Length, isSystem);
}

void
SKEditorConsumer::beginDocumentSubStructure(unsigned Offset,
                                            unsigned Length, UIdent Kind,
                                            UIdent AccessLevel,
                                            UIdent SetterAccessLevel,
                                            unsigned NameOffset,
                                            unsigned NameLength,
                                            unsigned BodyOffset,
                                            unsigned BodyLength,
                                            unsigned DocOffset,
                                            unsigned DocLength,
                                            StringRef DisplayName,
                                            StringRef TypeName,
                                            StringRef RuntimeName,
                                            StringRef SelectorName,
                                            ArrayRef<StringRef> InheritedTypes,
                                            ArrayRef<std::tuple<UIdent, unsigned, unsigned>> Attrs) {
  if (Opts.EnableStructure) {
    DocStructure.beginSubStructure(
        Offset, Length, Kind, AccessLevel, SetterAccessLevel, NameOffset,
        NameLength, BodyOffset, BodyLength, DocOffset, DocLength, DisplayName,
        TypeName, RuntimeName, SelectorName, InheritedTypes, Attrs);
  }
}

void SKEditorConsumer::endDocumentSubStructure() {
  if (Opts.EnableStructure)
    DocStructure.endSubStructure();
}

void SKEditorConsumer::handleDocumentSubStructureElement(UIdent Kind,
                                                         unsigned Offset,
                                                         unsigned Length) {
  if (Opts.EnableStructure)
    DocStructure.addElement(Kind, Offset, Length);
}

void SKEditorConsumer::recordAffectedRange(unsigned Offset, unsigned Length) {
  Dict.set(KeyOffset, Offset);
  Dict.set(KeyLength, Length);
}

void SKEditorConsumer::recordAffectedLineRange(unsigned Line, unsigned Length) {
  Dict.set(KeyLine, Line);
  Dict.set(KeyLength, Length);
}

void SKEditorConsumer::recordFormattedText(StringRef Text) {
  Dict.set(KeySourceText, Text);
}

static void fillDictionaryForRange(ResponseBuilder::Dictionary Elem,
                                   const RawCharSourceRange &R) {
  Elem.set(KeyOffset, R.Offset);
  Elem.set(KeyLength, R.Length);
}

namespace {
/// Buffer state that we need to persist when writing out diagnostics.
struct GeneratedBuffersState {
  /// The root dictionary.
  ResponseBuilder::Dictionary ParentElem;

  /// The generated buffer array.
  ResponseBuilder::Array GeneratedBuffersElem;

  /// The buffer names that have had their info written out.
  llvm::StringSet<> BufferInfosWritten;

  GeneratedBuffersState(ResponseBuilder::Dictionary ParentElem)
      : ParentElem(ParentElem) {}
};
} // end anonymous namespace

static void writeBufferInfoIfNeeded(const BufferInfo *Info,
                                    GeneratedBuffersState &GeneratedBuffers);

static void
fillDictionaryForGeneratedBufferInfo(ResponseBuilder::Dictionary Elem,
                                     const BufferInfo *Info,
                                     GeneratedBuffersState &GeneratedBuffers) {
  Elem.set(KeyBufferName, Info->BufferName);

  if (auto &Contents = Info->Contents)
    Elem.set(KeyBufferText, *Contents);

  if (auto &OriginalLoc = Info->OrigLocation) {
    // Write out the original buffer info if needed.
    auto *OrigBufferInfo = OriginalLoc->OrigBufferInfo.get();
    writeBufferInfoIfNeeded(OrigBufferInfo, GeneratedBuffers);

    auto OrigLocElem = Elem.setDictionary(KeyOriginalLocation);
    OrigLocElem.set(KeyBufferName, OrigBufferInfo->BufferName);
    fillDictionaryForRange(OrigLocElem, OriginalLoc->Range);
  }
}

/// If the given buffer info is for a generated buffer, and has not yet been
/// written to the generated buffer info array, write it out.
static void writeBufferInfoIfNeeded(const BufferInfo *Info,
                                    GeneratedBuffersState &GeneratedBuffers) {
  // No contents or original location, nothing interesting to write out.
  if (!Info->Contents && !Info->OrigLocation)
    return;

  if (!GeneratedBuffers.BufferInfosWritten.insert(Info->BufferName).second)
    return;

  // Add the generated_buffers key to the parent dictionary if we don't already
  // have it.
  auto &GeneratedBuffersElem = GeneratedBuffers.GeneratedBuffersElem;
  if (GeneratedBuffersElem.isNull()) {
    GeneratedBuffersElem =
        GeneratedBuffers.ParentElem.setArray(KeyGeneratedBuffers);
  }

  // Append the new element.
  auto Elt = GeneratedBuffers.GeneratedBuffersElem.appendDictionary();
  fillDictionaryForGeneratedBufferInfo(Elt, Info, GeneratedBuffers);
}

static void
fillDictionaryForDiagnosticInfoBase(ResponseBuilder::Dictionary Elem,
                                    const DiagnosticEntryInfoBase &Info,
                                    GeneratedBuffersState &GeneratedBuffers) {

  if (!Info.ID.empty())
    Elem.set(KeyID, Info.ID);

  if (!Info.Categories.empty()) {
    SmallVector<SourceKit::UIdent, 1> CategoryUIDs;

    static UIdent UIDKindDiagDeprecation(KindDiagDeprecation.str());
    static UIdent UIDKindDiagNoUsage(KindDiagNoUsage.str());

    for (auto C : Info.Categories) {
      switch (C) {
      case DiagnosticCategory::Deprecation:
        CategoryUIDs.push_back(UIDKindDiagDeprecation);
        break;
      case DiagnosticCategory::NoUsage:
        CategoryUIDs.push_back(UIDKindDiagNoUsage);
        break;
      }
    }
    Elem.set(KeyCategories, CategoryUIDs);
  }

  Elem.set(KeyDescription, Info.Description);
  if (Info.Line != 0) {
    Elem.set(KeyLine, Info.Line);
    Elem.set(KeyColumn, Info.Column);
  } else {
    Elem.set(KeyOffset, Info.Offset);
  }
  auto *BufferInfo = Info.FileInfo.get();
  if (!BufferInfo->BufferName.empty())
    Elem.set(KeyFilePath, Info.FileInfo->BufferName);

  writeBufferInfoIfNeeded(BufferInfo, GeneratedBuffers);

  if (!Info.EducationalNotePaths.empty())
    Elem.set(KeyEducationalNotePaths, Info.EducationalNotePaths);

  if (!Info.Ranges.empty()) {
    auto RangesArr = Elem.setArray(KeyRanges);
    for (auto R : Info.Ranges)
      fillDictionaryForRange(RangesArr.appendDictionary(), R);
  }

  if (!Info.Fixits.empty()) {
    auto FixitsArr = Elem.setArray(KeyFixits);
    for (auto F : Info.Fixits) {
      auto FixitElem = FixitsArr.appendDictionary();
      fillDictionaryForRange(FixitElem, F.Range);
      FixitElem.set(KeySourceText, F.Text);
    }
  }
}

static void
fillDictionaryForDiagnosticInfo(ResponseBuilder::Dictionary Elem,
                                const DiagnosticEntryInfo &Info,
                                GeneratedBuffersState &GeneratedBuffers) {

  UIdent SeverityUID;
  static UIdent UIDKindDiagWarning(KindDiagWarning.str());
  static UIdent UIDKindDiagError(KindDiagError.str());
  switch (Info.Severity) {
  case DiagnosticSeverityKind::Warning:
    SeverityUID = UIDKindDiagWarning;
    break;
  case DiagnosticSeverityKind::Error:
    SeverityUID = UIDKindDiagError;
    break;
  }

  Elem.set(KeySeverity, SeverityUID);
  fillDictionaryForDiagnosticInfoBase(Elem, Info, GeneratedBuffers);

  if (!Info.Notes.empty()) {
    auto NotesArr = Elem.setArray(KeyDiagnostics);
    for (auto &NoteDiag : Info.Notes) {
      auto NoteElem = NotesArr.appendDictionary();
      NoteElem.set(KeySeverity, KindDiagNote);
      fillDictionaryForDiagnosticInfoBase(NoteElem, NoteDiag, GeneratedBuffers);
    }
  }
}

/// Fill in diagnostic info for the 'diagnostics' key in the given \p
/// ParentElem.
static void fillDiagnosticInfo(ResponseBuilder::Dictionary ParentElem,
                               ArrayRef<DiagnosticEntryInfo> Diags,
                               Optional<UIdent> DiagStage) {
  // Add the key, and bail if the diags are empty.
  auto DiagsElem = ParentElem.setArray(KeyDiagnostics);
  if (Diags.empty())
    return;

  GeneratedBuffersState State(ParentElem);
  for (auto &Diag : Diags) {
    auto Elem = DiagsElem.appendDictionary();
    if (DiagStage)
      Elem.set(KeyDiagnosticStage, *DiagStage);

    fillDictionaryForDiagnosticInfo(Elem, Diag, State);
  }
}

void SKEditorConsumer::handleDiagnostics(ArrayRef<DiagnosticEntryInfo> Diags,
                                         UIdent DiagStage) {
  // TODO: Setting the stage here matches the old behavior, but should we
  // consider moving until after the below if check? Do we even need this key if
  // each individual diagnostic has its stage set? Or should we remove those
  // keys?
  Dict.set(KeyDiagnosticStage, DiagStage);

  if (!Opts.EnableDiagnostics || Diags.empty())
    return;

  fillDiagnosticInfo(Dict, Diags, DiagStage);
}

void SKEditorConsumer::handleSourceText(StringRef Text) {
  Dict.set(KeySourceText, Text);
}

static sourcekitd_response_t
editorFindUSR(StringRef DocumentName, StringRef USR) {
  ResponseBuilder RespBuilder;
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  llvm::Optional<std::pair<unsigned, unsigned>>
      Range = Lang.findUSRRange(DocumentName, USR);
  if (!Range) {
    // If cannot find the synthesized USR, find the actual USR instead.
    Range = Lang.findUSRRange(DocumentName,
                              USR.split(LangSupport::SynthesizedUSRSeparator).
                                first);
  }
  if (Range.has_value()) {
    RespBuilder.getDictionary().set(KeyOffset, Range->first);
    RespBuilder.getDictionary().set(KeyLength, Range->second);
  }
  return RespBuilder.createResponse();
}

static sourcekitd_response_t
editorFindInterfaceDoc(StringRef ModuleName, ArrayRef<const char *> Args) {
  ResponseBuilder RespBuilder;
  sourcekitd_response_t Resp;
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.findInterfaceDocument(ModuleName, Args,
    [&](const RequestResult<InterfaceDocInfo> &Result) {
      if (Result.isCancelled()) {
        Resp = createErrorRequestCancelled();
        return;
      }
      if (Result.isError()) {
        Resp = createErrorRequestFailed(Result.getError());
        return;
      }

      const InterfaceDocInfo &Info = Result.value();

      auto Elem = RespBuilder.getDictionary();
      if (!Info.ModuleInterfaceName.empty())
        Elem.set(KeyModuleInterfaceName, Info.ModuleInterfaceName);
      if (!Info.CompilerArgs.empty())
        Elem.set(KeyCompilerArgs, Info.CompilerArgs);
      Resp = RespBuilder.createResponse();
    });

  return Resp;
}

static sourcekitd_response_t
editorFindModuleGroups(StringRef ModuleName, ArrayRef<const char *> Args) {
  ResponseBuilder RespBuilder;
  sourcekitd_response_t Resp;
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  Lang.findModuleGroups(ModuleName, Args,
                        [&](const RequestResult<ArrayRef<StringRef>> &Result) {
    if (Result.isCancelled()) {
      Resp = createErrorRequestCancelled();
      return;
    }
    if (Result.isError()) {
      Resp = createErrorRequestFailed(Result.getError());
      return;
    }

    ArrayRef<StringRef> Groups = Result.value();

    auto Dict = RespBuilder.getDictionary();
    auto Arr = Dict.setArray(KeyModuleGroups);
    for (auto G : Groups) {
      auto Entry = Arr.appendDictionary();
      Entry.set(KeyGroupName, G);
    }
    Resp = RespBuilder.createResponse();
  });
  return Resp;
}

static bool
buildRenameLocationsFromDict(const RequestDict &Req, bool UseNewName,
                             std::vector<RenameLocations> &RenameLocations,
                             llvm::SmallString<64> &Error) {
  bool Failed = Req.dictionaryArrayApply(KeyRenameLocations,
                                         [&](RequestDict RenameLocation) {
    int64_t IsFunctionLike = false;
    if (RenameLocation.getInt64(KeyIsFunctionLike, IsFunctionLike, false)) {
      Error = "missing key.is_function_like";
      return true;
    }

    int64_t IsNonProtocolType = false;
    if (RenameLocation.getInt64(KeyIsNonProtocolType, IsNonProtocolType, false)) {
      Error = "missing key.is_non_protocol_type";
      return true;
    }

    Optional<StringRef> OldName = RenameLocation.getString(KeyName);
    if (!OldName.has_value()) {
      Error = "missing key.name";
      return true;
    }

    Optional<StringRef> NewName;
    if (UseNewName) {
      NewName = RenameLocation.getString(KeyNewName);
      if (!NewName.has_value()) {
        Error = "missing key.newname";
        return true;
      }
    }

    RenameLocations.push_back({*OldName,
                               UseNewName ? *NewName : "",
                               static_cast<bool>(IsFunctionLike),
                               static_cast<bool>(IsNonProtocolType),
                               {}});
    auto &LineCols = RenameLocations.back().LineColumnLocs;
    bool Failed = RenameLocation.dictionaryArrayApply(KeyLocations,
                                                      [&](RequestDict LineAndCol) {
      int64_t Line = 0;
      int64_t Column = 0;

      if (LineAndCol.getInt64(KeyLine, Line, false)) {
        Error = "missing key.line";
        return true;
      }
      if (LineAndCol.getInt64(KeyColumn, Column, false)) {
        Error = "missing key.column";
        return true;
      }

      sourcekitd_uid_t NameType = LineAndCol.getUID(KeyNameType);
      if (!NameType) {
        Error = "missing key.nametype";
        return true;
      }
      RenameType RenameType = RenameType::Unknown;
      if (NameType == KindDefinition) {
        RenameType = RenameType::Definition;
      } else if (NameType == KindReference) {
        RenameType = RenameType::Reference;
      } else if (NameType == KindCall) {
        RenameType = RenameType::Call;
      } else if (NameType != KindUnknown) {
        Error = "invalid value for 'key.nametype'";
        return true;
      }
      LineCols.push_back({static_cast<unsigned>(Line),
        static_cast<unsigned>(Column), RenameType});
      return false;
    });
    if (Failed && Error.empty()) {
      Error = "invalid key.locations";
    }
    return Failed;
  });
  if (Failed && Error.empty()) {
    Error = "invalid key.renamelocations";
  }
  return Failed;
}

static sourcekitd_response_t
createCategorizedEditsResponse(const RequestResult<ArrayRef<CategorizedEdits>> &Result) {
  if (Result.isCancelled())
    return createErrorRequestCancelled();
  if (Result.isError())
    return createErrorRequestFailed(Result.getError());

  const ArrayRef<CategorizedEdits> &AllEdits = Result.value();

  ResponseBuilder RespBuilder;
  auto Dict = RespBuilder.getDictionary();
  auto Arr = Dict.setArray(KeyCategorizedEdits);
  for (auto &TheEdit : AllEdits) {
    auto Entry = Arr.appendDictionary();
    Entry.set(KeyCategory, TheEdit.Category);
    auto Edits = Entry.setArray(KeyEdits);
    for(auto E: TheEdit.Edits) {
      auto Edit = Edits.appendDictionary();
      if (!E.Path.empty()) {
        Edit.set(KeyFilePath, E.Path);
      }
      Edit.set(KeyLine, E.StartLine);
      Edit.set(KeyColumn, E.StartColumn);
      Edit.set(KeyEndLine, E.EndLine);
      Edit.set(KeyEndColumn, E.EndColumn);
      if (!E.BufferName.empty()) {
        Edit.set(KeyBufferName, E.BufferName);
      }
      Edit.set(KeyText, E.NewText);
      if (!E.RegionsWithNote.empty()) {
        auto Notes = Edit.setArray(KeyRangesWorthNote);
        for (auto R : E.RegionsWithNote) {
          auto N = Notes.appendDictionary();
          N.set(KeyKind, R.Kind);
          N.set(KeyLine, R.StartLine);
          N.set(KeyColumn, R.StartColumn);
          N.set(KeyEndLine, R.EndLine);
          N.set(KeyEndColumn, R.EndColumn);
          if (R.ArgIndex)
            N.set(KeyArgIndex, *R.ArgIndex);
        }
      }
    }
  }
  return RespBuilder.createResponse();
}

static sourcekitd_response_t
syntacticRename(llvm::MemoryBuffer *InputBuf,
                ArrayRef<RenameLocations> RenameLocations,
                ArrayRef<const char*> Args) {
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  sourcekitd_response_t Result;
  Lang.syntacticRename(InputBuf, RenameLocations, Args,
    [&](const RequestResult<ArrayRef<CategorizedEdits>> &ReqResult) {
      Result = createCategorizedEditsResponse(ReqResult);
  });
  return Result;
}

static sourcekitd_response_t
createCategorizedRenameRangesResponse(const RequestResult<ArrayRef<CategorizedRenameRanges>> &Result) {
  if (Result.isCancelled())
    return createErrorRequestCancelled();
  if (Result.isError())
    return createErrorRequestFailed(Result.getError());

  const ArrayRef<CategorizedRenameRanges> &Ranges = Result.value();

  ResponseBuilder RespBuilder;
  auto Dict = RespBuilder.getDictionary();
  auto Arr = Dict.setArray(KeyCategorizedRanges);
  for (const auto &CategorizedRange : Ranges) {
    auto Entry = Arr.appendDictionary();
    Entry.set(KeyCategory, CategorizedRange.Category);
    auto Ranges = Entry.setArray(KeyRanges);
    for (const auto &R : CategorizedRange.Ranges) {
      auto Range = Ranges.appendDictionary();
      Range.set(KeyLine, R.StartLine);
      Range.set(KeyColumn, R.StartColumn);
      Range.set(KeyEndLine, R.EndLine);
      Range.set(KeyEndColumn, R.EndColumn);
      Range.set(KeyKind, R.Kind);
      if (R.ArgIndex) {
        Range.set(KeyArgIndex, *R.ArgIndex);
      }
    }
  }
  return RespBuilder.createResponse();
}

static sourcekitd_response_t
findRenameRanges(llvm::MemoryBuffer *InputBuf,
                 ArrayRef<RenameLocations> RenameLocations,
                 ArrayRef<const char *> Args) {
  LangSupport &Lang = getGlobalContext().getSwiftLangSupport();
  sourcekitd_response_t Result;
  Lang.findRenameRanges(
      InputBuf, RenameLocations, Args,
      [&](const RequestResult<ArrayRef<CategorizedRenameRanges>> &ReqResult) {
        Result = createCategorizedRenameRangesResponse(ReqResult);
      });
  return Result;
}

static bool isSemanticEditorDisabled() {
  enum class SemaInfoToggle : char {
    None, Disable, Enable
  };
  static SemaInfoToggle Toggle = SemaInfoToggle::None;

  if (Toggle == SemaInfoToggle::None) {
    static std::once_flag flag;
    std::call_once(flag, []() {
      Toggle = SemaInfoToggle::Enable;

      const char *EnvOpt = ::getenv("SOURCEKIT_DELAY_SEMA_EDITOR");
      if (!EnvOpt) {
        return;
      }

      unsigned Seconds;
      if (StringRef(EnvOpt).getAsInteger(10, Seconds))
        return;

      // A crash occurred previously. Disable semantic info in the editor for
      // the given amount, to avoid repeated crashers.
      LOG_WARN_FUNC("delaying semantic editor for " << Seconds << " seconds");
      Toggle = SemaInfoToggle::Disable;
      dispatch_time_t When = dispatch_time(DISPATCH_TIME_NOW,
                                           NSEC_PER_SEC * Seconds);
      dispatch_after(When, dispatch_get_main_queue(), ^{
        Toggle = SemaInfoToggle::Enable;
        getGlobalContext()
            .getNotificationCenter()
            ->postSemaEnabledNotification();
      });
    });
  }

  assert(Toggle != SemaInfoToggle::None);
  return Toggle == SemaInfoToggle::Disable;
}

namespace {
class CompileTrackingConsumer final : public trace::TraceConsumer {
public:
  void operationStarted(uint64_t OpId, trace::OperationKind OpKind,
                        const trace::SwiftInvocation &Inv,
                        const trace::StringPairs &OpArgs) override;
  void operationFinished(uint64_t OpId, trace::OperationKind OpKind,
                         ArrayRef<DiagnosticEntryInfo> Diagnostics) override;
  swift::OptionSet<trace::OperationKind> desiredOperations() override {
    return swift::OptionSet<trace::OperationKind>() |
           trace::OperationKind::PerformSema |
           trace::OperationKind::CodeCompletion;
  }
};
} // end anonymous namespace

static Optional<UIdent> getUIDForOperationKind(trace::OperationKind OpKind) {
  static UIdent CompileOperationIndexSource("source.compile.operation.index-source");
  static UIdent CompileOperationCodeCompletion("source.compile.operation.code-completion");
  switch (OpKind) {
    case trace::OperationKind::PerformSema:
      return None;
    case trace::OperationKind::IndexSource:
      return CompileOperationIndexSource;
    case trace::OperationKind::CodeCompletion:
      return CompileOperationCodeCompletion;
    default:
      llvm_unreachable("Unknown operation kind");
  }
}

void CompileTrackingConsumer::operationStarted(
    uint64_t OpId, trace::OperationKind OpKind,
    const trace::SwiftInvocation &Inv, const trace::StringPairs &OpArgs) {
  if (desiredOperations().contains(OpKind))
    getGlobalContext()
        .getNotificationCenter()
        ->postCompileWillStartNotification(OpId, OpKind, Inv);
}

void CompileTrackingConsumer::operationFinished(
    uint64_t OpId, trace::OperationKind OpKind,
    ArrayRef<DiagnosticEntryInfo> Diagnostics) {
  if (desiredOperations().contains(OpKind))
    getGlobalContext()
        .getNotificationCenter()
        ->postCompileDidFinishNotification(OpId, OpKind, Diagnostics);
}

static void enableCompileNotifications(bool value) {
  static std::atomic<bool> status{false};
  if (status.exchange(value) == value) {
    return; // Unchanged.
  }

  static CompileTrackingConsumer compileConsumer;
  if (value) {
    trace::registerConsumer(&compileConsumer);
  } else {
    trace::unregisterConsumer(&compileConsumer);
  }
}
