//===--- ModuleAnalyzerNodes.h - Nodes for API differ tool ---------------====//
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
#include "llvm/ADT/STLExtras.h"
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
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/DiagnosticsModuleDiffer.h"
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
namespace json {
class Output;
}
namespace ide {
namespace api {

/// Serialized json format  version number.
///
/// When the json format changes in a way that requires version-specific handling, this number should be incremented.
/// This ensures we could have backward compatibility so that version changes in the format won't stop the checker from working.
const uint8_t DIGESTER_JSON_VERSION = 8; // add isFromExtension
const uint8_t DIGESTER_JSON_DEFAULT_VERSION = 0; // Use this version number for files before we have a version number in json.
const StringRef ABIRootKey = "ABIRoot";
const StringRef ConstValuesKey = "ConstValues";

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

  // The nodes are considered a pair because they have same/similar name.
  Name,

  // The nodes are matched because they're in the same order, e.g. ith child of
  // a type declaration.
  Sequential,

  // The first node is a function and it chanaged to a property as the second
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

// Describing some attributes with ABI/API impact. The addition or removal of these
// attributes is considered breakage.
struct BreakingAttributeInfo {
  const DeclAttrKind Kind;
  const StringRef Content;
};

struct CheckerOptions {
  bool AvoidLocation;
  bool AvoidToolArgs;
  bool ABI;
  bool Verbose;
  bool AbortOnModuleLoadFailure;
  bool PrintModule;
  bool SwiftOnly;
  bool SkipOSCheck;
  bool SkipRemoveDeprecatedCheck;
  bool CompilerStyle;
  bool Migrator;
  StringRef LocationFilter;
  std::vector<std::string> ToolArgs;
  llvm::StringSet<> SPIGroupNamesToIgnore;
};

class SDKContext {
  std::vector<std::unique_ptr<CompilerInstance>> CIs;
  llvm::StringSet<> TextData;
  llvm::BumpPtrAllocator Allocator;
  SourceManager SourceMgr;
  DiagnosticEngine Diags;
  UpdatedNodesMap UpdateMap;
  NodeMap TypeAliasUpdateMap;
  NodeMap RevertTypeAliasUpdateMap;
  TypeMemberDiffVector TypeMemberDiffs;

  CheckerOptions Opts;
  std::vector<BreakingAttributeInfo> BreakingAttrs;
  // The common version of two ABI/API descriptors under comparison.
  llvm::Optional<uint8_t> CommonVersion;

public:
  // Define the set of known identifiers.
#define IDENTIFIER_WITH_NAME(Name, IdStr) StringRef Id_##Name = IdStr;
#include "swift/AST/KnownIdentifiers.def"

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
  // Find a DiagnosticEngine to use when emitting diagnostics at the given Loc.
  DiagnosticEngine &getDiags(SourceLoc Loc = SourceLoc());
  void addDiagConsumer(DiagnosticConsumer &Consumer);
  void setCommonVersion(uint8_t Ver) {
    assert(!CommonVersion.has_value());
    CommonVersion = Ver;
  }
  uint8_t getCommonVersion() const {
    return *CommonVersion;
  }
  bool commonVersionAtLeast(uint8_t Ver) const {
    return getCommonVersion() >= Ver;
  }
  StringRef getPlatformIntroVersion(Decl *D, PlatformKind Kind);
  StringRef getLanguageIntroVersion(Decl *D);
  StringRef getObjcName(Decl *D);
  StringRef getInitKind(Decl *D);
  bool isEqual(const SDKNode &Left, const SDKNode &Right);
  bool checkingABI() const { return Opts.ABI; }
  AccessLevel getAccessLevel(const ValueDecl *VD) const;
  const CheckerOptions &getOpts() const { return Opts; }
  bool shouldIgnore(Decl *D, const Decl* Parent = nullptr) const;
  ArrayRef<BreakingAttributeInfo> getBreakingAttributeInfo() const { return BreakingAttrs; }
  llvm::Optional<uint8_t> getFixedBinaryOrder(ValueDecl *VD) const;

  CompilerInstance &newCompilerInstance() {
    CIs.emplace_back(new CompilerInstance());
    return *CIs.back();
  }
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
class SDKNodeDeclGetter;
class SDKNodeDeclSetter;
struct SDKNodeInitInfo;

class SDKNode {
  typedef std::vector<SDKNode*>::iterator ChildIt;
protected:
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
  virtual ~SDKNode() = default;
public:
  static SDKNode *constructSDKNode(SDKContext &Ctx, llvm::yaml::MappingNode *Node);
  static void preorderVisit(NodePtr Root, SDKNodeVisitor &Visitor);
  static void postorderVisit(NodePtr Root, SDKNodeVisitor &Visitor);

  bool operator==(const SDKNode &Other) const;
  bool operator!=(const SDKNode &Other) const { return !((*this) == Other); }
  void output(json::Output &out, KeyKind Key, bool Value);
  void output(json::Output &out, KeyKind Key, StringRef Value);

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
  NodeVector& getChildren() { return Children; }
  ArrayRef<SDKNode*> getChildren() const { return Children; }
  bool hasSameChildren(const SDKNode &Other) const;
  unsigned getChildIndex(const SDKNode *Child) const;
  SDKNode* getOnlyChild() const;
  SDKContext &getSDKContext() const { return Ctx; }
  SDKNodeRoot *getRootNode() const;
  uint8_t getJsonFormatVersion() const;
  bool versionAtLeast(uint8_t Ver) const { return getJsonFormatVersion() >= Ver; }
  virtual void jsonize(json::Output &Out);
  virtual void diagnose(SDKNode *Right) {};
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

struct PlatformIntroVersion {
  StringRef macos;
  StringRef ios;
  StringRef tvos;
  StringRef watchos;
  StringRef swift;
  bool hasOSAvailability() const {
    return !macos.empty() || !ios.empty() || !tvos.empty() || !watchos.empty();
  }
};

class SDKNodeDecl: public SDKNode {
  DeclKind DKind;
  StringRef Usr;
  StringRef MangledName;
  SourceLoc Loc;
  StringRef Location;
  StringRef ModuleName;
  std::vector<DeclAttrKind> DeclAttributes;
  std::vector<StringRef> SPIGroups;
  bool IsImplicit;
  bool IsStatic;
  bool IsDeprecated;
  bool IsProtocolReq;
  bool IsOverriding;
  bool IsOpen;
  bool IsInternal;
  bool IsABIPlaceholder;
  bool IsFromExtension;
  uint8_t ReferenceOwnership;
  StringRef GenericSig;
  // In ABI mode, this field is populated as a user-friendly version of GenericSig.
  // Diagnostic preferes the sugared versions if they differ as well.
  StringRef SugaredGenericSig;
  llvm::Optional<uint8_t> FixedBinaryOrder;
  PlatformIntroVersion introVersions;
  StringRef ObjCName;

protected:
  SDKNodeDecl(SDKNodeInitInfo Info, SDKNodeKind Kind);
  virtual ~SDKNodeDecl() = default;
public:
  StringRef getUsr() const { return Usr; }
  StringRef getLocation() const { return Location; }
  StringRef getModuleName() const {return ModuleName;}
  StringRef getHeaderName() const;
  ArrayRef<DeclAttrKind> getDeclAttributes() const;
  ArrayRef<StringRef> getSPIGroups() const { return SPIGroups; }
  bool hasAttributeChange(const SDKNodeDecl &Another) const;
  swift::ReferenceOwnership getReferenceOwnership() const {
    return swift::ReferenceOwnership(ReferenceOwnership);
  }
  bool isObjc() const { return Usr.startswith("c:"); }
  static bool classof(const SDKNode *N);
  DeclKind getDeclKind() const { return DKind; }
  void printFullyQualifiedName(llvm::raw_ostream &OS) const;
  StringRef getFullyQualifiedName() const;
  bool isDeprecated() const { return IsDeprecated; };
  bool isProtocolRequirement() const { return IsProtocolReq; }
  bool isNonOptionalProtocolRequirement() const;
  bool hasDeclAttribute(DeclAttrKind DAKind) const;
  bool isImplicit() const { return IsImplicit; };
  bool isStatic() const { return IsStatic; };
  bool isOverriding() const { return IsOverriding; };
  bool isOptional() const { return hasDeclAttribute(DeclAttrKind::DAK_Optional); }
  bool isOpen() const { return IsOpen; }
  bool isInternal() const { return IsInternal; }
  bool isABIPlaceholder() const { return IsABIPlaceholder; }
  bool isFromExtension() const { return IsFromExtension; }
  StringRef getGenericSignature() const { return GenericSig; }
  StringRef getSugaredGenericSignature() const { return SugaredGenericSig; }
  StringRef getScreenInfo() const;
  bool hasFixedBinaryOrder() const { return FixedBinaryOrder.has_value(); }
  uint8_t getFixedBinaryOrder() const { return *FixedBinaryOrder; }
  PlatformIntroVersion getIntroducingVersion() const { return introVersions; }
  StringRef getObjCName() const { return ObjCName; }
  SourceLoc getLoc() const { return Loc; }
  virtual void jsonize(json::Output &Out) override;
  virtual void diagnose(SDKNode *Right) override;

  // The first argument of the diag is always screening info.
  template<typename ...ArgTypes>
  void emitDiag(SourceLoc Loc,
                Diag<StringRef, ArgTypes...> ID,
                typename detail::PassArgument<ArgTypes>::type... Args) const {
    // Don't emit objc decls if we care about swift exclusively
    if (Ctx.getOpts().SwiftOnly) {
      if (isObjc())
        return;
    }
    // Don't emit SPIs if the group name is out-out.
    for (auto spi: getSPIGroups()) {
      if (Ctx.getOpts().SPIGroupNamesToIgnore.contains(spi)) {
        return;
      }
    }
    Ctx.getDiags(Loc).diagnose(Loc, ID, getScreenInfo(), std::move(Args)...);
  }
};

class SDKNodeRoot: public SDKNode {
  /// This keeps track of all decl descendants with USRs.
  llvm::StringMap<llvm::SmallSetVector<SDKNodeDecl*, 2>> DescendantDeclTable;
  /// The tool invocation arguments to generate this root node. We shouldn't need APIs for it.
  std::vector<StringRef> ToolArgs;
  uint8_t JsonFormatVer;
public:
  SDKNodeRoot(SDKNodeInitInfo Info);
  static SDKNode *getInstance(SDKContext &Ctx);
  static bool classof(const SDKNode *N);
  void registerDescendant(SDKNode *D);
  virtual void jsonize(json::Output &Out) override;
  uint8_t getJsonFormatVersion() const { return JsonFormatVer; }
  ArrayRef<SDKNodeDecl*> getDescendantsByUsr(StringRef Usr) {
    return DescendantDeclTable[Usr].getArrayRef();
  }
};

class SDKNodeType: public SDKNode {
  std::vector<TypeAttrKind> TypeAttributes;
  bool HasDefaultArg;

  // Empty() implies "Default"
  StringRef ParamValueOwnership;

protected:
  SDKNodeType(SDKNodeInitInfo Info, SDKNodeKind Kind);
  ~SDKNodeType() = default;
public:
  bool hasTypeAttribute(TypeAttrKind DAKind) const;
  KnownTypeKind getTypeKind() const;
  void addTypeAttribute(TypeAttrKind AttrKind);
  ArrayRef<TypeAttrKind> getTypeAttributes() const;
  SDKNodeDecl *getClosestParentDecl() const;

  // When the type node represents a function parameter, this function returns
  // whether the parameter has a default value.
  bool hasDefaultArgument() const { return HasDefaultArg; }
  bool isTopLevelType() const { return !isa<SDKNodeType>(getParent()); }
  StringRef getTypeRoleDescription() const;
  StringRef getParamValueOwnership() const;
  static bool classof(const SDKNode *N);
  virtual void jsonize(json::Output &Out) override;
  virtual void diagnose(SDKNode *Right) override;
  bool hasAttributeChange(const SDKNodeType &Another) const;
};

class SDKNodeTypeNominal : public SDKNodeType {
  StringRef USR;
  StringRef MangledName;
public:
  SDKNodeTypeNominal(SDKNodeInitInfo Info);
  // Get the usr of the corresponding nominal type decl.
  StringRef getUsr() const { return USR; }
  static bool classof(const SDKNode *N);
  void jsonize(json::Output &Out) override;
};

class SDKNodeTypeFunc : public SDKNodeType {
public:
  SDKNodeTypeFunc(SDKNodeInitInfo Info);
  bool isEscaping() const { return hasTypeAttribute(TypeAttrKind::TAK_noescape); }
  static bool classof(const SDKNode *N);
  void diagnose(SDKNode *Right) override;
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

class SDKNodeVectorViewer::ViewerIterator {
  SDKNodeVectorViewer &Viewer;
  VectorIt P;
public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = VectorIt;
  using difference_type = std::ptrdiff_t;
  using pointer = value_type*;
  using reference = value_type&;    

  ViewerIterator(SDKNodeVectorViewer &Viewer, VectorIt P) : Viewer(Viewer), P(P) {}
  ViewerIterator(const ViewerIterator& mit) : Viewer(mit.Viewer), P(mit.P) {}
  ViewerIterator& operator++();
  ViewerIterator operator++(int) {ViewerIterator tmp(*this); operator++(); return tmp;}
  bool operator==(const ViewerIterator& rhs) {return P==rhs.P;}
  bool operator!=(const ViewerIterator& rhs) {return P!=rhs.P;}
  const NodePtr& operator*() {return *P;}
};

class SDKNodeDeclType: public SDKNodeDecl {
  StringRef SuperclassUsr;
  std::vector<StringRef> SuperclassNames;
  std::vector<SDKNode*> Conformances;
  StringRef EnumRawTypeName;
  // Check whether the type declaration is pulled from an external module so we
  // can incorporate extensions in the interested module.
  bool IsExternal;
  bool IsEnumExhaustive;
  bool HasMissingDesignatedInitializers;
  bool InheritsConvenienceInitializers;
public:
  SDKNodeDeclType(SDKNodeInitInfo Info);
  static bool classof(const SDKNode *N);
  StringRef getSuperClassUsr() const { return SuperclassUsr; }
  ArrayRef<StringRef> getClassInheritanceChain() const { return SuperclassNames; }
  void addConformance(SDKNode *Conf);
  ArrayRef<SDKNode*> getConformances() const { return Conformances; }
  NodeVector getConformances() { return Conformances; }
  bool isExternal() const { return IsExternal; }
  bool isExtension() const { return isExternal(); }
  StringRef getSuperClassName() const {
    return SuperclassNames.empty() ? StringRef() : SuperclassNames.front();
  };

#define NOMINAL_TYPE_DECL(ID, PARENT) \
  bool is##ID() const { return getDeclKind() == DeclKind::ID; }
#define DECL(ID, PARENT)
#include "swift/AST/DeclNodes.def"

  StringRef getEnumRawTypeName() const {
    assert(isEnum());
    return EnumRawTypeName;
  }

  bool isEnumExhaustive() const {
    assert(isEnum());
    return IsEnumExhaustive;
  }

  bool hasMissingDesignatedInitializers() const {
    return HasMissingDesignatedInitializers;
  };
  bool inheritsConvenienceInitializers() const {
    return InheritsConvenienceInitializers;
  };

  llvm::Optional<SDKNodeDeclType *> getSuperclass() const;

  /// Finding the node through all children, including the inherited ones,
  /// whose printed name matches with the given name.
  llvm::Optional<SDKNodeDecl *> lookupChildByPrintedName(StringRef Name) const;
  SDKNodeType *getRawValueType() const;
  bool isConformingTo(KnownProtocolKind Kind) const;
  void jsonize(json::Output &out) override;
  void diagnose(SDKNode *Right) override;
};

/// Keeps track of a conformance; the children of this node are
/// SDKNodeTypeWitness. The conformance node should have no parent since
/// they are stored as an additional property in SDKNodeDeclType.
/// The SDKNode part of the conformance node is constructed using the protocol
/// in the conformance, thus getName() will give us the name of the protocol.
class SDKNodeConformance: public SDKNode {
  StringRef Usr;
  StringRef MangledName;
  SDKNodeDeclType *TypeDecl;
  friend class SDKNodeDeclType;
  bool IsABIPlaceholder;
public:
  SDKNodeConformance(SDKNodeInitInfo Info);
  StringRef getUsr() const { return Usr; }
  ArrayRef<SDKNode*> getTypeWitnesses() const { return Children; }
  SDKNodeDeclType *getNominalTypeDecl() const { return TypeDecl; }
  bool isABIPlaceholder() const { return IsABIPlaceholder; }
  void jsonize(json::Output &out) override;
  static bool classof(const SDKNode *N);
};

/// Keep track of a type witness of an associated type requirement. These nodes
/// only appear as children of SDKNodeConformance.
/// The SDKNode part of this node is constructed using the associated type decl;
/// thus getName() will give us the name of the associated type. The only child
/// of this node is a type node witnessing the associated type requirement.
class SDKNodeTypeWitness: public SDKNode {
public:
  SDKNodeTypeWitness(SDKNodeInitInfo Info);
  StringRef getWitnessedTypeName() const;
  SDKNodeType *getUnderlyingType() const;
  static bool classof(const SDKNode *N);
};

class SDKNodeDeclOperator : public SDKNodeDecl {
public:
  SDKNodeDeclOperator(SDKNodeInitInfo Info);
  static bool classof(const SDKNode *N);
  void diagnose(SDKNode *Right) override;
};

class SDKNodeDeclTypeAlias : public SDKNodeDecl {
public:
  SDKNodeDeclTypeAlias(SDKNodeInitInfo Info);
  const SDKNodeType* getUnderlyingType() const {
    return getOnlyChild()->getAs<SDKNodeType>();
  }
  static bool classof(const SDKNode *N);
};

class SDKNodeDeclAssociatedType: public SDKNodeDecl {
public:
  SDKNodeDeclAssociatedType(SDKNodeInitInfo Info);
  const SDKNodeType* getDefault() const {
    return getChildrenCount() ? getOnlyChild()->getAs<SDKNodeType>(): nullptr;
  }
  static bool classof(const SDKNode *N);
};

class SDKNodeDeclAccessor;
class SDKNodeDeclVar : public SDKNodeDecl {
  bool IsLet;
  bool HasStorage;
  std::vector<SDKNode*> Accessors;
public:
  SDKNodeDeclVar(SDKNodeInitInfo Info);
  static bool classof(const SDKNode *N);
  SDKNodeDeclAccessor *getAccessor(AccessorKind Kind) const;
  ArrayRef<SDKNode*> getAllAccessors() const { return Accessors; }
  SDKNodeType *getType() const;
  bool isLet() const { return IsLet; }
  void jsonize(json::Output &Out) override;
  void diagnose(SDKNode *Right) override;
  bool hasStorage() const { return HasStorage; }
  void addAccessor(SDKNode* AC);
};

class SDKNodeDeclMacro : public SDKNodeDecl {
public:
  SDKNodeDeclMacro(SDKNodeInitInfo Info);
  static bool classof(const SDKNode *N);
  SDKNodeType *getType() const;
};

class SDKNodeDeclAbstractFunc : public SDKNodeDecl {
  bool IsThrowing;
  bool ReqNewWitnessTableEntry;
  llvm::Optional<uint8_t> SelfIndex;

protected:
  SDKNodeDeclAbstractFunc(SDKNodeInitInfo Info, SDKNodeKind Kind);
  virtual ~SDKNodeDeclAbstractFunc() = default;
public:
  bool isThrowing() const { return IsThrowing; }
  bool reqNewWitnessTableEntry() const { return ReqNewWitnessTableEntry; }
  uint8_t getSelfIndex() const { return SelfIndex.value(); }
  llvm::Optional<uint8_t> getSelfIndexOptional() const { return SelfIndex; }
  bool hasSelfIndex() const { return SelfIndex.has_value(); }
  static bool classof(const SDKNode *N);
  virtual void jsonize(json::Output &out) override;
  static StringRef getTypeRoleDescription(SDKContext &Ctx, unsigned Index);
  virtual void diagnose(SDKNode *Right) override;
};

class SDKNodeDeclSubscript: public SDKNodeDeclAbstractFunc {
  bool HasStorage;
  std::vector<SDKNode*> Accessors;
public:
  SDKNodeDeclSubscript(SDKNodeInitInfo Info);
  static bool classof(const SDKNode *N);
  bool hasStorage() const { return HasStorage; }
  SDKNodeDeclAccessor *getAccessor(AccessorKind Kind) const;
  ArrayRef<SDKNode*> getAllAccessors() const { return Accessors; }
  void jsonize(json::Output &Out) override;
  void addAccessor(SDKNode* AC);
};

class SDKNodeDeclFunction: public SDKNodeDeclAbstractFunc {
  StringRef FuncSelfKind;
public:
  SDKNodeDeclFunction(SDKNodeInitInfo Info);
  SDKNode *getReturnType() { return *getChildBegin(); }
  StringRef getSelfAccessKind() const { return FuncSelfKind; }
  static bool classof(const SDKNode *N);
  void jsonize(json::Output &Out) override;
  void diagnose(SDKNode *Right) override;
};

class SDKNodeDeclConstructor: public SDKNodeDeclAbstractFunc {
  StringRef InitKind;
public:
  SDKNodeDeclConstructor(SDKNodeInitInfo Info);
  static bool classof(const SDKNode *N);
  CtorInitializerKind getInitKind() const;
  void jsonize(json::Output &Out) override;
};

// Note: Accessor doesn't have Parent pointer.
class SDKNodeDeclAccessor: public SDKNodeDeclAbstractFunc {
  SDKNodeDecl *Owner;
  AccessorKind AccKind;
  friend class SDKNodeDeclVar;
  friend class SDKNodeDeclSubscript;
public:
  SDKNodeDeclAccessor(SDKNodeInitInfo Info);
  AccessorKind getAccessorKind() const { return AccKind; }
  static bool classof(const SDKNode *N);
  SDKNodeDecl* getStorage() const { return Owner; }
  void jsonize(json::Output &Out) override;
};

class SDKNodeDeclImport: public SDKNodeDecl {
public:
  SDKNodeDeclImport(SDKNodeInitInfo Info);
  static bool classof(const SDKNode *N);
};

// The additional information we need for a type node in the digest.
// We use type node to represent entities more than types, e.g. parameters, so
// this struct is necessary to pass down to create a type node.
struct TypeInitInfo {
  bool IsImplicitlyUnwrappedOptional = false;
  bool hasDefaultArgument = false;
  StringRef ValueOwnership;
};

struct PayLoad;

class SwiftDeclCollector: public VisibleDeclConsumer {
  SDKContext &Ctx;
  SDKNode *RootNode;
  llvm::SetVector<Decl*> KnownDecls;
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
  void serialize(llvm::raw_ostream &os);
  static void serialize(llvm::raw_ostream &os, SDKNode *Root, PayLoad otherInfo);

  // After collecting decls, either from imported modules or from a previously
  // serialized JSON file, using this function to get the root of the SDK.
  SDKNodeRoot* getSDKRoot() { return static_cast<SDKNodeRoot*>(RootNode); }

  void printTopLevelNames();

  /// Adds all conformances from the provided NominalTypeDecl to the provided
  /// SDK node for that type decl.
  void addConformancesToTypeDecl(SDKNodeDeclType *Root, NominalTypeDecl* NTD);
  void addMembersToRoot(SDKNode *Root, IterableDeclContext *Context);

  SDKNode *constructTypeWitnessNode(AssociatedTypeDecl *Assoc, Type Ty);
  SDKNode *constructConformanceNode(ProtocolConformance *Conform);
  SDKNode *constructSubscriptDeclNode(SubscriptDecl *SD);
  SDKNode *constructAssociatedTypeNode(AssociatedTypeDecl *ATD);
  SDKNode *constructTypeAliasNode(TypeAliasDecl *TAD);
  SDKNode *constructMacroNode(MacroDecl *MAD);
  SDKNode *constructVarNode(ValueDecl *VD);
  SDKNode *constructExternalExtensionNode(NominalTypeDecl *NTD,
                                          ArrayRef<ExtensionDecl*> AllExts);
  SDKNode *constructTypeDeclNode(NominalTypeDecl *NTD);
  SDKNode *constructInitNode(ConstructorDecl *CD);
  SDKNode *constructFunctionNode(FuncDecl* FD, SDKNodeKind Kind);
  SDKNode *constructOperatorDeclNode(OperatorDecl *OD);
  std::vector<SDKNode*> createParameterNodes(ParameterList *PL);
  SDKNode *constructTypeNode(Type T, TypeInitInfo Info = TypeInitInfo());
  void processValueDecl(ValueDecl *VD);
  void foundDecl(ValueDecl *VD, DeclVisibilityKind Reason,
                 DynamicLookupInfo dynamicLookupInfo = {}) override;
  void processDecl(Decl *D);
public:
  void lookupVisibleDecls(ArrayRef<ModuleDecl *> Modules);
};

void detectRename(SDKNode *L, SDKNode *R);

int dumpSwiftModules(const CompilerInvocation &InitInvoke,
                     const llvm::StringSet<> &ModuleNames,
                     StringRef OutputDir,
                     const std::vector<std::string> PrintApis,
                     CheckerOptions Opts);

SDKNodeRoot *getSDKNodeRoot(SDKContext &SDKCtx,
                            const CompilerInvocation &InitInvoke,
                            const llvm::StringSet<> &ModuleNames);

SDKNodeRoot *getEmptySDKNodeRoot(SDKContext &SDKCtx);

void dumpSDKRoot(SDKNodeRoot *Root, PayLoad load, llvm::raw_ostream &os);
void dumpSDKRoot(SDKNodeRoot *Root, llvm::raw_ostream &os);

int dumpSDKContent(const CompilerInvocation &InitInvoke,
                   const llvm::StringSet<> &ModuleNames,
                   llvm::raw_ostream &os, CheckerOptions Opts);

void dumpModuleContent(ModuleDecl *MD, llvm::raw_ostream &os, bool ABI,
                       bool Empty);

/// Mostly for testing purposes, this function de-serializes the SDK dump in
/// dumpPath and re-serialize them to OutputPath. If the tool performs correctly,
/// the contents in dumpPath and OutputPath should be identical.
int deserializeSDKDump(StringRef dumpPath, StringRef OutputPath,
                       CheckerOptions Opts);

int findDeclUsr(StringRef dumpPath, CheckerOptions Opts);

void nodeSetDifference(ArrayRef<SDKNode*> Left, ArrayRef<SDKNode*> Right,
  NodeVector &LeftMinusRight, NodeVector &RightMinusLeft);

bool hasValidParentPtr(SDKNodeKind kind);
} // end of abi namespace
} // end of ide namespace
} // end of Swift namespace

namespace swift {
namespace json {
template <> struct ObjectTraits<ide::api::SDKNodeRoot> {
  static void mapping(Output &out, ide::api::SDKNodeRoot &contents) {
    contents.jsonize(out);
  }
};
}}

#endif
