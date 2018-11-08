//===--- ExperimentalDependenciesProducer.cpp - Generates swiftdeps files -===//
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

#include <stdio.h>


#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/FileSystem.h"
#include "swift/AST/Module.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ReferencedNameTracker.h"
#include "swift/AST/Types.h"
#include "swift/Basic/ExperimentalDependencies.h"
#include "swift/Basic/FileSystem.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/ReferenceDependencyKeys.h"
#include "swift/Frontend/FrontendOptions.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/YAMLParser.h"

#include <unordered_map>

using namespace swift;
using namespace experimental_dependencies;






// shorthands

using StringVec = std::vector<std::string>;
template <typename T> using CPVec = std::vector<const T*>;
template <typename T1 = std::string, typename T2 = std::string> using PairVec = std::vector<std::pair<T1, T2>>;
template <typename T1, typename T2> using CPPairVec = std::vector<std::pair<const T1*, const T2*>>;





namespace {
  /// Takes all the Decls in a SourceFile, and collects them into buckets by groups of DeclKinds.
  /// Also casts them to more specific types.
  
  class SourceFileDeclDemux {
  private:
    template <typename SpecificDeclType, DeclKind f, DeclKind ...r>
    bool take(const Decl *const D, CPVec<SpecificDeclType> &decls) {
      if (D->getKind() != f)
        return take<SpecificDeclType, r...>(D, decls);
      decls.push_back(cast<SpecificDeclType>(D));
      return true;
    }
    template <typename SpecificDeclType>
    bool take(const Decl *const D, CPVec<SpecificDeclType> &decls) {
      return false;
    }
  public:
    CPVec<ExtensionDecl> extensions;
    CPVec<OperatorDecl> operators;
    CPVec<PrecedenceGroupDecl> precedenceGroups;
    CPVec<NominalTypeDecl> topNominals;
    CPVec<ValueDecl> topValues;
    CPVec<NominalTypeDecl> allNominals;
    CPVec<FuncDecl> memberOperatorDecls;
    CPVec<ValueDecl> valuesInExtensions;
    CPVec<ValueDecl> classMembers;
    
    SourceFileDeclDemux(const SourceFile *const SF) {
      for (const Decl *const D: SF->Decls) {
        take<ExtensionDecl, DeclKind::Extension>(D, extensions)
        || take<OperatorDecl, DeclKind::InfixOperator, DeclKind::PrefixOperator, DeclKind::PostfixOperator>(D, operators)
        || take<PrecedenceGroupDecl, DeclKind::PrecedenceGroup> (D, precedenceGroups)
        || take<NominalTypeDecl, DeclKind::Enum, DeclKind::Struct, DeclKind::Class, DeclKind::Protocol>(D, topNominals)
        || take<ValueDecl, DeclKind::TypeAlias, DeclKind::Var, DeclKind::Func, DeclKind::Accessor>(D, topValues);
      }
      findNominalsFromExtensions();
      findNominalsInTopNominals();
      findValuesInExtensions();
      findClassMembers(SF);
    }
  private:
    void findNominalsFromExtensions() {
      for (auto *ED: extensions)
        findNominalsAndOperatorsIn(ED->getExtendedNominal());
    }
    void findNominalsInTopNominals() {
      for (const auto *const NTD: topNominals)
        findNominalsAndOperatorsIn(NTD);
    }
    void findNominalsAndOperatorsIn(const NominalTypeDecl *const NTD) {
      allNominals.push_back(NTD);
      findNominalsAndOperatorsInMembers(NTD->getMembers());
    }
    void findNominalsAndOperatorsInMembers(const DeclRange members) {
      for (const Decl *const D: members) {
        if (dyn_cast<ValueDecl>(D)->getFullName().isOperator())
          memberOperatorDecls.push_back(cast<FuncDecl>(D));
        else if (const auto *const NTD = dyn_cast<NominalTypeDecl>(D))
          findNominalsAndOperatorsIn(NTD);
      }
    }
    void findValuesInExtensions() {
      for (const auto* ED: extensions) {
        for (const auto *member: ED->getMembers())
          if (const auto *VD = dyn_cast<ValueDecl>(member))
            if (VD->hasName())
              valuesInExtensions.push_back(VD);
      }
    }
    void findClassMembers(const SourceFile *const SF) {
      struct Collector: public VisibleDeclConsumer {
        CPVec<ValueDecl> &classMembers;
        Collector(CPVec<ValueDecl> &classMembers) : classMembers(classMembers) {}
        void foundDecl(ValueDecl *VD, DeclVisibilityKind) override {
          classMembers.push_back(VD);
        }
      } collector {classMembers};
      SF->lookupClassMembers({}, collector);
    }
  };

}

static std::string mangleTypeAsContext(const NominalTypeDecl * NTD) {
  Mangle::ASTMangler Mangler;
  return Mangler.mangleTypeAsContextUSR(NTD);
}


////////////




//////////////
  



////////////////////////////


using MemberTableEntryTy = std::pair<ReferencedNameTracker::MemberPair, bool>;

namespace {
  struct DependName {
    std::string name;
    bool cascades;
    DependName(const DeclBaseName name, const bool cascades) : name(name.userFacingName()), cascades(cascades) {}
    int compare(DependName other) const { return name.compare(other.name); }
    static int compare(const DependName &lhs, const DependName &rhs) {
      return lhs.compare(rhs);
    }
    bool operator< (const DependName &other) const {return compare(other) == -1; }
  };
}

namespace {
  class DependNames {
  private:
    const ReferencedNameTracker *const tracker;
    std::vector<DependName> sortedTops;
    std::vector<MemberTableEntryTy> sortedMembers;
    std::unordered_set<const NominalTypeDecl*>cascadingHolders;
    std::vector<MemberTableEntryTy> sortedMembersWithPropagatedCascades;
    std::vector<DependName> sortedDynamicLookupNames;
    StringVec externalNames;


    static std::vector<DependName> linearizeAndSort(const llvm::DenseMap<DeclBaseName, bool> &map);
    static std::vector<MemberTableEntryTy> sort(const llvm::DenseMap<ReferencedNameTracker::MemberPair, bool> &members);
    static std::unordered_set<const NominalTypeDecl*> findCascadingHolders(const std::vector<MemberTableEntryTy> &in);
    static std::vector<MemberTableEntryTy> spreadCascades(const std::vector<MemberTableEntryTy> &members,
                                                          const std::unordered_set<const NominalTypeDecl*> &cascades);
    static StringVec reversePathSortedFilenames(const ArrayRef<std::string> in);

  public:
    DependNames(const SourceFile *const SF, const DependencyTracker &depTracker) :
    tracker(SF->getReferencedNameTracker()),
    sortedTops(linearizeAndSort(tracker->getTopLevelNames())),
    sortedMembers(sort(tracker->getUsedMembers())),
    cascadingHolders(findCascadingHolders(sortedMembers)),
    sortedMembersWithPropagatedCascades(spreadCascades(sortedMembers, cascadingHolders)),
    sortedDynamicLookupNames(linearizeAndSort(tracker->getDynamicLookupNames())),
    externalNames(reversePathSortedFilenames(depTracker.getDependencies()))
    {
    }
    std::pair<const std::vector<DependName> &, Node::Kind> tops() const {
      return std::make_pair(sortedTops, Node::Kind::topLevel);
    }
    const std::vector<MemberTableEntryTy> &membersWithPropagatedCascades() const { return sortedMembersWithPropagatedCascades; } // nominals
    const std::vector<MemberTableEntryTy> &members() const { return sortedMembers; } // members
    std::pair<const std::vector<DependName> &, Node::Kind> dynamicLookups() const {
      return std::make_pair(sortedDynamicLookupNames, Node::Kind::dynamicLookup); }
    const StringVec &externals() const { return externalNames; }
  };
}

//////////////////////

std::vector<DependName> DependNames::linearizeAndSort(const llvm::DenseMap<DeclBaseName, bool> &map) {
  std::vector<DependName> out;
  for (const auto &p: map)
    out.push_back(DependName{p.getFirst(), p.getSecond()});

  std::sort(out.begin(), out.end());
  return out;
}




std::vector<MemberTableEntryTy> DependNames::sort(const llvm::DenseMap<ReferencedNameTracker::MemberPair, bool> &members) {
  std::vector<MemberTableEntryTy> out{members.begin(), members.end()};
  llvm::array_pod_sort(out.begin(), out.end(),
                       [](const MemberTableEntryTy *lhs,
                          const MemberTableEntryTy *rhs) -> int {
                         if (auto cmp = lhs->first.first->getName().compare(rhs->first.first->getName()))
                           return cmp;

                         if (auto cmp = lhs->first.second.compare(rhs->first.second))
                           return cmp;

                         // We can have two entries with the same member name if one of them
                         // was the special 'init' name and the other is the plain 'init' token.
                         if (lhs->second != rhs->second)
                           return lhs->second ? -1 : 1;

                         // Break type name ties by mangled name.
                         auto lhsMangledName = mangleTypeAsContext(lhs->first.first);
                         auto rhsMangledName = mangleTypeAsContext(rhs->first.first);
                         return lhsMangledName.compare(rhsMangledName);
                       });
  return out;
}

std::unordered_set<const NominalTypeDecl*> DependNames::findCascadingHolders(const std::vector<MemberTableEntryTy> &in) {
  std::unordered_set<const NominalTypeDecl*> out;
  for (const auto &entry: in)
    if (entry.second)
      out.insert(entry.first.first);
  return out;
}

std::vector<MemberTableEntryTy> DependNames::spreadCascades(const std::vector<MemberTableEntryTy> &members,
                                               const std::unordered_set<const NominalTypeDecl*> &cascades) {
  std::vector<MemberTableEntryTy> out;
  std::transform(members.begin(), members.end(), std::back_inserter(out),
                 [&cascades] (MemberTableEntryTy entry) -> MemberTableEntryTy {
                   entry.second = cascades.count(entry.first.first) != 0 ? true : false;
                   return entry;
                 });
  return out;
}

StringVec DependNames::reversePathSortedFilenames(const ArrayRef<std::string> in) {
  StringVec out{in.begin(), in.end()};
  std::sort(out.begin(), out.end(),  [](const std::string &a,
                                        const std::string &b) -> bool {
    return std::lexicographical_compare(a.rbegin(), a.rend(),
                                        b.rbegin(), b.rend());
  });
  return out;
}


class GraphConstructor {
  SourceFile *SF;
  const DependencyTracker &depTracker;
  StringRef outputPath;
  DeclNode *sourceFileNode;
  
  GraphConstructor(
                   SourceFile *SF,
                   const DependencyTracker &depTracker,
                   StringRef outputPath) : SF(SF), depTracker(depTracker), outputPath(outputPath) {}
  
  Graph g;
  void construct() {
    //TODO storage mgmt
    sourceFileNode = new DeclNode(DeclNode::Kind::sourceFileProvide, SF->getAsDecl(), nullptr, getInterfaceHash(), outputPath);
    g.addNode(sourceFileNode);
    
    constructProvidesNodes();
    constructDependsArcs();
  }
  
private:
  std::unordered_map<const Decl*, Node*> nodesByDecl;
  std::array<llvm::StringMap<Node*>, uint(Node::Kind::end)> nodesByKindAndName{};
  
  std::string getInterfaceHash() const {
    llvm::SmallString<32> interfaceHash;
    SF->getInterfaceHash(interfaceHash);
    return interfaceHash.str().str();
  }
  
  void constructProvidesNodes();
  void constructDependsArcs();
  void constructSingleNameDependsArcs(std::pair<const std::vector<DependName> &, Node::Kind>);
  
  
  
  /// name converters
  template <typename DeclT>
  static std::string getBaseName(DeclT const* D) { return D->getBaseName().userFacingName(); }
  
  template <typename DeclT>
  static std::string getName(DeclT const* D) { return DeclBaseName(D->getName()).userFacingName(); }
  
  
  
  template <typename DeclT>
  void createNodes(CPVec<DeclT> &decls, DeclNode::Kind kind, std::string(*nameFn)(const DeclT *)) {
    for (const auto* D: decls) {
      auto *context = D->getDeclContext();
      auto *containingDecl = context ? context->getAsDecl() : nullptr;
      auto iter = nodesByDecl.find(containingDecl);
      assert(iter != nodesByDecl.end() && "missing container");
      Node *containingNode = iter->second;
      addDeclNode(D, new DeclNode(kind, D, containingNode, "", (*nameFn)(D)));
    }
  }
  
  void addDeclNode(const Decl* D, DeclNode *node) {
    bool inserted = nodesByDecl.insert(std::make_pair(D, node)).second;
    assert(inserted && "dup node?");
    inserted = nodesByKindAndName[uint(node->kind)].insert(std::make_pair(node->nameForDependencies, node)).second;
    assert(inserted && "dup node??");
    g.addNode(node);
  }
  
  Node* getOrCreateDeclNode(Node::Kind kind, std::string name) {
    auto iter = nodesByKindAndName[uint(kind)].find(name);
    return iter->second;
  }
};

void GraphConstructor::constructProvidesNodes() {
  SourceFileDeclDemux demux(SF);
  
  createNodes<PrecedenceGroupDecl>(demux.precedenceGroups, DeclNode::Kind::topLevel, getName);
  createNodes<FuncDecl>(demux.memberOperatorDecls, DeclNode::Kind::topLevel, getName);
  createNodes<OperatorDecl>(demux.operators, DeclNode::Kind::topLevel, getName);
  createNodes<NominalTypeDecl>(demux.topNominals, DeclNode::Kind::topLevel, getName);
  createNodes<ValueDecl>(demux.topValues, DeclNode::Kind::topLevel, getBaseName);
  
  createNodes<NominalTypeDecl>(demux.allNominals, DeclNode::Kind::nominalAndBlankMembers, mangleTypeAsContext);
  
  createNodes<ValueDecl>(demux.valuesInExtensions, DeclNode::Kind::member, getBaseName);
  
  // could optimize by uniqueing by name, but then what of container?
  createNodes<ValueDecl>(demux.classMembers, DeclNode::Kind::dynamicLookup, getBaseName);
}
void GraphConstructor::constructDependsArcs() {
  DependNames dn(SF, depTracker);
  constructSingleNameDependsArcs(dn.tops());
  constructSingleNameDependsArcs(dn.dynamicLookups());
}
void GraphConstructor::constructSingleNameDependsArcs(std::pair<const std::vector<DependName> &, Node::Kind> namesAndKind) {
  for (auto &dependName: namesAndKind.first) {
    Node* def = getOrCreateDeclNode(namesAndKind.second, dependName.name);
    Node* use = sourceFileNode;
    g.addArc(Arc{use, def});
  }
}

class YAMLEmitter {
private:
  llvm::raw_ostream &out;
  
public:
  YAMLEmitter(llvm::raw_ostream &out) : out(out) {}
  
  void emitSectionStart(StringRef section) const {
    out << reference_dependency_keys::providesMember << ":\n";
  }
  void emitName(StringRef name) const {
    out << "- \"" << llvm::yaml::escape(name) << "\"\n";
  }
  void emitSingleValueSection(StringRef section, StringRef value) const {
    out << section << ": \"" << llvm::yaml::escape(value) << "\"\n";
  }
  void emitDoubleNameLine(StringRef name1, StringRef name2) const {
    out << "- [\"" << llvm::yaml::escape(name1) << "\", \""
    << (name2.empty() ? std::string() : llvm::yaml::escape(name2))
    << "\"]\n";
  }
  
  template <typename NodeT>
  void emitNodes(ArrayRef<NodeT> nodes) {
    for (auto &n: nodes)
      n.emit(this);
  }
};
  


  



// move filter etc into wherever
// do depends decls
// do output


////////////////////////


namespace {
  class FileRenamerAndWriter {
    DiagnosticEngine &diags;
    std::string outputPath;
    
  public:
    FileRenamerAndWriter(DiagnosticEngine &diags, StringRef outputPath) :diags(diags), outputPath(outputPath.str())
     {}
    
    /// Returns true on error
    bool operator() (ArrayRef<std::string> strings) {
      // Before writing to the dependencies file path, preserve any previous file
      // that may have been there. No error handling -- this is just a nicety, it
      // doesn't matter if it fails.
      llvm::sys::fs::rename(outputPath, outputPath + "~");
      return withOutputFile(diags, outputPath, [&](llvm::raw_pwrite_stream &out) {
        for (auto s: strings) out << s;
        return false;
      } );
    }
    
  };
}


//////////////////////////







/// Entry point to this whole file:

bool swift::experimental_dependencies::emitReferenceDependencies(
                                                                 DiagnosticEngine &diags, SourceFile *const SF,
                                                                 const DependencyTracker &depTracker, StringRef outputPath) {
  
  // Before writing to the dependencies file path, preserve any previous file
  // that may have been there. No error handling -- this is just a nicety, it
  // doesn't matter if it fails.
  llvm::sys::fs::rename(outputPath, outputPath + "~");
  return withOutputFile(diags, outputPath, [&](llvm::raw_pwrite_stream &out)  {
    abort();
    return false;
  });

}
