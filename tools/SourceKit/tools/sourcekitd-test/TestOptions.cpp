//===--- TestOptions.cpp --------------------------------------------------===//
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

#include "TestOptions.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace sourcekitd_test;
using llvm::StringRef;

namespace {

// Create enum with OPT_xxx values for each option in Options.td.
enum Opt {
  OPT_INVALID = 0,
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM,  \
               HELP, META, VALUES)                                             \
  OPT_##ID,
#include "Options.inc"
  LastOption
#undef OPTION
};

// Create prefix string literals used in Options.td.
#define PREFIX(NAME, VALUE) const char *const NAME[] = VALUE;
#include "Options.inc"
#undef PREFIX

// Create table mapping all options defined in Options.td.
static const llvm::opt::OptTable::Info InfoTable[] = {
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM,  \
               HELPTEXT, METAVAR, VALUES)                                      \
  {PREFIX,      NAME,      HELPTEXT,                                           \
   METAVAR,     OPT_##ID,  llvm::opt::Option::KIND##Class,                     \
   PARAM,       FLAGS,     OPT_##GROUP,                                        \
   OPT_##ALIAS, ALIASARGS, VALUES},
#include "Options.inc"
#undef OPTION
};

// Create OptTable class for parsing actual command line arguments
class TestOptTable : public llvm::opt::OptTable {
public:
  TestOptTable() : OptTable(InfoTable, std::size(InfoTable)){}
};

} // end anonymous namespace

static std::pair<unsigned, unsigned> parseLineCol(StringRef LineCol) {
  unsigned Line, Col;
  size_t ColonIdx = LineCol.find(':');
  if (ColonIdx == StringRef::npos) {
    llvm::errs() << "wrong pos format, it should be '<line>:<column>'\n";
    exit(1);
  }
  if (LineCol.substr(0, ColonIdx).getAsInteger(10, Line)) {
    llvm::errs() << "wrong pos format, it should be '<line>:<column>'\n";
    exit(1);
  }
  if (LineCol.substr(ColonIdx+1).getAsInteger(10, Col)) {
    llvm::errs() << "wrong pos format, it should be '<line>:<column>'\n";
    exit(1);
  }

  if (Line == 0 || Col == 0) {
    llvm::errs() << "wrong pos format, line/col should start from 1\n";
    exit(1);
  }

  return { Line, Col };
}

bool TestOptions::parseArgs(llvm::ArrayRef<const char *> Args) {
  if (Args.empty())
    return false;

  // Parse command line options using Options.td
  TestOptTable Table;
  unsigned MissingIndex;
  unsigned MissingCount;
  llvm::opt::InputArgList ParsedArgs =
      Table.ParseArgs(Args, MissingIndex, MissingCount);
  if (MissingCount) {
    llvm::errs() << "error: missing argument value for '"
        << ParsedArgs.getArgString(MissingIndex) << "', expected "
        << MissingCount << " argument(s)\n";
    return true;
  }

  for (auto InputArg : ParsedArgs) {
    switch (InputArg->getOption().getID()) {
    case OPT_req:
      Request = llvm::StringSwitch<SourceKitRequest>(InputArg->getValue())
        .Case("version", SourceKitRequest::ProtocolVersion)
        .Case("compiler-version", SourceKitRequest::CompilerVersion)
        .Case("demangle", SourceKitRequest::DemangleNames)
        .Case("mangle", SourceKitRequest::MangleSimpleClasses)
        .Case("index", SourceKitRequest::Index)
        .Case("complete", SourceKitRequest::CodeComplete)
        .Case("complete.open", SourceKitRequest::CodeCompleteOpen)
        .Case("complete.close", SourceKitRequest::CodeCompleteClose)
        .Case("complete.update", SourceKitRequest::CodeCompleteUpdate)
        .Case("complete.cache.ondisk", SourceKitRequest::CodeCompleteCacheOnDisk)
        .Case("complete.setpopularapi", SourceKitRequest::CodeCompleteSetPopularAPI)
        .Case("typecontextinfo", SourceKitRequest::TypeContextInfo)
        .Case("conformingmethods", SourceKitRequest::ConformingMethodList)
        .Case("cursor", SourceKitRequest::CursorInfo)
        .Case("related-idents", SourceKitRequest::RelatedIdents)
        .Case("active-regions", SourceKitRequest::ActiveRegions)
        .Case("syntax-map", SourceKitRequest::SyntaxMap)
        .Case("structure", SourceKitRequest::Structure)
        .Case("format", SourceKitRequest::Format)
        .Case("expand-placeholder", SourceKitRequest::ExpandPlaceholder)
        .Case("doc-info", SourceKitRequest::DocInfo)
        .Case("sema", SourceKitRequest::SemanticInfo)
        .Case("interface-gen", SourceKitRequest::InterfaceGen)
        .Case("interface-gen-open", SourceKitRequest::InterfaceGenOpen)
        .Case("find-usr", SourceKitRequest::FindUSR)
        .Case("find-interface", SourceKitRequest::FindInterfaceDoc)
        .Case("open", SourceKitRequest::Open)
        .Case("close", SourceKitRequest::Close)
        .Case("edit", SourceKitRequest::Edit)
        .Case("print-annotations", SourceKitRequest::PrintAnnotations)
        .Case("print-diags", SourceKitRequest::PrintDiags)
        .Case("extract-comment", SourceKitRequest::ExtractComment)
        .Case("module-groups", SourceKitRequest::ModuleGroups)
        .Case("range", SourceKitRequest::RangeInfo)
        .Case("syntactic-rename", SourceKitRequest::SyntacticRename)
        .Case("find-rename-ranges", SourceKitRequest::FindRenameRanges)
        .Case("find-local-rename-ranges", SourceKitRequest::FindLocalRenameRanges)
        .Case("translate", SourceKitRequest::NameTranslation)
        .Case("markup-xml", SourceKitRequest::MarkupToXML)
        .Case("stats", SourceKitRequest::Statistics)
        .Case("track-compiles", SourceKitRequest::EnableCompileNotifications)
        .Case("collect-type", SourceKitRequest::CollectExpressionType)
        .Case("collect-var-type", SourceKitRequest::CollectVariableType)
        .Case("global-config", SourceKitRequest::GlobalConfiguration)
        .Case("dependency-updated", SourceKitRequest::DependencyUpdated)
        .Case("diags", SourceKitRequest::Diagnostics)
        .Case("compile", SourceKitRequest::Compile)
        .Case("compile.close", SourceKitRequest::CompileClose)
        .Case("syntactic-expandmacro", SourceKitRequest::SyntacticMacroExpansion)
#define SEMANTIC_REFACTORING(KIND, NAME, ID) .Case("refactoring." #ID, SourceKitRequest::KIND)
#include "swift/Refactoring/RefactoringKinds.def"
        .Default(SourceKitRequest::None);

      if (Request == SourceKitRequest::None) {
        llvm::errs() << "error: invalid request '" << InputArg->getValue()
                     << "'\nexpected one of "
                     << "- version\n"
                     << "- compiler-version\n"
                     << "- demangle\n"
                     << "- mangle\n"
                     << "- index\n"
                     << "- complete\n"
                     << "- complete.open\n"
                     << "- complete.close\n"
                     << "- complete.update\n"
                     << "- complete.cache.ondisk\n"
                     << "- complete.setpopularapi\n"
                     << "- typecontextinfo\n"
                     << "- conformingmethods\n"
                     << "- cursor\n"
                     << "- related-idents\n"
                     << "- syntax-map\n"
                     << "- structure\n"
                     << "- format\n"
                     << "- expand-placeholder\n"
                     << "- doc-info\n"
                     << "- sema\n"
                     << "- interface-gen\n"
                     << "- interface-gen-open\n"
                     << "- find-usr\n"
                     << "- find-interface\n"
                     << "- open\n"
                     << "- close\n"
                     << "- edit\n"
                     << "- print-annotations\n"
                     << "- print-diags\n"
                     << "- extract-comment\n"
                     << "- module-groups\n"
                     << "- range\n"
                     << "- syntactic-rename\n"
                     << "- find-rename-ranges\n"
                     << "- find-local-rename-ranges\n"
                     << "- translate\n"
                     << "- markup-xml\n"
                     << "- stats\n"
                     << "- track-compiles\n"
                     << "- collect-type\n"
                     << "- global-config\n"
                     << "- dependency-updated\n"
                     << "- syntactic-expandmacro\n"
#define SEMANTIC_REFACTORING(KIND, NAME, ID) << "- refactoring." #ID "\n"
#include "swift/Refactoring/RefactoringKinds.def"
                        "\n";
        return true;
      }
      break;

    case OPT_help: {
      printHelp(false);
      return true;
    }

    case OPT_offset: {
      unsigned offset;
      if (StringRef(InputArg->getValue()).getAsInteger(10, offset)) {
        llvm::errs() << "error: expected integer for 'offset'\n";
        return true;
      }

      Offset = offset;
      break;
    }

    case OPT_length:
      if (StringRef(InputArg->getValue()).getAsInteger(10, Length)) {
        llvm::errs() << "error: expected integer for 'length'\n";
        return true;
      }
      break;

    case OPT_pos: {
      auto linecol = parseLineCol(InputArg->getValue());
      Line = linecol.first;
      Col = linecol.second;
      break;
    }

    case OPT_end_pos: {
      auto linecol = parseLineCol(InputArg->getValue());
      EndLine = linecol.first;
      EndCol = linecol.second;
      break;
    }

    case OPT_using_swift_args: {
      UsingSwiftArgs = true;
      break;
    }

    case OPT_swift_version:
      SwiftVersion = InputArg->getValue();
      break;

    case OPT_pass_version_as_string:
      PassVersionAsString = true;
      break;

    case OPT_line:
      if (StringRef(InputArg->getValue()).getAsInteger(10, Line)) {
        llvm::errs() << "error: expected integer for 'line'\n";
        return true;
      }
      Col = 1;
      break;

    case OPT_replace:
      ReplaceText = InputArg->getValue();
      break;

    case OPT_module:
      ModuleName = InputArg->getValue();
      break;

    case OPT_group_name:
      ModuleGroupName = InputArg->getValue();
      break;

    case OPT_id:
      RequestId = InputArg->getValue();
      break;

    case OPT_interested_usr:
      InterestedUSR = InputArg->getValue();
      break;

    case OPT_header:
      HeaderPath = InputArg->getValue();
      break;

    case OPT_text_input:
      TextInputFile = InputArg->getValue();
      break;

    case OPT_usr:
      USR = InputArg->getValue();
      break;

    case OPT_pass_as_sourcetext:
      PassAsSourceText = true;
      break;

    case OPT_cache_path:
      CachePath = InputArg->getValue();
      break;

    case OPT_req_opts:
      for (auto item : InputArg->getValues())
        RequestOptions.push_back(item);
      break;

    case OPT_check_interface_is_ascii:
      CheckInterfaceIsASCII = true;
      break;

    case OPT_dont_print_request:
      PrintRequest = false;
      break;

    case OPT_print_response_as_json:
      PrintResponseAsJSON = true;
      break;

    case OPT_print_raw_response:
      PrintRawResponse = true;
      break;

    case OPT_dont_print_response:
      PrintResponse = false;
      break;

    case OPT_INPUT:
      SourceFile = InputArg->getValue();
      SourceText = llvm::None;
      Inputs.push_back(InputArg->getValue());
      break;

    case OPT_primary_file:
      PrimaryFile = InputArg->getValue();
      break;

    case OPT_rename_spec:
      RenameSpecPath = InputArg->getValue();
      break;

    case OPT_json_request_path:
      JsonRequestPath = InputArg->getValue();
      break;

    case OPT_simplified_demangling:
      SimplifiedDemangling = true;
      break;

    case OPT_synthesized_extension:
      SynthesizedExtensions = true;
      break;

    case OPT_async:
      isAsyncRequest = true;
      break;

    case OPT_cursor_action:
      CollectActionables = true;
      break;

    case OPT_swift_name:
      SwiftName = InputArg->getValue();
      break;

    case OPT_objc_name:
      ObjCName = InputArg->getValue();
      break;

    case OPT_objc_selector:
      ObjCSelector = InputArg->getValue();
      break;

    case OPT_name:
      Name = InputArg->getValue();
      break;

    case OPT_cancel_on_subsequent_request:
      unsigned Cancel;
      if (StringRef(InputArg->getValue()).getAsInteger(10, Cancel)) {
        llvm::errs() << "error: expected integer for 'cancel-on-subsequent-request'\n";
        return true;
      }
      CancelOnSubsequentRequest = Cancel;
      break;

    case OPT_time_request:
      timeRequest = true;
      break;

    case OPT_measure_instructions:
      measureInstructions = true;
      break;

    case OPT_repeat_request:
      if (StringRef(InputArg->getValue()).getAsInteger(10, repeatRequest)) {
        llvm::errs() << "error: expected integer for 'cancel-on-subsequent-request'\n";
        return true;
      } else if (repeatRequest < 1) {
        llvm::errs() << "error: repeat-request must be >= 1\n";
        return true;
      }
      break;

    case OPT_vfs_files:
      VFSName = VFSName.value_or("in-memory-vfs");
      for (const char *vfsFile : InputArg->getValues()) {
        StringRef name, target;
        std::tie(name, target) = StringRef(vfsFile).split('=');
        llvm::SmallString<64> nativeName;
        llvm::sys::path::native(name, nativeName);
        bool passAsSourceText = target.consume_front("@");
        VFSFiles.try_emplace(nativeName.str(), VFSFile(target.str(), passAsSourceText));
      }
      break;

    case OPT_vfs_name:
      VFSName = InputArg->getValue();
      break;

    case OPT_module_cache_path:
      ModuleCachePath = InputArg->getValue();
      break;

    case OPT_simulate_long_request:
      unsigned SimulatedDuration;
      if (StringRef(InputArg->getValue()).getAsInteger(10, SimulatedDuration)) {
        llvm::errs() << "error: expected integer for 'simulate-long-request'\n";
        return true;
      }
      SimulateLongRequest = SimulatedDuration;
      break;

    case OPT_shell:
      ShellExecution = true;
      break;

    case OPT_cancel:
      CancelRequest = InputArg->getValue();
      break;

    case OPT_disable_implicit_concurrency_module_import:
      DisableImplicitConcurrencyModuleImport = true;
      break;

    case OPT_disable_implicit_string_processing_module_import:
      DisableImplicitStringProcessingModuleImport = true;
      break;

    case OPT_disable_implicit_backtracing_module_import:
      DisableImplicitBacktracingModuleImport = true;
      break;

    case OPT_UNKNOWN:
      llvm::errs() << "error: unknown argument: "
                   << InputArg->getAsString(ParsedArgs) << '\n'
                   << "Use -h or -help for assistance" << '\n';
      return true;
    }
  }

  if (Request == SourceKitRequest::InterfaceGenOpen && isAsyncRequest) {
    llvm::errs()
        << "error: cannot use -async with interface-gen-open request\n";
    return true;
  }

  return false;
}

void TestOptions::printHelp(bool ShowHidden) const {

  // Based off of swift/lib/Driver/Driver.cpp, at Driver::printHelp
  //FIXME: should we use IncludedFlagsBitmask and ExcludedFlagsBitmask?
  // Maybe not for modes such as Interactive, Batch, AutolinkExtract, etc,
  // as in Driver.cpp. But could be useful for extra info, like HelpHidden.

  TestOptTable Table;

  Table.printHelp(llvm::outs(), "sourcekitd-test [options] <inputs>",
                  "SourceKit Testing Tool", ShowHidden);
}
