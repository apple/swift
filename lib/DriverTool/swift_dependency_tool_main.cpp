//===--- swift-dependency-tool.cpp - Convert binary swiftdeps to YAML -----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/FileSystem.h"
#include "swift/AST/FineGrainedDependencies.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/FineGrainedDependencyFormat.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/LLVMInitialize.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualOutputBackends.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"

using namespace swift;
using namespace fine_grained_dependencies;

//==============================================================================
// MARK: SourceFileDepGraph YAML reading & writing
//==============================================================================

// This introduces a redefinition wherever std::is_same_t<size_t, uint64_t>
// holds.
#if !(defined(__linux__) || defined(_WIN64) || defined(__FreeBSD__))
LLVM_YAML_DECLARE_SCALAR_TRAITS(size_t, QuotingType::None)
#endif
LLVM_YAML_DECLARE_ENUM_TRAITS(swift::fine_grained_dependencies::NodeKind)
LLVM_YAML_DECLARE_ENUM_TRAITS(swift::fine_grained_dependencies::DeclAspect)
LLVM_YAML_DECLARE_MAPPING_TRAITS(
    swift::fine_grained_dependencies::DependencyKey)
LLVM_YAML_DECLARE_MAPPING_TRAITS(swift::fine_grained_dependencies::DepGraphNode)

namespace llvm {
namespace yaml {
template <>
struct MappingContextTraits<
    swift::fine_grained_dependencies::SourceFileDepGraphNode,
    swift::fine_grained_dependencies::SourceFileDepGraph> {
  using SourceFileDepGraphNode =
      swift::fine_grained_dependencies::SourceFileDepGraphNode;
  using SourceFileDepGraph =
      swift::fine_grained_dependencies::SourceFileDepGraph;

  static void mapping(IO &io, SourceFileDepGraphNode &node,
                      SourceFileDepGraph &g);
};

template <>
struct SequenceTraits<
    std::vector<swift::fine_grained_dependencies::SourceFileDepGraphNode *>> {
  using SourceFileDepGraphNode =
      swift::fine_grained_dependencies::SourceFileDepGraphNode;
  using NodeVec = std::vector<SourceFileDepGraphNode *>;
  static size_t size(IO &, NodeVec &vec);
  static SourceFileDepGraphNode &element(IO &, NodeVec &vec, size_t index);
};

template <> struct ScalarTraits<swift::Fingerprint> {
  static void output(const swift::Fingerprint &fp, void *c, raw_ostream &os) {
    os << fp.getRawValue();
  }
  static StringRef input(StringRef s, void *, swift::Fingerprint &fp) {
    if (auto convertedFP = swift::Fingerprint::fromString(s))
      fp = convertedFP.value();
    else {
      llvm::errs() << "Failed to convert fingerprint '" << s << "'\n";
      exit(1);
    }
    return StringRef();
  }
  static QuotingType mustQuote(StringRef S) { return needsQuotes(S); }
};

} // namespace yaml
} // namespace llvm

LLVM_YAML_DECLARE_MAPPING_TRAITS(
    swift::fine_grained_dependencies::SourceFileDepGraph)

namespace llvm {
namespace yaml {
// This introduces a redefinition wherever std::is_same_t<size_t, uint64_t>
// holds.
#if !(defined(__linux__) || defined(_WIN64) || defined(__FreeBSD__))
void ScalarTraits<size_t>::output(const size_t &Val, void *, raw_ostream &out) {
  out << Val;
}

StringRef ScalarTraits<size_t>::input(StringRef scalar, void *ctxt,
                                      size_t &value) {
  return scalar.getAsInteger(10, value) ? "could not parse size_t" : "";
}
#endif

void ScalarEnumerationTraits<swift::fine_grained_dependencies::NodeKind>::
    enumeration(IO &io, swift::fine_grained_dependencies::NodeKind &value) {
  using NodeKind = swift::fine_grained_dependencies::NodeKind;
  io.enumCase(value, "topLevel", NodeKind::topLevel);
  io.enumCase(value, "nominal", NodeKind::nominal);
  io.enumCase(value, "potentialMember", NodeKind::potentialMember);
  io.enumCase(value, "member", NodeKind::member);
  io.enumCase(value, "dynamicLookup", NodeKind::dynamicLookup);
  io.enumCase(value, "externalDepend", NodeKind::externalDepend);
  io.enumCase(value, "sourceFileProvide", NodeKind::sourceFileProvide);
}

void ScalarEnumerationTraits<DeclAspect>::enumeration(
    IO &io, swift::fine_grained_dependencies::DeclAspect &value) {
  using DeclAspect = swift::fine_grained_dependencies::DeclAspect;
  io.enumCase(value, "interface", DeclAspect::interface);
  io.enumCase(value, "implementation", DeclAspect::implementation);
}

void MappingTraits<DependencyKey>::mapping(
    IO &io, swift::fine_grained_dependencies::DependencyKey &key) {
  io.mapRequired("kind", key.kind);
  io.mapRequired("aspect", key.aspect);
  io.mapRequired("context", key.context);
  io.mapRequired("name", key.name);
}

void MappingTraits<DepGraphNode>::mapping(
    IO &io, swift::fine_grained_dependencies::DepGraphNode &node) {
  io.mapRequired("key", node.key);
  io.mapOptional("fingerprint", node.fingerprint);
}

void MappingContextTraits<SourceFileDepGraphNode, SourceFileDepGraph>::mapping(
    IO &io, SourceFileDepGraphNode &node, SourceFileDepGraph &g) {
  MappingTraits<DepGraphNode>::mapping(io, node);
  io.mapRequired("sequenceNumber", node.sequenceNumber);
  std::vector<size_t> defsIDependUponVec(node.defsIDependUpon.begin(),
                                         node.defsIDependUpon.end());
  io.mapRequired("defsIDependUpon", defsIDependUponVec);
  io.mapRequired("isProvides", node.isProvides);
  if (!io.outputting()) {
    for (size_t u : defsIDependUponVec)
      node.defsIDependUpon.insert(u);
  }
  assert(g.getNode(node.sequenceNumber) && "Bad sequence number");
}

size_t SequenceTraits<std::vector<SourceFileDepGraphNode *>>::size(
    IO &, std::vector<SourceFileDepGraphNode *> &vec) {
  return vec.size();
}

SourceFileDepGraphNode &
SequenceTraits<std::vector<SourceFileDepGraphNode *>>::element(
    IO &, std::vector<SourceFileDepGraphNode *> &vec, size_t index) {
  while (vec.size() <= index)
    vec.push_back(new SourceFileDepGraphNode());
  return *vec[index];
}

void MappingTraits<SourceFileDepGraph>::mapping(IO &io, SourceFileDepGraph &g) {
  io.mapRequired("allNodes", g.allNodes, g);
}
} // namespace yaml
} // namespace llvm

enum class ActionType : unsigned {
  None,
  BinaryToYAML,
  YAMLToBinary
};

int swift_dependency_tool_main(ArrayRef<const char *> argv, void *MainAddr) {
  INITIALIZE_LLVM();

  llvm::cl::OptionCategory Category("swift-dependency-tool Options");

  llvm::cl::opt<std::string>
  InputFilename("input-filename",
                llvm::cl::desc("Name of the input file"),
                llvm::cl::cat(Category));

  llvm::cl::opt<std::string>
  OutputFilename("output-filename",
                 llvm::cl::desc("Name of the output file"),
                 llvm::cl::cat(Category));

  llvm::cl::opt<ActionType>
  Action(llvm::cl::desc("Mode:"), llvm::cl::init(ActionType::None),
         llvm::cl::cat(Category),
         llvm::cl::values(
             clEnumValN(ActionType::BinaryToYAML,
                        "to-yaml", "Convert new binary .swiftdeps format to YAML"),
             clEnumValN(ActionType::YAMLToBinary,
                        "from-yaml", "Convert YAML to new binary .swiftdeps format")));

  llvm::cl::HideUnrelatedOptions(Category);
  llvm::cl::ParseCommandLineOptions(argv.size(), argv.data(), "Swift Dependency Tool\n");

  SourceManager sourceMgr;
  DiagnosticEngine diags(sourceMgr);
  llvm::vfs::OnDiskOutputBackend outputBackend;

  switch (Action) {
  case ActionType::None: {
    llvm::errs() << "action required\n";
    llvm::cl::PrintHelpMessage();
    return 1;
  }

  case ActionType::BinaryToYAML: {
    auto fg = SourceFileDepGraph::loadFromPath(InputFilename, true);
    if (!fg) {
      llvm::errs() << "Failed to read dependency file\n";
      return 1;
    }

    bool hadError =
      withOutputPath(diags, outputBackend, OutputFilename,
        [&](llvm::raw_pwrite_stream &out) {
          out << "# Fine-grained v0\n";
          llvm::yaml::Output yamlWriter(out);
          yamlWriter << *fg;
          return false;
        });

    if (hadError) {
      llvm::errs() << "Failed to write YAML swiftdeps\n";
    }
    break;
  }

  case ActionType::YAMLToBinary: {
    auto bufferOrError = llvm::MemoryBuffer::getFile(InputFilename);
    if (!bufferOrError) {
      llvm::errs() << "Failed to read dependency file\n";
      return 1;
    }

    auto &buffer = *bufferOrError.get();

    SourceFileDepGraph fg;
    llvm::yaml::Input yamlReader(llvm::MemoryBufferRef(buffer), nullptr);
    yamlReader >> fg;
    if (yamlReader.error()) {
      llvm::errs() << "Failed to parse YAML swiftdeps\n";
      return 1;
    }

    if (writeFineGrainedDependencyGraphToPath(
            diags, outputBackend, OutputFilename, fg)) {
      llvm::errs() << "Failed to write binary swiftdeps\n";
      return 1;
    }

    break;
  }
  }

  return 0;
}
