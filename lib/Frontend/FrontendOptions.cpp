//===--- FrontendOptions.cpp ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Frontend/FrontendOptions.h"

#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/Option/Options.h"
#include "swift/Parse/Lexer.h"
#include "swift/Strings.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/LineIterator.h"
#include "llvm/Support/Path.h"

using namespace swift;
using namespace llvm::opt;

bool FrontendOptions::needsProperModuleName(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::Parse:
  case ActionType::ResolveImports:
  case ActionType::Typecheck:
  case ActionType::DumpParse:
  case ActionType::DumpAST:
  case ActionType::DumpInterfaceHash:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::DumpPCM:
  case ActionType::EmitPCH:
    return false;
  case ActionType::EmitSILGen:
  case ActionType::EmitSIL:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
  case ActionType::EmitModuleOnly:
  case ActionType::MergeModules:
  case ActionType::CompileModuleFromInterface:
  case ActionType::TypecheckModuleFromInterface:
    return true;
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
    return false;
  case ActionType::EmitAssembly:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitObject:
  case ActionType::EmitImportedModules:
  case ActionType::DumpTypeInfo:
  case ActionType::EmitPCM:
  case ActionType::ScanDependencies:
    return true;
  }
  llvm_unreachable("Unknown ActionType");
}

bool FrontendOptions::shouldActionOnlyParse(ActionType action) {
  switch (action) {
  case ActionType::Parse:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::EmitImportedModules:
  case ActionType::ScanDependencies:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
    return true;
  default:
    return false;
  }
}

bool FrontendOptions::doesActionRequireSwiftStandardLibrary(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::Parse:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::EmitImportedModules:
  case ActionType::ScanDependencies:
  case ActionType::PrintVersion:
  case ActionType::EmitPCH:
  case ActionType::EmitPCM:
  case ActionType::DumpPCM:
  case ActionType::CompileModuleFromInterface:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::PrintFeature:
    return false;
  case ActionType::ResolveImports:
  case ActionType::Typecheck:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::EmitSILGen:
  case ActionType::EmitSIL:
  case ActionType::EmitModuleOnly:
  case ActionType::MergeModules:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::EmitAssembly:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitObject:
  case ActionType::DumpTypeInfo:
    assert(!FrontendOptions::shouldActionOnlyParse(action) &&
           "Parse-only actions should not load modules!");
    return true;
  }
  llvm_unreachable("Unknown ActionType");
}

bool FrontendOptions::doesActionRequireInputs(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
    return false;
  case ActionType::REPL:
  case ActionType::Parse:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::EmitImportedModules:
  case ActionType::ScanDependencies:
  case ActionType::EmitPCH:
  case ActionType::EmitPCM:
  case ActionType::DumpPCM:
  case ActionType::CompileModuleFromInterface:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::ResolveImports:
  case ActionType::Typecheck:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::EmitSILGen:
  case ActionType::EmitSIL:
  case ActionType::EmitModuleOnly:
  case ActionType::MergeModules:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
  case ActionType::Immediate:
  case ActionType::EmitAssembly:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitObject:
  case ActionType::DumpTypeInfo:
    return true;
  }
  llvm_unreachable("Unknown ActionType");
}

bool FrontendOptions::doesActionPerformEndOfPipelineActions(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
  case ActionType::EmitPCH:
  case ActionType::EmitPCM:
  case ActionType::DumpPCM:
    return false;
  case ActionType::REPL:
  case ActionType::Parse:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::EmitImportedModules:
  case ActionType::ScanDependencies:
  case ActionType::CompileModuleFromInterface:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::ResolveImports:
  case ActionType::Typecheck:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::EmitSILGen:
  case ActionType::EmitSIL:
  case ActionType::EmitModuleOnly:
  case ActionType::MergeModules:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
  case ActionType::Immediate:
  case ActionType::EmitAssembly:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitObject:
  case ActionType::DumpTypeInfo:
    return true;
  }
  llvm_unreachable("Unknown ActionType");
}

bool FrontendOptions::supportCompilationCaching(ActionType action) {
  // TODO: need to audit this list to make sure everything marked as true are
  // all supported and tested.
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
  case ActionType::DumpPCM:
  case ActionType::REPL:
  case ActionType::Parse:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::EmitImportedModules:
  case ActionType::ScanDependencies:
  case ActionType::ResolveImports:
  case ActionType::Typecheck:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::MergeModules:
  case ActionType::Immediate:
  case ActionType::DumpTypeInfo:
    return false;
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::CompileModuleFromInterface:
  case ActionType::EmitPCH:
  case ActionType::EmitPCM:
  case ActionType::EmitAssembly:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitObject:
  case ActionType::EmitSILGen:
  case ActionType::EmitSIL:
  case ActionType::EmitModuleOnly:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
    return true;
  }
  llvm_unreachable("Unknown ActionType");
}

void FrontendOptions::forAllOutputPaths(
    const InputFile &input, llvm::function_ref<void(StringRef)> fn) const {
  if (RequestedAction != FrontendOptions::ActionType::EmitModuleOnly &&
      RequestedAction != FrontendOptions::ActionType::MergeModules) {
    if (InputsAndOutputs.isWholeModule())
      InputsAndOutputs.forEachOutputFilename(fn);
    else
      fn(input.outputFilename());
  }
  const SupplementaryOutputPaths &outs =
      input.getPrimarySpecificPaths().SupplementaryOutputs;
  const std::string *outputs[] = {
      &outs.ModuleOutputPath,          &outs.ModuleDocOutputPath,
      &outs.ModuleInterfaceOutputPath, &outs.PrivateModuleInterfaceOutputPath,
      &outs.ClangHeaderOutputPath,     &outs.ModuleSourceInfoOutputPath};
  for (const std::string *next : outputs) {
    if (!next->empty())
      fn(*next);
  }
}

file_types::ID
FrontendOptions::formatForPrincipalOutputFileForAction(ActionType action) {
  using namespace file_types;

  switch (action) {
  case ActionType::NoneAction:
    return TY_Nothing;

  case ActionType::Parse:
  case ActionType::ResolveImports:
  case ActionType::Typecheck:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::DumpTypeInfo:
  case ActionType::DumpPCM:
  case ActionType::PrintVersion:
    return TY_Nothing;

  case ActionType::EmitPCH:
    return TY_PCH;

  case ActionType::EmitSILGen:
    return TY_RawSIL;

  case ActionType::EmitSIL:
    return TY_SIL;

  case ActionType::EmitSIBGen:
    return TY_RawSIB;

  case ActionType::EmitSIB:
    return TY_SIB;

  case ActionType::MergeModules:
  case ActionType::EmitModuleOnly:
  case ActionType::CompileModuleFromInterface:
    return TY_SwiftModuleFile;

  case ActionType::Immediate:
  case ActionType::REPL:
    // These modes have no frontend-generated output.
    return TY_Nothing;

  case ActionType::EmitAssembly:
    return TY_Assembly;

  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
    return TY_LLVM_IR;

  case ActionType::EmitBC:
    return TY_LLVM_BC;

  case ActionType::EmitObject:
    return TY_Object;

  case ActionType::EmitImportedModules:
    return TY_ImportedModules;

  case ActionType::EmitPCM:
    return TY_ClangModuleFile;

  case ActionType::ScanDependencies:
    return TY_JSONDependencies;
  case ActionType::PrintFeature:
    return TY_JSONFeatures;
  }
  llvm_unreachable("unhandled action");
}

bool FrontendOptions::canActionEmitDependencies(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::Parse:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::DumpTypeInfo:
  case ActionType::CompileModuleFromInterface:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::DumpPCM:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
    return false;
  case ActionType::ResolveImports:
  case ActionType::Typecheck:
  case ActionType::MergeModules:
  case ActionType::EmitModuleOnly:
  case ActionType::EmitPCH:
  case ActionType::EmitSILGen:
  case ActionType::EmitSIL:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitAssembly:
  case ActionType::EmitObject:
  case ActionType::EmitImportedModules:
  case ActionType::EmitPCM:
  case ActionType::ScanDependencies:
    return true;
  }
  llvm_unreachable("unhandled action");
}

bool FrontendOptions::canActionEmitReferenceDependencies(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::Parse:
  case ActionType::ResolveImports:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::DumpTypeInfo:
  case ActionType::CompileModuleFromInterface:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::EmitPCM:
  case ActionType::DumpPCM:
  case ActionType::ScanDependencies:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
    return false;
  case ActionType::Typecheck:
  case ActionType::MergeModules:
  case ActionType::EmitModuleOnly:
  case ActionType::EmitPCH:
  case ActionType::EmitSILGen:
  case ActionType::EmitSIL:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitAssembly:
  case ActionType::EmitObject:
  case ActionType::EmitImportedModules:
    return true;
  }
  llvm_unreachable("unhandled action");
}

bool FrontendOptions::canActionEmitModuleSummary(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::Parse:
  case ActionType::ResolveImports:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::EmitImportedModules:
  case ActionType::EmitPCH:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::DumpTypeInfo:
  case ActionType::EmitSILGen:
  case ActionType::EmitSIBGen:
  case ActionType::CompileModuleFromInterface:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::EmitPCM:
  case ActionType::DumpPCM:
  case ActionType::ScanDependencies:
  case ActionType::Typecheck:
  case ActionType::MergeModules:
  case ActionType::EmitModuleOnly:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
    return false;
  case ActionType::EmitSIL:
  case ActionType::EmitSIB:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitAssembly:
  case ActionType::EmitObject:
    return true;
  }
  llvm_unreachable("unhandled action");
}

bool FrontendOptions::canActionEmitClangHeader(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::Parse:
  case ActionType::ResolveImports:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::EmitPCH:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::DumpTypeInfo:
  case ActionType::CompileModuleFromInterface:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::EmitPCM:
  case ActionType::DumpPCM:
  case ActionType::ScanDependencies:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
    return false;
  case ActionType::Typecheck:
  case ActionType::MergeModules:
  case ActionType::EmitModuleOnly:
  case ActionType::EmitSILGen:
  case ActionType::EmitSIL:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitAssembly:
  case ActionType::EmitObject:
  case ActionType::EmitImportedModules:
    return true;
  }
  llvm_unreachable("unhandled action");
}

bool FrontendOptions::canActionEmitLoadedModuleTrace(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::Parse:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::DumpTypeInfo:
  case ActionType::CompileModuleFromInterface:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::EmitPCM:
  case ActionType::DumpPCM:
  case ActionType::ScanDependencies:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
    return false;
  case ActionType::ResolveImports:
  case ActionType::Typecheck:
  case ActionType::MergeModules:
  case ActionType::EmitModuleOnly:
  case ActionType::EmitPCH:
  case ActionType::EmitSILGen:
  case ActionType::EmitSIL:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitAssembly:
  case ActionType::EmitObject:
  case ActionType::EmitImportedModules:
    return true;
  }
  llvm_unreachable("unhandled action");
}
bool FrontendOptions::canActionEmitModuleSemanticInfo(ActionType action) {
  switch (action) {
  case ActionType::MergeModules:
  case ActionType::EmitModuleOnly:
  case ActionType::CompileModuleFromInterface:
  // For test
  case ActionType::Typecheck:
    return true;
  case ActionType::NoneAction:
  case ActionType::Parse:
  case ActionType::ResolveImports:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::EmitPCH:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::DumpTypeInfo:
  case ActionType::EmitSILGen:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::EmitPCM:
  case ActionType::DumpPCM:
  case ActionType::ScanDependencies:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
  case ActionType::EmitSIL:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitAssembly:
  case ActionType::EmitObject:
  case ActionType::EmitImportedModules:
    return false;
  }
  llvm_unreachable("unhandled action");
}
bool FrontendOptions::canActionEmitABIDescriptor(ActionType action) {
  if (canActionEmitModule(action))
    return true;
  if (action == ActionType::CompileModuleFromInterface)
    return true;
  return false;
}
bool FrontendOptions::canActionEmitConstValues(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::Parse:
  case ActionType::ResolveImports:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::DumpTypeInfo:
  case ActionType::CompileModuleFromInterface:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::EmitPCM:
  case ActionType::DumpPCM:
  case ActionType::ScanDependencies:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
    return false;
  case ActionType::Typecheck:
  case ActionType::MergeModules:
  case ActionType::EmitModuleOnly:
  case ActionType::EmitPCH:
  case ActionType::EmitSILGen:
  case ActionType::EmitSIL:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitAssembly:
  case ActionType::EmitObject:
  case ActionType::EmitImportedModules:
    return true;
  }
  llvm_unreachable("unhandled action");
}
bool FrontendOptions::canActionEmitModule(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::Parse:
  case ActionType::ResolveImports:
  case ActionType::Typecheck:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::EmitPCH:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::DumpTypeInfo:
  case ActionType::EmitSILGen:
  case ActionType::CompileModuleFromInterface:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::EmitPCM:
  case ActionType::DumpPCM:
  case ActionType::ScanDependencies:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
    return false;
  case ActionType::MergeModules:
  case ActionType::EmitModuleOnly:
  case ActionType::EmitSIL:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitAssembly:
  case ActionType::EmitObject:
  case ActionType::EmitImportedModules:
    return true;
  }
  llvm_unreachable("unhandled action");
}

bool FrontendOptions::canActionEmitModuleDoc(ActionType action) {
  return canActionEmitModule(action);
}

bool FrontendOptions::canActionEmitInterface(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::Parse:
  case ActionType::ResolveImports:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::EmitImportedModules:
  case ActionType::EmitPCH:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::DumpTypeInfo:
  case ActionType::EmitSILGen:
  case ActionType::EmitSIBGen:
  case ActionType::CompileModuleFromInterface:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::EmitPCM:
  case ActionType::DumpPCM:
  case ActionType::ScanDependencies:
  case ActionType::PrintFeature:
    return false;
  case ActionType::Typecheck:
  case ActionType::MergeModules:
  case ActionType::EmitModuleOnly:
  case ActionType::EmitSIL:
  case ActionType::EmitSIB:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitAssembly:
  case ActionType::EmitObject:
  case ActionType::PrintVersion:
    return true;
  }
  llvm_unreachable("unhandled action");
}

bool FrontendOptions::doesActionProduceOutput(ActionType action) {
  switch (action) {
  case ActionType::Parse:
  case ActionType::ResolveImports:
  case ActionType::Typecheck:
  case ActionType::DumpParse:
  case ActionType::DumpAST:
  case ActionType::DumpInterfaceHash:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::EmitPCH:
  case ActionType::EmitSILGen:
  case ActionType::EmitSIL:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
  case ActionType::EmitModuleOnly:
  case ActionType::EmitAssembly:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitObject:
  case ActionType::EmitImportedModules:
  case ActionType::MergeModules:
  case ActionType::CompileModuleFromInterface:
  case ActionType::DumpTypeInfo:
  case ActionType::EmitPCM:
  case ActionType::DumpPCM:
  case ActionType::ScanDependencies:
  case ActionType::PrintFeature:
    return true;

  case ActionType::TypecheckModuleFromInterface:
  case ActionType::NoneAction:
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::PrintVersion:
    return false;
  }
  llvm_unreachable("Unknown ActionType");
}

bool FrontendOptions::doesActionProduceTextualOutput(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::EmitPCH:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
  case ActionType::MergeModules:
  case ActionType::EmitModuleOnly:
  case ActionType::CompileModuleFromInterface:
  case ActionType::EmitBC:
  case ActionType::EmitObject:
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::EmitPCM:
  case ActionType::TypecheckModuleFromInterface:
    return false;

  case ActionType::Parse:
  case ActionType::ResolveImports:
  case ActionType::Typecheck:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::EmitImportedModules:
  case ActionType::EmitSILGen:
  case ActionType::EmitSIL:
  case ActionType::EmitAssembly:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::DumpTypeInfo:
  case ActionType::DumpPCM:
  case ActionType::ScanDependencies:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
    return true;
  }
  llvm_unreachable("unhandled action");
}

bool FrontendOptions::doesActionGenerateSIL(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::Parse:
  case ActionType::ResolveImports:
  case ActionType::Typecheck:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::EmitImportedModules:
  case ActionType::EmitPCH:
  case ActionType::CompileModuleFromInterface:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::EmitPCM:
  case ActionType::DumpPCM:
  case ActionType::ScanDependencies:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
    return false;
  case ActionType::EmitSILGen:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIL:
  case ActionType::EmitSIB:
  case ActionType::EmitModuleOnly:
  case ActionType::MergeModules:
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::EmitAssembly:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitObject:
  case ActionType::DumpTypeInfo:
    return true;
  }
  llvm_unreachable("unhandled action");
}

bool FrontendOptions::doesActionGenerateIR(ActionType action) {
  switch (action) {
  case ActionType::NoneAction:
  case ActionType::Parse:
  case ActionType::DumpParse:
  case ActionType::DumpInterfaceHash:
  case ActionType::DumpAST:
  case ActionType::PrintAST:
  case ActionType::PrintASTDecl:
  case ActionType::DumpScopeMaps:
  case ActionType::DumpTypeRefinementContexts:
  case ActionType::DumpTypeInfo:
  case ActionType::CompileModuleFromInterface:
  case ActionType::TypecheckModuleFromInterface:
  case ActionType::Typecheck:
  case ActionType::ResolveImports:
  case ActionType::MergeModules:
  case ActionType::EmitModuleOnly:
  case ActionType::EmitPCH:
  case ActionType::EmitSILGen:
  case ActionType::EmitSIL:
  case ActionType::EmitSIBGen:
  case ActionType::EmitSIB:
  case ActionType::EmitImportedModules:
  case ActionType::EmitPCM:
  case ActionType::DumpPCM:
  case ActionType::ScanDependencies:
  case ActionType::PrintVersion:
  case ActionType::PrintFeature:
    return false;
  case ActionType::Immediate:
  case ActionType::REPL:
  case ActionType::EmitIRGen:
  case ActionType::EmitIR:
  case ActionType::EmitBC:
  case ActionType::EmitAssembly:
  case ActionType::EmitObject:
    return true;
  }
  llvm_unreachable("unhandled action");
}


const PrimarySpecificPaths &
FrontendOptions::getPrimarySpecificPathsForAtMostOnePrimary() const {
  return InputsAndOutputs.getPrimarySpecificPathsForAtMostOnePrimary();
}

const PrimarySpecificPaths &
FrontendOptions::getPrimarySpecificPathsForPrimary(StringRef filename) const {
  return InputsAndOutputs.getPrimarySpecificPathsForPrimary(filename);
}

bool FrontendOptions::shouldTrackSystemDependencies() const {
  return IntermoduleDependencyTracking ==
         IntermoduleDepTrackingMode::IncludeSystem;
}
