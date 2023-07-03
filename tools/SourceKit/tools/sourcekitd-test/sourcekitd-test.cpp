//===--- sourcekitd-test.cpp ----------------------------------------------===//
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

#include "sourcekitd/sourcekitd.h"

#include "SourceKit/Support/Concurrency.h"
#include "TestOptions.h"
#include "swift/Demangling/ManglingMacros.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/thread.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <sys/param.h>
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

// FIXME: Platform compatibility.
#include <dispatch/dispatch.h>

using namespace llvm;

using namespace sourcekitd_test;

#if defined(_WIN32)
namespace {
int STDOUT_FILENO = _fileno(stdout);
}
#endif

static int handleTestInvocation(ArrayRef<const char *> Args, TestOptions &InitOpts,
                                bool IsFirstInvocation);
static bool handleResponse(sourcekitd_response_t Resp, const TestOptions &Opts,
                           const std::string &SourceFile,
                           std::unique_ptr<llvm::MemoryBuffer> SourceBuf,
                           TestOptions *InitOpts);
static void printCursorInfo(sourcekitd_variant_t Info, StringRef Filename,
                            const llvm::StringMap<TestOptions::VFSFile> &VFSFiles,
                            llvm::raw_ostream &OS);
static void printNameTranslationInfo(sourcekitd_variant_t Info, llvm::raw_ostream &OS);
static void printRangeInfo(sourcekitd_variant_t Info, StringRef Filename,
                           llvm::raw_ostream &OS);
static void printExpressionType(sourcekitd_variant_t Info, llvm::raw_ostream &OS);
static void printVariableType(sourcekitd_variant_t Info,
                              llvm::MemoryBuffer *SourceBuf,
                              llvm::raw_ostream &OS);
static void printDocInfo(sourcekitd_variant_t Info, StringRef Filename);
static void printInterfaceGen(sourcekitd_variant_t Info, bool CheckASCII);
static void printSemanticInfo();
static void printRelatedIdents(sourcekitd_variant_t Info, StringRef Filename,
                               const llvm::StringMap<TestOptions::VFSFile> &VFSFiles,
                               llvm::raw_ostream &OS);
static void printActiveRegions(sourcekitd_variant_t Info, StringRef Filename,
                               const llvm::StringMap<TestOptions::VFSFile> &VFSFiles,
                               llvm::raw_ostream &OS);
static void printFoundInterface(sourcekitd_variant_t Info,
                                llvm::raw_ostream &OS);
static void printFoundUSR(sourcekitd_variant_t Info,
                          llvm::MemoryBuffer *SourceBuf,
                          llvm::raw_ostream &OS);
static void printNormalizedDocComment(sourcekitd_variant_t Info);
static void expandPlaceholders(llvm::MemoryBuffer *SourceBuf,
                               llvm::raw_ostream &OS);

static void printModuleGroupNames(sourcekitd_variant_t Info,
                                  llvm::raw_ostream &OS);
static void printSyntacticRenameEdits(sourcekitd_variant_t Info,
                                      llvm::raw_ostream &OS);
static void printRenameRanges(sourcekitd_variant_t Info, llvm::raw_ostream &OS);
static void prepareDemangleRequest(sourcekitd_object_t Req,
                                   const TestOptions &Opts);
static void printDemangleResults(sourcekitd_variant_t Info, raw_ostream &OS);
static void prepareMangleRequest(sourcekitd_object_t Req,
                                 const TestOptions &Opts);
static void printMangleResults(sourcekitd_variant_t Info, raw_ostream &OS);
static void printStatistics(sourcekitd_variant_t Info, raw_ostream &OS);

static unsigned
resolveFromLineCol(unsigned Line, unsigned Col, StringRef Filename,
                   const llvm::StringMap<TestOptions::VFSFile> &VFSFiles);
static unsigned resolveFromLineCol(unsigned Line, unsigned Col,
                                   llvm::MemoryBuffer *InputBuf);
static std::pair<unsigned, unsigned>
resolveToLineCol(unsigned Offset, StringRef Filename,
                 const llvm::StringMap<TestOptions::VFSFile> &VFSFiles,
                 bool ExitOnError = true);
static std::pair<unsigned, unsigned>
resolveToLineCol(unsigned Offset, llvm::MemoryBuffer *InputBuf,
                 bool ExitOnError = true);
static std::pair<unsigned, unsigned> resolveToLineColFromBuf(unsigned Offset,
                                                      const char *Buf);
static llvm::MemoryBuffer *
getBufferForFilename(StringRef Filename,
                     const llvm::StringMap<TestOptions::VFSFile> &VFSFiles,
                     bool ExitOnError = true);

static void notification_receiver(sourcekitd_response_t resp);

static SourceKitRequest ActiveRequest = SourceKitRequest::None;

#define KEY(NAME, CONTENT) static sourcekitd_uid_t Key##NAME;
#define REQUEST(NAME, CONTENT) static sourcekitd_uid_t Request##NAME;
#define KIND(NAME, CONTENT) static sourcekitd_uid_t Kind##NAME;
#include "SourceKit/Core/ProtocolUIDs.def"

#define REFACTORING(KIND, NAME, ID) static sourcekitd_uid_t Kind##Refactoring##KIND;
#include "swift/Refactoring/RefactoringKinds.def"

static sourcekitd_uid_t SemaDiagnosticStage;

static sourcekitd_uid_t NoteDocUpdate;
static SourceKit::Semaphore semaSemaphore(0);
static sourcekitd_response_t semaResponse;
static const char *semaName;

static sourcekitd_uid_t NoteTest;
static SourceKit::Semaphore noteSyncSemaphore(0);

namespace {
struct AsyncResponseInfo {
  SourceKit::Semaphore semaphore{0};
  sourcekitd_response_t response = nullptr;
  TestOptions options;
  std::string sourceFilename;
  std::unique_ptr<llvm::MemoryBuffer> sourceBuffer;
  sourcekitd_request_handle_t requestHandle;
};
} // end anonymous namespace

static std::vector<AsyncResponseInfo> asyncResponses;

struct NotificationBuffer {
  std::vector<sourcekitd_response_t> notes;
  /// Add a notification to the buffer, taking ownership of it. Must be called
  /// from the main queue.
  void add(sourcekitd_response_t note) {
    notes.push_back(note);
  }
  /// Call the given handler for all notifications currently buffered.
  void handleNotifications(llvm::function_ref<void(sourcekitd_response_t)> f) {
    // Notifications are handled on the main queue.
    dispatch_sync(dispatch_get_main_queue(), ^{
      for (auto note : notes) {
        f(note);
        sourcekitd_response_dispose(note);
      }
      notes.clear();
    });
  }
};
static NotificationBuffer notificationBuffer;

static void printRawResponse(sourcekitd_response_t resp) {
  llvm::outs().flush();
  sourcekitd_response_description_dump_filedesc(resp, STDOUT_FILENO);
}

static void printRawVariant(sourcekitd_variant_t obj) {
  llvm::outs().flush();
  sourcekitd_variant_description_dump_filedesc(obj, STDOUT_FILENO);
}

static void syncNotificationsWithService() {
  // Send TestNotification request, then wait for the notification. This ensures
  // that all notifications previously posted on the service side have been
  // passed to our notification handler.
  sourcekitd_object_t req = sourcekitd_request_dictionary_create(nullptr, nullptr, 0);
  sourcekitd_request_dictionary_set_uid(req, KeyRequest, RequestTestNotification);
  auto resp = sourcekitd_send_request_sync(req);
  if (sourcekitd_response_is_error(resp)) {
    sourcekitd_response_description_dump(resp);
    exit(1);
  }
  sourcekitd_response_dispose(resp);
  sourcekitd_request_release(req);
  if (noteSyncSemaphore.wait(60 * 1000)) {
    llvm::report_fatal_error("Test notification not received");
  }
}

static void printBufferedNotifications(bool syncWithService = true) {
  if (syncWithService) {
    syncNotificationsWithService();
  }
  notificationBuffer.handleNotifications(
      [](sourcekitd_response_t note) { printRawResponse(note); });
}

struct skt_args {
  int argc;
  const char **argv;
  int ret;
};
static void skt_main(skt_args *args);

int main(int argc, const char **argv) {
  dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), ^{
    skt_args args = {argc, argv, 0};
    llvm::thread thread(llvm::thread::DefaultStackSize,
                        skt_main, &args);
    thread.join();
    exit(args.ret);
  });

  dispatch_main();
}

static void skt_main(skt_args *args) {
  int argc = args->argc;
  const char **argv = args->argv;
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

  sourcekitd_initialize();

  sourcekitd_set_notification_handler(^(sourcekitd_response_t resp) {
    notification_receiver(resp);
  });

#define KEY(NAME, CONTENT) Key##NAME = sourcekitd_uid_get_from_cstr(CONTENT);
#include "SourceKit/Core/ProtocolUIDs.def"

  SemaDiagnosticStage = sourcekitd_uid_get_from_cstr("source.diagnostic.stage.swift.sema");

  NoteDocUpdate = sourcekitd_uid_get_from_cstr("source.notification.editor.documentupdate");
  NoteTest = sourcekitd_uid_get_from_cstr("source.notification.test");

#define REQUEST(NAME, CONTENT) Request##NAME = sourcekitd_uid_get_from_cstr(CONTENT);
#define KIND(NAME, CONTENT) Kind##NAME = sourcekitd_uid_get_from_cstr(CONTENT);
#include "SourceKit/Core/ProtocolUIDs.def"

#define REFACTORING(KIND, NAME, ID) Kind##Refactoring##KIND = sourcekitd_uid_get_from_cstr("source.refactoring.kind."#ID);
#include "swift/Refactoring/RefactoringKinds.def"

  // A test invocation may initialize the options to be used for subsequent
  // invocations.
  TestOptions InitOpts;
  auto Args = llvm::makeArrayRef(argv+1, argc-1);
  bool firstInvocation = true;
  while (1) {
    unsigned i = 0;
    for (auto Arg: Args) {
      if (StringRef(Arg) == "==")
        break;
      ++i;
    }
    if (i == Args.size())
      break;
    if (int ret = handleTestInvocation(Args.slice(0, i), InitOpts,
                                       firstInvocation)) {
      sourcekitd_shutdown();
      args->ret = ret;
      return;
    }
    Args = Args.slice(i + 1);
    firstInvocation = false;
  }

  if (int ret = handleTestInvocation(Args, InitOpts, firstInvocation)) {
    sourcekitd_shutdown();
    args->ret = ret;
    return;
  }

  for (auto &info : asyncResponses) {
    if (info.semaphore.wait(60 * 1000)) {
      llvm::report_fatal_error("async request timed out");
    }

    if (handleResponse(info.response, info.options, info.sourceFilename,
                       std::move(info.sourceBuffer), nullptr)) {
      sourcekitd_shutdown();
      args->ret = 1;
      return;
    }
  }
  printBufferedNotifications();

  sourcekitd_shutdown();
  args->ret = 0;
  return;
}

static inline std::string getInterfaceGenDocumentName() {
  // "Absolute path" on all platforms since handleTestInvocation will attempt to make this absolute
  llvm::SmallString<64> path = llvm::StringRef("/<interface-gen>");
  llvm::sys::fs::make_absolute(path);
  llvm::sys::path::native(path);
  return std::string(path.str());
}

static int printAnnotations();
static int printDiags();

static void getSemanticInfo(sourcekitd_variant_t Info, StringRef Filename);

static Optional<int64_t> getReqOptValueAsInt(StringRef Value) {
  if (Value.equals_insensitive("true"))
    return 1;
  if (Value.equals_insensitive("false"))
    return 0;
  int64_t Ret;
  if (Value.find_first_not_of("-0123456789") != StringRef::npos ||
      Value.getAsInteger(0, Ret)) {
    return None;
  }
  return Ret;
}

static Optional<sourcekitd_uid_t> getReqOptValueAsUID(StringRef Value) {
  if (!Value.startswith("uid:"))
    return None;
  Value = Value.drop_front(4);
  return sourcekitd_uid_get_from_buf(Value.data(), Value.size());
}

static Optional<sourcekitd_object_t> getReqOptValueAsArray(StringRef Value) {
  if (!Value.startswith("[") || !Value.endswith("]"))
    return None;
  SmallVector<StringRef, 4> Elements;
  Value.drop_front().drop_back().split(Elements, ';');
  auto Array = sourcekitd_request_array_create(nullptr, 0);
  for (auto &Elem : Elements) {
    if (auto Val = getReqOptValueAsInt(Elem)) {
      sourcekitd_request_array_set_int64(Array, SOURCEKITD_ARRAY_APPEND, *Val);
    } else if (auto Val = getReqOptValueAsUID(Elem)) {
      sourcekitd_request_array_set_uid(Array, SOURCEKITD_ARRAY_APPEND, *Val);
    } else if (auto Val = getReqOptValueAsArray(Elem)) {
      sourcekitd_request_array_set_value(Array, SOURCEKITD_ARRAY_APPEND, *Val);
    } else {
      sourcekitd_request_array_set_stringbuf(Array, SOURCEKITD_ARRAY_APPEND,
                                             Elem.data(), Elem.size());
    }
  }
  return Array;
}

static void addRequestOptionsDirect(sourcekitd_object_t Req, TestOptions &Opts,
                                    StringRef prefix = "key.") {
  if (Opts.RequestOptions.empty())
    return;

  for (auto &Opt: Opts.RequestOptions) {
    auto KeyValue = StringRef(Opt).split('=');
    std::string KeyStr(prefix.str());
    KeyStr.append(KeyValue.first.str());
    sourcekitd_uid_t Key = sourcekitd_uid_get_from_cstr(KeyStr.c_str());

    StringRef RawValue = KeyValue.second;

    if (auto Val = getReqOptValueAsInt(RawValue)) {
      sourcekitd_request_dictionary_set_int64(Req, Key, *Val);
    } else if (auto Val = getReqOptValueAsUID(RawValue)) {
      sourcekitd_request_dictionary_set_uid(Req, Key, *Val);
    } else if (auto Val = getReqOptValueAsArray(RawValue)) {
      sourcekitd_request_dictionary_set_value(Req, Key, *Val);
      sourcekitd_request_release(*Val);
    } else {
      sourcekitd_request_dictionary_set_stringbuf(Req, Key, RawValue.data(), RawValue.size());
    }
  }
}

static void addRequestOptions(sourcekitd_object_t Req, TestOptions &Opts,
                              sourcekitd_uid_t Key, StringRef prefix = "key.") {
  if (Opts.RequestOptions.empty())
    return;

  sourcekitd_object_t CCOpts =
      sourcekitd_request_dictionary_create(nullptr, nullptr, 0);

  addRequestOptionsDirect(CCOpts, Opts, prefix);

  sourcekitd_request_dictionary_set_value(Req, Key, CCOpts);
  sourcekitd_request_release(CCOpts);
}

static bool readPopularAPIList(StringRef filename,
                               std::vector<std::string> &result) {
  std::ifstream in(filename.str());
  if (!in.is_open()) {
    llvm::errs() << "error opening '" << filename << "'\n";
    return true;
  }

  std::string line;
  while (std::getline(in, line)) {
    result.emplace_back();
    std::swap(result.back(), line);
  }
  return false;
}

/// Read '-req-opts' for syntactic macro expansion request and apply it to 'req'
/// object.
/// The format of the argument is '-req-opts={line}:{column}:{path}'
/// where {path} is a path to a JSON file that has macro roles and definition.
/// {line} and {column} is resolved to 'offset' using \p inputBuf .
static bool setSyntacticMacroExpansions(sourcekitd_object_t req,
                                        TestOptions &opts,
                                        llvm::MemoryBuffer *inputBuf) {
  SmallVector<sourcekitd_object_t, 4> expansions;
  for (std::string &opt : opts.RequestOptions) {
    SmallVector<StringRef, 3> args;
    StringRef(opt).split(args, ":");
    unsigned line, column;

    if (args.size() != 3 || args[0].getAsInteger(10, line) ||
        args[1].getAsInteger(10, column)) {
      llvm::errs() << "-req-opts should be {line}:{column}:{json-path}";
      return true;
    }
    unsigned offset = resolveFromLineCol(line, column, inputBuf);

    auto Buffer = getBufferForFilename(args[2], opts.VFSFiles)->getBuffer();
    char *Err = nullptr;
    auto expansion = sourcekitd_request_create_from_yaml(Buffer.data(), &Err);
    if (!expansion) {
      assert(Err);
      llvm::errs() << Err;
      free(Err);
      return true;
    }
    sourcekitd_request_dictionary_set_int64(expansion, KeyOffset,
                                            int64_t(offset));
    expansions.push_back(expansion);
  }
  sourcekitd_request_dictionary_set_value(
      req, KeyExpansions,
      sourcekitd_request_array_create(expansions.data(), expansions.size()));
  return false;
}

namespace {
class PrintingTimer {
  std::string desc;
  llvm::sys::TimePoint<> start;
  llvm::raw_ostream &OS;
public:
  PrintingTimer(std::string desc, llvm::raw_ostream &OS = llvm::errs())
      : desc(std::move(desc)), start(std::chrono::system_clock::now()), OS(OS) {
  }
  ~PrintingTimer() {
    std::chrono::duration<float, std::milli> delta(
        std::chrono::system_clock::now() - start);
    OS << desc << ": " << llvm::formatv("{0:ms+f3}", delta) << "\n";
  }
};
}

/// Wrapper for sourcekitd_send_request_sync that handles printing options.
static sourcekitd_response_t sendRequestSync(sourcekitd_object_t req,
                                             const TestOptions &opts) {
  if (opts.PrintRequest)
    sourcekitd_request_description_dump(req);
  Optional<PrintingTimer> timer;
  if (opts.timeRequest)
    timer.emplace("request time");
  return sourcekitd_send_request_sync(req);
}

static int handleJsonRequestPath(StringRef QueryPath, const TestOptions &Opts) {
  auto Buffer = getBufferForFilename(QueryPath, Opts.VFSFiles)->getBuffer();
  char *Err = nullptr;
  auto Req = sourcekitd_request_create_from_yaml(Buffer.data(), &Err);
  if (!Req) {
    assert(Err);
    llvm::errs() << Err;
    free(Err);
    return 1;
  }

  sourcekitd_response_t Resp = sendRequestSync(Req, Opts);
  auto Error = sourcekitd_response_is_error(Resp);
  if (Opts.PrintResponse) {
    printRawResponse(Resp);
  }
  return Error ? 1 : 0;
}

static int performShellExecution(ArrayRef<const char *> Args) {
  llvm::outs().flush();
  llvm::errs().flush();
  auto Program = llvm::sys::findProgramByName(Args[0]);
  if (std::error_code ec = Program.getError()) {
    llvm::errs() << "command not found: " << Args[0] << "\n";
    return ec.value();
  }
  SmallVector<StringRef, 8> execArgs(Args.begin(), Args.end());
  return llvm::sys::ExecuteAndWait(*Program, execArgs);
}

static int handleTestInvocation(TestOptions Opts, TestOptions &InitOpts);

static int handleTestInvocation(ArrayRef<const char *> Args,
                                TestOptions &InitOpts, bool firstInvocation) {

  unsigned Optargc = 0;
  for (auto Arg: Args) {
    if (StringRef(Arg) == "--")
      break;
    ++Optargc;
  }

  TestOptions Opts = InitOpts;
  if (Opts.parseArgs(Args.slice(0, Optargc)))
    return 1;

  if (!Opts.ModuleCachePath.empty())
    InitOpts.ModuleCachePath = Opts.ModuleCachePath;

  if (Optargc < Args.size())
    Opts.CompilerArgs = Args.slice(Optargc+1);

  if (Opts.ShellExecution)
    return performShellExecution(Opts.CompilerArgs);

  if (!Opts.CancelRequest.empty()) {
    for (auto &asyncResponse : asyncResponses) {
      if (asyncResponse.options.RequestId == Opts.CancelRequest) {
        sourcekitd_cancel_request(asyncResponse.requestHandle);
      }
    }
    return 0;
  }

  assert(Opts.repeatRequest >= 1);
  for (unsigned i = 0; i < Opts.repeatRequest; ++i) {
    if (int ret = handleTestInvocation(Opts, InitOpts)) {
      printBufferedNotifications(/*syncWithService=*/true);
      return ret;
    }
    // We will sync with the service before exiting; don't do so here.
    printBufferedNotifications(/*syncWithService=*/false);
  }
  return 0;
}

static void setRefactoringFields(sourcekitd_object_t &Req, TestOptions Opts,
                                 sourcekitd_uid_t RefactoringKind,
                                 llvm::MemoryBuffer *SourceBuf) {
  unsigned line = Opts.Line;
  unsigned col = Opts.Col;
  if (SourceBuf && Opts.Offset && line == 0) {
    std::tie(line, col) = resolveToLineCol(*Opts.Offset, SourceBuf);
  }

  sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                        RequestSemanticRefactoring);
  sourcekitd_request_dictionary_set_uid(Req, KeyActionUID, RefactoringKind);
  sourcekitd_request_dictionary_set_string(Req, KeyName, Opts.Name.c_str());
  sourcekitd_request_dictionary_set_int64(Req, KeyLine, line);
  sourcekitd_request_dictionary_set_int64(Req, KeyColumn, col);
  sourcekitd_request_dictionary_set_int64(Req, KeyLength, Opts.Length);
}

/// Returns the number of instructions executed by the SourceKit process since
/// its launch. If SourceKit is running in-process this is the instruction count
/// of the current process. If it's running out-of process it is the instruction
/// count of the XPC process.
int64_t getSourceKitInstructionCount() {
  sourcekitd_object_t Req =
      sourcekitd_request_dictionary_create(nullptr, nullptr, 0);
  sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestStatistics);
  sourcekitd_response_t Resp = sourcekitd_send_request_sync(Req);
  sourcekitd_variant_t Info = sourcekitd_response_get_value(Resp);
  sourcekitd_variant_t Results =
      sourcekitd_variant_dictionary_get_value(Info, KeyResults);
  __block size_t InstructionCount = 0;
  sourcekitd_variant_array_apply(
      Results, ^bool(size_t index, sourcekitd_variant_t value) {
        auto UID = sourcekitd_variant_dictionary_get_uid(value, KeyKind);
        if (UID == KindStatInstructionCount) {
          InstructionCount =
              sourcekitd_variant_dictionary_get_int64(value, KeyValue);
          return false;
        }
        return true;
      });
  return InstructionCount;
}

static int handleTestInvocation(TestOptions Opts, TestOptions &InitOpts) {
  if (!Opts.JsonRequestPath.empty())
    return handleJsonRequestPath(Opts.JsonRequestPath, Opts);

  if (Opts.Request == SourceKitRequest::DemangleNames ||
      Opts.Request == SourceKitRequest::MangleSimpleClasses)
    Opts.SourceFile.clear();

  if (!Opts.SourceFile.empty() && Opts.PrimaryFile.empty()) {
    // Only canonicalize if primary file isn't set (since we expect sourcefile
    // to be a relative name otherwise).
    llvm::SmallString<64> AbsSourceFile;
    AbsSourceFile += Opts.SourceFile;
    llvm::sys::fs::make_absolute(AbsSourceFile);
    llvm::sys::path::native(AbsSourceFile);
    Opts.SourceFile = std::string(AbsSourceFile.str());
    if (Opts.PrimaryFile.empty()) {
      Opts.PrimaryFile = Opts.SourceFile;
    }
  }
  // FIXME: It's super confusing that we use name for the file sometimes.
  std::string SemaName = !Opts.Name.empty() ? Opts.Name : Opts.SourceFile;

  if (!Opts.TextInputFile.empty()) {
    auto Buf = getBufferForFilename(Opts.TextInputFile, Opts.VFSFiles);
    Opts.SourceText = Buf->getBuffer().str();
  }

  std::unique_ptr<llvm::MemoryBuffer> SourceBuf;
  if (Opts.SourceText.has_value()) {
    SourceBuf = llvm::MemoryBuffer::getMemBuffer(*Opts.SourceText, Opts.SourceFile);
  } else if (!Opts.SourceFile.empty() && Opts.PrimaryFile == Opts.SourceFile) {
    // If we have a primary file, that implies that the source file is actually
    // a generated buffer. In that case we can't get the text.
    SourceBuf = llvm::MemoryBuffer::getMemBuffer(
        getBufferForFilename(Opts.SourceFile, Opts.VFSFiles)->getBuffer(),
        Opts.SourceFile);
  }

  unsigned ByteOffset = Opts.Offset.value_or(0);
  if (SourceBuf) {
    // Fill in offset/length if we're able to, ie. we have access to the
    // underlying sourcefile. Ideally we would error here if any of these
    // are needed but not (or cannot) be set.

    if (!Opts.Offset && Opts.Line > 0) {
      ByteOffset = resolveFromLineCol(Opts.Line, Opts.Col, SourceBuf.get());
    }

    if (Opts.Length == 0 && Opts.EndLine > 0) {
      Opts.Length =
          resolveFromLineCol(Opts.EndLine, Opts.EndCol, SourceBuf.get()) -
          ByteOffset;
    }
  }

  bool compilerArgsAreClang = false;

  sourcekitd_object_t Req = sourcekitd_request_dictionary_create(nullptr,
                                                                 nullptr, 0);
  ActiveRequest = Opts.Request;
  switch (Opts.Request) {
  case SourceKitRequest::None:
    llvm::errs() << "request is not set\n";
    // FIXME: This non-zero return value is not propagated as an exit code.
    //        In other words, despite returning 1 here, the program still exits
    //        with a zero (successful) exit code.
    return 1;

  case SourceKitRequest::GlobalConfiguration:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestGlobalConfiguration);

    for (auto &Opt : Opts.RequestOptions) {
      auto KeyValue = StringRef(Opt).split('=');
      std::string KeyStr("key.");
      KeyStr.append(KeyValue.first.str());
      sourcekitd_uid_t Key = sourcekitd_uid_get_from_cstr(KeyStr.c_str());

      int64_t Value = 0;
      KeyValue.second.getAsInteger(0, Value);
      sourcekitd_request_dictionary_set_int64(Req, Key, Value);
    }
    break;

  case SourceKitRequest::ProtocolVersion:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestProtocolVersion);
    break;
  
  case SourceKitRequest::CompilerVersion:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestCompilerVersion);
    break;

  case SourceKitRequest::DemangleNames:
    prepareDemangleRequest(Req, Opts);
    break;

  case SourceKitRequest::MangleSimpleClasses:
    prepareMangleRequest(Req, Opts);
    break;

  case SourceKitRequest::EnableCompileNotifications: {
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                          RequestEnableCompileNotifications);
    int64_t value = 1;
    for (auto &Opt : Opts.RequestOptions) {
      auto KeyValue = StringRef(Opt).split('=');
      if (KeyValue.first == "value") {
        KeyValue.second.getAsInteger(0, value);
      } else {
        llvm::errs() << "unknown parameter '" << KeyValue.first
                     << "' in -req-opts";
        return 1;
      }
    }
    sourcekitd_request_dictionary_set_int64(Req, KeyValue, value);
    break;
  }

  case SourceKitRequest::Index:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestIndex);
    break;

  case SourceKitRequest::CodeComplete:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestCodeComplete);
    sourcekitd_request_dictionary_set_int64(Req, KeyOffset, ByteOffset);
    sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
    // Default to sort by name.
    Opts.RequestOptions.insert(Opts.RequestOptions.begin(), "sort.byname=1");
    addRequestOptions(Req, Opts, KeyCodeCompleteOptions, "key.codecomplete.");
    break;

  case SourceKitRequest::CodeCompleteOpen:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                          RequestCodeCompleteOpen);
    sourcekitd_request_dictionary_set_int64(Req, KeyOffset, ByteOffset);
    sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
    addRequestOptions(Req, Opts, KeyCodeCompleteOptions, "key.codecomplete.");
    break;

  case SourceKitRequest::CodeCompleteClose:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                          RequestCodeCompleteClose);
    sourcekitd_request_dictionary_set_int64(Req, KeyOffset, ByteOffset);
    sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
    break;

  case SourceKitRequest::CodeCompleteUpdate:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                          RequestCodeCompleteUpdate);
    sourcekitd_request_dictionary_set_int64(Req, KeyOffset, ByteOffset);
    sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
    addRequestOptions(Req, Opts, KeyCodeCompleteOptions, "key.codecomplete.");
    break;

  case SourceKitRequest::CodeCompleteCacheOnDisk:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                          RequestCodeCompleteCacheOnDisk);
    sourcekitd_request_dictionary_set_string(Req, KeyName,
                                             Opts.CachePath.c_str());
    break;

  case SourceKitRequest::CodeCompleteSetPopularAPI: {
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                          RequestCodeCompleteSetPopularAPI);

    auto addPopularList = [&Req](StringRef filename, sourcekitd_uid_t key) {
      std::vector<std::string> names;
      if (readPopularAPIList(filename, names))
        return true;

      sourcekitd_object_t popular = sourcekitd_request_array_create(nullptr, 0);
      for (auto name : names)
        sourcekitd_request_array_set_string(popular, SOURCEKITD_ARRAY_APPEND,
                                            name.c_str());
      sourcekitd_request_dictionary_set_value(Req, key, popular);
      return false;
    };

    for (auto &Opt : Opts.RequestOptions) {
      auto KeyValue = StringRef(Opt).split('=');
      auto key = llvm::StringSwitch<sourcekitd_uid_t>(KeyValue.first)
        .Case("popular", KeyPopular)
        .Case("unpopular", KeyUnpopular)
        .Default(nullptr);
      if (!key) {
        llvm::errs() << "invalid key '" << KeyValue.first << "' in -req-opts\n";
        return 1;
      }

      if (addPopularList(KeyValue.second, key))
        return 1;
    }

    break;
  }

  case SourceKitRequest::TypeContextInfo:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                          RequestTypeContextInfo);
    sourcekitd_request_dictionary_set_int64(Req, KeyOffset, ByteOffset);
    addRequestOptions(Req, Opts, KeyTypeContextInfoOptions,
                      "key.typecontextinfo.");
    break;

  case SourceKitRequest::ConformingMethodList:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                          RequestConformingMethodList);
    sourcekitd_request_dictionary_set_int64(Req, KeyOffset, ByteOffset);
    addRequestOptionsDirect(Req, Opts);
    break;

  case SourceKitRequest::CursorInfo:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestCursorInfo);
    if (Opts.CollectActionables) {
      sourcekitd_request_dictionary_set_int64(Req, KeyRetrieveRefactorActions, 1);
    }
    if (Opts.Length) {
      sourcekitd_request_dictionary_set_int64(Req, KeyLength, Opts.Length);
    }
    if (!Opts.USR.empty()) {
      sourcekitd_request_dictionary_set_string(Req, KeyUSR, Opts.USR.c_str());
    } else {
      sourcekitd_request_dictionary_set_int64(Req, KeyOffset, ByteOffset);
    }
    addRequestOptionsDirect(Req, Opts);
    break;
  case SourceKitRequest::RangeInfo: {
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestRangeInfo);
    sourcekitd_request_dictionary_set_int64(Req, KeyOffset, ByteOffset);
    auto Length = Opts.Length;
    if (Opts.Length == 0 && Opts.EndLine > 0) {
      auto EndOff = resolveFromLineCol(Opts.EndLine, Opts.EndCol,
                                       Opts.SourceFile, Opts.VFSFiles);
      Length = EndOff - ByteOffset;
    }
    sourcekitd_request_dictionary_set_int64(Req, KeyLength, Length);
    break;
  }

  case SourceKitRequest::CollectExpressionType: {
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestCollectExpressionType);
    addRequestOptionsDirect(Req, Opts);
    break;
  }

  case SourceKitRequest::CollectVariableType: {
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                          RequestCollectVariableType);
    if (Opts.Length) {
      sourcekitd_request_dictionary_set_int64(Req, KeyOffset, ByteOffset);
      sourcekitd_request_dictionary_set_int64(Req, KeyLength, Opts.Length);
    }
    addRequestOptionsDirect(Req, Opts);
    break;
  }

#define SEMANTIC_REFACTORING(KIND, NAME, ID)                                   \
  case SourceKitRequest::KIND:                                                 \
    setRefactoringFields(Req, Opts, KindRefactoring##KIND, SourceBuf.get());   \
    break;
#include "swift/Refactoring/RefactoringKinds.def"

  case SourceKitRequest::MarkupToXML: {
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestMarkupToXML);
    break;
  }
  case SourceKitRequest::NameTranslation: {
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestNameTranslation);
    sourcekitd_request_dictionary_set_int64(Req, KeyOffset, ByteOffset);
    StringRef BaseName;
    llvm::SmallVector<StringRef, 4> ArgPieces;
    sourcekitd_uid_t ArgName;
    if (!Opts.SwiftName.empty()) {
      sourcekitd_request_dictionary_set_uid(Req, KeyNameKind, KindNameSwift);
      ArgName = KeyArgNames;
      StringRef Text(Opts.SwiftName);
      auto ArgStart = Text.find_first_of('(');
      if (ArgStart == StringRef::npos) {
        BaseName = Text;
      } else {
        BaseName = Text.substr(0, ArgStart);
        auto ArgEnd = Text.find_last_of(')');
        if (ArgEnd == StringRef::npos) {
          llvm::errs() << "Swift name is malformed.\n";
          return 1;
        }
        StringRef AllArgs = Text.substr(ArgStart + 1, ArgEnd - ArgStart - 1);
        AllArgs.split(ArgPieces, ':');
        if (!ArgPieces.empty()) {
          if (!ArgPieces.back().empty()) {
            llvm::errs() << "Swift name is malformed.\n";
            return 1;
          }
          ArgPieces.pop_back();
        }
      }
    } else if (!Opts.ObjCName.empty()) {
      sourcekitd_request_dictionary_set_uid(Req, KeyNameKind, KindNameObjc);
      BaseName = Opts.ObjCName;
      ArgName = KeySelectorPieces;
    } else if (!Opts.ObjCSelector.empty()) {
      sourcekitd_request_dictionary_set_uid(Req, KeyNameKind, KindNameObjc);
      StringRef Name(Opts.ObjCSelector);
      Name.split(ArgPieces, ':');
      if (ArgPieces.back().empty())
        ArgPieces.pop_back();
      ArgName = KeySelectorPieces;
    } else {
      llvm::errs() << "must specify either -swift-name or -objc-name or -objc-selector\n";
      return 1;
    }
    if (!BaseName.empty()) {
      std::string S = BaseName.str();
      sourcekitd_request_dictionary_set_string(Req, KeyBaseName, S.c_str());
    }
    if (!ArgPieces.empty()) {
      sourcekitd_object_t Arr = sourcekitd_request_array_create(nullptr, 0);
      for (StringRef A : ArgPieces) {
        std::string S = A.str();
        sourcekitd_request_array_set_string(Arr, SOURCEKITD_ARRAY_APPEND,
                                            S.c_str());
      }
      sourcekitd_request_dictionary_set_value(Req, ArgName, Arr);
    }
    break;
  }

  case SourceKitRequest::RelatedIdents:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestRelatedIdents);
    sourcekitd_request_dictionary_set_int64(Req, KeyOffset, ByteOffset);
    break;
      
  case SourceKitRequest::ActiveRegions:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestActiveRegions);
    break;

  case SourceKitRequest::SyntaxMap:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestEditorOpen);
    sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
    sourcekitd_request_dictionary_set_int64(Req, KeyEnableSyntaxMap, true);
    sourcekitd_request_dictionary_set_int64(Req, KeyEnableStructure, false);
    sourcekitd_request_dictionary_set_int64(Req, KeySyntacticOnly, !Opts.UsedSema);
    break;

  case SourceKitRequest::Structure:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestEditorOpen);
    sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
    sourcekitd_request_dictionary_set_int64(Req, KeyEnableSyntaxMap, false);
    sourcekitd_request_dictionary_set_int64(Req, KeyEnableStructure, true);
    sourcekitd_request_dictionary_set_int64(Req, KeySyntacticOnly, !Opts.UsedSema);
    break;

  case SourceKitRequest::Format:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestEditorOpen);
    sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
    sourcekitd_request_dictionary_set_int64(Req, KeyEnableSyntaxMap, false);
    sourcekitd_request_dictionary_set_int64(Req, KeyEnableStructure, false);
    sourcekitd_request_dictionary_set_int64(Req, KeySyntacticOnly, !Opts.UsedSema);
    break;

  case SourceKitRequest::ExpandPlaceholder:
    if (Opts.Length) {
      // Single placeholder by location.
      sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestEditorExpandPlaceholder);
      sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
      sourcekitd_request_dictionary_set_int64(Req, KeyOffset, ByteOffset);
      sourcekitd_request_dictionary_set_int64(Req, KeyLength, Opts.Length);
    } else {
      if (ByteOffset) {
        llvm::errs() << "Missing '-length <number>'\n";
        return 1;
      }
      // Expand all placeholders.
      sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestEditorOpen);
      sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
      sourcekitd_request_dictionary_set_int64(Req, KeyEnableSyntaxMap, false);
      sourcekitd_request_dictionary_set_int64(Req, KeyEnableStructure, false);
      sourcekitd_request_dictionary_set_int64(Req, KeySyntacticOnly, !Opts.UsedSema);
    }
    break;

  case SourceKitRequest::DocInfo:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestDocInfo);
    break;

  case SourceKitRequest::SemanticInfo:
    InitOpts.UsedSema = true;
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestEditorOpen);
    sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
    break;

  case SourceKitRequest::Open:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestEditorOpen);
    sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
    addRequestOptionsDirect(Req, Opts);
    break;

  case SourceKitRequest::Close:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestEditorClose);
    sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
    break;

  case SourceKitRequest::Edit:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                          RequestEditorReplaceText);
    sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
    sourcekitd_request_dictionary_set_int64(Req, KeyOffset, ByteOffset);
    sourcekitd_request_dictionary_set_int64(Req, KeyLength, Opts.Length);
    sourcekitd_request_dictionary_set_string(Req, KeySourceText,
                                       Opts.ReplaceText.value().c_str());
    addRequestOptionsDirect(Req, Opts);
    break;

  case SourceKitRequest::PrintAnnotations:
    return printAnnotations();
  case SourceKitRequest::PrintDiags:
    return printDiags();
  case SourceKitRequest::ExtractComment:
    if (Opts.SourceFile.empty()) {
      llvm::errs() << "Missing '<source-file>' \n";
      return 1;
    }
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                          RequestEditorExtractTextFromComment);
    break;

  case SourceKitRequest::InterfaceGen:
  case SourceKitRequest::InterfaceGenOpen:
    sourcekitd_request_dictionary_set_int64(Req, KeySynthesizedExtension,
                                            Opts.SynthesizedExtensions);
    if (Opts.ModuleName.empty() && Opts.HeaderPath.empty() &&
        Opts.SourceFile.empty() && Opts.USR.empty()) {
      llvm::errs() << "Missing '-module <module name>' or '-header <path>'" <<
        "or '<source-file>' or '-usr <USR>' \n";
      return 1;
    }
    if (!Opts.ModuleName.empty()) {
      sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                            RequestEditorOpenInterface);
    } else if (!Opts.USR.empty()) {
      sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                            RequestEditorOpenSwiftTypeInterface);
    } else if (!Opts.SourceFile.empty()) {
      sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                            RequestEditorOpenSwiftSourceInterface);
    } else {
      if (Opts.UsingSwiftArgs)
          sourcekitd_request_dictionary_set_int64(Req, KeyUsingSwiftArgs, true);
      else
        compilerArgsAreClang = true;
      sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                            RequestEditorOpenHeaderInterface);
    }

    sourcekitd_request_dictionary_set_string(Req, KeyName, getInterfaceGenDocumentName().c_str());
    if (!Opts.ModuleGroupName.empty())
      sourcekitd_request_dictionary_set_string(Req, KeyGroupName,
                                               Opts.ModuleGroupName.c_str());
    if (!Opts.InterestedUSR.empty())
      sourcekitd_request_dictionary_set_string(Req, KeyInterestedUSR,
                                               Opts.InterestedUSR.c_str());
    if (!Opts.USR.empty())
      sourcekitd_request_dictionary_set_string(Req, KeyUSR, Opts.USR.c_str());
    break;

  case SourceKitRequest::FindInterfaceDoc:
    if (Opts.ModuleName.empty()) {
      llvm::errs() << "Missing '-module <module name>'\n";
      return 1;
    }
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestEditorFindInterfaceDoc);
    break;

  case SourceKitRequest::FindUSR:
    if (Opts.USR.empty()) {
      llvm::errs() << "Missing '-usr <USR string>'\n";
      return 1;
    }
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestEditorFindUSR);
    sourcekitd_request_dictionary_set_string(Req, KeyUSR, Opts.USR.c_str());
    break;

  case SourceKitRequest::ModuleGroups:
    if (Opts.ModuleName.empty()) {
      llvm::errs() << "Missing '-module <module name>'\n";
      return 1;
    }
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestModuleGroups);
    break;

  case SourceKitRequest::FindLocalRenameRanges:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestFindLocalRenameRanges);
    sourcekitd_request_dictionary_set_int64(Req, KeyLine, Opts.Line);
    sourcekitd_request_dictionary_set_int64(Req, KeyColumn, Opts.Col);
    sourcekitd_request_dictionary_set_int64(Req, KeyLength, Opts.Length);
    break;

  case SourceKitRequest::SyntacticRename:
  case SourceKitRequest::FindRenameRanges: {
    if (Opts.Request == SourceKitRequest::SyntacticRename) {
      sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestSyntacticRename);
    } else {
      sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestFindRenameRanges);
    }
    if (Opts.RenameSpecPath.empty()) {
      llvm::errs() << "Missing '-rename-spec <file path>'\n";
      return 1;
    }
    auto Buffer =
        getBufferForFilename(Opts.RenameSpecPath, Opts.VFSFiles)->getBuffer();
    char *Err = nullptr;
    auto RenameSpec = sourcekitd_request_create_from_yaml(Buffer.data(), &Err);
    if (!RenameSpec) {
      assert(Err);
      llvm::errs() << Err;
      free(Err);
      return 1;
    }
    sourcekitd_request_dictionary_set_value(Req, KeyRenameLocations, RenameSpec);
    break;
  }
  case SourceKitRequest::Statistics:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestStatistics);
    break;

  case SourceKitRequest::DependencyUpdated:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                          RequestDependencyUpdated);
    break;
  case SourceKitRequest::Diagnostics:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestDiagnostics);
    break;

  case SourceKitRequest::Compile:
    sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestCompile);
    break;

  case SourceKitRequest::CompileClose:
    sourcekitd_request_dictionary_set_string(Req, KeyName, SemaName.c_str());
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestCompileClose);
    break;

  case SourceKitRequest::SyntacticMacroExpansion:
    sourcekitd_request_dictionary_set_uid(Req, KeyRequest,
                                          RequestSyntacticMacroExpansion);
    setSyntacticMacroExpansions(Req, Opts, SourceBuf.get());
    break;
  }

  if (!Opts.SourceFile.empty()) {
    if (Opts.PassAsSourceText) {
      auto Buf = getBufferForFilename(Opts.SourceFile, Opts.VFSFiles);
      sourcekitd_request_dictionary_set_string(Req, KeySourceText,
                                               Buf->getBufferStart());
    }
    sourcekitd_request_dictionary_set_string(Req, KeySourceFile,
                                             Opts.SourceFile.c_str());
  }

  if (!Opts.PrimaryFile.empty()) {
    sourcekitd_request_dictionary_set_string(Req, KeyPrimaryFile,
                                             Opts.PrimaryFile.c_str());
  }

  if (Opts.SourceText) {
    sourcekitd_request_dictionary_set_string(Req, KeySourceText,
                                             Opts.SourceText->c_str());
    sourcekitd_request_dictionary_set_string(Req, KeySourceFile,
                                             SemaName.c_str());
  }

  if (!Opts.CompilerArgs.empty() ||
      !Opts.ModuleCachePath.empty() ||
      Opts.DisableImplicitConcurrencyModuleImport) {
    sourcekitd_object_t Args = sourcekitd_request_array_create(nullptr, 0);
    if (!Opts.ModuleCachePath.empty()) {
      if (compilerArgsAreClang) {
        // We need -fmodules or else the clang argument parsing does not honour
        // -fmodules-cache-path. In reality, the swift ClangImporter will always
        // enable modules when importing, so this should only impact the
        // clang argument parsing. This is needed even if the header doesn't
        // use modules, since Swift itself will import its shims module, and
        // that needs to honour the -module-cache-path option when testing.
        sourcekitd_request_array_set_string(Args, SOURCEKITD_ARRAY_APPEND, "-fmodules");
        std::string opt = "-fmodules-cache-path=" + Opts.ModuleCachePath;
        sourcekitd_request_array_set_string(Args, SOURCEKITD_ARRAY_APPEND, opt.c_str());
      } else {
        sourcekitd_request_array_set_string(Args, SOURCEKITD_ARRAY_APPEND, "-module-cache-path");
        sourcekitd_request_array_set_string(Args, SOURCEKITD_ARRAY_APPEND, Opts.ModuleCachePath.c_str());
      }
    }
    if (Opts.DisableImplicitConcurrencyModuleImport && !compilerArgsAreClang) {
      sourcekitd_request_array_set_string(Args, SOURCEKITD_ARRAY_APPEND,
                                          "-Xfrontend");
      sourcekitd_request_array_set_string(Args, SOURCEKITD_ARRAY_APPEND,
          "-disable-implicit-concurrency-module-import");
    }
    if (Opts.DisableImplicitStringProcessingModuleImport &&
        !compilerArgsAreClang) {
      sourcekitd_request_array_set_string(Args, SOURCEKITD_ARRAY_APPEND,
                                          "-Xfrontend");
      sourcekitd_request_array_set_string(Args, SOURCEKITD_ARRAY_APPEND,
          "-disable-implicit-string-processing-module-import");
    }
    if (Opts.EnableImplicitBacktracingModuleImport &&
        !compilerArgsAreClang) {
      sourcekitd_request_array_set_string(Args, SOURCEKITD_ARRAY_APPEND,
                                          "-Xfrontend");
      sourcekitd_request_array_set_string(Args, SOURCEKITD_ARRAY_APPEND,
          "-enable-implicit-backtracing-module-import");
    }
    if (Opts.DisableImplicitBacktracingModuleImport &&
        !compilerArgsAreClang) {
      sourcekitd_request_array_set_string(Args, SOURCEKITD_ARRAY_APPEND,
                                          "-Xfrontend");
      sourcekitd_request_array_set_string(Args, SOURCEKITD_ARRAY_APPEND,
          "-disable-implicit-backtracing-module-import");
    }

    for (auto Arg : Opts.CompilerArgs)
      sourcekitd_request_array_set_string(Args, SOURCEKITD_ARRAY_APPEND, Arg);
    sourcekitd_request_dictionary_set_value(Req, KeyCompilerArgs, Args);
    sourcekitd_request_release(Args);
  }

  if (!Opts.ModuleName.empty()) {
    sourcekitd_request_dictionary_set_string(Req, KeyModuleName,
                                             Opts.ModuleName.c_str());
  }
  if (!Opts.HeaderPath.empty()) {
    sourcekitd_request_dictionary_set_string(Req, KeyFilePath,
                                             Opts.HeaderPath.c_str());
  }
  if (Opts.CancelOnSubsequentRequest.has_value()) {
    sourcekitd_request_dictionary_set_int64(Req, KeyCancelOnSubsequentRequest,
                                            *Opts.CancelOnSubsequentRequest);
  }
  if (Opts.SimulateLongRequest.has_value()) {
    sourcekitd_request_dictionary_set_int64(Req, KeySimulateLongRequest,
                                            *Opts.SimulateLongRequest);
  }

  if (!Opts.SwiftVersion.empty()) {
    if (Opts.PassVersionAsString) {
      sourcekitd_request_dictionary_set_string(Req, KeySwiftVersion,
                                               Opts.SwiftVersion.c_str());
    } else {
      unsigned ver;
      if (StringRef(Opts.SwiftVersion).getAsInteger(10, ver)) {
        llvm::errs() << "error: expected integer for 'swift-version'\n";
        return true;
      }
      sourcekitd_request_dictionary_set_int64(Req, KeySwiftVersion, ver);
    }
  }

  if (Opts.VFSName) {
    sourcekitd_request_dictionary_set_string(Req, KeyVFSName, Opts.VFSName->c_str());
  }
  if (!Opts.VFSFiles.empty()) {
    sourcekitd_object_t files = sourcekitd_request_array_create(nullptr, 0);
    for (auto &NameAndTarget : Opts.VFSFiles) {
      sourcekitd_object_t file = sourcekitd_request_dictionary_create(nullptr, nullptr, 0);
      sourcekitd_request_dictionary_set_string(file, KeyName, NameAndTarget.first().data());

      if (NameAndTarget.second.passAsSourceText) {
        auto content = getBufferForFilename(NameAndTarget.first(), Opts.VFSFiles);
        sourcekitd_request_dictionary_set_string(file, KeySourceText,  content->getBufferStart());
      } else {
        sourcekitd_request_dictionary_set_string(file, KeySourceFile,  NameAndTarget.second.path.c_str());
      }
      sourcekitd_request_array_set_value(files, SOURCEKITD_ARRAY_APPEND, file);
    }
    sourcekitd_object_t vfsOpts = sourcekitd_request_dictionary_create(nullptr, nullptr, 0);
    sourcekitd_request_dictionary_set_value(vfsOpts, KeyFiles, files);
    sourcekitd_request_dictionary_set_value(Req, KeyVFSOptions, vfsOpts);
    sourcekitd_request_release(vfsOpts);
    sourcekitd_request_release(files);
  }

  int64_t BeforeInstructions;
  if (Opts.measureInstructions)
    BeforeInstructions = getSourceKitInstructionCount();

  if (!Opts.isAsyncRequest) {
    sourcekitd_response_t Resp = sendRequestSync(Req, Opts);

    if (Opts.measureInstructions) {
      int64_t AfterInstructions = getSourceKitInstructionCount();
      llvm::errs() << "request instructions: "
                   << (AfterInstructions - BeforeInstructions);
    }

    sourcekitd_request_release(Req);
    return handleResponse(Resp, Opts, SemaName, std::move(SourceBuf),
                          &InitOpts)
               ? 1
               : 0;
  } else {
#if SOURCEKITD_HAS_BLOCKS
    AsyncResponseInfo info;
    info.options = Opts;
    info.sourceFilename = std::move(SemaName);
    info.sourceBuffer = std::move(SourceBuf);
    unsigned respIndex = asyncResponses.size();
    asyncResponses.push_back(std::move(info));

    if (Opts.PrintRequest)
      sourcekitd_request_description_dump(Req);

    sourcekitd_send_request(Req, &asyncResponses[respIndex].requestHandle,
                            ^(sourcekitd_response_t resp) {
                              auto &info = asyncResponses[respIndex];
                              info.response = resp;
                              sourcekitd_request_handle_dispose(
                                  info.requestHandle);
                              info.semaphore.signal(); // Ready to be handled!
                            });

#else
    llvm::report_fatal_error(
        "-async not supported when sourcekitd is built without blocks support");
#endif

    if (Opts.measureInstructions) {
      int64_t AfterInstructions = getSourceKitInstructionCount();
      llvm::errs() << "request instructions: "
                   << (AfterInstructions - BeforeInstructions);
    }

    sourcekitd_request_release(Req);
    return 0;
  }
}

static bool handleResponse(sourcekitd_response_t Resp, const TestOptions &Opts,
                           const std::string &SourceFile,
                           std::unique_ptr<llvm::MemoryBuffer> SourceBuf,
                           TestOptions *InitOpts) {
  bool KeepResponseAlive = false;
  bool IsError = sourcekitd_response_is_error(Resp);
  if (IsError) {
    sourcekitd_response_description_dump(Resp);

  } else if (!Opts.PrintResponse) {
    // Nothing.
  } else if (Opts.PrintResponseAsJSON) {
    sourcekitd_variant_t Info = sourcekitd_response_get_value(Resp);
    char *json = sourcekitd_variant_json_description_copy(Info);
    llvm::outs() << json << '\n';
    free(json);

  } else if (Opts.PrintRawResponse) {
    printRawResponse(Resp);

  } else {
    sourcekitd_variant_t Info = sourcekitd_response_get_value(Resp);
    switch (Opts.Request) {
    case SourceKitRequest::None:
      llvm_unreachable("request should be set");
    case SourceKitRequest::PrintAnnotations:
    case SourceKitRequest::PrintDiags:
      llvm_unreachable("print-annotations/print-diags is handled elsewhere");

    case SourceKitRequest::EnableCompileNotifications:
      // Ignore the response.  If it was an error it is handled above.
      break;

    case SourceKitRequest::Open:
      getSemanticInfo(Info, SourceFile);
      KeepResponseAlive = true;
      break;

    case SourceKitRequest::Edit: {
      // Length=0, replace="" is a nop and will not trigger sema.
      bool SyntacticOnly = Opts.Length == 0 && Opts.ReplaceText->empty();
      for (const std::string &ReqOpt : Opts.RequestOptions) {
        if (SyntacticOnly)
          break;
        if (ReqOpt.find("syntactic_only=1") != std::string::npos) {
          SyntacticOnly = true;
        }
      }
      if (SyntacticOnly) {
        printRawResponse(Resp);
      } else {
        getSemanticInfo(Info, SourceFile);
        KeepResponseAlive = true;
      }
      break;
    }
    case SourceKitRequest::DemangleNames:
      printDemangleResults(sourcekitd_response_get_value(Resp), outs());
      break;

    case SourceKitRequest::MangleSimpleClasses:
      printMangleResults(sourcekitd_response_get_value(Resp), outs());
      break;

    case SourceKitRequest::GlobalConfiguration:
    case SourceKitRequest::ProtocolVersion:
    case SourceKitRequest::CompilerVersion:
    case SourceKitRequest::Close:
    case SourceKitRequest::Index:
    case SourceKitRequest::CodeComplete:
    case SourceKitRequest::CodeCompleteOpen:
    case SourceKitRequest::CodeCompleteClose:
    case SourceKitRequest::CodeCompleteUpdate:
    case SourceKitRequest::CodeCompleteCacheOnDisk:
    case SourceKitRequest::CodeCompleteSetPopularAPI:
    case SourceKitRequest::TypeContextInfo:
    case SourceKitRequest::ConformingMethodList:
    case SourceKitRequest::DependencyUpdated:
    case SourceKitRequest::Diagnostics:
      printRawResponse(Resp);
      break;
    case SourceKitRequest::Compile:
      sourcekitd_response_description_dump_filedesc(Resp, STDOUT_FILENO);
      break;
    case SourceKitRequest::CompileClose:
      sourcekitd_response_description_dump_filedesc(Resp, STDOUT_FILENO);
      break;

    case SourceKitRequest::RelatedIdents:
      printRelatedIdents(Info, SourceFile, Opts.VFSFiles, llvm::outs());
      break;

    case SourceKitRequest::ActiveRegions:
      printActiveRegions(Info, SourceFile, Opts.VFSFiles, llvm::outs());
      break;

    case SourceKitRequest::CursorInfo:
      printCursorInfo(Info, SourceFile, Opts.VFSFiles, llvm::outs());
      break;

    case SourceKitRequest::NameTranslation:
      printNameTranslationInfo(Info, llvm::outs());
      break;

    case SourceKitRequest::RangeInfo:
      printRangeInfo(Info, SourceFile, llvm::outs());
      break;

    case SourceKitRequest::CollectExpressionType:
      printExpressionType(Info, llvm::outs());
      break;

    case SourceKitRequest::CollectVariableType:
      printVariableType(Info, SourceBuf.get(), llvm::outs());
      break;

    case SourceKitRequest::DocInfo:
      printDocInfo(Info, SourceFile);
      break;

    case SourceKitRequest::SemanticInfo:
      getSemanticInfo(Info, SourceFile);
      printSemanticInfo();
      break;

    case SourceKitRequest::InterfaceGen:
      printInterfaceGen(Info, Opts.CheckInterfaceIsASCII);
      break;

    case SourceKitRequest::ExtractComment:
    case SourceKitRequest::MarkupToXML:
      printNormalizedDocComment(Info);
      break;

    case SourceKitRequest::InterfaceGenOpen:
      // Just initialize the options for the subsequent request.
      assert(!Opts.isAsyncRequest && InitOpts &&
             "async interface-gen-open is not supported");
      InitOpts->SourceFile = getInterfaceGenDocumentName();
      InitOpts->SourceText =
          sourcekitd_variant_dictionary_get_string(Info, KeySourceText);
      break;

    case SourceKitRequest::FindInterfaceDoc:
      printFoundInterface(Info, llvm::outs());
      break;

    case SourceKitRequest::FindUSR:
      printFoundUSR(Info, SourceBuf.get(), llvm::outs());
      break;

    case SourceKitRequest::SyntaxMap:
    case SourceKitRequest::Structure:
      printRawResponse(Resp);
      if (Opts.ReplaceText.has_value()) {
        unsigned Offset =
            resolveFromLineCol(Opts.Line, Opts.Col, SourceFile, Opts.VFSFiles);
        unsigned Length = Opts.Length;
        sourcekitd_object_t EdReq = sourcekitd_request_dictionary_create(nullptr,
                                                                    nullptr, 0);
        sourcekitd_request_dictionary_set_uid(EdReq, KeyRequest,
                                              RequestEditorReplaceText);
        sourcekitd_request_dictionary_set_string(EdReq, KeyName,
                                                 SourceFile.c_str());
        sourcekitd_request_dictionary_set_int64(EdReq, KeyOffset, Offset);
        sourcekitd_request_dictionary_set_int64(EdReq, KeyLength, Length);
        sourcekitd_request_dictionary_set_string(EdReq, KeySourceText,
                                           Opts.ReplaceText.value().c_str());
        bool EnableSyntaxMax = Opts.Request == SourceKitRequest::SyntaxMap;
        bool EnableSubStructure = Opts.Request == SourceKitRequest::Structure;
        sourcekitd_request_dictionary_set_int64(EdReq, KeyEnableSyntaxMap,
                                                EnableSyntaxMax);
        sourcekitd_request_dictionary_set_int64(EdReq, KeyEnableStructure,
                                                EnableSubStructure);
        sourcekitd_request_dictionary_set_int64(EdReq, KeySyntacticOnly,
                                                !Opts.UsedSema);

        sourcekitd_response_t EdResp = sendRequestSync(EdReq, Opts);
        printRawResponse(EdResp);
        sourcekitd_response_dispose(EdResp);
        sourcekitd_request_release(EdReq);
      }
      break;

    case SourceKitRequest::Format:
      {
        sourcekitd_object_t Fmt = sourcekitd_request_dictionary_create(nullptr,
                                                                    nullptr, 0);
        sourcekitd_request_dictionary_set_uid(Fmt, KeyRequest,
                                              RequestEditorFormatText);
        sourcekitd_request_dictionary_set_string(Fmt, KeyName,
                                                 SourceFile.c_str());
        sourcekitd_request_dictionary_set_string(Fmt, KeySourceText, "");
        sourcekitd_request_dictionary_set_int64(Fmt, KeyLine, Opts.Line);
        sourcekitd_request_dictionary_set_int64(Fmt, KeyLength, Opts.Length);

        if (!Opts.RequestOptions.empty()) {
          sourcekitd_object_t FO = sourcekitd_request_dictionary_create(nullptr,
                                                                    nullptr, 0);
          for (auto &FmtOpt : Opts.RequestOptions) {
            auto KeyValue = StringRef(FmtOpt).split('=');
            std::string KeyStr("key.editor.format.");
            KeyStr.append(KeyValue.first.str());
            sourcekitd_uid_t Key = sourcekitd_uid_get_from_cstr(KeyStr.c_str());
            int64_t Value = 0;
            KeyValue.second.getAsInteger(0, Value);
            sourcekitd_request_dictionary_set_int64(FO, Key, Value);
          }
          sourcekitd_request_dictionary_set_value(Fmt, KeyFormatOptions, FO);
          sourcekitd_request_release(FO);
        }

        sourcekitd_response_t FmtResp = sendRequestSync(Fmt, Opts);
        printRawResponse(FmtResp);
        sourcekitd_response_dispose(FmtResp);
        sourcekitd_request_release(Fmt);
      }
      break;

      case SourceKitRequest::ExpandPlaceholder:
        if (Opts.Length) {
          // Single placeholder by location.
          printRawResponse(Resp);
        } else {
          // Expand all placeholders.
          expandPlaceholders(SourceBuf.get(), llvm::outs());
        }
        break;
      case SourceKitRequest::ModuleGroups:
        printModuleGroupNames(Info, llvm::outs());
        break;
#define SEMANTIC_REFACTORING(KIND, NAME, ID) case SourceKitRequest::KIND:
#include "swift/Refactoring/RefactoringKinds.def"
      case SourceKitRequest::SyntacticRename:
      case SourceKitRequest::SyntacticMacroExpansion:
        printSyntacticRenameEdits(Info, llvm::outs());
        break;
      case SourceKitRequest::FindRenameRanges:
      case SourceKitRequest::FindLocalRenameRanges:
        printRenameRanges(Info, llvm::outs());
        break;
      case SourceKitRequest::Statistics:
        printStatistics(Info, llvm::outs());
        break;
    }
  }

  if (!KeepResponseAlive)
    sourcekitd_response_dispose(Resp);
  return IsError;
}

sourcekitd_variant_t LatestSemaAnnotations = {{0,0,0}};
sourcekitd_variant_t LatestSemaDiags = {{0,0,0}};

static void getSemanticInfoImpl(sourcekitd_variant_t Info) {
  LatestSemaAnnotations =
    sourcekitd_variant_dictionary_get_value(Info, KeyAnnotations);
  LatestSemaDiags =
    sourcekitd_variant_dictionary_get_value(Info, KeyDiagnostics);
}

static void getSemanticInfoImplAfterDocUpdate(sourcekitd_variant_t EditOrOpen,
                                              sourcekitd_variant_t DocUpdate) {
  if (sourcekitd_variant_dictionary_get_uid(EditOrOpen, KeyDiagnosticStage) ==
      SemaDiagnosticStage) {
    // FIXME: currently we only return annotations once, so if the original edit
    // or open request was slow enough, it may "take" the annotations. If that
    // is fixed, we can skip checking the diagnostic stage and always use the
    // DocUpdate variant.
    assert(sourcekitd_variant_get_type(sourcekitd_variant_dictionary_get_value(
               DocUpdate, KeyAnnotations)) == SOURCEKITD_VARIANT_TYPE_NULL);

    getSemanticInfoImpl(EditOrOpen);
  } else {
    getSemanticInfoImpl(DocUpdate);
  }
}

static void getSemanticInfo(sourcekitd_variant_t Info, StringRef Filename) {
  // Wait for the notification that semantic info is available.
  // But only for 1 min.
  bool expired = semaSemaphore.wait(60 * 1000);
  if (expired) {
    llvm::report_fatal_error("Never got notification for semantic info");
  }

  if (Filename != semaName){
    llvm::report_fatal_error(
      llvm::Twine("Got notification for different doc name: ") + semaName);
  }

  getSemanticInfoImplAfterDocUpdate(
      Info, sourcekitd_response_get_value(semaResponse));
}

static int printAnnotations() {
  printRawVariant(LatestSemaAnnotations);
  return 0;
}

static int printDiags() {
  printRawVariant(LatestSemaDiags);
  return 0;
}

static void printSemanticInfo() {
  printAnnotations();
  if (sourcekitd_variant_get_type(LatestSemaDiags) != SOURCEKITD_VARIANT_TYPE_NULL)
    printDiags();
}

static void notification_receiver(sourcekitd_response_t resp) {
  if (sourcekitd_response_is_error(resp)) {
    sourcekitd_response_description_dump(resp);
    exit(1);
  }

  sourcekitd_variant_t payload = sourcekitd_response_get_value(resp);
  sourcekitd_uid_t note =
      sourcekitd_variant_dictionary_get_uid(payload, KeyNotification);

  if (note == NoteDocUpdate) {
    semaName = sourcekitd_variant_dictionary_get_string(payload, KeyName);

    sourcekitd_object_t edReq = sourcekitd_request_dictionary_create(nullptr,
                                                                nullptr, 0);
    sourcekitd_request_dictionary_set_uid(edReq, KeyRequest,
                                          RequestEditorReplaceText);
    sourcekitd_request_dictionary_set_string(edReq, KeyName, semaName);
    sourcekitd_request_dictionary_set_string(edReq, KeySourceText, "");
    semaResponse = sourcekitd_send_request_sync(edReq);
    sourcekitd_request_release(edReq);
    semaSemaphore.signal();
  } else if (note == NoteTest) {
    noteSyncSemaphore.signal();
  } else {
    notificationBuffer.add(resp);
  }
}

static void printNameTranslationInfo(sourcekitd_variant_t Info,
                                     llvm::raw_ostream &OS) {
  const char *InternalDiagnostic =
      sourcekitd_variant_dictionary_get_string(Info, KeyInternalDiagnostic);
  if (InternalDiagnostic) {
    OS << "<empty name translation info; internal diagnostic: \""
       << InternalDiagnostic << "\">\n";
    return;
  }
  sourcekitd_uid_t KindUID = sourcekitd_variant_dictionary_get_uid(Info,
                                                                   KeyNameKind);
  if (KindUID == nullptr) {
    OS << "<empty name translation info>\n";
    return;
  }
  const char *Kind = sourcekitd_uid_get_string_ptr(KindUID);
  const char *BaseName = sourcekitd_variant_dictionary_get_string(Info,
                                                                  KeyBaseName);
  std::vector<const char *> Selectors;
  sourcekitd_variant_t SelectorsObj =
    sourcekitd_variant_dictionary_get_value(Info, KeySelectorPieces);
  for (unsigned i = 0, e = sourcekitd_variant_array_get_count(SelectorsObj);
         i != e; ++i) {
    sourcekitd_variant_t Entry =
      sourcekitd_variant_array_get_value(SelectorsObj, i);
    Selectors.push_back(sourcekitd_variant_dictionary_get_string(Entry, KeyName));
  }

  bool IsZeroArgSelector = false;
  auto IsZeroArgObj = sourcekitd_variant_dictionary_get_value(Info, KeyIsZeroArgSelector);
  if (sourcekitd_variant_get_type(IsZeroArgObj) != SOURCEKITD_VARIANT_TYPE_NULL) {
    IsZeroArgSelector = sourcekitd_variant_int64_get_value(IsZeroArgObj);
  }

  std::vector<const char *> Args;
  sourcekitd_variant_t ArgsObj =
    sourcekitd_variant_dictionary_get_value(Info, KeyArgNames);
  for (unsigned i = 0, e = sourcekitd_variant_array_get_count(ArgsObj);
       i != e; ++i) {
    sourcekitd_variant_t Entry =
      sourcekitd_variant_array_get_value(ArgsObj, i);
    Args.push_back(sourcekitd_variant_dictionary_get_string(Entry, KeyName));
  }

  OS << Kind << "\n";
  OS << StringRef(BaseName);
  if (!Args.empty()) {
    OS << "(";
    for (auto A : Args) {
      StringRef Text(A);
      if (Text.empty())
        OS << "_" << ":";
      else
        OS << Text << ":";
    }
    OS << ")";
  }
  for (auto S : Selectors) {
    OS << S;
    if (!IsZeroArgSelector) {
      OS << ":";
    }
  }
  OS << '\n';
}

template <typename T>
static std::vector<T> readArray(
    sourcekitd_variant_t Info, sourcekitd_uid_t Key,
    std::function<T(sourcekitd_variant_t)> elementFromEntry) {
  std::vector<T> Elements;
  sourcekitd_variant_t Obj =
    sourcekitd_variant_dictionary_get_value(Info, Key);
  for (unsigned i = 0, e = sourcekitd_variant_array_get_count(Obj);
       i != e; ++i) {
    sourcekitd_variant_t Entry = sourcekitd_variant_array_get_value(Obj, i);
    Elements.push_back(elementFromEntry(Entry));
  }
  return Elements;
}

static std::vector<const char *> readStringArray(
    sourcekitd_variant_t Info, sourcekitd_uid_t Key,
    sourcekitd_uid_t ElementKey) {
  return readArray<const char *>(Info, Key, [&](sourcekitd_variant_t Entry) {
    return sourcekitd_variant_dictionary_get_string(Entry, ElementKey);
  });
}

struct ResponseSymbolInfo {
  struct ParentInfo {
    const char *Title;
    const char *Kind;
    const char *USR;
  };
  struct ReferencedSymbol {
    const char *USR;
    const char *AccessLevel;
    const char *Filename;
    const char *ModuleName;
    const char *DeclLang;
    bool IsSystem;
    bool IsSPI;
    std::vector<ParentInfo> ParentContexts;
  };

  const char *Kind = nullptr;
  const char *Lang = nullptr;
  const char *Name = nullptr;
  const char *USR = nullptr;
  const char *TypeName = nullptr;
  const char *TypeUSR = nullptr;
  const char *ContainerTypeUSR = nullptr;
  const char *DocComment = nullptr;
  const char *GroupName = nullptr;
  const char *LocalizationKey = nullptr;
  const char *AnnotatedDeclaration = nullptr;
  const char *FullyAnnotatedDeclaration = nullptr;
  const char *SymbolGraph = nullptr;
  const char *ModuleName = nullptr;
  const char *ModuleInterfaceName = nullptr;
  const char *FilePath = nullptr;
  unsigned Offset = 0;
  unsigned Length = 0;
  unsigned Line = 0;
  unsigned Column = 0;
  std::vector<const char *> OverrideUSRs;
  std::vector<const char *> AnnotatedRelatedDeclarations;
  std::vector<const char *> ModuleGroups;
  std::vector<ParentInfo> ParentContexts;
  std::vector<ReferencedSymbol> ReferencedSymbols;

  std::vector<const char *> ReceiverUSRs;
  bool IsSystem = false;
  bool IsDynamic = false;
  bool IsSynthesized = false;
  unsigned ParentOffset = 0;

  static ResponseSymbolInfo read(sourcekitd_variant_t Info) {
    ResponseSymbolInfo Symbol;

    sourcekitd_uid_t KindUID =
        sourcekitd_variant_dictionary_get_uid(Info, KeyKind);
    if (KindUID == nullptr)
      return Symbol;
    Symbol.Kind = sourcekitd_uid_get_string_ptr(KindUID);

    sourcekitd_uid_t LangUID =
        sourcekitd_variant_dictionary_get_uid(Info, KeyDeclarationLang);
    if (LangUID)
      Symbol.Lang = sourcekitd_uid_get_string_ptr(LangUID);

    Symbol.Name = sourcekitd_variant_dictionary_get_string(Info, KeyName);
    Symbol.USR = sourcekitd_variant_dictionary_get_string(Info, KeyUSR);

    Symbol.TypeName =
        sourcekitd_variant_dictionary_get_string(Info, KeyTypeName);
    Symbol.TypeUSR = sourcekitd_variant_dictionary_get_string(Info, KeyTypeUsr);
    Symbol.ContainerTypeUSR =
        sourcekitd_variant_dictionary_get_string(Info, KeyContainerTypeUsr);

    Symbol.DocComment =
        sourcekitd_variant_dictionary_get_string(Info, KeyDocFullAsXML);
    Symbol.GroupName =
        sourcekitd_variant_dictionary_get_string(Info, KeyGroupName);
    Symbol.LocalizationKey =
        sourcekitd_variant_dictionary_get_string(Info, KeyLocalizationKey);

    Symbol.AnnotatedDeclaration =
        sourcekitd_variant_dictionary_get_string(Info, KeyAnnotatedDecl);
    Symbol.FullyAnnotatedDeclaration =
        sourcekitd_variant_dictionary_get_string(Info, KeyFullyAnnotatedDecl);
    Symbol.SymbolGraph =
        sourcekitd_variant_dictionary_get_string(Info, KeySymbolGraph);

    Symbol.ModuleName =
        sourcekitd_variant_dictionary_get_string(Info, KeyModuleName);
    Symbol.ModuleInterfaceName =
        sourcekitd_variant_dictionary_get_string(Info, KeyModuleInterfaceName);

    Symbol.FilePath =
        sourcekitd_variant_dictionary_get_string(Info, KeyFilePath);
    if (Symbol.FilePath) {
      Symbol.Offset = sourcekitd_variant_dictionary_get_int64(Info, KeyOffset);
      Symbol.Length = sourcekitd_variant_dictionary_get_int64(Info, KeyLength);
      Symbol.Line = sourcekitd_variant_dictionary_get_int64(Info, KeyLine);
      Symbol.Column = sourcekitd_variant_dictionary_get_int64(Info, KeyColumn);
    }

    Symbol.OverrideUSRs = readStringArray(Info, KeyOverrides, KeyUSR);
    Symbol.AnnotatedRelatedDeclarations =
        readStringArray(Info, KeyRelatedDecls, KeyAnnotatedDecl);
    Symbol.ModuleGroups = readStringArray(Info, KeyModuleGroups, KeyGroupName);

    Symbol.ParentContexts = readArray<ParentInfo>(
        Info, KeyParentContexts, [&](sourcekitd_variant_t Entry) {
          return ParentInfo{
              sourcekitd_variant_dictionary_get_string(Entry, KeyName),
              sourcekitd_variant_dictionary_get_string(Entry, KeyKind),
              sourcekitd_variant_dictionary_get_string(Entry, KeyUSR)};
        });
    Symbol.ReferencedSymbols = readArray<ReferencedSymbol>(
        Info, KeyReferencedSymbols, [&](sourcekitd_variant_t Entry){
          return ReferencedSymbol{
            sourcekitd_variant_dictionary_get_string(Entry, KeyUSR),
            sourcekitd_variant_dictionary_get_string(Entry, KeyAccessLevel),
            sourcekitd_variant_dictionary_get_string(Entry, KeyFilePath),
            sourcekitd_variant_dictionary_get_string(Entry, KeyModuleName),
            sourcekitd_uid_get_string_ptr(
              sourcekitd_variant_dictionary_get_uid(Entry, KeyDeclarationLang)),
            sourcekitd_variant_dictionary_get_bool(Entry, KeyIsSystem),
            sourcekitd_variant_dictionary_get_bool(Entry, KeyIsSPI),
            readArray<ParentInfo>(Entry, KeyParentContexts,
                                  [&](sourcekitd_variant_t Entry){
              return ParentInfo{
                  sourcekitd_variant_dictionary_get_string(Entry, KeyName),
                  sourcekitd_variant_dictionary_get_string(Entry, KeyKind),
                  sourcekitd_variant_dictionary_get_string(Entry, KeyUSR)};
            })
          };
        });

    Symbol.ReceiverUSRs = readStringArray(Info, KeyReceivers, KeyUSR);

    Symbol.IsSystem = sourcekitd_variant_dictionary_get_bool(Info, KeyIsSystem);
    Symbol.IsDynamic =
        sourcekitd_variant_dictionary_get_bool(Info, KeyIsDynamic);
    Symbol.IsSynthesized =
        sourcekitd_variant_dictionary_get_bool(Info, KeyIsSynthesized);

    Symbol.ParentOffset =
        sourcekitd_variant_dictionary_get_int64(Info, KeyParentLoc);

    return Symbol;
  }

  void print(llvm::raw_ostream &OS, StringRef CurrentFilename,
             const llvm::StringMap<TestOptions::VFSFile> &VFSFiles) {
    if (Kind == nullptr) {
      OS << "<empty symbol info>\n";
      return;
    }

    OS << Kind << " (";
    if (FilePath) {
      if (CurrentFilename != StringRef(FilePath))
        OS << FilePath << ':';

      auto LineCol =
          resolveToLineCol(Offset, FilePath, VFSFiles, /*ExitOnError=*/false);
      if (LineCol.first == 0 && LineCol.second == 0) {
        OS << "*missing file*";
      } else if (LineCol.first != Line || LineCol.second != Column) {
        OS << "*offset does not match line/column in response*";
      } else {
        OS << LineCol.first << ':' << LineCol.second;
        auto EndLineCol = resolveToLineCol(Offset + Length, FilePath,
                                           VFSFiles);
        OS << '-' << EndLineCol.first << ':' << EndLineCol.second;
      }
    }
    OS << ")" << '\n';

    OS << Name << '\n';
    if (USR)
      OS << USR << '\n';
    if (Lang)
      OS << Lang << '\n';
    if (TypeName)
      OS << TypeName << '\n';
    if (TypeUSR)
      OS << TypeUSR << '\n';
    if (ContainerTypeUSR)
      OS << "<Container>" << ContainerTypeUSR << "</Container>" << '\n';
    if (ModuleName)
      OS << ModuleName << '\n';
    if (GroupName)
      OS << "<Group>" << GroupName << "</Group>" << '\n';
    if (ModuleInterfaceName)
      OS << ModuleInterfaceName << '\n';
    if (IsSystem)
      OS << "SYSTEM" << '\n';
    if (AnnotatedDeclaration)
      OS << AnnotatedDeclaration << '\n';
    if (FullyAnnotatedDeclaration)
      OS << FullyAnnotatedDeclaration << '\n';
    if (DocComment)
      OS << DocComment << '\n';
    if (LocalizationKey) {
      OS << "<LocalizationKey>" << LocalizationKey;
      OS << "</LocalizationKey>" << '\n';
    }
    if (IsDynamic)
      OS << "DYNAMIC\n";
    if (IsSynthesized)
      OS << "SYNTHESIZED\n";
    if (ParentOffset) {
      OS << "PARENT OFFSET: " << ParentOffset << "\n";
    }

    OS << "SYMBOL GRAPH BEGIN\n";
    if (SymbolGraph) {
      if (auto PrettyJsonOrErr = json::parse(StringRef(SymbolGraph))) {
        OS << formatv("{0:2}", *PrettyJsonOrErr);
      } else {
        llvm::handleAllErrors(PrettyJsonOrErr.takeError(),
                              [&](const llvm::ErrorInfoBase &E) {});
        OS << SymbolGraph;
      }
      OS << "\n";
    }
    OS << "SYMBOL GRAPH END\n";

    OS << "PARENT CONTEXTS BEGIN\n";
    for (auto Parent : ParentContexts)
      OS << Parent.Title << " " << Parent.Kind << " " << Parent.USR << '\n';
    OS << "PARENT CONTEXTS END\n";

    OS << "REFERENCED DECLS BEGIN\n";
    for (auto Ref : ReferencedSymbols) {
      OS << Ref.USR << " | " << Ref.AccessLevel << " | "
         << (strlen(Ref.Filename) ? Ref.Filename : "<empty>") << " | "
         << (strlen(Ref.ModuleName) ? Ref.ModuleName : "<empty>") << " | "
         << (Ref.IsSystem ? "System" : "User") << " | "
         << (Ref.IsSPI ? "SPI" : "NonSPI") << " | "
         << Ref.DeclLang << "\n";
      for (auto Parent: Ref.ParentContexts)
        OS << "  " << Parent.Title << " " << Parent.Kind << " " << Parent.USR
           << '\n';
    }
    OS << "REFERENCED DECLS END\n";

    OS << "OVERRIDES BEGIN\n";
    for (auto OverUSR : OverrideUSRs)
      OS << OverUSR << '\n';
    OS << "OVERRIDES END\n";

    OS << "RELATED BEGIN\n";
    for (auto RelDecl : AnnotatedRelatedDeclarations)
      OS << RelDecl << '\n';
    OS << "RELATED END\n";

    OS << "MODULE GROUPS BEGIN\n";
    for (auto Group : ModuleGroups)
      OS << Group << '\n';
    OS << "MODULE GROUPS END\n";

    OS << "RECEIVERS BEGIN\n";
    for (auto Receiver : ReceiverUSRs)
      OS << Receiver << '\n';
    OS << "RECEIVERS END\n";
  }
};

static void printCursorInfo(sourcekitd_variant_t Info, StringRef FilenameIn,
                            const llvm::StringMap<TestOptions::VFSFile> &VFSFiles,
                            llvm::raw_ostream &OS) {
  const char *InternalDiagnostic =
      sourcekitd_variant_dictionary_get_string(Info, KeyInternalDiagnostic);
  if (InternalDiagnostic) {
    OS << "<empty cursor info; internal diagnostic: \""
       << InternalDiagnostic << "\">\n";
    return;
  }

  auto SymbolInfo = ResponseSymbolInfo::read(Info);
  struct ActionInfo {
    const char *KindUID;
    const char *KindName;
    const char *UnavailReason;
  };
  std::vector<ActionInfo> AvailableActions = readArray<ActionInfo>(
      Info, KeyRefactorActions, [&](sourcekitd_variant_t Entry) {
    return ActionInfo {
      sourcekitd_uid_get_string_ptr(
          sourcekitd_variant_dictionary_get_uid(Entry, KeyActionUID)),
      sourcekitd_variant_dictionary_get_string(Entry, KeyActionName),
      sourcekitd_variant_dictionary_get_string(Entry,
                                               KeyActionUnavailableReason)
    };
  });

  std::vector<ResponseSymbolInfo> SecondarySymbols =
      readArray<ResponseSymbolInfo>(Info, KeySecondarySymbols,
                                    [&](sourcekitd_variant_t Entry) {
                                      return ResponseSymbolInfo::read(Entry);
                                    });

  std::string Filename = FilenameIn.str();
  llvm::SmallString<256> output;
  if (!llvm::sys::fs::real_path(Filename, output))
    Filename = std::string(output.str());

  SymbolInfo.print(OS, Filename, VFSFiles);
  OS << "ACTIONS BEGIN\n";
  for (auto Action : AvailableActions) {
    OS << Action.KindUID << '\n';
    OS << Action.KindName << '\n';
    if (Action.UnavailReason) {
      OS << Action.UnavailReason << '\n';
    }
  }
  OS << "ACTIONS END\n";

  OS << "SECONDARY SYMBOLS BEGIN\n";
  for (auto Secondary : SecondarySymbols) {
    Secondary.print(OS, Filename, VFSFiles);
    OS << "-----\n";
  }
  OS << "SECONDARY SYMBOLS END\n";
  OS << "DID REUSE AST CONTEXT: "
     << sourcekitd_variant_dictionary_get_bool(Info, KeyReusingASTContext)
     << '\n';
}

static void printRangeInfo(sourcekitd_variant_t Info, StringRef FilenameIn,
                            llvm::raw_ostream &OS) {
  sourcekitd_uid_t KindUID = sourcekitd_variant_dictionary_get_uid(Info,
                                      sourcekitd_uid_get_from_cstr("key.kind"));
  if (KindUID == nullptr) {
    OS << "<empty range info>\n";
    return;
  }

  std::string Filename = FilenameIn.str();
  llvm::SmallString<256> output;
  if (llvm::sys::fs::real_path(Filename, output))
    Filename = std::string(output.str());

  sourcekitd_variant_t OffsetObj =
    sourcekitd_variant_dictionary_get_value(Info, KeyOffset);
  llvm::Optional<int64_t> Offset;
  if (sourcekitd_variant_get_type(OffsetObj) != SOURCEKITD_VARIANT_TYPE_NULL) {
    Offset = sourcekitd_variant_int64_get_value(OffsetObj);
  }
  const char *Kind = sourcekitd_uid_get_string_ptr(KindUID);
  const char *Typename = sourcekitd_variant_dictionary_get_string(Info,
                                                                  KeyTypeName);

  const char *RangeContent = sourcekitd_variant_dictionary_get_string(Info,
                                                              KeyRangeContent);
  OS << "<kind>" << Kind << "</kind>\n";
  OS << "<content>" <<RangeContent << "</content>\n";
  if (Typename)
    OS << "<type>" <<Typename << "</type>\n";
}

static void printExpressionType(sourcekitd_variant_t Info, llvm::raw_ostream &OS) {
  auto TypeBuffer = sourcekitd_variant_dictionary_get_value(Info, KeyExpressionTypeList);
  unsigned Count = sourcekitd_variant_array_get_count(TypeBuffer);
  if (!Count) {
    OS << "cannot find expression types in the file\n";
    return;
  }
  OS << "<ExpressionTypes>\n";
  for (unsigned i = 0; i != Count; ++i) {
    sourcekitd_variant_t Item = sourcekitd_variant_array_get_value(TypeBuffer, i);
    unsigned Offset = sourcekitd_variant_dictionary_get_int64(Item, KeyExpressionOffset);
    unsigned Length = sourcekitd_variant_dictionary_get_int64(Item, KeyExpressionLength);
    OS << "(" << Offset << ", " << Offset + Length << "): " <<
      sourcekitd_variant_dictionary_get_string(Item, KeyExpressionType) << "\n";
    sourcekitd_variant_t protocols = sourcekitd_variant_dictionary_get_value(Item,
      KeyExpectedTypes);
    unsigned Count = sourcekitd_variant_array_get_count(protocols);
    for (unsigned i = 0; i != Count; i ++) {
      OS << "conforming to: " << sourcekitd_variant_array_get_string(protocols, i) << "\n";
    }
  }
  OS << "</ExpressionTypes>\n";
}

static void printVariableType(sourcekitd_variant_t Info,
                              llvm::MemoryBuffer *SourceBuf,
                              llvm::raw_ostream &OS) {
  auto TypeBuffer =
      sourcekitd_variant_dictionary_get_value(Info, KeyVariableTypeList);
  unsigned Count = sourcekitd_variant_array_get_count(TypeBuffer);
  if (!Count) {
    OS << "cannot find variable types in the file\n";
    return;
  }
  OS << "<VariableTypes>\n";
  for (unsigned i = 0; i != Count; ++i) {
    sourcekitd_variant_t Item = sourcekitd_variant_array_get_value(TypeBuffer, i);
    unsigned Offset = sourcekitd_variant_dictionary_get_int64(Item, KeyVariableOffset);
    unsigned Length = sourcekitd_variant_dictionary_get_int64(Item, KeyVariableLength);
    auto Start = resolveToLineCol(Offset, SourceBuf);
    auto End = resolveToLineCol(Offset + Length, SourceBuf);
    bool HasExplicitType = sourcekitd_variant_dictionary_get_bool(Item, KeyVariableTypeExplicit);
    auto PrintedType = sourcekitd_variant_dictionary_get_string(Item, KeyVariableType);
    OS << "("
       << Start.first << ":" << Start.second
       << ", "
       << End.first << ":" << End.second
       << "): "
       << PrintedType
       << " (explicit type: " << HasExplicitType << ")\n";
  }
  OS << "</VariableTypes>\n";
}

static void printFoundInterface(sourcekitd_variant_t Info,
                                llvm::raw_ostream &OS) {
  const char *Name = sourcekitd_variant_dictionary_get_string(Info,
                                                        KeyModuleInterfaceName);
  OS << "DOC: (";
  if (Name)
    OS << Name;
  OS << ")\n";

  sourcekitd_variant_t ArgObj =
      sourcekitd_variant_dictionary_get_value(Info, KeyCompilerArgs);

  OS << "ARGS: [";
  if (sourcekitd_variant_get_type(ArgObj) != SOURCEKITD_VARIANT_TYPE_NULL) {
    for (unsigned i = 0, e = sourcekitd_variant_array_get_count(ArgObj);
           i != e; ++i) {
      OS << sourcekitd_variant_array_get_string(ArgObj, i);
      OS << ' ';
    }
  }
  OS << "]\n";
}

static void printFoundUSR(sourcekitd_variant_t Info,
                          llvm::MemoryBuffer *SourceBuf,
                          llvm::raw_ostream &OS) {
  sourcekitd_variant_t OffsetObj =
      sourcekitd_variant_dictionary_get_value(Info, KeyOffset);
  llvm::Optional<int64_t> Offset;
  if (sourcekitd_variant_get_type(OffsetObj) != SOURCEKITD_VARIANT_TYPE_NULL)
    Offset = sourcekitd_variant_int64_get_value(OffsetObj);

  if (!Offset.has_value()) {
    OS << "USR NOT FOUND\n";
    return;
  }

  int64_t Length = sourcekitd_variant_dictionary_get_int64(Info, KeyLength);

  auto LineCol1 = resolveToLineCol(Offset.value(), SourceBuf);
  auto LineCol2 = resolveToLineCol(Offset.value() + Length, SourceBuf);
  OS << '(' << LineCol1.first << ':' << LineCol1.second << '-'
            << LineCol2.first << ':' << LineCol2.second << ")\n";
}

static void printNormalizedDocComment(sourcekitd_variant_t Info) {
  sourcekitd_variant_t Source =
    sourcekitd_variant_dictionary_get_value(Info, KeySourceText);
  printRawVariant(Source);
}

static void printDocInfo(sourcekitd_variant_t Info, StringRef Filename) {
  const char *text =
      sourcekitd_variant_dictionary_get_string(Info, KeySourceText);
  if (text) {
    llvm::outs() << text << '\n';
  }

  sourcekitd_variant_t annotations =
      sourcekitd_variant_dictionary_get_value(Info, KeyAnnotations);
  sourcekitd_variant_t entities =
      sourcekitd_variant_dictionary_get_value(Info,
                                  sourcekitd_uid_get_from_cstr("key.entities"));
  sourcekitd_variant_t diags =
      sourcekitd_variant_dictionary_get_value(Info, KeyDiagnostics);

  printRawVariant(annotations);
  printRawVariant(entities);

  if (sourcekitd_variant_get_type(diags) != SOURCEKITD_VARIANT_TYPE_NULL)
    printRawVariant(diags);
}

static void checkTextIsASCII(const char *Text) {
  for (const char *p = Text; *p; ++p) {
    if (*p & 0x80) {
      auto LineCol = resolveToLineColFromBuf(p - Text, Text);

      llvm::errs() << "!!Interface text is non-ascii!!\n"
                   << "@ " << LineCol.first << ":" << LineCol.second << "\n";
      exit(1);
    }
  }
}

static void printModuleGroupNames(sourcekitd_variant_t Info,
                                  llvm::raw_ostream &OS) {
  sourcekitd_variant_t Groups =
    sourcekitd_variant_dictionary_get_value(Info, KeyModuleGroups);
  OS << "<GROUPS>\n";
  for (unsigned i = 0, e = sourcekitd_variant_array_get_count(Groups);
       i != e; ++i) {
    sourcekitd_variant_t Entry =
    sourcekitd_variant_array_get_value(Groups, i);
    OS << sourcekitd_variant_dictionary_get_string(Entry, KeyGroupName) << "\n";
  }
  OS << "<\\GROUPS>\n";
}

static StringRef getLineColRange(StringRef Text, int64_t StartLine,
                                 int64_t StartCol, int64_t EndLine,
                                 int64_t EndCol) {
  unsigned Line = 1;
  size_t Length = 0;

  while (Line < StartLine) {
    Text = Text.split('\n').second;
    ++Line;
  }
  Text = Text.drop_front(StartCol - 1);
  if (StartLine == EndLine)
    return Text.substr(0, EndCol - StartCol);

  while(Line < EndLine) {
    Length = Text.find('\n', Length) + 1;
    ++Line;
  }
  Length += EndCol - 1;

  return Text.substr(0, Length);
}

static void printSyntacticRenameEdits(sourcekitd_variant_t Info,
                                      llvm::raw_ostream &OS) {
  sourcekitd_variant_t CategorizedEdits =
    sourcekitd_variant_dictionary_get_value(Info, KeyCategorizedEdits);
  for (unsigned i = 0, e = sourcekitd_variant_array_get_count(CategorizedEdits);
       i != e; ++i) {
    sourcekitd_variant_t
    Categorized = sourcekitd_variant_array_get_value(CategorizedEdits, i);
    sourcekitd_uid_t
    Category = sourcekitd_variant_dictionary_get_uid(Categorized, KeyCategory);
    OS << sourcekitd_uid_get_string_ptr(Category) << ":\n";

    sourcekitd_variant_t Edits =
    sourcekitd_variant_dictionary_get_value(Categorized, KeyEdits);
    for(unsigned j = 0, je = sourcekitd_variant_array_get_count(Edits);
        j != je; ++j) {
      OS << "  "; // indent

      sourcekitd_variant_t Edit = sourcekitd_variant_array_get_value(Edits, j);

      StringRef Path(
          sourcekitd_variant_dictionary_get_string(Edit, KeyFilePath));
      if (!Path.empty()) {
        OS << Path << " ";
      }

      int64_t Line = sourcekitd_variant_dictionary_get_int64(Edit, KeyLine);
      int64_t Column = sourcekitd_variant_dictionary_get_int64(Edit, KeyColumn);
      int64_t EndLine = sourcekitd_variant_dictionary_get_int64(Edit, KeyEndLine);
      int64_t EndColumn = sourcekitd_variant_dictionary_get_int64(Edit, KeyEndColumn);
      OS << Line << ':' << Column << '-' << EndLine << ':' << EndColumn << " ";

      StringRef BufferName(
          sourcekitd_variant_dictionary_get_string(Edit, KeyBufferName));
      if (!BufferName.empty()) {
        OS << "(" << BufferName << ") ";
      }

      StringRef Text(sourcekitd_variant_dictionary_get_string(Edit, KeyText));
      OS << "\"" << Text << "\"\n";

      sourcekitd_variant_t NoteRanges =
        sourcekitd_variant_dictionary_get_value(Edit, KeyRangesWorthNote);
      if (unsigned e = sourcekitd_variant_array_get_count(NoteRanges)) {
        for (unsigned i = 0; i != e; ++i) {
          OS << "  <note>";
          sourcekitd_variant_t Note =
            sourcekitd_variant_array_get_value(NoteRanges, i);
          int64_t Line = sourcekitd_variant_dictionary_get_int64(Note, KeyLine);
          int64_t Column = sourcekitd_variant_dictionary_get_int64(Note, KeyColumn);
          int64_t EndLine = sourcekitd_variant_dictionary_get_int64(Note, KeyEndLine);
          int64_t EndColumn = sourcekitd_variant_dictionary_get_int64(Note, KeyEndColumn);
          auto index = sourcekitd_variant_dictionary_get_value(Note, KeyArgIndex);
          sourcekitd_uid_t Kind = sourcekitd_variant_dictionary_get_uid(Note,
                                                              KeyKind);
          StringRef NoteText = getLineColRange(Text, Line, Column, EndLine, EndColumn);
          OS << sourcekitd_uid_get_string_ptr(Kind) << " ";
          OS << Line << ":" << Column << "-" << EndLine << ":" << EndColumn;
          if (sourcekitd_variant_get_type(index) != SOURCEKITD_VARIANT_TYPE_NULL) {
            OS << " arg-index=" << sourcekitd_variant_int64_get_value(index);
          }
          OS << " \"" << NoteText << "\"";
          OS << "</note>\n";
        }
      }
    }
  }
}

static void printRenameRanges(sourcekitd_variant_t Info,
                              llvm::raw_ostream &OS) {
  sourcekitd_variant_t CategorizedRanges =
      sourcekitd_variant_dictionary_get_value(Info, KeyCategorizedRanges);
  sourcekitd_variant_array_apply(CategorizedRanges, ^bool(
                                     size_t i,
                                     sourcekitd_variant_t Categorized) {
    sourcekitd_uid_t Category =
        sourcekitd_variant_dictionary_get_uid(Categorized, KeyCategory);
    OS << sourcekitd_uid_get_string_ptr(Category) << ":\n";

    sourcekitd_variant_t Ranges =
        sourcekitd_variant_dictionary_get_value(Categorized, KeyRanges);
    sourcekitd_variant_array_apply(Ranges, ^bool(size_t j,
                                                 sourcekitd_variant_t Range) {

      OS << "  "; // indent
      int64_t Line = sourcekitd_variant_dictionary_get_int64(Range, KeyLine);
      int64_t Column =
          sourcekitd_variant_dictionary_get_int64(Range, KeyColumn);
      int64_t EndLine =
          sourcekitd_variant_dictionary_get_int64(Range, KeyEndLine);
      int64_t EndColumn =
          sourcekitd_variant_dictionary_get_int64(Range, KeyEndColumn);
      OS << Line << ':' << Column << '-' << EndLine << ':' << EndColumn << " ";
      auto Kind = sourcekitd_variant_dictionary_get_uid(Range, KeyKind);
      OS << sourcekitd_uid_get_string_ptr(Kind);
      auto index = sourcekitd_variant_dictionary_get_value(Range, KeyArgIndex);
      if (sourcekitd_variant_get_type(index) != SOURCEKITD_VARIANT_TYPE_NULL) {
        OS << " arg-index=" << sourcekitd_variant_int64_get_value(index);
      }
      OS << "\n";
      return true;
    });
    return true;
  });
}

static void printInterfaceGen(sourcekitd_variant_t Info, bool CheckASCII) {
  const char *text =
      sourcekitd_variant_dictionary_get_string(Info, KeySourceText);

  if (text) {
    llvm::outs() << text << '\n';
  }

  if (CheckASCII) {
    checkTextIsASCII(text);
  }

  sourcekitd_variant_t syntaxmap =
      sourcekitd_variant_dictionary_get_value(Info, KeySyntaxMap);
  printRawVariant(syntaxmap);
  sourcekitd_variant_t annotations =
      sourcekitd_variant_dictionary_get_value(Info, KeyAnnotations);
  printRawVariant(annotations);
  sourcekitd_variant_t structure =
      sourcekitd_variant_dictionary_get_value(Info, KeySubStructure);
  printRawVariant(structure);
}

static void printRelatedIdents(sourcekitd_variant_t Info, StringRef Filename,
                               const llvm::StringMap<TestOptions::VFSFile> &VFSFiles,
                               llvm::raw_ostream &OS) {
  OS << "START RANGES\n";
  sourcekitd_variant_t Res =
      sourcekitd_variant_dictionary_get_value(Info, KeyResults);
  for (unsigned i=0, e = sourcekitd_variant_array_get_count(Res); i != e; ++i) {
    sourcekitd_variant_t Range = sourcekitd_variant_array_get_value(Res, i);
    int64_t Offset = sourcekitd_variant_dictionary_get_int64(Range, KeyOffset);
    int64_t Length = sourcekitd_variant_dictionary_get_int64(Range, KeyLength);
    auto LineCol = resolveToLineCol(Offset, Filename, VFSFiles);
    OS << LineCol.first << ':' << LineCol.second << " - " << Length << '\n';
  }
  OS << "END RANGES\n";
}

static void printActiveRegions(sourcekitd_variant_t Info, StringRef Filename,
                               const llvm::StringMap<TestOptions::VFSFile> &VFSFiles,
                               llvm::raw_ostream &OS) {
  OS << "START IF CONFIGS\n";
  sourcekitd_variant_t Res =
      sourcekitd_variant_dictionary_get_value(Info, KeyResults);
  for (unsigned i=0, e = sourcekitd_variant_array_get_count(Res); i != e; ++i) {
    sourcekitd_variant_t IfConfig = sourcekitd_variant_array_get_value(Res, i);
    int64_t Offset = sourcekitd_variant_dictionary_get_int64(IfConfig, KeyOffset);
    auto LineCol = resolveToLineCol(Offset, Filename, VFSFiles);
    bool IsActive = sourcekitd_variant_dictionary_get_bool(IfConfig, KeyIsActive);
    OS << LineCol.first << ':' << LineCol.second << " - " << (IsActive ? "active" : "inactive") << '\n';
  }
  OS << "END IF CONFIGS\n";
}

static void prepareDemangleRequest(sourcekitd_object_t Req,
                                   const TestOptions &Opts) {
  sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestDemangle);
  if (Opts.SimplifiedDemangling)
    sourcekitd_request_dictionary_set_int64(Req, KeySimplified, 1);
  sourcekitd_object_t arr = sourcekitd_request_array_create(nullptr, 0);
  sourcekitd_request_dictionary_set_value(Req, KeyNames, arr);

  auto addName = [&](StringRef MangledName) {
    sourcekitd_request_array_set_stringbuf(arr, SOURCEKITD_ARRAY_APPEND,
                                        MangledName.data(), MangledName.size());
  };

  if (Opts.Inputs.empty()) {
    auto input = llvm::MemoryBuffer::getSTDIN();
    if (!input) {
      llvm::errs() << input.getError().message() << '\n';
      ::exit(1);
    }
    llvm::StringRef inputContents = input.get()->getBuffer();

    // This doesn't handle Unicode symbols, but maybe that's okay.
    // Also accept the future mangling prefix.
    llvm::Regex maybeSymbol("(_T|_?\\$[Ss])[_a-zA-Z0-9$.]+");
    llvm::SmallVector<llvm::StringRef, 1> matches;
    while (maybeSymbol.match(inputContents, &matches)) {
      addName(matches.front());
      auto offset = matches.front().data() - inputContents.data();
      inputContents = inputContents.substr(offset + matches.front().size());
    }

  } else {
    for (llvm::StringRef name : Opts.Inputs) {
      addName(name);
    }
  }

  sourcekitd_request_release(arr);
}

static void prepareMangleRequest(sourcekitd_object_t Req,
                                 const TestOptions &Opts) {
  sourcekitd_request_dictionary_set_uid(Req, KeyRequest, RequestMangleSimpleClass);
  sourcekitd_object_t arr = sourcekitd_request_array_create(nullptr, 0);
  sourcekitd_request_dictionary_set_value(Req, KeyNames, arr);

  auto addPair = [&](StringRef ModuleName, StringRef ClassName) {
    sourcekitd_object_t pair =
      sourcekitd_request_dictionary_create(nullptr, nullptr, 0);
    sourcekitd_request_dictionary_set_stringbuf(pair, KeyModuleName,
                                          ModuleName.data(), ModuleName.size());
    sourcekitd_request_dictionary_set_stringbuf(pair, KeyName,
                                            ClassName.data(), ClassName.size());
    sourcekitd_request_array_set_value(arr, SOURCEKITD_ARRAY_APPEND, pair);
    sourcekitd_request_release(pair);
  };

  for (StringRef pair : Opts.Inputs) {
    auto Idx = pair.find('.');
    if (Idx == StringRef::npos) {
      errs() << "expected pairs with format '<module>.<class name>'\n";
      ::exit(1);
    }
    StringRef moduleName = pair.substr(0, Idx);
    StringRef className = pair.substr(Idx+1);
    addPair(moduleName, className);
  }

  sourcekitd_request_release(arr);
}

static void printDemangleResults(sourcekitd_variant_t Info, raw_ostream &OS) {
  OS << "START DEMANGLE\n";
  sourcekitd_variant_t results =
    sourcekitd_variant_dictionary_get_value(Info, KeyResults);
  sourcekitd_variant_array_apply(results, ^bool(size_t index, sourcekitd_variant_t value) {
    StringRef name = sourcekitd_variant_dictionary_get_string(value, KeyName);
    if (name.empty())
      OS << "<empty>";
    else
      OS << name;
    OS << '\n';
    return true;
  });
  OS << "END DEMANGLE\n";
}

static void printMangleResults(sourcekitd_variant_t Info, raw_ostream &OS) {
  OS << "START MANGLE\n";
  sourcekitd_variant_t results =
    sourcekitd_variant_dictionary_get_value(Info, KeyResults);
  sourcekitd_variant_array_apply(results, ^bool(size_t index, sourcekitd_variant_t value) {
    StringRef name = sourcekitd_variant_dictionary_get_string(value, KeyName);
    if (name.empty())
      OS << "<empty>";
    else
      OS << name;
    OS << '\n';
    return true;
  });
  OS << "END MANGLE\n";
}

static void printStatistics(sourcekitd_variant_t Info, raw_ostream &OS) {
  sourcekitd_variant_t results =
    sourcekitd_variant_dictionary_get_value(Info, KeyResults);
  sourcekitd_variant_array_apply(results, ^bool(size_t index, sourcekitd_variant_t value) {
    auto uid = sourcekitd_variant_dictionary_get_uid(value, KeyKind);
    auto desc = sourcekitd_variant_dictionary_get_string(value, KeyDescription);
    auto statValue = sourcekitd_variant_dictionary_get_int64(value, KeyValue);
    OS << statValue << "\t" << desc << "\t- " << sourcekitd_uid_get_string_ptr(uid) << "\n";
    return true;
  });
}

static std::string initializeSource(StringRef Input) {
  std::string result;
  {
    llvm::raw_string_ostream OS(result);
    StringRef CheckStr = "CHECK";
    size_t Pos = 0;
    while (true) {
      auto checkPos = Input.find(CheckStr, Pos);
      if (checkPos == StringRef::npos)
        break;
      checkPos = Input.substr(0, checkPos).rfind("//");
      assert(checkPos != StringRef::npos);
      size_t EndLine = Input.find('\n', checkPos);
      assert(EndLine != StringRef::npos);
      ++EndLine;
      OS << Input.slice(Pos, checkPos);
      Pos = EndLine;
    }

    OS << Input.slice(Pos, StringRef::npos);
  }
  return result;
}

static Optional<std::pair<unsigned, unsigned>>
firstPlaceholderRange(StringRef Source, unsigned from) {
  const char *StartPtr = Source.data();
  Source = Source.drop_front(from);

  while (true) {
    size_t Pos = Source.find("<#");
    if (Pos == StringRef::npos)
      break;
    unsigned OffsetStart = Source.data() + Pos - StartPtr;
    Source = Source.substr(Pos+2);
    if (Source.startswith("__skip__") || Source.startswith("T##__skip__"))
      continue;
    Pos = Source.find("#>");
    if (Pos == StringRef::npos)
      break;
    unsigned OffsetEnd = Source.data() + Pos + 2 - StartPtr;
    Source = Source.substr(Pos+2);
    return std::make_pair(OffsetStart, OffsetEnd-OffsetStart);
  }
  return llvm::None;
}

static void expandPlaceholders(llvm::MemoryBuffer *SourceBuf,
                               llvm::raw_ostream &OS) {
  auto syncEdit = [=](unsigned offset, unsigned length, const char *text) {
    auto SourceBufID = SourceBuf->getBufferIdentifier();
    auto req = sourcekitd_request_dictionary_create(nullptr, nullptr, 0);
    sourcekitd_request_dictionary_set_uid(req, KeyRequest,
                                          RequestEditorReplaceText);
    sourcekitd_request_dictionary_set_stringbuf(req, KeyName,
                                                SourceBufID.data(),
                                                SourceBufID.size());
    sourcekitd_request_dictionary_set_int64(req, KeyOffset, offset);
    sourcekitd_request_dictionary_set_int64(req, KeyLength, length);
    sourcekitd_request_dictionary_set_string(req, KeySourceText, text);

    sourcekitd_response_t resp = sourcekitd_send_request_sync(req);
    if (sourcekitd_response_is_error(resp)) {
      sourcekitd_response_description_dump(resp);
      exit(1);
    }
    sourcekitd_request_release(req);
    sourcekitd_response_dispose(resp);
  };

  std::string source = initializeSource(SourceBuf->getBuffer());
  // Sync contents with modified source.
  syncEdit(0, SourceBuf->getBuffer().size(), source.c_str());

  unsigned cursor = 0;

  while (auto Range = firstPlaceholderRange(source, cursor)) {
    unsigned Offset = Range->first;
    unsigned Length = Range->second;
    sourcekitd_object_t Exp = sourcekitd_request_dictionary_create(nullptr,
                                                                   nullptr, 0);
    sourcekitd_request_dictionary_set_uid(Exp, KeyRequest,
                                          RequestEditorExpandPlaceholder);
    auto SourceBufID = SourceBuf->getBufferIdentifier();
    sourcekitd_request_dictionary_set_stringbuf(Exp, KeyName,
                                                SourceBufID.data(),
                                                SourceBufID.size());
    sourcekitd_request_dictionary_set_string(Exp, KeySourceText, "");
    sourcekitd_request_dictionary_set_int64(Exp, KeyOffset, Offset);
    sourcekitd_request_dictionary_set_int64(Exp, KeyLength, Length);

    sourcekitd_response_t Resp = sourcekitd_send_request_sync(Exp);
    if (sourcekitd_response_is_error(Resp)) {
      sourcekitd_response_description_dump(Resp);
      exit(1);
    }
    sourcekitd_request_release(Exp);
    sourcekitd_variant_t Info = sourcekitd_response_get_value(Resp);
    const char *Text = sourcekitd_variant_dictionary_get_string(Info, KeySourceText);
    if (!Text) {
      cursor = Offset + Length;
      sourcekitd_response_dispose(Resp);
      continue;
    }
    unsigned EditOffset = sourcekitd_variant_dictionary_get_int64(Info, KeyOffset);
    unsigned EditLength = sourcekitd_variant_dictionary_get_int64(Info, KeyLength);

    // Apply edit locally.
    source.replace(EditOffset, EditLength, Text);

    // Apply edit on server.
    syncEdit(EditOffset, EditLength, Text);

    // Adjust cursor to after the edit (we do not expand recursively).
    cursor = EditOffset + strlen(Text);
    sourcekitd_response_dispose(Resp);
  }

  OS << source;
}

static std::pair<unsigned, unsigned>
resolveToLineCol(unsigned Offset, StringRef Filename,
                 const llvm::StringMap<TestOptions::VFSFile> &VFSFiles,
                 bool ExitOnError) {
  return resolveToLineCol(Offset,
                          getBufferForFilename(Filename, VFSFiles, ExitOnError),
                          ExitOnError);
}

/// Maps \p Offset to the {Line, Col} position in \p InputBuf. If it could not
/// be resolved and \p ExitOnError is \c true, the process exits with an error
/// message. Otherwise, {0, 0} is returned.
static std::pair<unsigned, unsigned>
resolveToLineCol(unsigned Offset, llvm::MemoryBuffer *InputBuf,
                 bool ExitOnError) {
  if (Offset >= InputBuf->getBufferSize()) {
    if (!ExitOnError)
      return {0, 0};

    llvm::errs() << "offset " << Offset << " for filename '"
        << InputBuf->getBufferIdentifier() << "' is too large\n";
    exit(1);
  }
  return resolveToLineColFromBuf(Offset, InputBuf->getBufferStart());
}

static std::pair<unsigned, unsigned>
resolveToLineColFromBuf(unsigned Offset, const char *Ptr) {
  const char *End = Ptr+Offset;

  unsigned Line = 1;
  const char *LineStart = Ptr;
  for (; Ptr < End; ++Ptr) {
    if (*Ptr == '\n') {
      ++Line;
      LineStart = Ptr+1;
    }
  }
  unsigned Col = Ptr-LineStart + 1;

  return { Line, Col };
}

static unsigned
resolveFromLineCol(unsigned Line, unsigned Col, StringRef Filename,
                   const llvm::StringMap<TestOptions::VFSFile> &VFSFiles) {
  return resolveFromLineCol(Line, Col,
                            getBufferForFilename(Filename, VFSFiles));
}

static unsigned resolveFromLineCol(unsigned Line, unsigned Col,
                                   llvm::MemoryBuffer *InputBuf) {
  if (Line == 0 || Col == 0) {
    llvm::errs() << "wrong pos format, line/col should start from 1\n";
    exit(1);
  }

  const char *Ptr = InputBuf->getBufferStart();
  const char *End = InputBuf->getBufferEnd();
  const char *LineStart = Ptr;
  --Line;
  for (; Line && (Ptr < End); ++Ptr) {
    if (*Ptr == '\n') {
      --Line;
      LineStart = Ptr+1;
    }
  }
  if (Line != 0) {
    llvm::errs() << "wrong pos format, line too large\n";
    exit(1);
  }
  Ptr = LineStart;
  for (; Ptr <= End; ++Ptr) {
    --Col;
    if (Col == 0)
      return Ptr - InputBuf->getBufferStart();
    if (*Ptr == '\n')
      break;
  }

  llvm::errs() << "wrong pos format, column too large\n";
  exit(1);
}

/// Opens \p Filename, first checking \p VFSFiles and then falling back to the
/// filesystem otherwise. If the file could not be opened and \p ExitOnError is
/// true, the process exits with an error message. Otherwise a buffer
/// containing "<missing file>" is returned.
static llvm::MemoryBuffer *
getBufferForFilename(StringRef Filename,
                     const llvm::StringMap<TestOptions::VFSFile> &VFSFiles,
                     bool ExitOnError) {
  llvm::SmallString<128> nativeName;
  llvm::sys::path::native(Filename, nativeName);

  auto VFSFileIt = VFSFiles.find(nativeName);
  auto MappedFilename =
      VFSFileIt == VFSFiles.end() ? Filename : StringRef(VFSFileIt->second.path);

  auto FileBufOrErr = llvm::MemoryBuffer::getFile(MappedFilename);
  std::unique_ptr<llvm::MemoryBuffer> Buffer;
  if (!FileBufOrErr) {
    if (ExitOnError) {
      llvm::errs() << "error opening input file '" << MappedFilename << "' ("
                   << FileBufOrErr.getError().message() << ")\n";
      exit(1);
    }

    Buffer = llvm::MemoryBuffer::getMemBuffer("<missing file>");
  } else {
    Buffer = std::move(FileBufOrErr.get());
  }

  return Buffer.release();
}
