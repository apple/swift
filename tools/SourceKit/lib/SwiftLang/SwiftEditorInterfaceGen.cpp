//===--- SwiftEditorInterfaceGen.cpp --------------------------------------===//
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

#include "SwiftLangSupport.h"
#include "SwiftInterfaceGenContext.h"
#include "SwiftASTManager.h"

#include "swift/AST/ASTPrinter.h"
#include "swift/AST/ASTWalker.h"
#include "swift/Basic/Version.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/IDE/ModuleInterfacePrinting.h"
#include "swift/IDE/SyntaxModel.h"
#include "swift/IDETool/CompilerInvocation.h"
#include "swift/Parse/ParseVersion.h"
#include "swift/Strings.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/ConvertUTF.h"

using namespace SourceKit;
using namespace swift;
using namespace ide;

class SwiftInterfaceGenContext::Implementation {
public:
  struct TextRange {
    unsigned Offset;
    unsigned Length;
  };

  struct TextReference {
    /// The declaration from the module.
    const ValueDecl *Dcl = nullptr;
    const ModuleEntity Mod;
    TextRange Range;

    TextReference(const ValueDecl *D, unsigned Offset, unsigned Length)
      : Dcl(D), Mod(), Range{Offset, Length} {}
    TextReference(const ModuleEntity Mod, unsigned Offset, unsigned Length)
    : Mod(Mod), Range{Offset, Length} {}
  };

  struct TextDecl {
    /// The declaration from the module.
    const Decl *Dcl = nullptr;
    /// The range in the interface source.
    TextRange Range;

    TextDecl(const Decl *D, TextRange Range)
      : Dcl(D), Range(Range) {}
    TextDecl() = default;
  };

  struct SourceTextInfo {
    std::string Text;
    std::vector<TextReference> References;
    std::vector<TextDecl> Decls;
    llvm::StringMap<TextDecl> USRMap;
  };

  // Hold an AstUnit so that the Decl* we have are always valid.
  ASTUnitRef AstUnit;
  std::string DocumentName;
  bool IsModule = false;
  std::string ModuleOrHeaderName;
  CompilerInvocation Invocation;
  PrintingDiagnosticConsumer DiagConsumer;
  CompilerInstance Instance;
  ModuleDecl *Mod = nullptr;
  SourceTextInfo Info;
  // This is the non-typechecked AST for the generated interface source.
  CompilerInstance TextCI;
  // Synchronize access to the embedded compiler instance (if we don't have an
  // ASTUnit).
  WorkQueue Queue{WorkQueue::Dequeuing::Serial,
                  "sourcekit.swift.InterfaceGenContext"};
};

typedef SwiftInterfaceGenContext::Implementation::TextRange TextRange;
typedef SwiftInterfaceGenContext::Implementation::TextReference TextReference;
typedef SwiftInterfaceGenContext::Implementation::TextDecl TextDecl;
typedef SwiftInterfaceGenContext::Implementation::SourceTextInfo SourceTextInfo;

namespace {
class AnnotatingPrinter : public StreamPrinter {
  SourceTextInfo &Info;

  struct DeclUSR {
    const Decl *Dcl = nullptr;
    std::string USR;
    DeclUSR(const Decl *D, StringRef USR) : Dcl(D), USR(USR) {}
  };
  std::vector<DeclUSR> DeclUSRs;

  // For members of a synthesized extension, we should append the USR of the
  // synthesize target to the original USR.
  std::string TargetUSR;

public:
  AnnotatingPrinter(SourceTextInfo &Info, llvm::raw_ostream &OS)
    : StreamPrinter(OS), Info(Info) { }

  ~AnnotatingPrinter() override {
    assert(DeclUSRs.empty() && "unmatched printDeclLoc call ?");
  }

  void printSynthesizedExtensionPre(const ExtensionDecl *ED,
                                    TypeOrExtensionDecl Target,
                                    Optional<BracketOptions> Bracket) override {
    // When we start print a synthesized extension, record the target's USR.
    llvm::SmallString<64> Buf;
    llvm::raw_svector_ostream OS(Buf);
    auto TargetNTD = Target.getBaseNominal();
    if (!SwiftLangSupport::printUSR(TargetNTD, OS)) {
      TargetUSR = std::string(OS.str());
    }
  }

  void
  printSynthesizedExtensionPost(const ExtensionDecl *ED,
                                TypeOrExtensionDecl Target,
                                Optional<BracketOptions> Bracket) override {
    // When we leave a synthesized extension, clear target's USR.
    TargetUSR = "";
  }

  void printDeclLoc(const Decl *D) override {
    unsigned LocOffset = OS.tell();
    TextDecl Entry(D, TextRange{LocOffset, 0});
    Info.Decls.emplace_back(Entry);

    if (auto VD = dyn_cast<ValueDecl>(D)) {
      // Only record non-local USRs.
      if (D->getDeclContext()->getLocalContext())
        return;

      llvm::SmallString<64> Buf;
      llvm::raw_svector_ostream OS(Buf);
      if (!SwiftLangSupport::printUSR(VD, OS)) {

        // Append target's USR if this is a member of a synthesized extension.
        if (!TargetUSR.empty()) {
          OS << LangSupport::SynthesizedUSRSeparator;
          OS << TargetUSR;
        }
        StringRef USR = OS.str();
        Info.USRMap[USR] = Entry;
        DeclUSRs.emplace_back(VD, USR);
      }
    }
  }

  void printDeclNameOrSignatureEndLoc(const Decl *D) override {
    unsigned Offset = OS.tell();

    if (!Info.Decls.empty() && Info.Decls.back().Dcl == D) {
      TextDecl &Entry = Info.Decls.back();
      Entry.Range.Length = Offset - Entry.Range.Offset;
    }

    if (!DeclUSRs.empty() && DeclUSRs.back().Dcl == D) {
      TextDecl &Entry = Info.USRMap[DeclUSRs.back().USR];
      assert(Entry.Dcl == D);
      Entry.Range.Length = Offset - Entry.Range.Offset;
      DeclUSRs.pop_back();
    }
  }

  void printTypeRef(
      Type T, const TypeDecl *TD, Identifier Name,
      PrintNameContext NameContext = PrintNameContext::Normal) override {
    unsigned StartOffset = OS.tell();
    Info.References.emplace_back(TD, StartOffset, Name.str().size());
    StreamPrinter::printTypeRef(T, TD, Name, NameContext);
  }

  void printModuleRef(ModuleEntity Mod, Identifier Name) override {
    unsigned StartOffset = OS.tell();
    Info.References.emplace_back(Mod, StartOffset, Name.str().size());
    StreamPrinter::printModuleRef(Mod, Name);
  }
};

class DocSyntaxWalker : public SyntaxModelWalker {
  SourceManager &SM;
  unsigned BufferID;
  EditorConsumer &Consumer;

public:
  DocSyntaxWalker(SourceManager &SM, unsigned BufferID,
                  EditorConsumer &Consumer)
    : SM(SM), BufferID(BufferID), Consumer(Consumer) {}

  bool walkToNodePre(SyntaxNode Node) override {
    unsigned Offset = SM.getLocOffsetInBuffer(Node.Range.getStart(), BufferID);
    unsigned Length = Node.Range.getByteLength();

    UIdent UID = SwiftLangSupport::getUIDForSyntaxNodeKind(Node.Kind);
    if (UID.isValid())
      Consumer.handleSyntaxMap(Offset, Length, UID);
    return true;
  }
};

} // end anonymous namespace

static bool makeParserAST(CompilerInstance &CI, StringRef Text,
                          CompilerInvocation Invocation) {
  Invocation.getFrontendOptions().InputsAndOutputs.clearInputs();
  Invocation.setModuleName("main");
  Invocation.getLangOptions().DisablePoundIfEvaluation = true;

  std::unique_ptr<llvm::MemoryBuffer> Buf;
  Buf = llvm::MemoryBuffer::getMemBuffer(Text, "<module-interface>");
  Invocation.getFrontendOptions().InputsAndOutputs.addInput(
      InputFile(Buf.get()->getBufferIdentifier(), /*isPrimary*/false, Buf.get(),
                file_types::TY_Swift));
  std::string InstanceSetupError;
  return CI.setup(Invocation, InstanceSetupError);
}

static void reportSyntacticAnnotations(CompilerInstance &CI,
                                       EditorConsumer &Consumer) {
  auto SF = dyn_cast<SourceFile>(CI.getMainModule()->getFiles()[0]);
  SyntaxModelContext SyntaxContext(*SF);
  DocSyntaxWalker SyntaxWalker(CI.getSourceMgr(), *SF->getBufferID(),
                               Consumer);
  SyntaxContext.walk(SyntaxWalker);
}

static void reportDocumentStructure(CompilerInstance &CI,
                                    EditorConsumer &Consumer) {
  auto SF = dyn_cast<SourceFile>(CI.getMainModule()->getFiles()[0]);
  SwiftEditorDocument::reportDocumentStructure(*SF, Consumer);
}

static void reportSemanticAnnotations(const SourceTextInfo &IFaceInfo,
                                      EditorConsumer &Consumer) {
  for (auto &Ref : IFaceInfo.References) {
    UIdent Kind;
    bool IsSystem;
    if (Ref.Mod) {
      Kind = SwiftLangSupport::getUIDForModuleRef();
      IsSystem = Ref.Mod.isNonUserModule();
    } else if (Ref.Dcl) {
      Kind = SwiftLangSupport::getUIDForDecl(Ref.Dcl, /*IsRef=*/true);
      IsSystem = Ref.Dcl->getModuleContext()->isNonUserModule();
    }
    if (Kind.isInvalid())
      continue;
    unsigned Offset = Ref.Range.Offset;
    unsigned Length = Ref.Range.Length;
    Consumer.handleSemanticAnnotation(Offset, Length, Kind, IsSystem);
  }
}

namespace {
/// A diagnostic consumer that picks up module loading errors.
class ModuleLoadingErrorConsumer final : public DiagnosticConsumer {
  llvm::SmallVector<std::string, 2> DiagMessages;

  void handleDiagnostic(SourceManager &SM,
                        const DiagnosticInfo &Info) override {
    // Only record errors for now. In the future it might be useful to pick up
    // some notes, but some notes are just noise.
    if (Info.Kind != DiagnosticKind::Error)
      return;

    std::string Message;
    {
      llvm::raw_string_ostream Out(Message);
      DiagnosticEngine::formatDiagnosticText(Out, Info.FormatString,
                                             Info.FormatArgs);
    }
    // We're only interested in the first and last errors. For a clang module
    // failure, the first error will be the reason why the module failed to
    // load, and the last error will be a generic "could not build Obj-C module"
    // error. For a Swift module, we'll typically only emit one error.
    //
    // NOTE: Currently when loading transitive dependencies for a Swift module,
    // we'll only diagnose the root failure, and not record the error for the
    // top-level module failure, as we stop emitting errors after a fatal error
    // has been recorded. This is currently fine for our use case though, as
    // we already include the top-level module name in the error we hand back.
    if (DiagMessages.size() < 2) {
      DiagMessages.emplace_back(std::move(Message));
    } else {
      DiagMessages.back() = std::move(Message);
    }
  }

public:
  ArrayRef<std::string> getDiagMessages() { return DiagMessages; }
};
} // end anonymous namespace

static bool getModuleInterfaceInfo(ASTContext &Ctx,
                                   StringRef ModuleName,
                                   Optional<StringRef> Group,
                                 SwiftInterfaceGenContext::Implementation &Impl,
                                   std::string &ErrMsg,
                                   bool SynthesizedExtensions,
                                   Optional<StringRef> InterestedUSR) {
  ModuleDecl *&Mod = Impl.Mod;
  SourceTextInfo &Info = Impl.Info;

  if (ModuleName.empty()) {
    ErrMsg = "Module name is empty";
    return true;
  }

  // Get the (sub)module to generate, recording the errors emitted.
  ModuleLoadingErrorConsumer DiagConsumer;
  {
    DiagnosticConsumerRAII R(Ctx.Diags, DiagConsumer);
    Mod = Ctx.getModuleByName(ModuleName);
  }

  // Check to see if we either couldn't find the module, or we failed to load
  // it, and report an error message back that includes the diagnostics we
  // collected, which should help pinpoint what the issue was. Note we do this
  // even if `Mod` is null, as the clang importer currently returns nullptr
  // when a module fails to load, and there may be interesting errors to
  // collect there.
  // Note that us failing here also means the caller may retry with e.g C++
  // interoperability enabled.
  if (!Mod || Mod->failedToLoad()) {
    llvm::raw_string_ostream OS(ErrMsg);

    OS << "Could not load module: ";
    OS << ModuleName;
    auto ModuleErrs = DiagConsumer.getDiagMessages();
    if (!ModuleErrs.empty()) {
      // We print the errors in reverse, as they are typically emitted in
      // a bottom-up manner by module loading, and a top-down presentation
      // makes more sense.
      OS << " (";
      llvm::interleaveComma(llvm::reverse(ModuleErrs), OS);
      OS << ")";
    }
    return true;
  }

  PrintOptions Options = PrintOptions::printModuleInterface(
      Ctx.TypeCheckerOpts.PrintFullConvention);
  if (Mod->findUnderlyingClangModule()) {
    if (Ctx.LangOpts.EnableCXXInterop) {
      // Show unavailable C++ APIs.
      Options.SkipUnavailable = false;
      // Skip over inline namespaces.
      Options.SkipInlineCXXNamespace = true;
    }
  }
  ModuleTraversalOptions TraversalOptions = None; // Don't print submodules.
  SmallString<128> Text;
  llvm::raw_svector_ostream OS(Text);
  AnnotatingPrinter Printer(Info, OS);
  if (!Group && InterestedUSR) {
    Group = findGroupNameForUSR(Mod, InterestedUSR.value());
  }
  printModuleInterface(Mod, Group.has_value()
                         ? llvm::makeArrayRef(Group.value())
                         : ArrayRef<StringRef>(),
                       TraversalOptions, Printer, Options,
                       Group.has_value() && SynthesizedExtensions);

  Info.Text = std::string(OS.str());
  return false;
}

static bool getHeaderInterfaceInfo(ASTContext &Ctx,
                                   StringRef HeaderName,
                                   SourceTextInfo &Info,
                                   std::string &ErrMsg) {
  if (HeaderName.empty()) {
    ErrMsg = "Header name is empty";
    return true;
  }

  PrintOptions Options = PrintOptions::printModuleInterface(
      Ctx.TypeCheckerOpts.PrintFullConvention);

  SmallString<128> Text;
  llvm::raw_svector_ostream OS(Text);
  AnnotatingPrinter Printer(Info, OS);
  printHeaderInterface(HeaderName, Ctx, Printer, Options);

  Info.Text = std::string(OS.str());
  return false;
}

SwiftInterfaceGenContextRef
SwiftInterfaceGenContext::createForSwiftSource(StringRef DocumentName,
                                               StringRef SourceFileName,
                                               ASTUnitRef AstUnit,
                                               CompilerInvocation Invocation,
                                               std::string &ErrMsg) {
  SwiftInterfaceGenContextRef IFaceGenCtx{ new SwiftInterfaceGenContext() };
  IFaceGenCtx->Impl.DocumentName = DocumentName.str();
  IFaceGenCtx->Impl.IsModule = true;
  IFaceGenCtx->Impl.ModuleOrHeaderName = SourceFileName.str();
  IFaceGenCtx->Impl.AstUnit = AstUnit;

  PrintOptions Options = PrintOptions::printSwiftFileInterface(
      Invocation.getFrontendOptions().PrintFullConvention);
  SmallString<128> Text;
  llvm::raw_svector_ostream OS(Text);
  AnnotatingPrinter Printer(IFaceGenCtx->Impl.Info, OS);
  printSwiftSourceInterface(AstUnit->getPrimarySourceFile(), Printer, Options);
  IFaceGenCtx->Impl.Info.Text = std::string(OS.str());
  if (makeParserAST(IFaceGenCtx->Impl.TextCI, IFaceGenCtx->Impl.Info.Text,
                    Invocation)) {
    ErrMsg = "Error during syntactic parsing";
    return nullptr;
  }
  return IFaceGenCtx;
}

SwiftInterfaceGenContextRef
SwiftInterfaceGenContext::create(StringRef DocumentName,
                                 bool IsModule,
                                 StringRef ModuleOrHeaderName,
                                 Optional<StringRef> Group,
                                 CompilerInvocation Invocation,
                                 std::string &ErrMsg,
                                 bool SynthesizedExtensions,
                                 Optional<StringRef> InterestedUSR) {
  SwiftInterfaceGenContextRef IFaceGenCtx{ new SwiftInterfaceGenContext() };
  IFaceGenCtx->Impl.DocumentName = DocumentName.str();
  IFaceGenCtx->Impl.IsModule = IsModule;
  IFaceGenCtx->Impl.ModuleOrHeaderName = ModuleOrHeaderName.str();
  IFaceGenCtx->Impl.Invocation = Invocation;
  CompilerInstance &CI = IFaceGenCtx->Impl.Instance;

  // Display diagnostics to stderr.
  CI.addDiagnosticConsumer(&IFaceGenCtx->Impl.DiagConsumer);

  Invocation.getFrontendOptions().InputsAndOutputs.clearInputs();
  if (CI.setup(Invocation, ErrMsg)) {
    return nullptr;
  }

  ASTContext &Ctx = CI.getASTContext();
  CloseClangModuleFiles scopedCloseFiles(*Ctx.getClangModuleLoader());

  // Load implicit imports so that Clang importer can use them.
  for (auto unloadedImport :
       CI.getMainModule()->getImplicitImportInfo().AdditionalUnloadedImports) {
    (void)Ctx.getModule(unloadedImport.module.getModulePath());
  }

  if (IsModule) {
    if (getModuleInterfaceInfo(Ctx, ModuleOrHeaderName, Group, IFaceGenCtx->Impl,
                               ErrMsg, SynthesizedExtensions, InterestedUSR))
      return nullptr;
  } else {
    auto &FEOpts = Invocation.getFrontendOptions();
    if (FEOpts.ImplicitObjCHeaderPath.empty()) {
      ErrMsg = "Implicit ObjC header path is empty";
      return nullptr;
    }

    auto &Importer = static_cast<ClangImporter &>(*Ctx.getClangModuleLoader());
    Importer.importBridgingHeader(FEOpts.ImplicitObjCHeaderPath,
                                  CI.getMainModule(),
                                  /*diagLoc=*/{},
                                  /*trackParsedSymbols=*/true);
    if (getHeaderInterfaceInfo(Ctx, ModuleOrHeaderName,
                               IFaceGenCtx->Impl.Info, ErrMsg))
      return nullptr;
  }

  if (makeParserAST(IFaceGenCtx->Impl.TextCI, IFaceGenCtx->Impl.Info.Text,
                    Invocation)) {
    ErrMsg = "Error during syntactic parsing";
    return nullptr;
  }

  return IFaceGenCtx;
}

SwiftInterfaceGenContextRef
SwiftInterfaceGenContext::createForTypeInterface(CompilerInvocation Invocation,
                                                 StringRef TypeUSR,
                                                 std::string &ErrorMsg) {
  SwiftInterfaceGenContextRef IFaceGenCtx{ new SwiftInterfaceGenContext() };
  IFaceGenCtx->Impl.IsModule = false;
  IFaceGenCtx->Impl.ModuleOrHeaderName = TypeUSR.str();
  IFaceGenCtx->Impl.Invocation = Invocation;
  CompilerInstance &CI = IFaceGenCtx->Impl.Instance;
  SourceTextInfo &Info = IFaceGenCtx->Impl.Info;

  // Display diagnostics to stderr.
  CI.addDiagnosticConsumer(&IFaceGenCtx->Impl.DiagConsumer);

  if (CI.setup(Invocation, ErrorMsg)) {
    return nullptr;
  }
  registerIDETypeCheckRequestFunctions(CI.getASTContext().evaluator);
  CI.performSema();
  ASTContext &Ctx = CI.getASTContext();
  CloseClangModuleFiles scopedCloseFiles(*Ctx.getClangModuleLoader());

  // Load standard library so that Clang importer can use it.
  auto *Stdlib = Ctx.getModuleByIdentifier(Ctx.StdlibModuleName);
  if (!Stdlib) {
    ErrorMsg = "Could not load the stdlib module";
    return nullptr;
  }
  auto *Module = CI.getMainModule();
  if (!Module) {
    ErrorMsg = "Could not load the main module";
    return nullptr;
  }
  SmallString<128> Text;
  llvm::raw_svector_ostream OS(Text);
  AnnotatingPrinter Printer(Info, OS);
  if (ide::printTypeInterface(Module, TypeUSR, Printer,
                              IFaceGenCtx->Impl.DocumentName, ErrorMsg))
    return nullptr;
  IFaceGenCtx->Impl.Info.Text = std::string(OS.str());
  if (makeParserAST(IFaceGenCtx->Impl.TextCI, IFaceGenCtx->Impl.Info.Text,
                    Invocation)) {
    ErrorMsg = "Error during syntactic parsing";
    return nullptr;
  }
  return IFaceGenCtx;
}

SwiftInterfaceGenContext::SwiftInterfaceGenContext()
  : Impl(*new Implementation) {
}
SwiftInterfaceGenContext::~SwiftInterfaceGenContext() {
  delete &Impl;
}

StringRef SwiftInterfaceGenContext::getDocumentName() const {
  return Impl.DocumentName;
}

StringRef SwiftInterfaceGenContext::getModuleOrHeaderName() const {
  return Impl.ModuleOrHeaderName;
}

bool SwiftInterfaceGenContext::isModule() const {
  return Impl.IsModule;
}

ModuleDecl *SwiftInterfaceGenContext::getModuleDecl() const {
  return Impl.Mod;
}

bool SwiftInterfaceGenContext::matches(StringRef ModuleName,
                                       const swift::CompilerInvocation &Invok) {
  if (!Impl.IsModule)
    return false;
  if (ModuleName != Impl.ModuleOrHeaderName)
    return false;

  if (Invok.getTargetTriple() != Impl.Invocation.getTargetTriple())
    return false;

  if (ModuleName == STDLIB_NAME)
    return true;

  if (Invok.getSDKPath() != Impl.Invocation.getSDKPath())
    return false;

  if (Impl.Mod->isNonUserModule())
    return true;

  const SearchPathOptions &SPOpts = Invok.getSearchPathOptions();
  const SearchPathOptions &ImplSPOpts = Impl.Invocation.getSearchPathOptions();
  if (SPOpts.getImportSearchPaths() != ImplSPOpts.getImportSearchPaths())
    return false;
  if (SPOpts.getFrameworkSearchPaths() != ImplSPOpts.getFrameworkSearchPaths())
    return false;

  if (Invok.getClangImporterOptions().ExtraArgs !=
      Impl.Invocation.getClangImporterOptions().ExtraArgs)
    return false;

  return true;
}

void SwiftInterfaceGenContext::reportEditorInfo(EditorConsumer &Consumer) const {
  Consumer.handleSourceText(Impl.Info.Text);
  reportSyntacticAnnotations(Impl.TextCI, Consumer);
  reportDocumentStructure(Impl.TextCI, Consumer);
  reportSemanticAnnotations(Impl.Info, Consumer);
  Consumer.finished();
}

void SwiftInterfaceGenContext::accessASTAsync(std::function<void()> Fn) {
  if (Impl.AstUnit) {
    Impl.AstUnit->performAsync(std::move(Fn));
  } else {
    Impl.Queue.dispatch(std::move(Fn));
  }
}

SwiftInterfaceGenContext::ResolvedEntity
SwiftInterfaceGenContext::resolveEntityForOffset(unsigned Offset) const {
  // Search among the references.
  {
    auto Pos = std::upper_bound(Impl.Info.References.begin(),
                                Impl.Info.References.end(),
                                Offset,
      [&](unsigned Offset, const TextReference &RHS) -> bool {
        return Offset < RHS.Range.Offset+RHS.Range.Length;
      });
    if (Pos != Impl.Info.References.end() && Pos->Range.Offset <= Offset) {
      if (Pos->Mod)
        return ResolvedEntity(Pos->Mod, true);
      else
        return ResolvedEntity(Pos->Dcl, true);
    }
  }

  SourceManager &SM = Impl.TextCI.getSourceMgr();
  auto SF = dyn_cast<SourceFile>(Impl.TextCI.getMainModule()->getFiles()[0]);
  unsigned BufferID = *SF->getBufferID();
  SourceLoc Loc = Lexer::getLocForStartOfToken(SM, BufferID, Offset);
  Offset = SM.getLocOffsetInBuffer(Loc, BufferID);

  // Search among the declarations.
  {
    auto Pos = std::lower_bound(Impl.Info.Decls.begin(),
                                Impl.Info.Decls.end(),
                                Offset,
      [&](const TextDecl &LHS, unsigned Offset) -> bool {
        return LHS.Range.Offset < Offset;
      });
    if (Pos != Impl.Info.Decls.end() && Pos->Range.Offset == Offset)
      return ResolvedEntity(dyn_cast<ValueDecl>(Pos->Dcl), false);
  }

  return ResolvedEntity();
}

llvm::Optional<std::pair<unsigned, unsigned>>
SwiftInterfaceGenContext::findUSRRange(StringRef USR) const {
  auto Pos = Impl.Info.USRMap.find(USR);
  if (Pos == Impl.Info.USRMap.end())
    return None;

  return std::make_pair(Pos->getValue().Range.Offset,
                        Pos->getValue().Range.Length);
}

void SwiftInterfaceGenContext::applyTo(
    swift::CompilerInvocation &CompInvok) const {
  CompInvok = Impl.Invocation;
}

SwiftInterfaceGenContextRef SwiftInterfaceGenMap::get(StringRef Name) const {
  llvm::sys::ScopedLock L(Mtx);
  auto It = IFaceGens.find(Name);
  if (It == IFaceGens.end())
    return nullptr;
  return It->second;
}

void SwiftInterfaceGenMap::set(StringRef Name,
                               SwiftInterfaceGenContextRef IFaceGen) {
  llvm::sys::ScopedLock L(Mtx);
  IFaceGens[Name] = IFaceGen;
}

bool SwiftInterfaceGenMap::remove(StringRef Name) {
  llvm::sys::ScopedLock L(Mtx);
  return IFaceGens.erase(Name);
}

SwiftInterfaceGenContextRef
SwiftInterfaceGenMap::find(StringRef ModuleName,
                           const CompilerInvocation &Invok) {
  llvm::sys::ScopedLock L(Mtx);
  for (auto &Entry : IFaceGens) {
    if (Entry.getValue()->matches(ModuleName, Invok))
      return Entry.getValue();
  }
  return nullptr;
}
//===----------------------------------------------------------------------===//
// EditorOpenTypeInterface
//===----------------------------------------------------------------------===//
void SwiftLangSupport::editorOpenTypeInterface(EditorConsumer &Consumer,
                                               ArrayRef<const char *> Args,
                                               StringRef TypeUSR) {
  CompilerInstance CI;
  // Display diagnostics to stderr.
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);

  CompilerInvocation Invocation;
  std::string Error;
  if (getASTManager()->initCompilerInvocation(
          Invocation, Args, FrontendOptions::ActionType::Typecheck,
          CI.getDiags(), StringRef(), Error)) {
    Consumer.handleRequestError(Error.c_str());
    return;
  }
  Invocation.getClangImporterOptions().ImportForwardDeclarations = true;

  std::string ErrMsg;
  auto IFaceGenRef = SwiftInterfaceGenContext::createForTypeInterface(
                                                      Invocation,
                                                      TypeUSR,
                                                      ErrMsg);
  if (!IFaceGenRef) {
    Consumer.handleRequestError(ErrMsg.c_str());
    return;
  }

  IFaceGenRef->reportEditorInfo(Consumer);
  // reportEditorInfo requires exclusive access to the AST, so don't add this
  // to the service cache until it has returned.
  IFaceGenContexts.set(TypeUSR, IFaceGenRef);
}

//===----------------------------------------------------------------------===//
// EditorOpenInterface
//===----------------------------------------------------------------------===//
void SwiftLangSupport::editorOpenInterface(EditorConsumer &Consumer,
                                           StringRef Name,
                                           StringRef ModuleName,
                                           Optional<StringRef> Group,
                                           ArrayRef<const char *> Args,
                                           bool SynthesizedExtensions,
                                           Optional<StringRef> InterestedUSR) {
  CompilerInstance CI;
  // Display diagnostics to stderr.
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);

  CompilerInvocation Invocation;
  std::string Error;
  if (getASTManager()->initCompilerInvocationNoInputs(
          Invocation, Args, FrontendOptions::ActionType::Typecheck,
          CI.getDiags(), Error)) {
    Consumer.handleRequestError(Error.c_str());
    return;
  }

  Invocation.getClangImporterOptions().ImportForwardDeclarations = true;

  std::string ErrMsg;
  auto IFaceGenRef = SwiftInterfaceGenContext::create(Name,
                                                      /*IsModule=*/true,
                                                      ModuleName,
                                                      Group,
                                                      Invocation,
                                                      ErrMsg,
                                                      SynthesizedExtensions,
                                                      InterestedUSR);
  if (!IFaceGenRef) {
      // Retry to generate a module interface with C++ interop enabled,
      // if the first attempt failed.
      bool retryWithCxxEnabled = true;
      for (const auto &arg: Args) {
          if (StringRef(arg).startswith("-cxx-interoperability-mode=") ||
              StringRef(arg).startswith("-enable-experimental-cxx-interop")) {
              retryWithCxxEnabled = false;
              break;
          }
      }
      if (retryWithCxxEnabled) {
          std::vector<const char *> AdjustedArgs(Args.begin(), Args.end());
          AdjustedArgs.push_back("-cxx-interoperability-mode=default");
          return editorOpenInterface(Consumer, Name, ModuleName, Group, AdjustedArgs,
                                     SynthesizedExtensions, InterestedUSR);
      }
      else {
          Consumer.handleRequestError(ErrMsg.c_str());
          return;
      }
  }

  IFaceGenRef->reportEditorInfo(Consumer);
  // reportEditorInfo requires exclusive access to the AST, so don't add this
  // to the service cache until it has returned.
  IFaceGenContexts.set(Name, IFaceGenRef);
}

class PrimaryFileInterfaceConsumer : public SwiftASTConsumer {

  std::string Name;
  std::string SourceFileName;
  SwiftInterfaceGenMap &Contexts;
  std::shared_ptr<EditorConsumer> Consumer;
  SwiftInvocationRef ASTInvok;

public:
  PrimaryFileInterfaceConsumer(StringRef Name, StringRef SourceFileName,
                               SwiftInterfaceGenMap &Contexts,
                               std::shared_ptr<EditorConsumer> Consumer,
                               SwiftInvocationRef ASTInvok) :
    Name(Name), SourceFileName(SourceFileName), Contexts(Contexts),
      Consumer(Consumer), ASTInvok(ASTInvok) {}

  void failed(StringRef Error) override {
    Consumer->handleRequestError(Error.data());
  }

  void handlePrimaryAST(ASTUnitRef AstUnit) override {
    CompilerInvocation CompInvok;
    ASTInvok->applyTo(CompInvok);
    std::string Error;
    auto IFaceGenRef = SwiftInterfaceGenContext::createForSwiftSource(Name,
      SourceFileName, AstUnit, CompInvok, Error);
    if (!Error.empty())
      Consumer->handleRequestError(Error.data());
    Contexts.set(Name, IFaceGenRef);
    IFaceGenRef->reportEditorInfo(*Consumer);
  }
};

void SwiftLangSupport::editorOpenSwiftSourceInterface(
    StringRef Name, StringRef SourceName, ArrayRef<const char *> Args,
    SourceKitCancellationToken CancellationToken,
    std::shared_ptr<EditorConsumer> Consumer) {
  std::string Error;
  auto Invocation = ASTMgr->getTypecheckInvocation(Args, SourceName, Error);
  if (!Invocation) {
    Consumer->handleRequestError(Error.c_str());
    return;
  }
  auto AstConsumer = std::make_shared<PrimaryFileInterfaceConsumer>(Name,
    SourceName, IFaceGenContexts, Consumer, Invocation);
  static const char OncePerASTToken = 0;
  getASTManager()->processASTAsync(Invocation, AstConsumer, &OncePerASTToken,
                                   CancellationToken,
                                   llvm::vfs::getRealFileSystem());
}

void SwiftLangSupport::editorOpenHeaderInterface(EditorConsumer &Consumer,
                                                 StringRef Name,
                                                 StringRef HeaderName,
                                                 ArrayRef<const char *> Args,
                                                 bool UsingSwiftArgs,
                                                 bool SynthesizedExtensions,
                                                 StringRef swiftVersion) {
  CompilerInstance CI;
  // Display diagnostics to stderr.
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);

  CompilerInvocation Invocation;
  std::string Error;

  ArrayRef<const char *> SwiftArgs = UsingSwiftArgs ? Args : llvm::None;
  if (getASTManager()->initCompilerInvocationNoInputs(
          Invocation, SwiftArgs, FrontendOptions::ActionType::Typecheck,
          CI.getDiags(), Error)) {
    Consumer.handleRequestError(Error.c_str());
    return;
  }

  if (!UsingSwiftArgs && initInvocationByClangArguments(Args, Invocation, Error)) {
    Consumer.handleRequestError(Error.c_str());
    return;
  }

  Invocation.getClangImporterOptions().ImportForwardDeclarations = true;
  if (!swiftVersion.empty()) {
    auto swiftVer =
        VersionParser::parseVersionString(swiftVersion, SourceLoc(), nullptr);
    if (swiftVer.has_value())
      Invocation.getLangOptions().EffectiveLanguageVersion =
          swiftVer.value();
  }
  auto IFaceGenRef = SwiftInterfaceGenContext::create(Name,
                                                      /*IsModule=*/false,
                                                      HeaderName,
                                                      None,
                                                      Invocation,
                                                      Error,
                                                      SynthesizedExtensions,
                                                      None);
  if (!IFaceGenRef) {
    Consumer.handleRequestError(Error.c_str());
    return;
  }

  IFaceGenRef->reportEditorInfo(Consumer);
  // reportEditorInfo requires exclusive access to the AST, so don't add this
  // to the service cache until it has returned.
  IFaceGenContexts.set(Name, IFaceGenRef);
}

void SwiftLangSupport::findInterfaceDocument(StringRef ModuleName,
                                             ArrayRef<const char *> Args,
                       std::function<void(const RequestResult<InterfaceDocInfo> &)> Receiver) {
  InterfaceDocInfo Info;

  CompilerInstance CI;
  // Display diagnostics to stderr.
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);

  CompilerInvocation Invocation;
  std::string Error;
  if (getASTManager()->initCompilerInvocation(
          Invocation, Args, FrontendOptions::ActionType::Typecheck,
          CI.getDiags(), StringRef(), Error)) {
    return Receiver(RequestResult<InterfaceDocInfo>::fromError(Error));
  }

  if (auto IFaceGenRef = IFaceGenContexts.find(ModuleName, Invocation))
    Info.ModuleInterfaceName = IFaceGenRef->getDocumentName();

  SmallString<128> Buf;
  SmallVector<std::pair<unsigned, unsigned>, 16> ArgOffs;
  auto addArgPair = [&](StringRef Arg, StringRef Val) {
    assert(!Arg.empty());
    if (Val.empty())
      return;
    unsigned ArgBegin = Buf.size();
    Buf += Arg;
    unsigned ArgEnd = Buf.size();
    unsigned ValBegin = Buf.size();
    Buf += Val;
    unsigned ValEnd = Buf.size();
    ArgOffs.push_back(std::make_pair(ArgBegin, ArgEnd));
    ArgOffs.push_back(std::make_pair(ValBegin, ValEnd));
  };
  auto addSingleArg = [&](StringRef Arg) {
    assert(!Arg.empty());
    unsigned ArgBegin = Buf.size();
    Buf += Arg;
    unsigned ArgEnd = Buf.size();
    ArgOffs.push_back(std::make_pair(ArgBegin, ArgEnd));
  };

  addArgPair("-target", Invocation.getTargetTriple());

  const auto &SPOpts = Invocation.getSearchPathOptions();
  addArgPair("-sdk", SPOpts.getSDKPath());
  for (const auto &FramePath : SPOpts.getFrameworkSearchPaths()) {
    if (FramePath.IsSystem)
      addArgPair("-Fsystem", FramePath.Path);
    else
      addArgPair("-F", FramePath.Path);
  }
  for (const auto &Path : SPOpts.getImportSearchPaths())
    addArgPair("-I", Path);

  const auto &ClangOpts = Invocation.getClangImporterOptions();
  addArgPair("-module-cache-path", ClangOpts.ModuleCachePath);
  for (auto &ExtraArg : ClangOpts.ExtraArgs)
    addArgPair("-Xcc", ExtraArg);

  if (Invocation.getFrontendOptions().ImportUnderlyingModule)
    addSingleArg("-import-underlying-module");
  addArgPair("-import-objc-header",
             Invocation.getFrontendOptions().ImplicitObjCHeaderPath);

  SmallVector<StringRef, 16> NewArgs;
  for (auto Pair : ArgOffs) {
    NewArgs.push_back(StringRef(Buf.begin()+Pair.first, Pair.second-Pair.first));
  }
  Info.CompilerArgs = NewArgs;

  return Receiver(RequestResult<InterfaceDocInfo>::fromResult(Info));
}
