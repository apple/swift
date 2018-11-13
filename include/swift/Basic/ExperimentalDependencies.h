//===--- ExperimentalDependencies.h - Keys for swiftdeps files --*- C++ -*-===//
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

#ifndef ExperimentalDependencies_h
#define ExperimentalDependencies_h

#include "swift/Basic/LLVM.h"
#include "llvm/Support/MD5.h"
#include <vector>
#include <unordered_map>
#include "swift/AST/Decl.h"



namespace swift {
class DependencyTracker;
class DiagnosticEngine;
class FrontendOptions;
class SourceFile;

/// Emit a Swift-style dependencies file for \p SF.
namespace experimental_dependencies {
  bool emitReferenceDependencies(DiagnosticEngine &diags, SourceFile *SF,
                                 const DependencyTracker &depTracker,
                                 StringRef outputPath);
  
  class Node {
  public:
    enum class Kind {
      topLevel,
      nominals,
      blankMembers,
      member, dynamicLookup,
      externalDepend,
      sourceFileProvide,
      kindCount };
    enum class SerializationKeys {
      kind,
      nameForDependencies,
      nameForHolderOfMember,
      fingerprint,
      sequenceNumber,
      departures,
      arrivals,
      serializationKeyCount
    };
  private:
    Kind kind;
    std::string nameForDependencies;
    std::string nameForHolderOfMember;
    std::string fingerprint;
    
    friend class Graph;
    friend class Arc;
    uint sequenceNumber;
    std::vector<uint> departures, arrivals;

  public:
  Node() = default;
  Node(
       Kind kind,
       std::string nameForDependencies,
       std::string nameForHolderOfMember,
       std::string fingerprint,
       uint sequenceNumber = ~0,
       std::vector<uint>&& departures = {},
       std::vector<uint>&& arrivals = {}) :
    kind(kind),
    nameForDependencies(nameForDependencies),
    nameForHolderOfMember(nameForHolderOfMember),
    fingerprint(fingerprint),
    sequenceNumber(sequenceNumber),
    departures(departures),
    arrivals(arrivals)
    {
      assert((kind == Kind::member) == !nameForHolderOfMember.empty() && "only member nodes have the holder name");
      assert(kind != Kind::sourceFileProvide || !fingerprint.empty() && "source files must have fingerprint (old interfaceHash");
    }
    Node(const Node& other) = default;
    
    virtual ~Node() = default;
    Kind getKind() const { return kind; }
    StringRef getNameForDependencies() const { return nameForDependencies; }
    StringRef getNameForHolderOfMember() const { return nameForHolderOfMember; }
    StringRef getFingerprint() const { return fingerprint; }
    void setFingerprint(StringRef fp) { fingerprint = fp; }
    uint getSequenceNumber() const { return sequenceNumber; }
    ArrayRef<uint> getDepartures() const { return departures; }
    ArrayRef<uint> getArrivals() const { return arrivals; }
  };
  
  /// Memoize nodes serving as heads of dependency arcs:
  /// Could be a definition in another file that a lookup here depends upon,
  /// or could be definition in this file that a lookup here depends upon.
  
  class Graph;
  class MemoizedNode: public Node {
  public:
    using Key = std::tuple<std::string, std::string, Node::Kind>;
    
    static Key createMemoizedKey(Node::Kind kind,
                                 std::string nameForDependencies,
                                 std::string nameForHolderOfMember) {
      return std::make_tuple(nameForHolderOfMember, nameForDependencies, kind);
    }
    
    struct hash
    : public std::unary_function<Key, size_t>
    {
      size_t operator()(const Key key) const {
        return std::hash<std::string>()(std::get<0>(key)) ^
        std::hash<std::string>()(std::get<1>(key)) ^
        std::hash<size_t>()(size_t(std::get<2>(key)));
      }
    };
    
    using Cache = typename std::unordered_map<Key, MemoizedNode*, MemoizedNode::hash>;
    
    MemoizedNode(Kind kind,
                 std::string nameForDependencies,
                 std::string nameForHolderOfMember,
                 std::string fingerprint) :
    Node(kind, nameForDependencies, nameForHolderOfMember, fingerprint) {}
    
  public:
    Key memoizedKey() const {
      return createMemoizedKey(getKind(), getNameForDependencies(), getNameForHolderOfMember());
    }
    static MemoizedNode *create(Kind kind,
                                std::string nameForDependencies,
                                std::string nameForHolderOfMember,
                                std::string fingerprint,
                                Cache &cache,
                                Graph &g);
  };

 
  
  
  class Arc {
  public:
    const uint tailSeqNo, headSeqNo;
    Arc(const Node* tail, const Node* head) :
    tailSeqNo(tail->sequenceNumber), headSeqNo(head->sequenceNumber) {}
  };
  
  class Graph {
    std::vector<Node*> allNodes;
  public:
    void addNode(Node* n) {
      n->sequenceNumber = allNodes.size();
      allNodes.push_back(n);
    }
    void addArc(const Arc arc) {
      if (arc.headSeqNo == arc.tailSeqNo)
        return; // no point
      allNodes[arc.tailSeqNo]->departures.push_back(arc.headSeqNo);
      allNodes[arc.headSeqNo]->arrivals  .push_back(arc.tailSeqNo);
    }
    decltype(allNodes)::const_iterator nodesBegin() const { return allNodes.cbegin(); }
    decltype(allNodes)::const_iterator nodesEnd() const { return allNodes.cend(); }
    
    Graph() = default;
    Graph(const Graph& g) = delete;
    Graph(Graph&& g) = default;
    
    ~Graph() {
      for (Node* n: allNodes) {
        delete n;
      }
    }
  };
  
} // end namespace experimental_dependencies
} // end namespace swift

#endif /* ExperimentalDependencies_h */
