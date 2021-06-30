//===--- Utils.cpp - Misc utilities ---------------------------------------===//
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

#include "swift/IDE/Utils.h"
#include "swift/Basic/Edit.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Platform.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Driver/FrontendUtil.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Parse/Parser.h"
#include "swift/Subsystems.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/ObjectFilePCHContainerOperations.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Rewrite/Core/RewriteBuffer.h"
#include "clang/Serialization/ASTReader.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using namespace swift;
using namespace ide;

static const char *skipStringInCode(const char *p, const char *End);

static const char *skipParenExpression(const char *p, const char *End) {
  const char *e = p;
  if (*e == '(') {
    uint32_t ParenCount = 1;
    bool done = false;
    for (++e; e < End; ++e) {
      switch (*e) {
      case ')':
        done = --ParenCount == 0;
        break;

      case '(':
        ++ParenCount;
        break;

      case '"':
        e = skipStringInCode (e, End);
        break;

      default:
        break;
      }
      // If "done" is true make sure we don't increment "e"
      if (done)
        break;
    }
  }
  if (e >= End)
    return End;
  return e;
}

static const char *skipStringInCode(const char *p, const char *End) {
  const char *e = p;
  if (*e == '"') {
    bool done = false;
    for (++e; e < End; ++e) {
      switch (*e) {
      case '"':
        done = true;
        break;

      case '\\':
        ++e;
        if (e >= End)
          done = true;
        else if (*e == '(')
          e = skipParenExpression (e, End);
        break;

      default:
        break;
      }
      // If "done" is true make sure we don't increment "e"
      if (done)
          break;
    }
  }
  if (e >= End)
    return End;
  return e;
}

SourceCompleteResult
ide::isSourceInputComplete(std::unique_ptr<llvm::MemoryBuffer> MemBuf,
                           SourceFileKind SFKind) {
  SourceManager SM;
  auto BufferID = SM.addNewSourceBuffer(std::move(MemBuf));
  ParserUnit Parse(SM, SFKind, BufferID);
  Parse.parse();
  SourceCompleteResult SCR;
  SCR.IsComplete = !Parse.getParser().isInputIncomplete();

  // Use the same code that was in the REPL code to track the indent level
  // for now. In the future we should get this from the Parser if possible.

  CharSourceRange entireRange = SM.getRangeForBuffer(BufferID);
  StringRef Buffer = SM.extractText(entireRange);
  const char *SourceStart = Buffer.data();
  const char *SourceEnd = Buffer.data() + Buffer.size();
  const char *LineStart = SourceStart;
  const char *LineSourceStart = nullptr;
  uint32_t LineIndent = 0;
  struct IndentInfo {
    StringRef Prefix;
    uint32_t Indent;
    IndentInfo(const char *s, size_t n, uint32_t i) :
      Prefix(s, n),
      Indent(i) {}
  };
  SmallVector<IndentInfo, 4> IndentInfos;
  for (const char *p = SourceStart; p<SourceEnd; ++p) {
    switch (*p) {
    case '\r':
    case '\n':
      LineIndent = 0;
      LineSourceStart = nullptr;
      LineStart = p + 1;
      break;

    case '"':
      p = skipStringInCode (p, SourceEnd);
      break;

    case '{':
    case '(':
    case '[':
      ++LineIndent;
      if (LineSourceStart == nullptr)
        IndentInfos.push_back(IndentInfo(LineStart,
                                         p - LineStart,
                                         LineIndent));
      else
        IndentInfos.push_back(IndentInfo(LineStart,
                                         LineSourceStart - LineStart,
                                         LineIndent));
      break;

    case '}':
    case ')':
    case ']':
      if (LineIndent > 0)
        --LineIndent;
      if (!IndentInfos.empty())
        IndentInfos.pop_back();
      break;

    default:
      if (LineSourceStart == nullptr && !isspace(*p))
        LineSourceStart = p;
      break;
    }
    if (*p == '\0')
      break;
  }
  if (!IndentInfos.empty()) {
    SCR.IndentPrefix = IndentInfos.back().Prefix.str();
    // Trim off anything that follows a non-space character
    const size_t pos = SCR.IndentPrefix.find_first_not_of(" \t");
    if (pos != std::string::npos)
        SCR.IndentPrefix.erase(pos);
    SCR.IndentLevel = IndentInfos.back().Indent;
  }
  return SCR;
}

SourceCompleteResult
ide::isSourceInputComplete(StringRef Text,SourceFileKind SFKind) {
  return ide::isSourceInputComplete(llvm::MemoryBuffer::getMemBufferCopy(Text),
                                    SFKind);
}

static FrontendInputsAndOutputs resolveSymbolicLinksInInputs(
    FrontendInputsAndOutputs &inputsAndOutputs, StringRef UnresolvedPrimaryFile,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    std::string &Error) {
  assert(FileSystem);

  llvm::SmallString<128> PrimaryFile;
  if (auto err = FileSystem->getRealPath(UnresolvedPrimaryFile, PrimaryFile))
    PrimaryFile = UnresolvedPrimaryFile;

  unsigned primaryCount = 0;
  // FIXME: The frontend should be dealing with symlinks, maybe similar to
  // clang's FileManager ?
  FrontendInputsAndOutputs replacementInputsAndOutputs;
  for (const InputFile &input : inputsAndOutputs.getAllInputs()) {
    llvm::SmallString<128> newFilename;
    if (auto err = FileSystem->getRealPath(input.getFileName(), newFilename))
      newFilename = input.getFileName();
    llvm::sys::path::native(newFilename);
    bool newIsPrimary = input.isPrimary() ||
                        (!PrimaryFile.empty() && PrimaryFile == newFilename);
    if (newIsPrimary) {
      ++primaryCount;
    }
    assert(primaryCount < 2 && "cannot handle multiple primaries");

    replacementInputsAndOutputs.addInput(
        InputFile(newFilename.str(), newIsPrimary, input.getBuffer()));
  }

  if (PrimaryFile.empty() || primaryCount == 1) {
    return replacementInputsAndOutputs;
  }

  llvm::SmallString<64> Err;
  llvm::raw_svector_ostream OS(Err);
  OS << "'" << PrimaryFile << "' is not part of the input files";
  Error = std::string(OS.str());
  return replacementInputsAndOutputs;
}

static void disableExpensiveSILOptions(SILOptions &Opts) {
  // Disable the sanitizers.
  Opts.Sanitizers = {};

  // Disable PGO and code coverage.
  Opts.GenerateProfile = false;
  Opts.EmitProfileCoverageMapping = false;
  Opts.UseProfile = "";
}

namespace {
class StreamDiagConsumer : public DiagnosticConsumer {
  llvm::raw_ostream &OS;

public:
  StreamDiagConsumer(llvm::raw_ostream &OS) : OS(OS) {}

  void handleDiagnostic(SourceManager &SM,
                        const DiagnosticInfo &Info) override {
    // FIXME: Print location info if available.
    switch (Info.Kind) {
    case DiagnosticKind::Error:
      OS << "error: ";
      break;
    case DiagnosticKind::Warning:
      OS << "warning: ";
      break;
    case DiagnosticKind::Note:
      OS << "note: ";
      break;
    case DiagnosticKind::Remark:
      OS << "remark: ";
      break;
    }
    DiagnosticEngine::formatDiagnosticText(OS, Info.FormatString,
                                           Info.FormatArgs);
  }
};
} // end anonymous namespace

bool ide::initCompilerInvocation(
    CompilerInvocation &Invocation, ArrayRef<const char *> OrigArgs,
    DiagnosticEngine &Diags, StringRef UnresolvedPrimaryFile,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    const std::string &runtimeResourcePath,
    const std::string &diagnosticDocumentationPath, time_t sessionTimestamp,
    std::string &Error) {
  SmallVector<const char *, 16> Args;
  // Make sure to put '-resource-dir' and '-diagnostic-documentation-path' at
  // the top to allow overriding them with the passed in arguments.
  Args.push_back("-resource-dir");
  Args.push_back(runtimeResourcePath.c_str());
  Args.push_back("-Xfrontend");
  Args.push_back("-diagnostic-documentation-path");
  Args.push_back("-Xfrontend");
  Args.push_back(diagnosticDocumentationPath.c_str());
  Args.append(OrigArgs.begin(), OrigArgs.end());

  SmallString<32> ErrStr;
  llvm::raw_svector_ostream ErrOS(ErrStr);
  StreamDiagConsumer DiagConsumer(ErrOS);
  Diags.addConsumer(DiagConsumer);

  bool InvocationCreationFailed =
      driver::getSingleFrontendInvocationFromDriverArguments(
          Args, Diags,
          [&](ArrayRef<const char *> FrontendArgs) {
            return Invocation.parseArgs(FrontendArgs, Diags);
          },
          /*ForceNoOutputs=*/true);

  // Remove the StreamDiagConsumer as it's no longer needed.
  Diags.removeConsumer(DiagConsumer);

  Error = std::string(ErrOS.str());
  if (InvocationCreationFailed) {
    return true;
  }

  std::string SymlinkResolveError;
  Invocation.getFrontendOptions().InputsAndOutputs =
      resolveSymbolicLinksInInputs(
          Invocation.getFrontendOptions().InputsAndOutputs,
          UnresolvedPrimaryFile, FileSystem, SymlinkResolveError);

  // SourceKit functionalities want to proceed even if there are missing inputs.
  Invocation.getFrontendOptions().InputsAndOutputs
      .setShouldRecoverMissingInputs();

  if (!SymlinkResolveError.empty()) {
    // resolveSymbolicLinksInInputs fails if the unresolved primary file is not
    // in the input files. We can't recover from that.
    Error += SymlinkResolveError;
    return true;
  }

  ClangImporterOptions &ImporterOpts = Invocation.getClangImporterOptions();
  ImporterOpts.DetailedPreprocessingRecord = true;

  assert(!Invocation.getModuleName().empty());

  auto &LangOpts = Invocation.getLangOptions();
  LangOpts.AttachCommentsToDecls = true;
  LangOpts.DiagnosticsEditorMode = true;
  LangOpts.CollectParsedToken = true;
  if (LangOpts.PlaygroundTransform) {
    // The playground instrumenter changes the AST in ways that disrupt the
    // SourceKit functionality. Since we don't need the instrumenter, and all we
    // actually need is the playground semantics visible to the user, like
    // silencing the "expression resolves to an unused l-value" error, disable it.
    LangOpts.PlaygroundTransform = false;
  }

  // Disable the index-store functionality for the sourcekitd requests.
  auto &FrontendOpts = Invocation.getFrontendOptions();
  FrontendOpts.IndexStorePath.clear();
  ImporterOpts.IndexStorePath.clear();

  // Force the action type to be -typecheck. This affects importing the
  // SwiftONoneSupport module.
  FrontendOpts.RequestedAction = FrontendOptions::ActionType::Typecheck;

  // We don't care about LLVMArgs
  FrontendOpts.LLVMArgs.clear();

  // To save the time for module validation, consider the lifetime of ASTManager
  // as a single build session.
  // NOTE: Do this only if '-disable-modules-validate-system-headers' is *not*
  //       explicitly enabled.
  auto &SearchPathOpts = Invocation.getSearchPathOptions();
  if (!SearchPathOpts.DisableModulesValidateSystemDependencies) {
    // NOTE: 'SessionTimestamp - 1' because clang compares it with '<=' that may
    //       cause unnecessary validations if they happens within one second
    //       from the SourceKit startup.
    ImporterOpts.ExtraArgs.push_back("-fbuild-session-timestamp=" +
                                     std::to_string(sessionTimestamp - 1));
    ImporterOpts.ExtraArgs.push_back(
        "-fmodules-validate-once-per-build-session");

    SearchPathOpts.DisableModulesValidateSystemDependencies = true;
  }

  // Disable expensive SIL options to reduce time spent in SILGen.
  disableExpensiveSILOptions(Invocation.getSILOptions());

  return false;
}

// Adjust the cc1 triple string we got from clang, to make sure it will be
// accepted when it goes through the swift clang importer.
static std::string adjustClangTriple(StringRef TripleStr) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);

  llvm::Triple Triple(TripleStr);
  switch (Triple.getSubArch()) {
  case llvm::Triple::SubArchType::ARMSubArch_v7:
    OS << "armv7"; break;
  case llvm::Triple::SubArchType::ARMSubArch_v7s:
    OS << "armv7s"; break;
  case llvm::Triple::SubArchType::ARMSubArch_v7k:
    OS << "armv7k"; break;
  case llvm::Triple::SubArchType::ARMSubArch_v6:
    OS << "armv6"; break;
  case llvm::Triple::SubArchType::ARMSubArch_v6m:
    OS << "armv6m"; break;
  case llvm::Triple::SubArchType::ARMSubArch_v6k:
    OS << "armv6k"; break;
  case llvm::Triple::SubArchType::ARMSubArch_v6t2:
    OS << "armv6t2"; break;
  default:
    // Adjust i386-macosx to x86_64 because there is no Swift stdlib for i386.
    if ((Triple.getOS() == llvm::Triple::MacOSX ||
      Triple.getOS() == llvm::Triple::Darwin) && Triple.getArch() == llvm::Triple::x86) {
      OS << "x86_64";
    } else {
      OS << Triple.getArchName();
    }
    break;
  }
  OS << '-' << Triple.getVendorName() << '-' <<
      Triple.getOSAndEnvironmentName();
  OS.flush();
  return Result;
}

bool ide::initInvocationByClangArguments(ArrayRef<const char *> ArgList,
                                         CompilerInvocation &Invok,
                                         std::string &Error) {
  llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> DiagOpts{
    new clang::DiagnosticOptions()
  };

  clang::TextDiagnosticBuffer DiagBuf;
  llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> ClangDiags =
    clang::CompilerInstance::createDiagnostics(DiagOpts.get(),
                                               &DiagBuf,
                                               /*ShouldOwnClient=*/false);

  // Clang expects this to be like an actual command line. So we need to pass in
  // "clang" for argv[0].
  std::vector<const char *> ClangArgList;
  ClangArgList.push_back("clang");
  ClangArgList.insert(ClangArgList.end(), ArgList.begin(), ArgList.end());

  // Create a new Clang compiler invocation.
  std::unique_ptr<clang::CompilerInvocation> ClangInvok =
      clang::createInvocationFromCommandLine(ClangArgList, ClangDiags);
  if (!ClangInvok || ClangDiags->hasErrorOccurred()) {
    for (auto I = DiagBuf.err_begin(), E = DiagBuf.err_end(); I != E; ++I) {
      Error += I->second;
      Error += " ";
    }
    return true;
  }

  auto &PPOpts = ClangInvok->getPreprocessorOpts();
  auto &HSOpts = ClangInvok->getHeaderSearchOpts();

  Invok.setTargetTriple(adjustClangTriple(ClangInvok->getTargetOpts().Triple));
  if (!HSOpts.Sysroot.empty())
    Invok.setSDKPath(HSOpts.Sysroot);
  if (!HSOpts.ModuleCachePath.empty())
    Invok.setClangModuleCachePath(HSOpts.ModuleCachePath);

  auto &CCArgs = Invok.getClangImporterOptions().ExtraArgs;
  for (auto MacroEntry : PPOpts.Macros) {
    std::string MacroFlag;
    if (MacroEntry.second)
      MacroFlag += "-U";
    else
      MacroFlag += "-D";
    MacroFlag += MacroEntry.first;
    CCArgs.push_back(MacroFlag);
  }

  for (auto &Entry : HSOpts.UserEntries) {
    switch (Entry.Group) {
      case clang::frontend::Quoted:
        CCArgs.push_back("-iquote");
        CCArgs.push_back(Entry.Path);
        break;
      case clang::frontend::IndexHeaderMap:
        CCArgs.push_back("-index-header-map");
        LLVM_FALLTHROUGH;
      case clang::frontend::Angled: {
        std::string Flag;
        if (Entry.IsFramework)
          Flag += "-F";
        else
          Flag += "-I";
        Flag += Entry.Path;
        CCArgs.push_back(Flag);
        break;
      }
      case clang::frontend::System:
        if (Entry.IsFramework)
          CCArgs.push_back("-iframework");
        else
          CCArgs.push_back("-isystem");
        CCArgs.push_back(Entry.Path);
        break;
      case clang::frontend::ExternCSystem:
      case clang::frontend::CSystem:
      case clang::frontend::CXXSystem:
      case clang::frontend::ObjCSystem:
      case clang::frontend::ObjCXXSystem:
      case clang::frontend::After:
        break;
    }
  }

  if (!PPOpts.ImplicitPCHInclude.empty()) {
    clang::FileSystemOptions FileSysOpts;
    clang::FileManager FileMgr(FileSysOpts);
    auto PCHContainerOperations =
        std::make_shared<clang::PCHContainerOperations>();
    std::string HeaderFile = clang::ASTReader::getOriginalSourceFile(
        PPOpts.ImplicitPCHInclude, FileMgr,
        PCHContainerOperations->getRawReader(), *ClangDiags);
    if (!HeaderFile.empty()) {
      CCArgs.push_back("-include");
      CCArgs.push_back(std::move(HeaderFile));
    }
  }
  for (auto &Header : PPOpts.Includes) {
    CCArgs.push_back("-include");
    CCArgs.push_back(Header);
  }

  for (auto &Entry : HSOpts.ModulesIgnoreMacros) {
    std::string Flag = "-fmodules-ignore-macro=";
    Flag += Entry;
    CCArgs.push_back(Flag);
  }

  for (auto &Entry : HSOpts.VFSOverlayFiles) {
    CCArgs.push_back("-ivfsoverlay");
    CCArgs.push_back(Entry);
  }

  if (!ClangInvok->getLangOpts()->isCompilingModule()) {
    CCArgs.push_back("-Xclang");
    llvm::SmallString<64> Str;
    Str += "-fmodule-name=";
    Str += ClangInvok->getLangOpts()->CurrentModule;
    CCArgs.push_back(std::string(Str.str()));
  }

  if (PPOpts.DetailedRecord) {
    Invok.getClangImporterOptions().DetailedPreprocessingRecord = true;
  }

  if (!ClangInvok->getFrontendOpts().Inputs.empty()) {
    Invok.getFrontendOptions().ImplicitObjCHeaderPath =
        ClangInvok->getFrontendOpts().Inputs[0].getFile().str();
  }

  return false;
}

template <typename FnTy>
static void walkOverriddenClangDecls(const clang::NamedDecl *D, const FnTy &Fn){
  SmallVector<const clang::NamedDecl *, 8> OverDecls;
  D->getASTContext().getOverriddenMethods(D, OverDecls);
  for (auto Over : OverDecls)
    Fn(Over);
  for (auto Over : OverDecls)
    walkOverriddenClangDecls(Over, Fn);
}

void
ide::walkOverriddenDecls(const ValueDecl *VD,
                         llvm::function_ref<void(llvm::PointerUnion<
                             const ValueDecl*, const clang::NamedDecl*>)> Fn) {
  for (auto CurrOver = VD; CurrOver; CurrOver = CurrOver->getOverriddenDecl()) {
    if (CurrOver != VD)
      Fn(CurrOver);
    if (auto ClangD =
        dyn_cast_or_null<clang::NamedDecl>(CurrOver->getClangDecl())) {
      walkOverriddenClangDecls(ClangD, Fn);
      return;
    }
    for (auto Conf: CurrOver->getSatisfiedProtocolRequirements())
      Fn(Conf);
  }
}

/// \returns true if a placeholder was found.
static bool findPlaceholder(StringRef Input, PlaceholderOccurrence &Occur) {
  while (true) {
    size_t Pos = Input.find("<#");
    if (Pos == StringRef::npos)
      return false;

    const char *Begin = Input.begin() + Pos;
    const char *Ptr = Begin + 2;
    const char *End = Input.end();
    for (; Ptr < End-1; ++Ptr) {
      if (*Ptr == '\n')
        break;
      if (Ptr[0] == '<' && Ptr[1] == '#')
        break;
      if (Ptr[0] == '#' && Ptr[1] == '>') {
        // Found it.
        Occur.FullPlaceholder = Input.substr(Pos, Ptr-Begin + 2);
        Occur.PlaceholderContent =
          Occur.FullPlaceholder.drop_front(2).drop_back(2);
        return true;
      }
    }

    // Try again.
    Input = Input.substr(Ptr - Input.begin());
  }
}

std::unique_ptr<llvm::MemoryBuffer>
ide::replacePlaceholders(std::unique_ptr<llvm::MemoryBuffer> InputBuf,
             llvm::function_ref<void(const PlaceholderOccurrence &)> Callback) {
  StringRef Input = InputBuf->getBuffer();
  PlaceholderOccurrence Occur;
  bool Found = findPlaceholder(Input, Occur);
  if (!Found)
    return InputBuf;

  std::unique_ptr<llvm::MemoryBuffer> NewBuf =
    llvm::MemoryBuffer::getMemBufferCopy(InputBuf->getBuffer(),
                                         InputBuf->getBufferIdentifier());

  unsigned Counter = 0;
  auto replacePlaceholder = [&](PlaceholderOccurrence &Occur) {
    llvm::SmallString<10> Id;
    Id = "$_";
    llvm::raw_svector_ostream(Id) << (Counter++);
    assert(Occur.FullPlaceholder.size() >= 2);
    if (Id.size() > Occur.FullPlaceholder.size()) {
      // The identifier+counter exceeds placeholder size; replace it without
      // using the counter.
      Id = "$";
      Id.append(Occur.FullPlaceholder.size()-1, '_');
    } else {
      Id.append(Occur.FullPlaceholder.size()-Id.size(), '_');
    }
    assert(Id.size() == Occur.FullPlaceholder.size());

    unsigned Offset = Occur.FullPlaceholder.data() - InputBuf->getBufferStart();
    char *Ptr = const_cast<char *>(NewBuf->getBufferStart()) + Offset;
    std::copy(Id.begin(), Id.end(), Ptr);

    Occur.IdentifierReplacement = Id.str();
    Callback(Occur);
  };

  while (true) {
    replacePlaceholder(Occur);
    unsigned Offset = Occur.FullPlaceholder.data() - InputBuf->getBufferStart();
    Input = InputBuf->getBuffer().substr(Offset+Occur.FullPlaceholder.size());

    bool Found = findPlaceholder(Input, Occur);
    if (!Found)
      break;
  }

  return NewBuf;
}

std::unique_ptr<llvm::MemoryBuffer>
ide::replacePlaceholders(std::unique_ptr<llvm::MemoryBuffer> InputBuf,
                         bool *HadPlaceholder) {
  if (HadPlaceholder)
    *HadPlaceholder = false;
  return replacePlaceholders(std::move(InputBuf),
                             [&](const PlaceholderOccurrence &){
    if (HadPlaceholder)
      *HadPlaceholder = true;
  });
}

// Modules failing to load are commented-out.
static const char *OSXModuleList[] = {
  "AGL",
  "AVFoundation",
  "AVKit",
  "Accelerate",
  "Accounts",
  "AddressBook",
  "AppKit",
  "AppKitScripting",
  "AppleScriptKit",
  "AppleScriptObjC",
  "ApplicationServices",
  "AudioToolbox",
  "AudioUnit",
  "AudioVideoBridging",
  "Automator",
  "CFNetwork",
  // "CalendarStore",
  "Carbon",
  "CloudKit",
  "Cocoa",
  "Collaboration",
  "Contacts",
  "CoreAudio",
  "CoreAudioKit",
  "CoreBluetooth",
  "CoreData",
  "CoreFoundation",
  "CoreGraphics",
  "CoreImage",
  "CoreLocation",
  "CoreMIDI",
  //  "CoreMIDIServer",
  "CoreMedia",
  "CoreMediaIO",
  "CoreServices",
  "CoreTelephony",
  "CoreText",
  "CoreVideo",
  "CoreWLAN",
  //  "CryptoTokenKit",
  //  "DVComponentGlue",
  "DVDPlayback",
  "Darwin",
  "DirectoryService",
  "DiscRecording",
  "DiscRecordingUI",
  "DiskArbitration",
  "Dispatch",
  //  "DrawSprocket",
  "EventKit",
  "ExceptionHandling",
  "FWAUserLib",
  "FinderSync",
  "ForceFeedback",
  "Foundation",
  "GLKit",
  "GLUT",
  "GSS",
  "GameController",
  "GameKit",
  "GameplayKit",
  "Hypervisor",
  //  "ICADevices",
  "IMServicePlugIn",
  "IOBluetooth",
  "IOBluetoothUI",
  "IOKit",
  "IOSurface",
  "ImageCaptureCore",
  "ImageIO",
  "InputMethodKit",
  // "InstallerPlugins",
  // "InstantMessage",
  // "JavaFrameEmbedding",
  "JavaScriptCore",
  // "JavaVM",
  // "Kerberos",
  // "LDAP",
  "LatentSemanticMapping",
  "LocalAuthentication",
  "MachO",
  "MapKit",
  "MediaAccessibility",
  "MediaLibrary",
  "MediaToolbox",
  //  "Message",
  "Metal",
  "MetalKit",
  "ModelIO",
  "MultipeerConnectivity",
  "NetFS",
  //  "NetworkExtension",
  "NotificationCenter",
  "OSAKit",
  "ObjectiveC",
  "OpenAL",
  "OpenCL",
  "OpenDirectory",
  "OpenGL",
  // "PCSC",
  "PreferencePanes",
  "PubSub",
  "Python",
  //  "QTKit", QTKit is unavailable on Swift.
  "Quartz",
  "QuartzCore",
  "QuickLook",
  "QuickTime",
  //  "Ruby",
  "SceneKit",
  "ScreenSaver",
  "Scripting",
  "ScriptingBridge",
  "Security",
  "SecurityFoundation",
  "SecurityInterface",
  // "ServiceManagement",
  "Social",
  "SpriteKit",
  "StoreKit",
  // "SyncServices",
  "SystemConfiguration",
  "TWAIN",
  "Tcl",
  // "VideoDecodeAcceleration",
  "VideoToolbox",
  "WebKit",
  "XPC",
  "libkern",
  "os",
  //  "vecLib",
  "vmnet",
};

// Modules failing to load are commented-out.
static const char *iOSModuleList[] = {
  "AVFoundation",
  "AVKit",
  "Accelerate",
  "Accounts",
  "AdSupport",
  "AddressBook",
  "AddressBookUI",
  "AssetsLibrary",
  "AudioToolbox",
  "AudioUnit",
  "CFNetwork",
  "CloudKit",
  "Contacts",
  "ContactsUI",
  "CoreAudio",
  "CoreAudioKit",
  "CoreBluetooth",
  "CoreData",
  "CoreFoundation",
  "CoreGraphics",
  "CoreImage",
  "CoreLocation",
  "CoreMIDI",
  "CoreMedia",
  "CoreMotion",
  "CoreSpotlight",
  "CoreTelephony",
  "CoreText",
  "CoreVideo",
  "Darwin",
  "Dispatch",
  "EventKit",
  "EventKitUI",
  "ExternalAccessory",
  "Foundation",
  "GLKit",
  "GSS",
  "GameController",
  "GameFoundation",
  "GameKit",
  "GameplayKit",
  "HealthKit",
  "HomeKit",
  "IMCommonCore",
  // "IOKit",
  "ImageIO",
  "JavaScriptCore",
  "LocalAuthentication",
  "MachO",
  "MapKit",
  "MediaAccessibility",
  "MediaPlayer",
  "MediaToolbox",
  "MessageUI",
  "MobileCoreServices",
  "ModelIO",
  "MultipeerConnectivity",
  "NetworkExtension",
  "NewsstandKit",
  "NotificationCenter",
  "ObjectiveC",
  "OpenAL",
  "OpenGLES",
  "PassKit",
  "Photos",
  "PhotosUI",
  "PushKit",
  "QuartzCore",
  "QuickLook",
  "SafariServices",
  "SceneKit",
  "ScreenRecorder",
  "Security",
  "Social",
  "SpriteKit",
  "StoreKit",
  "SystemConfiguration",
  "Twitter",
  "UIKit",
  "UIKit.UIGestureRecognizerSubclass",
  "VideoToolbox",
  "WatchConnectivity",
  "WatchKit",
  "WebKit",
  "iAd",
  "libkern",
  "os",
};

static const char *DeviceOnlyModuleList[] = {
  "Metal",
  "MetalKit",
  "MetalShaders",
};


static ArrayRef<const char *> getOSXModuleList() {
  return OSXModuleList;
}

static ArrayRef<const char *> getiOSModuleList() {
  return iOSModuleList;
}

static ArrayRef<const char *> getDeviceOnlyModuleList() {
  return DeviceOnlyModuleList;
}

void ide::collectModuleNames(StringRef SDKPath,
                               std::vector<std::string> &Modules) {
  std::string SDKName = getSDKName(SDKPath);
  std::string lowerSDKName = StringRef(SDKName).lower();
  bool isOSXSDK = StringRef(lowerSDKName).contains("macosx");
  bool isDeviceOnly = StringRef(lowerSDKName).contains("iphoneos");
  auto Mods = isOSXSDK ? getOSXModuleList() : getiOSModuleList();
  Modules.insert(Modules.end(), Mods.begin(), Mods.end());
  if (isDeviceOnly) {
    Mods = getDeviceOnlyModuleList();
    Modules.insert(Modules.end(), Mods.begin(), Mods.end());
  }
}

DeclNameViewer::DeclNameViewer(StringRef Text): IsValid(true), HasParen(false) {
  auto ArgStart = Text.find_first_of('(');
  if (ArgStart == StringRef::npos) {
    BaseName = Text;
    return;
  }
  HasParen = true;
  BaseName = Text.substr(0, ArgStart);
  auto ArgEnd = Text.find_last_of(')');
  if (ArgEnd == StringRef::npos) {
    IsValid = false;
    return;
  }
  StringRef AllArgs = Text.substr(ArgStart + 1, ArgEnd - ArgStart - 1);
  AllArgs.split(Labels, ":");
  if (Labels.empty())
    return;
  if ((IsValid = Labels.back().empty())) {
    Labels.pop_back();
    llvm::transform(Labels, Labels.begin(), [](StringRef Label) {
      return Label == "_" ? StringRef() : Label;
    });
  }
}

unsigned DeclNameViewer::commonPartsCount(DeclNameViewer &Other) const {
  if (base() != Other.base())
    return 0;
  unsigned Result = 1;
  unsigned Len = std::min(args().size(), Other.args().size());
  for (unsigned I = 0; I < Len; ++ I) {
    if (args()[I] == Other.args()[I])
      ++Result;
    else
      return Result;
  }
  return Result;
}

void swift::ide::SourceEditConsumer::
accept(SourceManager &SM, SourceLoc Loc, StringRef Text,
       ArrayRef<NoteRegion> SubRegions) {
  accept(SM, CharSourceRange(Loc, 0), Text, SubRegions);
}

void swift::ide::SourceEditConsumer::
accept(SourceManager &SM, CharSourceRange Range, StringRef Text,
       ArrayRef<NoteRegion> SubRegions) {
  accept(SM, RegionType::ActiveCode, {{Range, Text, SubRegions}});
}

void swift::ide::SourceEditConsumer::
insertAfter(SourceManager &SM, SourceLoc Loc, StringRef Text,
            ArrayRef<NoteRegion> SubRegions) {
  accept(SM, Lexer::getLocForEndOfToken(SM, Loc), Text, SubRegions);
}

void swift::ide::SourceEditConsumer::
remove(SourceManager &SM, CharSourceRange Range) {
  accept(SM, Range, "");
}

struct swift::ide::SourceEditJsonConsumer::Implementation {
  llvm::raw_ostream &OS;
  std::vector<SingleEdit> AllEdits;
  Implementation(llvm::raw_ostream &OS) : OS(OS) {}
  ~Implementation() {
    writeEditsInJson(AllEdits, OS);
  }
  void accept(SourceManager &SM, CharSourceRange Range,
              llvm::StringRef Text) {
    AllEdits.push_back({SM, Range, Text.str()});
  }
};

swift::ide::SourceEditJsonConsumer::SourceEditJsonConsumer(llvm::raw_ostream &OS) :
  Impl(*new Implementation(OS)) {}

swift::ide::SourceEditJsonConsumer::~SourceEditJsonConsumer() { delete &Impl; }

void swift::ide::SourceEditJsonConsumer::
accept(SourceManager &SM, RegionType Type, ArrayRef<Replacement> Replacements) {
  for (const auto &Replacement: Replacements) {
    Impl.accept(SM, Replacement.Range, Replacement.Text);
  }
}

void swift::ide::SourceEditTextConsumer::
accept(SourceManager &SM, RegionType Type, ArrayRef<Replacement> Replacements) {
  for (const auto &Replacement: Replacements) {
    CharSourceRange Range = Replacement.Range;
    unsigned BufID = SM.findBufferContainingLoc(Range.getStart());
    auto Path(SM.getIdentifierForBuffer(BufID));
    auto Start = SM.getLineAndColumnInBuffer(Range.getStart());
    auto End = SM.getLineAndColumnInBuffer(Range.getEnd());

    OS << "// " << Path.str() << " ";
    OS << Start.first << ":" << Start.second << " -> ";
    OS << End.first << ":" << End.second << "\n";
    OS << Replacement.Text << "\n";
  }
}

namespace {
class ClangFileRewriterHelper {
  unsigned InterestedId;
  clang::RewriteBuffer RewriteBuf;
  bool HasChange;
  llvm::raw_ostream &OS;

  void removeCommentLines(clang::RewriteBuffer &Buffer, StringRef Input,
                          StringRef LineHeader) {
    size_t Pos = 0;
    while (true) {
      Pos = Input.find(LineHeader, Pos);
      if (Pos == StringRef::npos)
        break;
      Pos = Input.substr(0, Pos).rfind("//");
      assert(Pos != StringRef::npos);
      size_t EndLine = Input.find('\n', Pos);
      assert(EndLine != StringRef::npos);
      ++EndLine;
      Buffer.RemoveText(Pos, EndLine-Pos);
      Pos = EndLine;
    }
  }

public:
  ClangFileRewriterHelper(SourceManager &SM, unsigned InterestedId,
                          llvm::raw_ostream &OS)
  : InterestedId(InterestedId), HasChange(false), OS(OS) {
    StringRef Input(SM.getLLVMSourceMgr().getMemoryBuffer(InterestedId)->
                    getBuffer());
    RewriteBuf.Initialize(Input);
    removeCommentLines(RewriteBuf, Input, "RUN");
    removeCommentLines(RewriteBuf, Input, "CHECK");
  }

  void replaceText(SourceManager &SM, CharSourceRange Range, StringRef Text) {
    auto BufferId = SM.findBufferContainingLoc(Range.getStart());
    if (BufferId == InterestedId) {
      HasChange = true;
      auto StartLoc = SM.getLocOffsetInBuffer(Range.getStart(), BufferId);
      if (!Range.getByteLength())
          RewriteBuf.InsertText(StartLoc, Text);
      else
          RewriteBuf.ReplaceText(StartLoc, Range.str().size(), Text);
    }
  }

  ~ClangFileRewriterHelper() {
    if (HasChange)
      RewriteBuf.write(OS);
  }
};
} // end anonymous namespace
struct swift::ide::SourceEditOutputConsumer::Implementation {
  ClangFileRewriterHelper Rewriter;
  Implementation(SourceManager &SM, unsigned BufferId, llvm::raw_ostream &OS)
  : Rewriter(SM, BufferId, OS) {}
  void accept(SourceManager &SM, CharSourceRange Range, StringRef Text) {
    Rewriter.replaceText(SM, Range, Text);
  }
};

swift::ide::SourceEditOutputConsumer::
SourceEditOutputConsumer(SourceManager &SM, unsigned BufferId,
  llvm::raw_ostream &OS) : Impl(*new Implementation(SM, BufferId, OS)) {}

swift::ide::SourceEditOutputConsumer::~SourceEditOutputConsumer() { delete &Impl; }

void swift::ide::SourceEditOutputConsumer::
accept(SourceManager &SM, RegionType RegionType,
       ArrayRef<Replacement> Replacements) {
  // ignore mismatched or
  if (RegionType == RegionType::Unmatched || RegionType == RegionType::Mismatch)
    return;

  for (const auto &Replacement : Replacements) {
    Impl.accept(SM, Replacement.Range, Replacement.Text);
  }
}

void swift::ide::BroadcastingSourceEditConsumer::accept(
    SourceManager &SM, RegionType RegionType,
    ArrayRef<Replacement> Replacements) {
  for (auto &Consumer : Consumers) {
    Consumer->accept(SM, RegionType, Replacements);
  }
}

bool swift::ide::isFromClang(const Decl *D) {
  if (getEffectiveClangNode(D))
    return true;
  if (auto *Ext = dyn_cast<ExtensionDecl>(D))
    return static_cast<bool>(extensionGetClangNode(Ext));
  return false;
}

ClangNode swift::ide::getEffectiveClangNode(const Decl *decl) {
  auto &ctx = decl->getASTContext();
  auto *importer = static_cast<ClangImporter *>(ctx.getClangModuleLoader());
  return importer->getEffectiveClangNode(decl);
}

/// Retrieve the Clang node for the given extension, if it has one.
ClangNode swift::ide::extensionGetClangNode(const ExtensionDecl *ext) {
  // If it has a Clang node (directly),
  if (ext->hasClangNode()) return ext->getClangNode();

  // Check whether it was syntheszed into a module-scope context.
  if (!isa<ClangModuleUnit>(ext->getModuleScopeContext()))
    return ClangNode();

  // It may have a global imported as a member.
  for (auto member : ext->getMembers()) {
    if (auto clangNode = getEffectiveClangNode(member))
      return clangNode;
  }

  return ClangNode();
}

std::pair<Type, ConcreteDeclRef> swift::ide::getReferencedDecl(Expr *expr) {
  auto exprTy = expr->getType();

  // Look through unbound instance member accesses.
  if (auto *dotSyntaxExpr = dyn_cast<DotSyntaxBaseIgnoredExpr>(expr))
    expr = dotSyntaxExpr->getRHS();

  // Look through the 'self' application.
  if (auto *selfApplyExpr = dyn_cast<SelfApplyExpr>(expr))
    expr = selfApplyExpr->getFn();

  // Look through curry thunks.
  if (auto *closure = dyn_cast<AutoClosureExpr>(expr))
    if (auto *unwrappedThunk = closure->getUnwrappedCurryThunkExpr())
      expr = unwrappedThunk;

  // If this is an IUO result, unwrap the optional type.
  auto refDecl = expr->getReferencedDecl();
  if (!refDecl) {
    if (auto *applyExpr = dyn_cast<ApplyExpr>(expr)) {
      auto fnDecl = applyExpr->getFn()->getReferencedDecl();
      if (auto *func = fnDecl.getDecl()) {
        if (func->isImplicitlyUnwrappedOptional()) {
          if (auto objectTy = exprTy->getOptionalObjectType())
            exprTy = objectTy;
        }
      }
    }
  }

  return std::make_pair(exprTy, refDecl);
}

bool swift::ide::isBeingCalled(ArrayRef<Expr *> ExprStack) {
  if (ExprStack.empty())
    return false;

  Expr *Target = ExprStack.back();
  auto UnderlyingDecl = getReferencedDecl(Target).second;
  for (Expr *E: reverse(ExprStack)) {
    auto *AE = dyn_cast<ApplyExpr>(E);
    if (!AE || AE->isImplicit())
      continue;
    if (auto *CRCE = dyn_cast<ConstructorRefCallExpr>(AE)) {
      if (CRCE->getBase() == Target)
        return true;
    }
    if (isa<SelfApplyExpr>(AE))
      continue;
    if (getReferencedDecl(AE->getFn()).second == UnderlyingDecl)
      return true;
  }
  return false;
}

static Expr *getContainingExpr(ArrayRef<Expr *> ExprStack, size_t index) {
  if (ExprStack.size() > index)
    return ExprStack.end()[-std::ptrdiff_t(index + 1)];
  return nullptr;
}

Expr *swift::ide::getBase(ArrayRef<Expr *> ExprStack) {
  if (ExprStack.empty())
    return nullptr;

  Expr *CurrentE = ExprStack.back();
  Expr *ParentE = getContainingExpr(ExprStack, 1);
  Expr *Base = nullptr;
  if (auto DSE = dyn_cast_or_null<DotSyntaxCallExpr>(ParentE))
    Base = DSE->getBase();
  else if (auto MRE = dyn_cast<MemberRefExpr>(CurrentE))
    Base = MRE->getBase();
  else if (auto SE = dyn_cast<SubscriptExpr>(CurrentE))
    Base = SE->getBase();

  if (Base) {
    while (auto ICE = dyn_cast<ImplicitConversionExpr>(Base))
      Base = ICE->getSubExpr();
    // DotSyntaxCallExpr with getBase() == CurrentE (ie. the current call is
    // the base of another expression)
    if (Base == CurrentE)
      return nullptr;
  }
  return Base;
}

bool swift::ide::isDynamicCall(Expr *Base, ValueDecl *D) {
  auto TyD = D->getDeclContext()->getSelfNominalTypeDecl();
  if (!TyD)
    return false;

  if (isa<StructDecl>(TyD) || isa<EnumDecl>(TyD) || D->isFinal())
    return false;

  // super.method()
  // TODO: Should be dynamic if `D` is marked as dynamic and @objc, but in
  //       that case we really need to change the role the index outputs as
  //       well - the overrides we'd want to include are from the type of
  //       super up to `D`
  if (Base->isSuperExpr())
    return false;

  // `SomeType.staticOrClassMethod()`
  if (isa<TypeExpr>(Base))
    return false;

  // `type(of: foo).staticOrClassMethod()`, not "dynamic" if the instance type
  // is a struct/enum or if it is a class and the function is a static method
  // (rather than a class method).
  if (auto IT = Base->getType()->getAs<MetatypeType>()) {
    auto InstanceType = IT->getInstanceType();
    if (InstanceType->getStructOrBoundGenericStruct() ||
        InstanceType->getEnumOrBoundGenericEnum())
      return false;
    if (InstanceType->getClassOrBoundGenericClass() && D->isFinal())
      return false;
  }

  return true;
}

void swift::ide::getReceiverType(Expr *Base,
                                 SmallVectorImpl<NominalTypeDecl *> &Types) {
  Type ReceiverTy = Base->getType();
  if (!ReceiverTy)
    return;

  if (auto LVT = ReceiverTy->getAs<LValueType>())
    ReceiverTy = LVT->getObjectType();
  else if (auto MetaT = ReceiverTy->getAs<MetatypeType>())
    ReceiverTy = MetaT->getInstanceType();
  else if (auto SelfT = ReceiverTy->getAs<DynamicSelfType>())
    ReceiverTy = SelfT->getSelfType();

  // TODO: Handle generics and composed protocols
  if (auto OpenedTy = ReceiverTy->getAs<OpenedArchetypeType>())
    ReceiverTy = OpenedTy->getOpenedExistentialType();

  if (auto TyD = ReceiverTy->getAnyNominal()) {
    Types.push_back(TyD);
  }
}
