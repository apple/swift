//===--- LLVMInlineTree.cpp - Prints the inline tree ----------------------===//
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
//
// This pass prints the tree of inlined instructions. It also prints a sorted
// table containing the contribution to the code size increase for all inlined
// functions.
// The output is only an estimation because all printed numbers are LLVM
// instruction counts rather than real code size bytes.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "llvm-inlinetree"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Range.h"
#include "swift/Demangling/Demangle.h"
#include "swift/LLVMPasses/Passes.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace swift;

//===----------------------------------------------------------------------===//
//                            LLVMInlineTree Pass
//===----------------------------------------------------------------------===//

char InlineTreePrinter::ID = 0;

INITIALIZE_PASS_BEGIN(InlineTreePrinter,
                      "inline-tree-printer", "Inline tree printer pass",
                      false, false)
INITIALIZE_PASS_END(InlineTreePrinter,
                    "inline-tree-printer", "Inline tree printer pass",
                    false, false)

llvm::cl::opt<bool>
InlineTreeNoDemangle("inline-tree-no-demangle", llvm::cl::init(false),
              llvm::cl::desc("Don't demangle symbols in inline tree output"));


ModulePass *swift::createInlineTreePrinterPass() {
  initializeInlineTreePrinterPass(*PassRegistry::getPassRegistry());
  return new InlineTreePrinter();
}

void InlineTreePrinter::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

namespace {

class InlineTree {
  struct Node;

  using NodeMap = DenseMap<StringRef, Node *>;
  using NodeList = SmallVector<Node *, 8>;

  /// Defines a unique inline location.
  /// Used to distinguish between different instances of an inlined function.
  struct LocationKey {
    unsigned line;
    unsigned column;
    void *file;

    LocationKey(DILocation *DL) :
      line(DL->getLine()), column(DL->getColumn()), file(DL->getFile()) {
    }

    bool operator==(const LocationKey &RHS) const {
      return line == RHS.line && column == RHS.column && file == RHS.file;
    }

    bool operator<(const LocationKey &RHS) const {
      if (line != RHS.line)
        return line < RHS.line;
      if (column != RHS.column)
        return column < RHS.column;
      return file < RHS.file;
    }
  };

  /// Represents a function or inlined function. There may be multiple nodes
  /// for a function in case the function is inlined into different callers.
  struct Node {
    StringRef FunctionName;

    /// Number of inlined instructions of this function in its caller.
    int numSelfInsts = 0;

    /// numSelfInsts + the callee-instructions inlined into this function.
    int numTotalInsts = 0;

    /// Callees which are inlined into this function.
    NodeMap UnsortedChildren;

    /// Contains all nodes of UnsortedChildren, but sorted by numTotalInsts.
    NodeList SortedChildren;

    /// Unique inline locations. The size is the number of times this function
    /// is inlined into its caller.
    SmallSet<LocationKey, 16> UniqueLocations;

    /// If true, this is a "real" function not an inlined one.
    bool isTopLevel = false;

    const NodeList &getChildren() {
      if (SortedChildren.empty() && !UnsortedChildren.empty())
        sortNodes(UnsortedChildren, SortedChildren);
      return SortedChildren;
    }

    Node(StringRef FunctionName) : FunctionName(FunctionName) {}
  };

  /// The summary for a function. It contains the total overhead of inlining
  /// the function. It is the summarized numbers of all nodes referring to the
  /// function minus the size of the original function (which is not inlined).
  struct Summary {
    StringRef FunctionName;
    unsigned totalInstOverhead = 0;
    unsigned selfInstOverhead = 0;
    unsigned instanceOverhead = 0;

    Summary(StringRef FunctionName) : FunctionName(FunctionName) { }
  };

  SpecificBumpPtrAllocator<Node> NodeAllocator;

  /// Top level functions in the module (= not inlined functions).
  NodeList FunctionRootNodes;

  /// Mapping for the top level functions.
  NodeMap Functions2Nodes;

  /// All nodes: top level functions + inlined functions.
  NodeList allNodes;

  /// The total number of LLVM instructions in the module.
  unsigned totalNumberOfInstructions = 0;

  Node *getNode(StringRef FunctionName, NodeMap &Nodes) {
    Node *&Nd = Nodes[FunctionName];
    if (!Nd) {
      Nd = new (NodeAllocator.Allocate()) Node(FunctionName);
      allNodes.push_back(Nd);
    }
    return Nd;
  }

  /// Create a sorted list of nodes according to numTotalInsts.
  static void sortNodes(const NodeMap &Map, NodeList &Result);

  /// Build the inline-tree for a function.
  void buildTree(Function *F);

  /// Returns a printable percent string of the \p numInsts compared to
  /// totalNumberOfInstructions.
  std::string getPercent(int numInsts) const;

  /// Recursively print the node tree starting at \p Nd.
  void printNode(Node *Nd, int indent, raw_ostream &os);

public:
  /// Build the inline tree.
  void build(Module *M);

  /// Print the inline tree.
  void print(raw_ostream &os);
};

/// Print the function \p Name as simplified demangled name or optionally
/// as not demangled name.
static void printSymbol(StringRef Name, raw_ostream &os) {
  if (InlineTreeNoDemangle) {
    os << Name;
  } else {
    os << demangleSymbolAsString(Name,
                   Demangle::DemangleOptions::SimplifiedUIDemangleOptions());
  }
}

void InlineTree::sortNodes(const NodeMap &Map, NodeList &Result) {
  for (auto Iter : Map) {
    Node *Nd = Iter.second;
    Result.push_back(Nd);
  }

  std::sort(Result.begin(), Result.end(), [](Node *Nd1, Node *Nd2) -> bool {
    return Nd1->numTotalInsts > Nd2->numTotalInsts;
  });
}

void InlineTree::buildTree(Function *F) {
  Node *rootNode = getNode(F->getName(), Functions2Nodes);
  rootNode->isTopLevel = true;

  LLVM_DEBUG(dbgs() << "\nFunction " << F->getName() << '\n');
  for (BasicBlock &BB : *F) {
    for (Instruction &I : BB) {

      LLVM_DEBUG(dbgs() << I << '\n');

      ++totalNumberOfInstructions;
      SmallVector<DILocation *, 8> InlineChain;

      // Scan the chain of inlined scopes.
      DILocation *DL = I.getDebugLoc().get();
      while (DL) {
        InlineChain.push_back(DL);
        DL = DL->getInlinedAt();
      }
      Node *Nd = nullptr;
      DILocation *PrevDL = nullptr;
      for (DILocation *DL : llvm::reverse(InlineChain)) {
        DILocalScope *Sc = DL->getScope();
        DISubprogram *SP = Sc->getSubprogram();
        assert(SP);
        LLVM_DEBUG(dbgs() << "    f=" << SP->getLinkageName());
        if (Nd) {
          Nd = getNode(SP->getLinkageName(), Nd->UnsortedChildren);
          Nd->UniqueLocations.insert(LocationKey(PrevDL));
          LLVM_DEBUG(dbgs() << ", loc="; PrevDL->print(dbgs()); dbgs() << '\n');
        } else {
          Nd = rootNode;
          LLVM_DEBUG(dbgs() << ", root\n");
        }
        ++Nd->numTotalInsts;
        PrevDL = DL;
      }

      if (!Nd) {
        Nd = rootNode;
        ++Nd->numTotalInsts;
      }
      ++Nd->numSelfInsts;
    }
  }
}

std::string InlineTree::getPercent(int numInsts) const {
  assert(totalNumberOfInstructions > 0);
  std::string str;
  raw_string_ostream os(str);
  double percent = (double)numInsts * 100. / totalNumberOfInstructions;
  os << format("%0.2f", percent) << '%';
  return os.str();
}

void InlineTree::printNode(Node *Nd, int indent, raw_ostream &os) {
  os << std::string(indent * 4, ' ') << '[' << Nd->UniqueLocations.size()
     << "x," << getPercent(Nd->numTotalInsts) << '=' << Nd->numTotalInsts
     << ",self=" << Nd->numSelfInsts << ']' << ' ';
  printSymbol(Nd->FunctionName, os);
  os << '\n';

  for (Node *Child : Nd->getChildren()) {
    printNode(Child, indent + 1, os);
  }
}

void InlineTree::build(Module *M) {
  // Build the trees for all top-level functions.
  for (Function &F : *M) {
    if (F.empty())
      continue;
    buildTree(&F);
  }
  sortNodes(Functions2Nodes, FunctionRootNodes);

  // Sort all nodes by FunctionName -> isTopLevel -> numTotalInsts
  std::sort(allNodes.begin(), allNodes.end(), [](Node *Nd1, Node *Nd2) -> bool {
    if (Nd1->FunctionName != Nd2->FunctionName)
      return Nd1->FunctionName < Nd2->FunctionName;
    if (Nd1->isTopLevel != Nd2->isTopLevel)
      return (int)Nd1->isTopLevel > (int)Nd2->isTopLevel;
    return Nd1->numTotalInsts > Nd2->numTotalInsts;
  });
}

void InlineTree::print(raw_ostream &os) {
  // Calculate the summary information.
  os << "Inlining overhead (total = " << totalNumberOfInstructions << "):\n";
  SmallVector<Summary, 64> Summaries;
  Summary S = Summary(StringRef());

  // allNodes is sorted by function name.
  for (Node *Nd : allNodes) {
    if (Nd->FunctionName != S.FunctionName) {
      // Record the summary for the current function and continue with the
      // new function.
      if (S.instanceOverhead)
        Summaries.push_back(S);
      S = Summary(Nd->FunctionName);
      // The top-level node for the function appears first in the list. In this
      // case the size of UniqueLocations is 1. Only if the function is only
      // inlined and not present as top-level function the instanceOverhead may
      // be > 1.
      S.instanceOverhead = Nd->UniqueLocations.size();
      if (S.instanceOverhead > 0) {
        // Remove the size of the first instance, because a single instance
        // of a function is always needed and does not contribute to the
        // _overhead_.
        S.totalInstOverhead = Nd->numTotalInsts - Nd->numTotalInsts /
                              S.instanceOverhead;
        S.selfInstOverhead = Nd->numSelfInsts - Nd->numSelfInsts /
                             S.instanceOverhead;
        --S.instanceOverhead;
      }
    } else {
      S.totalInstOverhead += Nd->numTotalInsts;
      S.selfInstOverhead += Nd->numSelfInsts;
      S.instanceOverhead += Nd->UniqueLocations.size();
    }
  }
  if (S.instanceOverhead)
    Summaries.push_back(S);

  // Sort the summary table.
  std::sort(Summaries.begin(), Summaries.end(), [](const Summary &S1,
                                                   const Summary &S2) -> bool {
    if (S1.totalInstOverhead != S2.totalInstOverhead)
      return S1.totalInstOverhead > S2.totalInstOverhead;
    if (S1.selfInstOverhead != S2.selfInstOverhead)
      return S1.selfInstOverhead > S2.selfInstOverhead;
    return S1.instanceOverhead > S2.instanceOverhead;
  });

  // Print the summary table.
  unsigned totalOverhead = 0;
  for (const Summary &S : Summaries) {
    os << '[' << S.instanceOverhead << "x," << getPercent(S.totalInstOverhead)
       << '=' << S.totalInstOverhead << ",self=" << S.selfInstOverhead << "] ";
    printSymbol(S.FunctionName, os);
    os << '\n';
    totalOverhead += S.selfInstOverhead;
  }
  os << "\nTotal inlining overhead; " << getPercent(totalOverhead) << '='
     << totalOverhead << '\n';

  // Print the inline tree.
  os << "\nInlining tree:\n";
  for (Node *Nd : FunctionRootNodes) {
    printNode(Nd, 0, os);
    os << '\n';
  }

}

} // end anonymous namespace

/// The main entry point.
bool InlineTreePrinter::runOnModule(Module &M) {
  InlineTree Tree;
  Tree.build(&M);
  Tree.print(outs());
  return false;
}

llvm::PreservedAnalyses
swift::InlineTreePrinterPass::run(llvm::Module &M,
                                  llvm::ModuleAnalysisManager &AM) {
  InlineTree Tree;
  Tree.build(&M);
  Tree.print(outs());
  return llvm::PreservedAnalyses::all();
}
