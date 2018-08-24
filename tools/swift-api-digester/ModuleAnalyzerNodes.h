//===--- ModuleAnaluzerNodes.h - Nodes for API differ tool ---------------====//
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
//  Describing nodes from a swiftmodule file to detect ABI/API breakages.
//
//===----------------------------------------------------------------------===//

#ifndef __SWIFT_ABI_DIGESTER_MODULE_NODES_H__
#define __SWIFT_ABI_DIGESTER_MODULE_NODES_H__

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"
#include "swift/AST/Attr.h"
#include "swift/AST/Decl.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/USRGeneration.h"
#include "swift/AST/GenericSignature.h"
#include "swift/Basic/ColorUtils.h"
#include "swift/Basic/JSONSerialization.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/Version.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/IDE/Utils.h"
#include "swift/IDE/APIDigesterData.h"
#include <functional>

namespace swift {
namespace ide {
namespace api {

class SDKNode;
typedef SDKNode* NodePtr;
typedef std::map<NodePtr, NodePtr> ParentMap;
typedef std::map<NodePtr, NodePtr> NodeMap;
typedef std::vector<NodePtr> NodeVector;
typedef std::vector<CommonDiffItem> DiffVector;
typedef std::vector<TypeMemberDiffItem> TypeMemberDiffVector;
typedef llvm::MapVector<NodePtr, NodePtr> NodePairVector;

// The interface used to visit the SDK tree.
class SDKNodeVisitor {
  friend SDKNode;
protected:
  NodeVector Ancestors;
  virtual void visit(NodePtr Node) = 0;

  NodePtr parent() {
    if (Ancestors.empty())
      return nullptr;
    return Ancestors.back();
  }

  int depth() {
    return Ancestors.size() + 1;
  }
public:
  virtual ~SDKNodeVisitor() = default;
};

enum class NodeMatchReason: uint8_t {

  // Two nodes are matched because they're both roots.
  Root,

  // The first node is missing.
  Added,

  // The second node is missing.
  Removed,

  // The nodes are considered a pair becuase they have same/similar name.
  Name,

  // The nodes are matched because they're in the same order, e.g. ith child of
  // a type declaration.
  Sequential,

  // The first node is a function and it chanaged to a propery as the second
  // node.
  FuncToProperty,

  // The first node is a global variable and the second node is an enum element.
  ModernizeEnum,

  // The first node is a type declaration and the second node is a type alias
  // of another type declaration.
  TypeToTypeAlias,
};

// This map keeps track of updated nodes; thus we can conveniently find out what
// is the counterpart of a node before or after being updated.
class UpdatedNodesMap {
  NodePairVector MapImpl;
  UpdatedNodesMap(const UpdatedNodesMap& that) = delete;
public:
  UpdatedNodesMap() = default;
  NodePtr findUpdateCounterpart(const SDKNode *Node) const;
  void insert(NodePtr Left, NodePtr Right) {
    assert(Left && Right && "Not update operation.");
    MapImpl.insert({Left, Right});
  }
};

// Describing some attributes with ABI impact. The addition or removal of these
// attributes is considerred ABI-breaking.
struct ABIAttributeInfo {
  const DeclAttrKind Kind;
  const NodeAnnotation Annotation;
  const StringRef Content;
};

struct CheckerOptions {
  bool AvoidLocation;
  bool ABI;
  bool Verbose;
  bool AbortOnModuleLoadFailure;
  bool PrintModule;
  StringRef LocationFilter;
};

class SDKContext {
  llvm::StringSet<> TextData;
  llvm::BumpPtrAllocator Allocator;
  SourceManager SourceMgr;
  DiagnosticEngine Diags;
  UpdatedNodesMap UpdateMap;
  NodeMap TypeAliasUpdateMap;
  NodeMap RevertTypeAliasUpdateMap;
  TypeMemberDiffVector TypeMemberDiffs;

  CheckerOptions Opts;
  std::vector<ABIAttributeInfo> ABIAttrs;

public:
  SDKContext(CheckerOptions Options);
  llvm::BumpPtrAllocator &allocator() {
    return Allocator;
  }
  StringRef buffer(StringRef Text) {
    return TextData.insert(Text).first->getKey();
  }
  UpdatedNodesMap &getNodeUpdateMap() {
    return UpdateMap;
  }
  NodeMap &getTypeAliasUpdateMap() {
    return TypeAliasUpdateMap;
  }
  NodeMap &getRevertTypeAliasUpdateMap() {
    return RevertTypeAliasUpdateMap;
  }
  TypeMemberDiffVector &getTypeMemberDiffs() {
    return TypeMemberDiffs;
  }
  SourceManager &getSourceMgr() {
    return SourceMgr;
  }
  DiagnosticEngine &getDiags() {
    return Diags;
  }
  bool checkingABI() const { return Opts.ABI; }
  const CheckerOptions &getOpts() const { return Opts; }
  ArrayRef<ABIAttributeInfo> getABIAttributeInfo() const { return ABIAttrs; }

  template<class YAMLNodeTy, typename ...ArgTypes>
  void diagnose(YAMLNodeTy node, Diag<ArgTypes...> ID,
                typename detail::PassArgument<ArgTypes>::type... args) {
    auto smRange = node->getSourceRange();
    auto range = SourceRange(SourceLoc(smRange.Start), SourceLoc(smRange.End));
    Diags.diagnose(range.Start, ID, std::forward<ArgTypes>(args)...)
      .highlight(range);
  }
};

enum class KnownTypeKind: uint8_t {
#define KNOWN_TYPE(NAME) NAME,
#include "swift/IDE/DigesterEnums.def"
  Unknown,
};

enum class KnownProtocolKind: uint8_t {
#define KNOWN_PROTOCOL(NAME) NAME,
#include "swift/IDE/DigesterEnums.def"
};

class SDKNodeRoot;

struct SDKNodeInitInfo;

class SDKNode {
  typedef std::vector<SDKNode*>::iterator ChildIt;
  SDKContext &Ctx;
  StringRef Name;
  StringRef PrintedName;
  unsigned TheKind : 4;
  NodeVector Children;
  std::set<NodeAnnotation> Annotations;
  std::map<NodeAnnotation, StringRef> AnnotateComments;
  NodePtr Parent = nullptr;

protected:
  SDKNode(SDKNodeInitInfo Info, SDKNodeKind Kind);

public:
  static SDKNode *constructSDKNode(SDKContext &Ctx, llvm::yaml::MappingNode *Node);
  static void preorderVisit(NodePtr Root, SDKNodeVisitor &Visitor);
  static void postorderVisit(NodePtr Root, SDKNodeVisitor &Visitor);

  bool operator==(const SDKNode &Other) const;
  bool operator!=(const SDKNode &Other) const { return !((*this) == Other); }

  ArrayRef<NodeAnnotation>
    getAnnotations(std::vector<NodeAnnotation> &Scratch) const;
  bool isLeaf() const { return Children.empty(); }
  SDKNodeKind getKind() const { return SDKNodeKind(TheKind); }
  StringRef getName() const { return Name; }
  bool isNameValid() const { return Name != "_"; }
  StringRef getPrintedName() const { return PrintedName; }
  void removeChild(ChildIt CI) { Children.erase(CI); }
  ChildIt getChildBegin() { return Children.begin(); }
  void annotate(NodeAnnotation Anno) { Annotations.insert(Anno); }
  void annotate(NodeAnnotation Anno, StringRef Comment);
  void removeAnnotate(NodeAnnotation Anno);
  NodePtr getParent() const { return Parent; };
  unsigned getChildrenCount() const { return Children.size(); }
  NodePtr childAt(unsigned I) const;
  void removeChild(NodePtr C);
  StringRef getAnnotateComment(NodeAnnotation Anno) const;
  bool isAnnotatedAs(NodeAnnotation Anno) const;
  void addChild(SDKNode *Child);
  ArrayRef<SDKNode*> getChildren() const;
  bool hasSameChildren(const SDKNode &Other) const;
  unsigned getChildIndex(NodePtr Child) const;
  SDKNode* getOnlyChild() const;
  SDKContext &getSDKContext() const { return Ctx; }
  SDKNodeRoot *getRootNode() const;
  template <typename T> const T *getAs() const {
    if (T::classof(this))
      return static_cast<const T*>(this);
    llvm_unreachable("incompatible types");
  }
  template <typename T> T *getAs() {
    if (T::classof(this))
      return static_cast<T*>(this);
    llvm_unreachable("incompatible types");
  }
};

class SDKNodeDecl: public SDKNode {
  DeclKind DKind;
  StringRef Usr;
  StringRef Location;
  StringRef ModuleName;
  std::vector<DeclAttrKind> DeclAttributes;
  bool IsStatic;
  bool IsDeprecated;
  uint8_t ReferenceOwnership;
  StringRef GenericSig;

protected:
  SDKNodeDecl(SDKNodeInitInfo Info, SDKNodeKind Kind);

public:
  StringRef getUsr() const { return Usr; }
  StringRef getLocation() const { return Location; }
  StringRef getModuleName() const {return ModuleName;}
  StringRef getHeaderName() const;
  ArrayRef<DeclAttrKind> getDeclAttributes() const;
  bool hasAttributeChange(const SDKNodeDecl &Another) const;
  swift::ReferenceOwnership getReferenceOwnership() const {
    return swift::ReferenceOwnership(ReferenceOwnership);
  }
  bool isObjc() const { return Usr.startswith("c:"); }
  static bool classof(const SDKNode *N);
  DeclKind getDeclKind() const { return DKind; }
  void printFullyQualifiedName(llvm::raw_ostream &OS) const;
  StringRef getFullyQualifiedName() const;
  bool isSDKPrivate() const;
  bool isDeprecated() const { return IsDeprecated; };
  bool hasDeclAttribute(DeclAttrKind DAKind) const;
  bool isStatic() const { return IsStatic; };
  StringRef getGenericSignature() const { return GenericSig; }
  StringRef getScreenInfo() const;
};

class SDKNodeRoot: public SDKNode {
  /// This keeps track of all decl descendants with USRs.
  llvm::StringMap<llvm::SmallSetVector<SDKNodeDecl*, 2>> DescendantDeclTable;

public:
  SDKNodeRoot(SDKNodeInitInfo Info);
  static SDKNode *getInstance(SDKContext &Ctx);
  static bool classof(const SDKNode *N);
  void registerDescendant(SDKNode *D) {
    if (auto DD = dyn_cast<SDKNodeDecl>(D)) {
      assert(!DD->getUsr().empty());
      DescendantDeclTable[DD->getUsr()].insert(DD);
    }
  }
  ArrayRef<SDKNodeDecl*> getDescendantsByUsr(StringRef Usr) {
    return DescendantDeclTable[Usr].getArrayRef();
  }
};

class SDKNodeType : public SDKNode {
  std::vector<TypeAttrKind> TypeAttributes;
  bool HasDefaultArg;

protected:
  bool hasTypeAttribute(TypeAttrKind DAKind) const;
  SDKNodeType(SDKNodeInitInfo Info, SDKNodeKind Kind);

public:
  KnownTypeKind getTypeKind() const;
  void addTypeAttribute(TypeAttrKind AttrKind);
  ArrayRef<TypeAttrKind> getTypeAttributes() const;
  SDKNodeDecl *getClosestParentDecl() const;

  // When the type node represents a function parameter, this function returns
  // whether the parameter has a default value.
  bool hasDefaultArgument() const { return HasDefaultArg; }
  bool isTopLevelType() const { return !isa<SDKNodeType>(getParent()); }
  static bool classof(const SDKNode *N);
};

class SDKNodeTypeNominal : public SDKNodeType {
  StringRef USR;
public:
  SDKNodeTypeNominal(SDKNodeInitInfo Info);
  // Get the usr of the correspoding nominal type decl.
  StringRef getUsr() const { return USR; }
  static bool classof(const SDKNode *N);
};

class SDKNodeTypeFunc : public SDKNodeType {
public:
  SDKNodeTypeFunc(SDKNodeInitInfo Info);
  bool isEscaping() const { return !hasTypeAttribute(TypeAttrKind::TAK_noescape); }
  static bool classof(const SDKNode *N);
};

class SDKNodeTypeAlias : public SDKNodeType {
public:
  SDKNodeTypeAlias(SDKNodeInitInfo Info);
  const SDKNodeType *getUnderlyingType() const {
    return getOnlyChild()->getAs<SDKNodeType>();
  }
  static bool classof(const SDKNode *N);
};

class SDKNodeVectorViewer {
  ArrayRef<SDKNode*> Collection;
  llvm::function_ref<bool(NodePtr)> Selector;
  typedef ArrayRef<SDKNode*>::iterator VectorIt;
  VectorIt getNext(VectorIt Start);
  class ViewerIterator;

public:
  SDKNodeVectorViewer(ArrayRef<SDKNode*> Collection,
                      llvm::function_ref<bool(NodePtr)> Selector) :
                        Collection(Collection),
                        Selector(Selector) {}
  ViewerIterator begin();
  ViewerIterator end();
};

class SDKNodeVectorViewer::ViewerIterator :
    public std::iterator<std::input_iterator_tag, VectorIt> {
  SDKNodeVectorViewer &Viewer;
  VectorIt P;
public:
  ViewerIterator(SDKNodeVectorViewer &Viewer, VectorIt P) : Viewer(Viewer), P(P) {}
  ViewerIterator(const ViewerIterator& mit) : Viewer(mit.Viewer), P(mit.P) {}
  ViewerIterator& operator++();
  ViewerIterator operator++(int) {ViewerIterator tmp(*this); operator++(); return tmp;}
  bool operator==(const ViewerIterator& rhs) {return P==rhs.P;}
  bool operator!=(const ViewerIterator& rhs) {return P!=rhs.P;}
  const NodePtr& operator*() {return *P;}
};

class SDKNodeDeclType : public SDKNodeDecl {
  StringRef SuperclassUsr;
  std::vector<StringRef> ConformingProtocols;
  StringRef EnumRawTypeName;
public:
  SDKNodeDeclType(SDKNodeInitInfo Info);
  static bool classof(const SDKNode *N);
  StringRef getSuperClassUsr() const { return SuperclassUsr; }
  ArrayRef<StringRef> getAllProtocols() const { return ConformingProtocols; }

#define NOMINAL_TYPE_DECL(ID, PARENT) \
  bool is##ID() const { return getDeclKind() == DeclKind::ID; }
#define DECL(ID, PARENT)
#include "swift/AST/DeclNodes.def"

  StringRef getEnumRawTypeName() const {
    assert(isEnum());
    return EnumRawTypeName;
  }

  Optional<SDKNodeDeclType*> getSuperclass() const;

  /// Finding the node through all children, including the inheritted ones,
  /// whose printed name matches with the given name.
  Optional<SDKNodeDecl*> lookupChildByPrintedName(StringRef Name) const;
  SDKNodeType *getRawValueType() const;
  bool isConformingTo(KnownProtocolKind Kind) const;
};

class SDKNodeDeclTypeAlias : public SDKNodeDecl {
public:
  SDKNodeDeclTypeAlias(SDKNodeInitInfo Info);
  const SDKNodeType* getUnderlyingType() const {
    return getOnlyChild()->getAs<SDKNodeType>();
  }
  static bool classof(const SDKNode *N);
};

class SDKNodeDeclVar : public SDKNodeDecl {
public:
  SDKNodeDeclVar(SDKNodeInitInfo Info);
  static bool classof(const SDKNode *N);
};

class SDKNodeDeclAbstractFunc : public SDKNodeDecl {
  const bool IsThrowing;
  const bool IsMutating;
  const Optional<uint8_t> SelfIndex;

protected:
  SDKNodeDeclAbstractFunc(SDKNodeInitInfo Info, SDKNodeKind Kind);
public:
  bool isThrowing() const { return IsThrowing; }
  bool isMutating() const { return IsMutating; }
  uint8_t getSelfIndex() const { return SelfIndex.getValue(); }
  Optional<uint8_t> getSelfIndexOptional() const { return SelfIndex; }
  bool hasSelfIndex() const { return SelfIndex.hasValue(); }
  static bool classof(const SDKNode *N);
  static StringRef getTypeRoleDescription(SDKContext &Ctx, unsigned Index);
};

class SDKNodeDeclFunction: public SDKNodeDeclAbstractFunc {
public:
  SDKNodeDeclFunction(SDKNodeInitInfo Info);
  SDKNode *getReturnType() { return *getChildBegin(); }
  static bool classof(const SDKNode *N);
};

class SDKNodeDeclConstructor: public SDKNodeDeclAbstractFunc {
public:
  SDKNodeDeclConstructor(SDKNodeInitInfo Info);
  static bool classof(const SDKNode *N);
};

class SDKNodeDeclGetter: public SDKNodeDeclAbstractFunc {
public:
  SDKNodeDeclGetter(SDKNodeInitInfo Info);
  static bool classof(const SDKNode *N);
};

class SDKNodeDeclSetter: public SDKNodeDeclAbstractFunc {
public:
  SDKNodeDeclSetter(SDKNodeInitInfo Info);
  static bool classof(const SDKNode *N);
};

class SwiftDeclCollector: public VisibleDeclConsumer {
  SDKContext &Ctx;
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> OwnedBuffers;
  SDKNode *RootNode;
  llvm::DenseSet<Decl*> KnownDecls;
  // Collected and sorted after we get all of them.
  std::vector<ValueDecl *> ClangMacros;
  std::set<ExtensionDecl*> HandledExtensions;
public:
  void visitAllRoots(SDKNodeVisitor &Visitor) {
    SDKNode::preorderVisit(RootNode, Visitor);
  }
  SwiftDeclCollector(SDKContext &Ctx) : Ctx(Ctx),
    RootNode(SDKNodeRoot::getInstance(Ctx)) {}

  // Construct all roots vector from a given file where a forest was
  // previously dumped.
  void deSerialize(StringRef Filename);

  // Serialize the content of all roots to a given file using JSON format.
  void serialize(StringRef Filename);

  // After collecting decls, either from imported modules or from a previously
  // serialized JSON file, using this function to get the root of the SDK.
  SDKNodeRoot* getSDKRoot() { return static_cast<SDKNodeRoot*>(RootNode); }

  void printTopLevelNames();

public:
  void lookupVisibleDecls(ArrayRef<ModuleDecl *> Modules);
  void processDecl(ValueDecl *VD);
  void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason) override;
};

int dumpSwiftModules(const CompilerInvocation &InitInvok,
                     const llvm::StringSet<> &ModuleNames,
                     StringRef OutputDir,
                     const std::vector<std::string> PrintApis,
                     CheckerOptions Opts);

int dumpSDKContent(const CompilerInvocation &InitInvok,
                   const llvm::StringSet<> &ModuleNames,
                   StringRef OutputFile, CheckerOptions Opts);

/// Mostly for testing purposes, this function de-serializes the SDK dump in
/// dumpPath and re-serialize them to OutputPath. If the tool performs correctly,
/// the contents in dumpPath and OutputPath should be identical.
int deserializeSDKDump(StringRef dumpPath, StringRef OutputPath,
                       CheckerOptions Opts);

int findDeclUsr(StringRef dumpPath, CheckerOptions Opts);
} // end of abi namespace
} // end of ide namespace
} // end of Swift namespace

#endif
