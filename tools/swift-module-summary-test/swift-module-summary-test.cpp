//===--- swift-module-summary-test.cpp - Test util for module summary file-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Testing utility for the .swiftmodulesummary file.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/FileSystem.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Serialization/ModuleSummary.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"

using namespace swift;
using namespace modulesummary;

enum class ActionType : unsigned {
  None,
  BinaryToYAML,
  YAMLToBinary,
};

namespace options {

static llvm::cl::OptionCategory Category("swift-module-summary-test Options");

static llvm::cl::opt<std::string>
    InputFilename(llvm::cl::Positional, llvm::cl::desc("<input file>"),
                  llvm::cl::init("-"), llvm::cl::value_desc("filename"));

static llvm::cl::opt<std::string>
    OutputFilename("o", llvm::cl::desc("Override output filename"),
                   llvm::cl::value_desc("filename"));

static llvm::cl::opt<ActionType>
    Action(llvm::cl::desc("Mode:"), llvm::cl::init(ActionType::None),
           llvm::cl::cat(Category),
           llvm::cl::values(
               clEnumValN(ActionType::BinaryToYAML, "to-yaml",
                          "Convert new binary .swiftmodulesummary format to YAML"),
               clEnumValN(ActionType::YAMLToBinary, "from-yaml",
                          "Convert YAML to new binary .swiftmodulesummary format")));

} // namespace options

LLVM_YAML_DECLARE_MAPPING_TRAITS(modulesummary::ModuleSummaryIndex)
LLVM_YAML_DECLARE_MAPPING_TRAITS(modulesummary::FunctionSummary)
LLVM_YAML_DECLARE_MAPPING_TRAITS(modulesummary::FunctionSummary::Call)
LLVM_YAML_IS_SEQUENCE_VECTOR(modulesummary::FunctionSummary::Call)
LLVM_YAML_DECLARE_ENUM_TRAITS(modulesummary::FunctionSummary::Call::KindTy)

namespace llvm {
namespace yaml {

template <> struct MappingTraits<std::unique_ptr<FunctionSummary>> {
  static void mapping(IO &io, std::unique_ptr<FunctionSummary> &Ptr) {
    if (!Ptr) {
      Ptr.reset(new FunctionSummary());
    }
    MappingTraits<FunctionSummary>::mapping(io, *Ptr.get());
  }
};

void ScalarEnumerationTraits<FunctionSummary::Call::KindTy>::enumeration(
    IO &io, FunctionSummary::Call::KindTy &V) {
  using Kind = FunctionSummary::Call::KindTy;
  io.enumCase(V, "direct", Kind::Direct);
  io.enumCase(V, "vtable", Kind::VTable);
  io.enumCase(V, "witness", Kind::Witness);
}

void MappingTraits<FunctionSummary::Call>::mapping(IO &io,
                                                   FunctionSummary::Call &V) {
  io.mapRequired("callee_name", V.Name);
  io.mapRequired("callee_guid", V.Callee);
  io.mapRequired("kind", V.Kind);
}

void MappingTraits<FunctionSummary>::mapping(IO &io, FunctionSummary &V) {
  io.mapRequired("name", V.Name);
  io.mapRequired("guid", V.Guid);
  io.mapRequired("live", V.Flags.Live);
  io.mapRequired("preserved", V.Flags.Preserved);
  io.mapRequired("calls", V.CallGraphEdgeList);
}

template <> struct CustomMappingTraits<FunctionSummaryMapTy> {
  static void inputOne(IO &io, StringRef Key, FunctionSummaryMapTy &V) {
    GUID KeyInt;
    if (Key.getAsInteger(0, KeyInt)) {
      io.setError("key not an integer");
      return;
    }
    io.mapRequired(Key.str().c_str(), V[KeyInt]);
  }
  static void output(IO &io, FunctionSummaryMapTy &V) {
    for (auto &P : V)
      io.mapRequired(llvm::utostr(P.first).c_str(), P.second);
  }
};

template <> struct CustomMappingTraits<VFuncToImplsMapTy> {
  static void inputOne(IO &io, StringRef Key, VFuncToImplsMapTy &V) {
    GUID KeyInt;
    if (Key.getAsInteger(0, KeyInt)) {
      io.setError("key not an integer");
      return;
    }
    io.mapRequired(Key.str().c_str(), V[KeyInt]);
  }
  static void output(IO &io, VFuncToImplsMapTy &V) {
    for (auto &P : V)
      io.mapRequired(llvm::utostr(P.first).c_str(), P.second);
  }
};

void MappingTraits<ModuleSummaryIndex>::mapping(IO &io, ModuleSummaryIndex &V) {
  io.mapRequired("module_name", V.Name);
  io.mapRequired("functions", V.FunctionSummaryMap);
  io.mapRequired("witness_tables", V.WitnessTableMethodMap);
  io.mapRequired("vtables", V.VTableMethodMap);
}
} // namespace yaml
} // namespace llvm

int main(int argc, char *argv[]) {
  PROGRAM_START(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv, "Swift Module Summary Test\n");

  StringRef fname = options::InputFilename;
  SourceManager sourceMgr;
  DiagnosticEngine diags(sourceMgr);

  auto fileBufOrErr = llvm::MemoryBuffer::getFile(fname);
  if (!fileBufOrErr) {
    llvm::errs() << "error opening file '" << fname
                 << "': " << fileBufOrErr.getError().message();
    return 1;
  }

  switch (options::Action) {
  case ActionType::None: {
    llvm::errs() << "action required\n";
    llvm::cl::PrintHelpMessage();
    return 1;
  }
  case ActionType::BinaryToYAML: {
    modulesummary::ModuleSummaryIndex summary;
    modulesummary::loadModuleSummaryIndex(fileBufOrErr.get()->getMemBufferRef(),
                                          summary);

    bool hadError = withOutputFile(diags, options::OutputFilename,
                                   [&](llvm::raw_pwrite_stream &out) {
                                     out << "# Module-summary v0\n";
                                     llvm::yaml::Output yamlWriter(out);
                                     yamlWriter << summary;
                                     return false;
                                   });

    if (hadError) {
      llvm::errs() << "Failed to write YAML swiftmodulesummary\n";
    }
    break;
  }
  case ActionType::YAMLToBinary:
    ModuleSummaryIndex summary;
    llvm::yaml::Input yamlReader(fileBufOrErr.get()->getMemBufferRef(),
                                 nullptr);
    yamlReader >> summary;
    if (yamlReader.error()) {
      llvm::errs() << "Failed to parse YAML swiftmodulesummary\n";
      return 1;
    }

    if (writeModuleSummaryIndex(summary, diags, options::OutputFilename)) {
      llvm::errs() << "Failed to write binary module summary\n";
      return 1;
    }
    break;
  }
  return 0;
}
