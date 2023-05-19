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
#include "swift/Basic/Platform.h"
#include "swift/Basic/SourceManager.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Parse/Parser.h"
#include "swift/Subsystems.h"
#include "clang/AST/ASTContext.h"
#include "clang/Rewrite/Core/RewriteBuffer.h"
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
  accept(SM, RegionType::ActiveCode,
         {{/*Path=*/{}, Range, /*BufferName=*/{}, Text, SubRegions}});
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
    OS << "// ";
    StringRef Path = Replacement.Path;
    if (Path.empty()) {
      unsigned BufID = SM.findBufferContainingLoc(Replacement.Range.getStart());
      Path = SM.getIdentifierForBuffer(BufID);
    } else {
      OS << "explicit ";
    }
    OS << Path.str() << " ";

    auto Start = SM.getLineAndColumnInBuffer(Replacement.Range.getStart());
    auto End = SM.getLineAndColumnInBuffer(Replacement.Range.getEnd());
    OS << Start.first << ":" << Start.second << " -> ";
    OS << End.first << ":" << End.second;

    if (Replacement.BufferName.empty()) {
      OS << " (" << Replacement.BufferName << ")\n";
    } else {
      OS << "\n";
    }

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

std::pair<Type, ConcreteDeclRef> swift::ide::getReferencedDecl(Expr *expr,
                                                               bool semantic) {
  if (semantic)
    expr = expr->getSemanticsProvidingExpr();

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
    auto *LE = dyn_cast<LiteralExpr>(E);
    if (LE && getReferencedDecl(LE).second == UnderlyingDecl)
      return true;
    auto *CE = dyn_cast<CollectionExpr>(E);
    if (CE && getReferencedDecl(CE).second == UnderlyingDecl)
      return true;

    auto *AE = dyn_cast<ApplyExpr>(E);
    if (!AE || AE->isImplicit())
      continue;
    if (auto *CRCE = dyn_cast<ConstructorRefCallExpr>(AE->getFn())) {
      auto *Base = CRCE->getBase();
      while (auto *ICE = dyn_cast<ImplicitConversionExpr>(Base))
        Base = ICE->getSubExpr();
      if (Base == Target)
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
  if (ParentE && isa<FunctionConversionExpr>(ParentE)) {
    ParentE = getContainingExpr(ExprStack, 2);
  }
  Expr *Base = nullptr;

  if (auto DSE = dyn_cast_or_null<DotSyntaxCallExpr>(ParentE))
    Base = DSE->getBase();
  else if (auto MRE = dyn_cast<MemberRefExpr>(CurrentE))
    Base = MRE->getBase();
  else if (auto SE = dyn_cast<SubscriptExpr>(CurrentE))
    Base = SE->getBase();

  // Look through curry thunks
  if (auto ACE = dyn_cast_or_null<AutoClosureExpr>(Base))
    if (auto *Unwrapped = ACE->getUnwrappedCurryThunkExpr())
      Base = Unwrapped;

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

bool swift::ide::isDeclOverridable(ValueDecl *D) {
  auto *NTD = D->getDeclContext()->getSelfNominalTypeDecl();
  if (!NTD)
    return false;

  // Only classes and protocols support overridding by subtypes.
  if (!(isa<ClassDecl>(NTD) || isa<ProtocolDecl>(NTD)))
    return false;

  // Decls where either they themselves are final or their containing type is
  // final cannot be overridden. Actors cannot be subclassed and thus the given
  // decl also can't be overridden.
  if (D->isFinal() || NTD->isFinal() || NTD->isActor())
    return false;

  // No need to check accessors here - willSet/didSet are not "overridable",
  // but that's already covered by the `isFinal` check above (they are both
  // final).

  // Static functions on classes cannot be overridden. Static functions on
  // structs and enums are already covered by the more general check above.
  if (isa<ClassDecl>(NTD)) {
    if (auto *FD = dyn_cast<FuncDecl>(D)) {
      if (FD->isStatic() &&
          FD->getCorrectStaticSpelling() == StaticSpellingKind::KeywordStatic)
        return false;
    } else if (auto *ASD = dyn_cast<AbstractStorageDecl>(D)) {
      if (ASD->isStatic() &&
          ASD->getCorrectStaticSpelling() == StaticSpellingKind::KeywordStatic)
        return false;
    }
  }

  return true;
}

bool swift::ide::isDynamicRef(Expr *Base, ValueDecl *D, llvm::function_ref<Type(Expr *)> getType) {
  if (!isDeclOverridable(D))
    return false;

  Base = Base->getSemanticsProvidingExpr();

  // super.method()
  // TODO: Should be dynamic if `D` is marked as dynamic and @objc, but in
  //       that case we really need to change the role the index outputs as
  //       well - the overrides we'd want to include are from the type of
  //       super up to `D`
  if (Base->isSuperExpr())
    return false;

  // `SomeType.staticOrClassMethod()` spelled directly, so this must be a ref
  // to this exact decl.
  if (isa<TypeExpr>(Base))
    return false;

  // `type(of: foo).staticOrClassMethod()`. A static method may be "dynamic"
  // here, but not if the instance type is a struct/enum.
  if (auto IT = getType(Base)->getAs<MetatypeType>()) {
    auto InstanceType = IT->getInstanceType();
    if (InstanceType->getStructOrBoundGenericStruct() ||
        InstanceType->getEnumOrBoundGenericEnum())
      return false;
  }

  return true;
}

void swift::ide::getReceiverType(Expr *Base,
                                 SmallVectorImpl<NominalTypeDecl *> &Types) {
  Type ReceiverTy = Base->getType();
  if (!ReceiverTy)
    return;

  ReceiverTy = ReceiverTy->getWithoutSpecifierType();
  ReceiverTy = ReceiverTy->getMetatypeInstanceType();
  if (auto SelfT = ReceiverTy->getAs<DynamicSelfType>())
    ReceiverTy = SelfT->getSelfType();

  // TODO: Handle generics and composed protocols
  if (auto OpenedTy = ReceiverTy->getAs<OpenedArchetypeType>()) {
    assert(OpenedTy->isRoot());
    ReceiverTy = OpenedTy->getExistentialType();
  }

  if (auto TyD = ReceiverTy->getAnyNominal()) {
    Types.push_back(TyD);
  }
}
