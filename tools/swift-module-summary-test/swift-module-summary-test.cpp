//===--- swift-module-summary-test.cpp - Test util for C parser library ---===//
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
// Testing utility for the C API of the parser library.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/FileSystem.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Serialization/ModuleSummary.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/GenericDomTree.h"
#include "llvm/Support/GenericDomTreeConstruction.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"
#include <iomanip>

using namespace swift;
using namespace modulesummary;

enum class ActionType : unsigned {
  None,
  BinaryToYAML,
  YAMLToBinary,
  BinaryToDominators,
  BinaryToDominatorTree,
  BinaryToDot,
};

namespace options {

static llvm::cl::OptionCategory Category("swift-module-summary-test Options");

static llvm::cl::opt<std::string>
    InputFilename(llvm::cl::Positional, llvm::cl::desc("<input file>"),
                  llvm::cl::init("-"), llvm::cl::value_desc("filename"));

static llvm::cl::opt<std::string>
    OutputFilename("o", llvm::cl::desc("Override output filename"),
                   llvm::cl::value_desc("filename"));

static llvm::cl::opt<bool>
    OmitDeadSymbol("omit-dead-symbol", llvm::cl::init(false),
                   llvm::cl::desc("Omit dead symbols"));

static llvm::cl::opt<ActionType> Action(
    llvm::cl::desc("Mode:"), llvm::cl::init(ActionType::None),
    llvm::cl::cat(Category),
    llvm::cl::values(
        clEnumValN(ActionType::BinaryToYAML, "to-yaml",
                   "Convert new binary .swiftmodule.summary format to YAML"),
        clEnumValN(ActionType::YAMLToBinary, "from-yaml",
                   "Convert YAML to new binary .swiftmodule.summary format"),
        clEnumValN(ActionType::BinaryToDominators, "binary-to-dom",
                   "Convert new binary .swiftmodule.summary format to "
                   "dominators list"),
        clEnumValN(
            ActionType::BinaryToDominatorTree, "binary-to-dom-tree",
            "Convert new binary .swiftmodule.summary format to dominator tree"),
        clEnumValN(ActionType::BinaryToDot, "binary-to-dot",
                   "Convert new binary .swiftmodule.summary format to Dot")));

} // namespace options

LLVM_YAML_DECLARE_MAPPING_TRAITS(modulesummary::ModuleSummaryIndex)
LLVM_YAML_DECLARE_MAPPING_TRAITS(modulesummary::FunctionSummary)
LLVM_YAML_DECLARE_MAPPING_TRAITS(modulesummary::FunctionSummary::Call)
LLVM_YAML_DECLARE_MAPPING_TRAITS(modulesummary::FunctionSummary::TypeRef)
LLVM_YAML_DECLARE_MAPPING_TRAITS(modulesummary::VFuncImpl)
LLVM_YAML_IS_SEQUENCE_VECTOR(modulesummary::FunctionSummary::Call)
LLVM_YAML_IS_SEQUENCE_VECTOR(modulesummary::FunctionSummary::TypeRef)
LLVM_YAML_IS_SEQUENCE_VECTOR(modulesummary::VFuncImpl)
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

void MappingTraits<FunctionSummary::TypeRef>::mapping(IO &io,
                                                      FunctionSummary::TypeRef &V) {
  io.mapRequired("name", V.Name);
  io.mapRequired("guid", V.Guid);
}
void MappingTraits<FunctionSummary>::mapping(IO &io, FunctionSummary &V) {
  io.mapRequired("name", V.Name);
  io.mapRequired("guid", V.Guid);
  io.mapRequired("live", V.Flags.Live);
  io.mapRequired("preserved", V.Flags.Preserved);
  io.mapRequired("calls", V.CallGraphEdgeList);
  io.mapRequired("type_refs", V.TypeRefList);
  io.mapRequired("inst_size", V.InstSize);
}

template <>
struct CustomMappingTraits<FunctionSummaryMapTy> {
  static void inputOne(IO &io, StringRef Key,
                       FunctionSummaryMapTy &V) {
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

void MappingTraits<VFuncImpl>::mapping(IO &io, VFuncImpl &V) {
  io.mapRequired("guid", V.Guid);
  io.mapRequired("type_guid", V.TypeGuid);
}

template <>
struct CustomMappingTraits<VFuncToImplsMapTy> {
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
  io.mapRequired("used_types", V.UsedTypeList);
}
} // namespace yaml
} // namespace llvm

VFuncSlot createVFuncSlot(FunctionSummary::Call call) {
  VFuncSlot::KindTy slotKind;
  switch (call.getKind()) {
    case FunctionSummary::Call::Witness: {
      slotKind = VFuncSlot::Witness;
      break;
    }
    case FunctionSummary::Call::VTable: {
      slotKind = VFuncSlot::VTable;
      break;
    }
    case FunctionSummary::Call::Direct: {
      llvm_unreachable("Can't get slot for static call");
    }
    case FunctionSummary::Call::kindCount: {
      llvm_unreachable("impossible");
    }
  }
  return VFuncSlot(slotKind, call.getCallee());
}

struct CallGraph {
  struct Node;

  struct Edge {
    FunctionSummary::Call Call;
    GUID Target;
  };

  struct Node {
    GUID Guid;
    CallGraph *Parent;
    std::vector<Node *> Children;
    std::vector<Node *> PredChildren;
    std::vector<Edge> Edges;

    CallGraph *getParent() const { return Parent; }

    FunctionSummary *getFS() const {
      return Parent->Summary.getFunctionSummary(Guid);
    }

    void printAsOperand(raw_ostream &OS, bool /*PrintType*/) {
      FunctionSummary *FS = Parent->Summary.getFunctionSummary(Guid);
      if (!FS) {
        OS << "Entry node";
        return;
      }
      OS << "Fn#" << FS->getName();
    }
  };

  using child_iterator = std::vector<Node *>::iterator;

  std::vector<Node> Nodes;
  Node EntryNode;
  ModuleSummaryIndex Summary;
  using iterator = std::vector<Node>::iterator;
  CallGraph(ModuleSummaryIndex);
};

CallGraph::CallGraph(ModuleSummaryIndex Summary) : Nodes(), EntryNode() {
  std::map<GUID, Node *> NodeMap;
  Nodes.resize(Summary.functions_size());
  int idx = 0;
  for (auto FI = Summary.functions_begin(), FE = Summary.functions_end();
       FI != FE; ++FI) {
    Node *node = &Nodes[idx++];
    node->Guid = FI->first;
    node->Parent = this;
    NodeMap[FI->first] = node;
    if (FI->second->isPreserved()) {
      EntryNode.Children.push_back(node);
      EntryNode.PredChildren.push_back(node);
    }
  }
  EntryNode.Parent = this;

  for (Node &node : Nodes) {
    FunctionSummary *FS = Summary.getFunctionSummary(node.Guid);
    for (FunctionSummary::Call call : FS->calls()) {
      switch (call.getKind()) {
      case FunctionSummary::Call::Witness:
      case FunctionSummary::Call::VTable: {
        VFuncSlot slot = createVFuncSlot(call);
        for (auto Impl : Summary.getImplementations(slot)) {
          Node *CalleeNode = NodeMap[Impl.Guid];
          node.Children.push_back(CalleeNode);
          node.PredChildren.push_back(CalleeNode);
          node.Edges.push_back({call, Impl.Guid});
        }
        break;
      }
      case FunctionSummary::Call::Direct: {
        Node *CalleeNode = NodeMap[call.getCallee()];
        node.Children.push_back(CalleeNode);
        node.PredChildren.push_back(CalleeNode);
        node.Edges.push_back({call, call.getCallee()});
        break;
      }
      case FunctionSummary::Call::kindCount:
        llvm_unreachable("impossible");
      }
    }
  }
  this->Summary = std::move(Summary);
}

namespace llvm {

  template <> struct GraphTraits<CallGraph::Node *> {
    typedef CallGraph::child_iterator ChildIteratorType;
    typedef CallGraph::Node *NodeRef;
  
    static NodeRef getEntryNode(NodeRef N) { return N; }
    static inline ChildIteratorType child_begin(NodeRef N) {
      return N->Children.begin();
    }
    static inline ChildIteratorType child_end(NodeRef N) {
      return N->Children.end();
    }
  };

  template <> struct GraphTraits<Inverse<CallGraph::Node *>> {
    using ChildIteratorType = CallGraph::child_iterator;
    using Node = CallGraph::Node;
    using NodeRef = Node *;
    static NodeRef getEntryNode(Inverse<CallGraph::Node *> G) {
      return G.Graph;
    }
    static inline ChildIteratorType child_begin(NodeRef N) {
      return N->PredChildren.begin();
    }
    static inline ChildIteratorType child_end(NodeRef N) {
      return N->PredChildren.end();
    }
  };

  template <> struct GraphTraits<CallGraph *>
  : public GraphTraits<CallGraph::Node *> {
    typedef CallGraph *GraphType;
    typedef CallGraph::Node *NodeRef;

    static NodeRef getEntryNode(GraphType G) { return &G->EntryNode; }

    typedef pointer_iterator<CallGraph::iterator> nodes_iterator;
    static nodes_iterator nodes_begin(GraphType CG) {
      return nodes_iterator(CG->Nodes.begin());
    }
    static nodes_iterator nodes_end(GraphType CG) {
      return nodes_iterator(CG->Nodes.end());
    }
    static unsigned size(GraphType CG) { return CG->Nodes.size(); }
  };
  
  /// This is everything the llvm::GraphWriter needs to write the call graph in
  /// a dot file.
  template <>
  struct DOTGraphTraits<CallGraph *> : public DefaultDOTGraphTraits {
  
    DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(isSimple) {}
  
    std::string getNodeLabel(const CallGraph::Node *Node,
                             const CallGraph *Graph) {
      FunctionSummary *FS = Graph->Summary.getFunctionSummary(Node->Guid);
      Demangle::Context DCtx;
      return DCtx.demangleSymbolAsString(FS->getName());
    }

    static std::string getNodeAttributes(const CallGraph::Node *Node,
                                         const CallGraph *Graph) {
      FunctionSummary *FS = Graph->Summary.getFunctionSummary(Node->Guid);
      std::string color = FS->isLive() ? "green" : "red";
      std::string attrs = "color=\"" + color + "\"";
      return attrs;
    }

    static std::string getEdgeAttributes(const CallGraph::Node *Node,
                                         CallGraph::child_iterator I,
                                         const CallGraph *Graph) {
      unsigned ChildIdx = I - Node->Children.begin();
      std::string Label;
      raw_string_ostream O(Label);
      Demangle::Context DCtx;
      CallGraph::Edge edge = Node->Edges[ChildIdx];
      FunctionSummary::Call call = edge.Call;
      std::string demangled = DCtx.demangleSymbolAsString(call.getName());
      O << "label=\"";
      switch (call.getKind()) {
      case FunctionSummary::Call::Witness: {
        O << "(W) " << demangled;
        break;
      }
      case FunctionSummary::Call::VTable: {
        O << "(V) " << demangled;
        break;
      }
      case FunctionSummary::Call::Direct: {
        O << "(D)";
        break;
      }
      case FunctionSummary::Call::kindCount:
        llvm_unreachable("impossible");
      }
      O << "\"";
      return Label;
    }
  };
} // namespace llvm
using DomCallTreeBase = llvm::DominatorTreeBase<CallGraph::Node, false>;
using DomCallInfoNode = llvm::DomTreeNodeBase<CallGraph::Node>;

namespace llvm {

template <> struct GraphTraits<DomCallInfoNode *> {
  using ChildIteratorType = DomCallInfoNode::iterator;
  using NodeRef = DomCallInfoNode *;

  static NodeRef getEntryNode(NodeRef N) { return N; }
  static inline ChildIteratorType child_begin(NodeRef N) { return N->begin(); }
  static inline ChildIteratorType child_end(NodeRef N) { return N->end(); }
};

template <> struct GraphTraits<const DomCallInfoNode *> {
  using ChildIteratorType = DomCallInfoNode::const_iterator;
  using NodeRef = const DomCallInfoNode *;

  static NodeRef getEntryNode(NodeRef N) { return N; }
  static inline ChildIteratorType child_begin(NodeRef N) { return N->begin(); }
  static inline ChildIteratorType child_end(NodeRef N) { return N->end(); }
};

namespace DomTreeBuilder {
extern template void Calculate<DomCallTreeBase>(DomCallTreeBase &DT);
template void Calculate<DomCallTreeBase>(DomCallTreeBase &DT);
} // namespace DomTreeBuilder
} // namespace llvm

class DomCallTree : public DomCallTreeBase {
  using super = DominatorTreeBase;

public:
  DomCallTree(CallGraph &CG) : DominatorTreeBase() { recalculate(CG); }
};

class RetainedSizeCalculator {
  struct Entry {
    DomCallInfoNode *DomNode;
    uint32_t InstSize;
  };
  std::map<DomCallInfoNode *, uint32_t> SizeCache;
  std::vector<Entry> Entries;

  uint32_t getInstSize(DomCallInfoNode *domNode) {
    auto FS = domNode->getBlock()->getFS();
    uint32_t size = FS->getInstSize();
    for (auto I = domNode->begin(), E = domNode->end(); I != E; ++I) {
      auto Child = *I;
      size += getInstSize(Child);
    }
    SizeCache[domNode] = size;
    return size;
  }

  void calculateRecursively(DomCallInfoNode *domNode) {
    Entries.push_back({domNode, getInstSize(domNode)});
    for (auto child : domNode->children()) {
      calculateRecursively(child);
    }
  }

public:
  void calculate(DomCallInfoNode *root) {
    for (auto child : root->children()) {
      calculateRecursively(child);
    }
    std::sort(Entries.begin(), Entries.end(),
              [](Entry lhs, Entry rhs) { return lhs.InstSize > rhs.InstSize; });
  }

  void display(llvm::raw_ostream &O) {
    O << "size\t| symbol\n";
    for (auto entry : Entries) {
      auto FS = entry.DomNode->getBlock()->getFS();
      O << entry.InstSize << "\t| " << FS->getName() << "\n";
    }
  }

  void displayTreeRec(llvm::raw_ostream &O, uint32_t TotalSize,
                      DomCallInfoNode *Root, unsigned Lev) {
    auto Size = getInstSize(Root);
    auto Percent = (double(Size) / TotalSize * 100);
    auto FS = Root->getBlock()->getFS();
    O << Size << "\t| ";
    llvm::format_provider<double>::format(Percent, O, "f2");
    O << "\t| ";
    O.indent(2 * Lev) << FS->getName() << "\n";

    auto Children = Root->children();
    std::sort(Children.begin(), Children.end(),
              [&](DomCallInfoNode *lhs, DomCallInfoNode *rhs) {
                return getInstSize(lhs) > getInstSize(rhs);
              });
    for (auto Child : Children) {
      displayTreeRec(O, TotalSize, Child, Lev + 1);
    }
  }
  void displayTree(llvm::raw_ostream &O, DomCallInfoNode *Root) {
    O << "size\t| %\t| symbol\n";
    uint32_t TotalSize = 0;
    for (auto Child : Root->children()) {
      TotalSize += getInstSize(Child);
    }
    auto Children = Root->children();
    std::sort(Children.begin(), Children.end(),
              [&](DomCallInfoNode *lhs, DomCallInfoNode *rhs) {
                return getInstSize(lhs) > getInstSize(rhs);
              });
    for (auto Child : Children) {
      displayTreeRec(O, TotalSize, Child, 0);
    }
  }
};

int dominance_analysis(CallGraph &CG) {
  DomCallTree DT(CG);
  DT.print(llvm::dbgs());
  DomCallInfoNode *Root = DT.getRootNode();
  RetainedSizeCalculator calculator;
  calculator.calculate(Root);
  calculator.display(llvm::dbgs());
  return 0;
};

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
      llvm::errs() << "Failed to write YAML swiftdeps\n";
    }
    break;
  }
  case ActionType::YAMLToBinary: {
    ModuleSummaryIndex summary;
    llvm::yaml::Input yamlReader(fileBufOrErr.get()->getMemBufferRef(),
                                 nullptr);
    yamlReader >> summary;
    if (yamlReader.error()) {
      llvm::errs() << "Failed to parse YAML swiftdeps\n";
      return 1;
    }

    if (writeModuleSummaryIndex(summary, diags, options::OutputFilename)) {
      llvm::errs() << "Failed to write binary module summary\n";
      return 1;
    }
    break;
  }
  case ActionType::BinaryToDot: {
    modulesummary::ModuleSummaryIndex summary;
    modulesummary::loadModuleSummaryIndex(fileBufOrErr.get()->getMemBufferRef(),
                                          summary);
    CallGraph CG(std::move(summary));
    withOutputFile(diags, options::OutputFilename, [&](raw_ostream &out) {
      llvm::WriteGraph(out, &CG);
      return false;
    });
    break;
  }
  case ActionType::BinaryToDominators:
  case ActionType::BinaryToDominatorTree: {
    modulesummary::ModuleSummaryIndex summary;
    modulesummary::loadModuleSummaryIndex(fileBufOrErr.get()->getMemBufferRef(),
                                          summary);
    CallGraph CG(std::move(summary));
    DomCallTree DT(CG);
    DomCallInfoNode *Root = DT.getRootNode();
    RetainedSizeCalculator calculator;
    calculator.calculate(Root);
    withOutputFile(diags, options::OutputFilename, [&](raw_ostream &out) {
      if (options::Action == ActionType::BinaryToDominators) {
        calculator.display(out);
      } else {
        calculator.displayTree(out, Root);
      }
      return false;
    });
    break;
  }
  }
  return 0;
}
