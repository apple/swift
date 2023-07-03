//===--- RewriteContext.cpp - Term rewriting allocation arena -------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// The RewriteContext is a global singleton object with three primary
// responsibilities:
//
// - Arena allocation of uniqued immutable Symbols and Terms.
// - Caching requirement machine instances corresponding to generic signatures,
//   used for generic signature queries.
// - Building the graph of protocol connected components, in support of the
//   above.
//
// # Requirement machines for generic signatures
//
// The RewriteContext caches requirement machine instances built from generic
// signatures. When a generic signature is performed for the first time,
// a requirement machine is built for the generic signature by calling the
// RewriteContext::getRequirementMachine() method.
//
// An optimization is performed if this signature was written in source. When
// a new minimal generic signature is built from generic requirements, the
// AbstractGenericSignatureRequest and InferredGenericSignatureRequest requests
// transfer ownership of the requirement machine used for minimization to the
// RewriteContext by calling the installRequirementMachine() method, which
// associates this requirement machine with the newly-built generic signature.
//
// This saves the effort of rebuilding a new requirement machine from this
// signature the first time a query is performed, which typically happens when
// type checking the body of the generic declaration. 
//
// A requirement machine for a generic signature must include rewrite rules
// for all requirements in protocols referenced from this signature as well.
// Instead of rebuilding all of these rules every time a requirement machine
// is created, the rewrite rules for protocols themselves are also cached in
// the RewriteContext.
//
// # Protocol dependency graph
//
// The central concept behind this caching is the protocol dependency graph.
// This graph records which protocols mention other protocols via conformance
// requirements.
//
// Formally, the protocol dependency graph is a (directed) graph where the
// vertices are protocols, and there is an edge from protocol P to a protocol Q
// if P has a conformance requirement with Q on the right hand side.
//
// Consider these definitions:
//
//   protocol P1 : P2 {}
//   protocol P2 { associatedtype T : P3; associatedtype V : P4 }
//   protocol P3 { associatedtype U : P2; associatedtype V : P5 }
//   protocol P4 {}
//
// P1 has a dependency on P2; P2 and P3 depend on each other; P2 depends on P4,
// and finally, P3 depends on P5. The protocol dependency graph looks like this:
//
//             +----+
//             | P1 |
//             +----+
//              /  \
//             /    \
//            /      \
//           /        \
//          /          \
//         /            \
//        v              v
//     +----+  ----->  +----+
//     | P2 |          | P3 |
//     +----+  <-----  +----+
//       |                |
//       v                v
//     +----+          +----+
//     | P4 |          | P5 |
//     +----+          +----+
//
// When building a rewrite system for a generic signature that includes a
// conformance to protocol P2, we must include rewrite rules for P2, as well as
// all protocols reachable from P2 via the protocol dependency graph: P3, P4,
// and P5. Note that the set of all protocols reachable from P2 includes P3,
// and the set of all protocols reachable from P3 includes P2; so if a generic
// signature depends on one, it necessarily depends on the other.
//
// In general, this graph can contain cycles, as with P2 and P3 above. If we
// compute the strongly connected components of the protocol dependency graph,
// we get an acyclic graph:
//
//          +----+
//          | P1 |
//          +----+
//            |
//            v
//        +-------+
//        | P2 P3 |
//        +-------+
//          /   \
//         /     \
//        v       v
//     +----+   +----+
//     | P4 |   | P5 |
//     +----+   +----+
//
// The vertices of this graph are the strongly connected components of the
// original protocol dependency graph. Each connected component is a set of
// protocols that are interdependent and must be considered together as a
// single unit when building a rewrite system.
//
// # Requirement machines for protocol connected components
//
// The RewriteContext computes the protocol dependency graph and the associated
// graph of connected components. When building a rewrite system for a generic
// signature, the RuleBuilder queries the RewriteContext for the set of all
// connected components reachable from all conformance requirements in the
// generic signature.
//
// The RewriteContext associates a requirement machine to each connected
// component. This requirement machine is created when needed by the
// RewriteContext::getRequirementMachine(ProtocolDecl *) method.
//
// The rewrite rules from the requirement machine of each connected component
// are then imported into the newly-built requirement machine for the generic
// signature.
//
// If the protocol definitions in the connected component were parsed from
// source, this requirement machine is constructed when evaluating
// RequirementSignatureRequest, which computes a requirement signature for
// each protocol in the component from user-written requirements, and then
// saves the requirement machine in the RewriteContext by calling the
// installRequirementMachine() method.
//
// If the protocol definitions came from a deserialized module, we build a
// requirement machine from the previously-computed requirement signatures
// of those protocols.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "RequirementMachine.h"
#include "RewriteSystem.h"
#include "RewriteContext.h"

using namespace swift;
using namespace rewriting;

/// Parse a comma-separated list from the -debug-requirement-machine= frontend
/// flag.
static DebugOptions parseDebugFlags(StringRef debugFlags) {
  DebugOptions result;

  SmallVector<StringRef, 2> debug;
  debugFlags.split(debug, ',');
  for (auto flagStr : debug) {
    auto flag =
        llvm::StringSwitch<llvm::Optional<DebugFlags>>(flagStr)
            .Case("simplify", DebugFlags::Simplify)
            .Case("add", DebugFlags::Add)
            .Case("completion", DebugFlags::Completion)
            .Case("property-map", DebugFlags::PropertyMap)
            .Case("concrete-unification", DebugFlags::ConcreteUnification)
            .Case("concretize-nested-types", DebugFlags::ConcretizeNestedTypes)
            .Case("conditional-requirements",
                  DebugFlags::ConditionalRequirements)
            .Case("homotopy-reduction", DebugFlags::HomotopyReduction)
            .Case("homotopy-reduction-detail",
                  DebugFlags::HomotopyReductionDetail)
            .Case("minimal-conformances", DebugFlags::MinimalConformances)
            .Case("minimal-conformances-detail",
                  DebugFlags::MinimalConformancesDetail)
            .Case("protocol-dependencies", DebugFlags::ProtocolDependencies)
            .Case("minimization", DebugFlags::Minimization)
            .Case("redundant-rules", DebugFlags::RedundantRules)
            .Case("redundant-rules-detail", DebugFlags::RedundantRulesDetail)
            .Case("concrete-contraction", DebugFlags::ConcreteContraction)
            .Case("propagate-requirement-ids",
                  DebugFlags::PropagateRequirementIDs)
            .Case("timers", DebugFlags::Timers)
            .Case("conflicting-rules", DebugFlags::ConflictingRules)
            .Case("split-concrete-equiv-class",
                  DebugFlags::SplitConcreteEquivalenceClass)
            .Default(llvm::None);
    if (!flag) {
      llvm::errs() << "Unknown debug flag in -debug-requirement-machine "
                   << flagStr << "\n";
      abort();
    }

    result |= *flag;
  }

  return result;
}

RewriteContext::RewriteContext(ASTContext &ctx)
    : TheShapeSymbol(nullptr),
      Context(ctx),
      Stats(ctx.Stats),
      SymbolHistogram(Symbol::NumKinds),
      TermHistogram(4, /*Start=*/1),
      RuleTrieHistogram(16, /*Start=*/1),
      RuleTrieRootHistogram(16),
      PropertyTrieHistogram(16, /*Start=*/1),
      PropertyTrieRootHistogram(16),
      ConformanceRulesHistogram(16),
      MinimalConformancesHistogram(8, /*Start=*/2) {
  auto debugFlags = StringRef(ctx.LangOpts.DebugRequirementMachine);
  if (!debugFlags.empty())
    Debug = parseDebugFlags(debugFlags);
}

void RewriteContext::beginTimer(StringRef name) {
  auto now = std::chrono::system_clock::now();
  auto dur = now.time_since_epoch();

  for (unsigned i = 0; i < Timers.size(); ++i)
    llvm::dbgs() << "| ";
  llvm::dbgs() << "+ started " << name << " ";

  Timers.push_back(std::chrono::duration_cast<std::chrono::microseconds>(dur).count());
}

void RewriteContext::endTimer(StringRef name) {
  auto now = std::chrono::system_clock::now();
  auto dur = now.time_since_epoch();
  auto time = (std::chrono::duration_cast<std::chrono::microseconds>(dur).count()
               - Timers.back());
  Timers.pop_back();

  // If we're nested inside of another timer, don't charge our time to the parent.
  if (!Timers.empty()) {
    Timers.back() += time;
  }

  for (unsigned i = 0; i < Timers.size(); ++i)
    llvm::dbgs() << "| ";

  llvm::dbgs() << "+ ";

  if (time > 100000)
    llvm::dbgs() << "**** SLOW **** ";

  llvm::dbgs() << "finished " << name << " in " << time << "us: ";

}

const llvm::TinyPtrVector<const ProtocolDecl *> &
RewriteContext::getInheritedProtocols(const ProtocolDecl *proto) {
  auto found = AllInherited.find(proto);
  if (found != AllInherited.end())
    return found->second;

  AllInherited.insert(std::make_pair(proto, TinyPtrVector<const ProtocolDecl *>()));

  llvm::SmallDenseSet<const ProtocolDecl *, 4> visited;
  llvm::TinyPtrVector<const ProtocolDecl *> protos;

  for (auto *inheritedProto : proto->getInheritedProtocols()) {
    if (!visited.insert(inheritedProto).second)
      continue;

    protos.push_back(inheritedProto);
    const auto &allInherited = getInheritedProtocols(inheritedProto);

    for (auto *otherProto : allInherited) {
      if (!visited.insert(otherProto).second)
        continue;

      protos.push_back(otherProto);
    }
  }

  auto &result = AllInherited[proto];
  std::swap(protos, result);
  return result;
}

int RewriteContext::compareProtocols(const ProtocolDecl *lhs,
                                     const ProtocolDecl *rhs) {
  unsigned lhsSupport = getInheritedProtocols(lhs).size();
  unsigned rhsSupport = getInheritedProtocols(rhs).size();

  if (lhsSupport != rhsSupport)
    return rhsSupport - lhsSupport;

  return TypeDecl::compare(lhs, rhs);
}

RequirementMachine *RewriteContext::getRequirementMachine(
    CanGenericSignature sig) {
  auto &machine = Machines[sig];
  if (machine) {
    if (!machine->isComplete()) {
      llvm::errs() << "Re-entrant construction of requirement "
                   << "machine for " << sig << "\n";
      abort();
    }

    return machine;
  }

  if (Debug.contains(DebugFlags::Timers)) {
    beginTimer("getRequirementMachine()");
    llvm::dbgs() << sig << "\n";
  }

  // Store this requirement machine before adding the signature,
  // to catch re-entrant construction via initWithGenericSignature()
  // below.
  auto *newMachine = new rewriting::RequirementMachine(*this);
  machine = newMachine;

  // This might re-entrantly invalidate 'machine'.
  auto status = newMachine->initWithGenericSignature(sig);
  newMachine->checkCompletionResult(status.first);

  if (Debug.contains(DebugFlags::Timers)) {
    endTimer("getRequirementMachine()");
    llvm::dbgs() << sig << "\n";
  }

  return newMachine;
}

bool RewriteContext::isRecursivelyConstructingRequirementMachine(
    CanGenericSignature sig) {
  auto found = Machines.find(sig);
  if (found == Machines.end())
    return false;

  return !found->second->isComplete();
}

/// Given a requirement machine that built a minimized signature, attempt to
/// re-use it for subsequent queries against the minimized signature, instead
/// of building a new one later.
void RewriteContext::installRequirementMachine(
    CanGenericSignature sig,
    std::unique_ptr<RequirementMachine> machine) {
  if (!Context.LangOpts.EnableRequirementMachineReuse)
    return;

  auto &entry = Machines[sig];
  if (entry != nullptr)
    return;

  machine->freeze();
  entry = machine.release();
}

/// Implement Tarjan's algorithm to compute strongly-connected components in
/// the protocol dependency graph.
void RewriteContext::getProtocolComponentRec(
    const ProtocolDecl *proto,
    SmallVectorImpl<const ProtocolDecl *> &stack) {
  assert(Protos.count(proto) == 0);

  // Initialize the next component index and push the entry
  // on the stack
  {
    auto &entry = Protos[proto];
    entry.Index = NextComponentIndex;
    entry.LowLink = NextComponentIndex;
    entry.OnStack = 1;
  }

  NextComponentIndex++;
  stack.push_back(proto);

  // Look at each successor.
  auto found = Dependencies.find(proto);
  assert(found != Dependencies.end());

  for (auto *depProto : found->second) {
    auto found = Protos.find(depProto);
    if (found == Protos.end()) {
      // Successor has not yet been visited. Recurse.
      getProtocolComponentRec(depProto, stack);

      auto &entry = Protos[proto];
      assert(Protos.count(depProto) != 0);
      entry.LowLink = std::min(entry.LowLink, Protos[depProto].LowLink);
    } else if (found->second.OnStack) {
      // Successor is on the stack and hence in the current SCC.
      auto &entry = Protos[proto];
      entry.LowLink = std::min(entry.LowLink, found->second.Index);
    }
  }

  auto &entry = Protos[proto];

  // If this a root node, pop the stack and generate an SCC.
  if (entry.LowLink == entry.Index) {
    unsigned id = Components.size();
    SmallVector<const ProtocolDecl *, 3> protos;

    const ProtocolDecl *depProto = nullptr;
    do {
      depProto = stack.back();
      stack.pop_back();

      assert(Protos.count(depProto) != 0);
      Protos[depProto].OnStack = false;
      Protos[depProto].ComponentID = id;

      protos.push_back(depProto);
    } while (depProto != proto);

    if (Debug.contains(DebugFlags::ProtocolDependencies)) {
      llvm::dbgs() << "Connected component: [";
      bool first = true;
      for (auto *depProto : protos) {
        if (!first) {
          llvm::dbgs() << ", ";
        } else {
          first = false;
        }
        llvm::dbgs() << depProto->getName();
      }
      llvm::dbgs() << "]\n";
    }

    Components[id].Protos = Context.AllocateCopy(protos);
  }
}

/// Get the strongly connected component (SCC) of the protocol dependency
/// graph containing the given protocol.
///
/// You must not hold on to this reference across calls to any other
/// Requirement Machine operations, since they might insert new entries
/// into the underlying DenseMap, invalidating the reference.
RewriteContext::ProtocolComponent &
RewriteContext::getProtocolComponentImpl(const ProtocolDecl *proto) {
  {
    // We pre-load protocol dependencies into the Dependencies map
    // because getProtocolDependencies() can trigger recursive calls into
    // the requirement machine in highly-invalid code, which violates
    // invariants in getProtocolComponentRec().
    SmallVector<const ProtocolDecl *, 3> worklist;
    worklist.push_back(proto);

    while (!worklist.empty()) {
      const auto *otherProto = worklist.back();
      worklist.pop_back();

      auto found = Dependencies.find(otherProto);
      if (found != Dependencies.end())
        continue;

      auto protoDeps = otherProto->getProtocolDependencies();
      Dependencies.insert(std::make_pair(otherProto, protoDeps));
      for (auto *nextProto : protoDeps)
        worklist.push_back(nextProto);
    }
  }

  auto found = Protos.find(proto);
  if (found == Protos.end()) {
    if (ProtectProtocolComponentRec) {
      llvm::errs() << "Too much recursion is bad\n";
      abort();
    }

    ProtectProtocolComponentRec = true;

    SmallVector<const ProtocolDecl *, 3> stack;
    getProtocolComponentRec(proto, stack);
    assert(stack.empty());

    found = Protos.find(proto);
    assert(found != Protos.end());

    ProtectProtocolComponentRec = false;
  }

  assert(Components.count(found->second.ComponentID) != 0);
  auto &component = Components[found->second.ComponentID];

  assert(std::find(component.Protos.begin(), component.Protos.end(), proto)
         != component.Protos.end() && "Protocol is in the wrong SCC");
  return component;
}

/// Get the list of protocols in the strongly connected component (SCC)
/// of the protocol dependency graph containing the given protocol.
///
/// This can only be called once, to prevent multiple requirement machines
/// for being built with the same component.
ArrayRef<const ProtocolDecl *>
RewriteContext::startComputingRequirementSignatures(
    const ProtocolDecl *proto) {
  auto &component = getProtocolComponentImpl(proto);

  if (component.ComputingRequirementSignatures) {
    llvm::errs() << "Re-entrant minimization of requirement signatures for: ";
    for (auto *proto : component.Protos)
      llvm::errs() << " " << proto->getName();
    llvm::errs() << "\n";
    abort();
  }

  component.ComputingRequirementSignatures = true;

  return component.Protos;
}

/// Mark the component as having completed, which will ensure that
/// isRecursivelyComputingRequirementMachine() returns false.
void RewriteContext::finishComputingRequirementSignatures(
    const ProtocolDecl *proto) {
  auto &component = getProtocolComponentImpl(proto);

  assert(component.ComputingRequirementSignatures &&
         "Didn't call startComputingRequirementSignatures()");
  component.ComputedRequirementSignatures = true;
}

/// Get the list of protocols in the strongly connected component (SCC)
/// of the protocol dependency graph containing the given protocol.
///
/// This can only be called once, to prevent multiple requirement machines
/// for being built with the same component.
RequirementMachine *RewriteContext::getRequirementMachine(
    const ProtocolDecl *proto) {
  // First, get the requirement signature. If this protocol was written in
  // source, we'll minimize it and install the machine below, saving us the
  // effort of recomputing it.
  (void) proto->getRequirementSignature();

  auto &component = getProtocolComponentImpl(proto);

  if (component.Machine) {
    if (!component.Machine->isComplete()) {
      llvm::errs() << "Re-entrant construction of requirement machine for: ";
      for (auto *proto : component.Protos)
        llvm::errs() << " " << proto->getName();
      llvm::errs() << "\n";
      abort();
    }

    return component.Machine;
  }

  auto protos = component.Protos;

  if (Debug.contains(DebugFlags::Timers)) {
    beginTimer("getRequirementMachine()");
    llvm::dbgs() << "[";
    for (auto *proto : protos)
      llvm::dbgs() << " " << proto->getName();
    llvm::dbgs() << " ]\n";
  }

  // Store this requirement machine before adding the protocols, to catch
  // re-entrant construction via initWithProtocolSignatureRequirements()
  // below.
  auto *newMachine = new rewriting::RequirementMachine(*this);
  component.Machine = newMachine;

  // This might re-entrantly invalidate 'component.Machine'.
  auto status = newMachine->initWithProtocolSignatureRequirements(protos);
  newMachine->checkCompletionResult(status.first);

  if (Debug.contains(DebugFlags::Timers)) {
    endTimer("getRequirementMachine()");
    llvm::dbgs() << "[";
    for (auto *proto : protos)
      llvm::dbgs() << " " << proto->getName();
    llvm::dbgs() << " ]\n";
  }

  return newMachine;
}

/// Note: This doesn't use Evaluator::hasActiveRequest(), because in reality
/// the active request could be for any protocol in the connected component.
///
/// Instead, just check a flag set in the component itself.
bool RewriteContext::isRecursivelyConstructingRequirementMachine(
    const ProtocolDecl *proto) {
  auto found = Protos.find(proto);
  if (found == Protos.end())
    return false;

  auto component = Components.find(found->second.ComponentID);
  if (component == Components.end())
    return false;

  // If we've started but not finished, we're in the middle of computing
  // requirement signatures.
  return (component->second.ComputingRequirementSignatures &&
          !component->second.ComputedRequirementSignatures);
}

/// Given a requirement machine that built the requirement signatures for a
/// protocol connected component, attempt to re-use it for subsequent
/// queries against the connected component, instead of building a new one
/// later.
void RewriteContext::installRequirementMachine(
    const ProtocolDecl *proto,
    std::unique_ptr<RequirementMachine> machine) {
  if (!Context.LangOpts.EnableRequirementMachineReuse)
    return;

  auto &component = getProtocolComponentImpl(proto);
  if (component.Machine != nullptr)
    return;

  machine->freeze();
  component.Machine = machine.release();
}

/// We print stats in the destructor, which should get executed at the end of
/// a compilation job.
RewriteContext::~RewriteContext() {
  for (const auto &pair : Components)
    delete pair.second.Machine;

  Components.clear();

  for (const auto &pair : Machines)
    delete pair.second;

  Machines.clear();

  if (Context.LangOpts.AnalyzeRequirementMachine) {
    llvm::dbgs() << "--- Requirement Machine Statistics ---\n";
    llvm::dbgs() << "\n* Symbol kind:\n";
    SymbolHistogram.dump(llvm::dbgs(), Symbol::Kinds);
    llvm::dbgs() << "\n* Term length:\n";
    TermHistogram.dump(llvm::dbgs());
    llvm::dbgs() << "\n* Rule trie fanout:\n";
    RuleTrieHistogram.dump(llvm::dbgs());
    llvm::dbgs() << "\n* Rule trie root fanout:\n";
    RuleTrieRootHistogram.dump(llvm::dbgs());
    llvm::dbgs() << "\n* Property trie fanout:\n";
    PropertyTrieHistogram.dump(llvm::dbgs());
    llvm::dbgs() << "\n* Property trie root fanout:\n";
    PropertyTrieRootHistogram.dump(llvm::dbgs());
    llvm::dbgs() << "\n* Conformance rules:\n";
    ConformanceRulesHistogram.dump(llvm::dbgs());
    llvm::dbgs() << "\n* Minimal conformance equations:\n";
    MinimalConformancesHistogram.dump(llvm::dbgs());
  }
}
