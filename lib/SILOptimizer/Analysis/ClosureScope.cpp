//===--- ClosureScope.cpp - Closure Scope Analysis ------------------------===//
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
///
/// Implementation of ClosureScopeAnalysis.
///
/// The "scope" of a closure is the function body that refers to the closure.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "closure-scope"

#include "swift/SILOptimizer/Analysis/ClosureScope.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/SILModule.h"
#include "llvm/ADT/iterator.h"

namespace swift {

// A function is a node in this graph if it either refers to a closure or is
// itself a closure.
//
// The graph is represented with two vectors. One for forward edges from "scope
// functions" to closures. Another with backward edges from closures to
// "scope functions".
class ClosureGraph {
  struct Edge {
    SILFunction *from;
    SILFunction *to;

    bool operator<(const Edge &other) const {
      unsigned from1 = from->getIndex();
      unsigned from2 = other.from->getIndex();
      if (from1 != from2)
        return from1 < from2;

      return to->getIndex() < other.to->getIndex();
    }
  };

  using EdgePos = std::vector<Edge>::iterator;
  using EdgeRange = llvm::iterator_range<EdgePos>;

  struct NodeIterator
      : llvm::iterator_adaptor_base<
            NodeIterator, EdgePos, std::random_access_iterator_tag,
            SILFunction *, ptrdiff_t, SILFunction *, SILFunction *> {

    NodeIterator() = default;

    explicit NodeIterator(EdgePos pos) : iterator_adaptor_base(pos) {}

    EdgePos edgeIter() const { return I; }

    SILFunction *operator*() const { return I->from; }
  };

  // Compare the node identified by the from index of \p edge with the node
  // identified by \p functionIndex.
  struct NodeCompare {
    bool operator()(SILFunction *node1, SILFunction *node2) {
      return node1->getIndex() < node2->getIndex();
    }
  };

  // A set of edges in the relational mapping from a function to the set of
  // nonescaping closures that are referenced within its body.
  //
  // from: parent function
  //   to: nonescaping child closure
  std::vector<Edge> functionToClosures;

  // Map every nonescaping closure to the parent function that refers to it.
  //
  // from: nonescaping child closure
  //   to: parent function
  //
  // A closure almost always has a single parent. A local function, however, may
  // be referenced recursively, even within a different closure. Notice that
  // capturedInt is captured from the outer-most function, but is passed down
  //
  //   func localFunc(b: Apply) {
  //     capturedInt += 1
  //     let closure = { (c: Apply) in
  //       c.apply(localFunc)
  //     }
  //     b.apply(closure)
  //   }
  //   a.apply(localFunc)
  std::vector<Edge> closureToFunctions;

public:
  ClosureGraph() = default;

  /// Visit the parent scopes of \p closure if it has any. If \p visitor returns
  /// false, exit early and return false. Otherwise return true.
  bool visitClosureScopes(SILFunction *closure,
                          std::function<bool(SILFunction *scopeFunc)> visitor);

  /// Visit the closures directly referenced by \p scopeFunc.
  bool visitClosures(SILFunction *scopeFunc,
                     std::function<bool(SILFunction *closure)> visitor);

  /// Called when a \p function is removed from this module.
  void erase(SILFunction *function);

  /// Record all closure scopes in this module.
  void compute(SILModule *M);

protected:
  ClosureGraph(const ClosureGraph &) = delete;
  ClosureGraph &operator=(const ClosureGraph &) = delete;

  EdgeRange getEdgeRange(SILFunction *node, std::vector<Edge> &edges) {
    auto it = std::lower_bound(NodeIterator(edges.begin()),
                               NodeIterator(edges.end()),
                               node, NodeCompare());
    auto next = it.edgeIter();
    for (auto end = edges.end(); next != end; ++next) {
      if (next->from != node)
        break;
    }
    return EdgeRange(it.edgeIter(), next);
  }
  
  EdgeRange getFunctionToClosureEdges(SILFunction *scopeFunc) {
    return getEdgeRange(scopeFunc, functionToClosures);
  }

  EdgeRange getClosureToFunctionEdges(SILFunction *closure) {
    return getEdgeRange(closure, closureToFunctions);
  }

  void recordScope(ApplySite apply);

  void finalize();

  SWIFT_ASSERT_ONLY_DECL(void dump());
};

bool ClosureGraph::visitClosureScopes(
    SILFunction *closure, std::function<bool(SILFunction *scopeFunc)> visitor) {
  for (ClosureGraph::Edge &edge : getClosureToFunctionEdges(closure)) {
    if (!visitor(edge.to))
      return false;
  }
  return true;
}

bool ClosureGraph::visitClosures(
    SILFunction *scopeFunc, std::function<bool(SILFunction *closure)> visitor) {
  for (ClosureGraph::Edge &edge : getFunctionToClosureEdges(scopeFunc)) {
    if (!visitor(edge.to))
      return false;
  }
  return true;
}

void ClosureGraph::erase(SILFunction *function) {
  struct RefersToFunction {
    SILFunction *function;

    bool operator()(const Edge &edge) {
      return edge.from == function || edge.to == function;
    }
  };
  llvm::erase_if(functionToClosures, RefersToFunction{function});
  llvm::erase_if(closureToFunctions, RefersToFunction{function});
}

// Handle both partial_apply and directly applied closures of the form:
//   %f = function_ref @... : $(...inout_aliasable...) -> ...
//   apply %f(...)
void ClosureGraph::recordScope(ApplySite apply) {
  // Only track scopes of non-escaping closures.
  auto closureTy = apply.getCallee()->getType().castTo<SILFunctionType>();

  // FIXME: isCalleeDynamicallyReplaceable should not be true but can today
  // because local functions can be marked dynamic.
  if (!isNonEscapingClosure(closureTy)
      || apply.isCalleeDynamicallyReplaceable()) {
    return;
  }
  auto closureFunc = apply.getCalleeFunction();
  assert(closureFunc && "non-escaping closure needs a direct partial_apply.");

  auto scopeFunc = apply.getFunction();
  // Passes may assume that a deserialized function can only refer to
  // deserialized closures. For example, AccessEnforcementSelection skips
  // deserialized functions but assumes all a closure's parent scope have been
  // processed.
  assert(scopeFunc->wasDeserializedCanonical() ==
             closureFunc->wasDeserializedCanonical() &&
         "A closure cannot be serialized in a different module than its "
         "parent context");

  functionToClosures.push_back({scopeFunc, closureFunc});
  closureToFunctions.push_back({closureFunc, scopeFunc});
}

void ClosureGraph::finalize() {
  llvm::stable_sort(functionToClosures);
  llvm::stable_sort(closureToFunctions);

  LLVM_DEBUG(dump());
}

#ifndef NDEBUG
static void dumpFunctionName(SILFunction *function) {
  auto opts = Demangle::DemangleOptions::SimplifiedUIDemangleOptions();
  opts.ShowAsyncResumePartial = true;
  llvm::dbgs() << Demangle::demangleSymbolAsString(function->getName(), opts)
               << " '" << function->getName() << "'\n";
}

void ClosureGraph::dump() {
  llvm::dbgs() << "\n";
  SILFunction *currentFunc = nullptr;
  for (auto &edge : functionToClosures) {
    auto *scopeFunc = edge.from;
    if (currentFunc != scopeFunc) {
      currentFunc = scopeFunc;
      llvm::dbgs() << "SCOPE: ";
      dumpFunctionName(scopeFunc);
    }
    llvm::dbgs() << "    CLOSURE: ";
    dumpFunctionName(edge.to);
  }
  currentFunc = nullptr;
  for (auto &edge : closureToFunctions) {
    auto *closure = edge.from;
    if (currentFunc != closure) {
      currentFunc = closure;
      llvm::dbgs() << "CLOSURE: ";
      dumpFunctionName(closure);
    }
    llvm::dbgs() << "    SCOPE: ";
    dumpFunctionName(edge.to);
  }
}
#endif

void ClosureGraph::compute(SILModule *M) {
  for (auto &F : *M) {
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto apply = ApplySite::isa(&I)) {
          recordScope(apply);
        }
      }
    }
  }
  finalize();
}

ClosureScopeAnalysis::ClosureScopeAnalysis(SILModule *M)
    : SILAnalysis(SILAnalysisKind::ClosureScope), M(M), scopeGraph(nullptr) {}

ClosureScopeAnalysis::~ClosureScopeAnalysis() = default;

bool ClosureScopeAnalysis::visitClosureScopes(
    SILFunction *closure, std::function<bool(SILFunction *scopeFunc)> visitor) {
  return getOrComputeGraph()->visitClosureScopes(closure, visitor);
}

bool ClosureScopeAnalysis::visitClosures(
    SILFunction *scopeFunc, std::function<bool(SILFunction *closure)> visitor) {
  return getOrComputeGraph()->visitClosures(scopeFunc, visitor);
}

void ClosureScopeAnalysis::invalidate() {
  scopeGraph.reset();
}

void ClosureScopeAnalysis::notifyWillDeleteFunction(SILFunction *F) {
  if (scopeGraph)
    scopeGraph->erase(F);
}

ClosureGraph *ClosureScopeAnalysis::getOrComputeGraph() {
  if (!scopeGraph) {
    scopeGraph = std::make_unique<ClosureGraph>();
    scopeGraph->compute(M);
  }
  return scopeGraph.get();
}

SILAnalysis *createClosureScopeAnalysis(SILModule *M) {
  return new ClosureScopeAnalysis(M);
}

class ClosureFunctionOrder::ClosureDFS {
  ClosureFunctionOrder &functionOrder;

  llvm::SmallBitVector visited;
  llvm::SmallBitVector finished;

  SmallVector<SILFunction *, 4> postorderClosures;

public:
  ClosureDFS(ClosureFunctionOrder &functionOrder)
    : functionOrder(functionOrder),
      visited(functionOrder.csa->getModule()->getNumFunctionIndices()),
      finished(functionOrder.csa->getModule()->getNumFunctionIndices())
  {}

  bool isVisited(SILFunction *function) const {
    return visited.test(function->getIndex());
  }

  void performDFS(SILFunction *root) {
    postorderClosures.clear();

    recursiveDFS(root);

    // Closures are discovered in postorder, bottom-up.
    // Reverse-append them onto the top-down function list.
    llvm::append_range(functionOrder.topDownFunctions,
                       llvm::reverse(postorderClosures));
  }

protected:
  void recursiveDFS(SILFunction *function) {
    unsigned index = function->getIndex();
    if (visited.test(index)) {
      if (!finished.test(index)) {
        // Cycle in the closure graph.
        functionOrder.closureCycleHeads.insert(function);
      }
      return;
    }
    visited.set(index);

    functionOrder.csa->visitClosures(function, [this](SILFunction *closure) {
      recursiveDFS(closure);
      return true;
    });
    finished.set(index);
    postorderClosures.push_back(function);
  };
};

void ClosureFunctionOrder::compute() {
  auto *module = csa->getModule();

  assert(topDownFunctions.empty() && closureCycleHeads.empty() &&
         "attempting to recompute");
  topDownFunctions.reserve(module->getNumFunctionIndices());

  ClosureDFS dfs(*this);
  SmallVector<SILFunction *, 4> closureWorklist;

  unsigned numFunctions = 0;
  for (auto &function : module->getFunctionList()) {
    ++numFunctions;
    if (dfs.isVisited(&function))
      continue;

    if (csa->isReachableClosure(&function)) {
      // This is a closure, reachable from some other function. Reaching this
      // point is rare, because closures typically follow their parent in the
      // function list.
      //
      // A closure at the head of a cycle might only be reachable from other
      // closures. So use a worklist to visit them once more as DFS roots after
      // finishing DFS from all acyclic functions.
      closureWorklist.push_back(&function);
      continue;
    }
    dfs.performDFS(&function);
  }
  // Revisit closures in forward order. DFS will immediately return except in
  // the unlikely event that this is an orphaned closure.
  for (auto *closure : closureWorklist) {
    dfs.performDFS(closure);
  }
  LLVM_DEBUG(dump());
  assert(numFunctions == topDownFunctions.size() && "DFS missed a function");
}

#ifndef NDEBUG
void ClosureFunctionOrder::dump() {
  llvm::dbgs() << "\nRPO function order:\n";
  for (auto *function : getTopDownFunctions()) {
    llvm::dbgs() << "[" << function->getIndex() << "] ";
    if (isHeadOfClosureCycle(function))
      llvm::dbgs() << "CYCLE HEAD: ";

    dumpFunctionName(function);
  }
}
#endif

} // namespace swift
