#include "llvm/ADT/STLExtras.h"
#include "swift/AST/ASTMangler.h"
#include "swift/Basic/Defer.h"
#include "swift/Sema/IDETypeChecking.h"
#include <swift/APIDigester/ModuleAnalyzerNodes.h>
#include <algorithm>

using namespace swift;
using namespace ide;
using namespace api;

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

namespace {
static PrintOptions getTypePrintOpts(CheckerOptions CheckerOpts) {
  PrintOptions Opts;
  Opts.SynthesizeSugarOnTypes = true;
  Opts.UseOriginallyDefinedInModuleNames = true;
  if (!CheckerOpts.Migrator) {
    // We should always print fully qualified type names for checking either
    // API or ABI stability.
    Opts.FullyQualifiedTypes = true;
  }
  return Opts;
}
} // End of anonymous namespace.

struct swift::ide::api::SDKNodeInitInfo {
  SDKContext &Ctx;
  DeclKind DKind;
  AccessorKind AccKind;
  SourceLoc Loc;
#define KEY_STRING(X, Y) StringRef X;
#include "swift/IDE/DigesterEnums.def"
#define KEY_BOOL(X, Y) bool X = false;
#include "swift/IDE/DigesterEnums.def"
#define KEY_UINT(X, Y) llvm::Optional<uint8_t> X;
#include "swift/IDE/DigesterEnums.def"
#define KEY_STRING_ARR(X, Y) std::vector<StringRef> X;
#include "swift/IDE/DigesterEnums.def"

  swift::ReferenceOwnership ReferenceOwnership = ReferenceOwnership::Strong;
  std::vector<DeclAttrKind> DeclAttrs;
  std::vector<TypeAttrKind> TypeAttrs;

  SDKNodeInitInfo(SDKContext &Ctx) : Ctx(Ctx) {}
  SDKNodeInitInfo(SDKContext &Ctx, Decl *D);
  SDKNodeInitInfo(SDKContext &Ctx, ValueDecl *VD);
  SDKNodeInitInfo(SDKContext &Ctx, OperatorDecl *D);
  SDKNodeInitInfo(SDKContext &Ctx, ImportDecl *ID);
  SDKNodeInitInfo(SDKContext &Ctx, ProtocolConformanceRef Conform);
  SDKNodeInitInfo(SDKContext &Ctx, Type Ty, TypeInitInfo Info = TypeInitInfo());
  SDKNode* createSDKNode(SDKNodeKind Kind);
};

bool swift::ide::api::hasValidParentPtr(SDKNodeKind kind) {
  switch(kind) {
  case SDKNodeKind::Conformance:
  case SDKNodeKind::DeclAccessor:
    return false;
  default:
    return true;
  }
}

SDKContext::SDKContext(CheckerOptions Opts): Diags(SourceMgr), Opts(Opts) {}

DiagnosticEngine &SDKContext::getDiags(SourceLoc Loc) {
  // If the location is invalid, we just use the locally created DiagEngine.
  if (Loc.isInvalid())
    return Diags;
  // If the Loc is valid, it may belong to any of the SourceManagers owned by
  // the ASTContexts we created, thus we should go through the ASTContxts to find
  // the right DiagnosticEngine to use.
  for (auto &CI: CIs) {
    if (CI->getSourceMgr().isOwning(Loc))
      return CI->getDiags();
  }
  llvm_unreachable("cannot find diagnostic engine to use");
}

void SDKContext::addDiagConsumer(DiagnosticConsumer &Consumer) {
  // we may emit diagnostics via any of the diagnostic engine, so add the consumer
  // to all of them.
  Diags.addConsumer(Consumer);
  for (auto &CI: CIs) {
    CI->getDiags().addConsumer(Consumer);
  }
}

void SDKNodeRoot::registerDescendant(SDKNode *D) {
  // Operator doesn't have usr
  if (isa<SDKNodeDeclOperator>(D))
    return;
  // Import doesn't have usr
  if (isa<SDKNodeDeclImport>(D))
    return;
  if (auto DD = dyn_cast<SDKNodeDecl>(D)) {
    assert(!DD->getUsr().empty());
    DescendantDeclTable[DD->getUsr()].insert(DD);
  }
}

SDKNode::SDKNode(SDKNodeInitInfo Info, SDKNodeKind Kind): Ctx(Info.Ctx),
  Name(Info.Name), PrintedName(Info.PrintedName), TheKind(unsigned(Kind)) {}

SDKNodeRoot::SDKNodeRoot(SDKNodeInitInfo Info): SDKNode(Info, SDKNodeKind::Root),
  ToolArgs(Info.ToolArgs),
  JsonFormatVer(Info.JsonFormatVer.has_value() ? *Info.JsonFormatVer : DIGESTER_JSON_DEFAULT_VERSION) {}

SDKNodeDecl::SDKNodeDecl(SDKNodeInitInfo Info, SDKNodeKind Kind)
      : SDKNode(Info, Kind), DKind(Info.DKind), Usr(Info.Usr),
        MangledName(Info.MangledName), Loc(Info.Loc),
        Location(Info.Location), ModuleName(Info.ModuleName),
        DeclAttributes(Info.DeclAttrs),
        SPIGroups(Info.SPIGroups),
        IsImplicit(Info.IsImplicit),
        IsStatic(Info.IsStatic), IsDeprecated(Info.IsDeprecated),
        IsProtocolReq(Info.IsProtocolReq),
        IsOverriding(Info.IsOverriding),
        IsOpen(Info.IsOpen),
        IsInternal(Info.IsInternal), IsABIPlaceholder(Info.IsABIPlaceholder),
        IsFromExtension(Info.IsFromExtension),
        ReferenceOwnership(uint8_t(Info.ReferenceOwnership)),
        GenericSig(Info.GenericSig),
        SugaredGenericSig(Info.SugaredGenericSig),
        FixedBinaryOrder(Info.FixedBinaryOrder),
        introVersions({Info.IntromacOS, Info.IntroiOS, Info.IntrotvOS,
                       Info.IntrowatchOS, Info.Introswift}),
        ObjCName(Info.ObjCName) {}

SDKNodeType::SDKNodeType(SDKNodeInitInfo Info, SDKNodeKind Kind):
  SDKNode(Info, Kind), TypeAttributes(Info.TypeAttrs),
  HasDefaultArg(Info.HasDefaultArg),
  ParamValueOwnership(Info.ParamValueOwnership) {}

SDKNodeTypeNominal::SDKNodeTypeNominal(SDKNodeInitInfo Info):
  SDKNodeType(Info, SDKNodeKind::TypeNominal), USR(Info.Usr),
  MangledName(Info.MangledName) {}

SDKNodeTypeFunc::SDKNodeTypeFunc(SDKNodeInitInfo Info):
  SDKNodeType(Info, SDKNodeKind::TypeFunc) {}

SDKNodeTypeAlias::SDKNodeTypeAlias(SDKNodeInitInfo Info):
  SDKNodeType(Info, SDKNodeKind::TypeAlias) {}

SDKNodeDeclType::SDKNodeDeclType(SDKNodeInitInfo Info):
  SDKNodeDecl(Info, SDKNodeKind::DeclType), SuperclassUsr(Info.SuperclassUsr),
  SuperclassNames(Info.SuperclassNames),
  EnumRawTypeName(Info.EnumRawTypeName),
  IsExternal(Info.IsExternal),
  IsEnumExhaustive(Info.IsEnumExhaustive),
  HasMissingDesignatedInitializers(Info.HasMissingDesignatedInitializers),
  InheritsConvenienceInitializers(Info.InheritsConvenienceInitializers) {}

SDKNodeConformance::SDKNodeConformance(SDKNodeInitInfo Info):
  SDKNode(Info, SDKNodeKind::Conformance),
  Usr(Info.Usr), MangledName(Info.MangledName),
  IsABIPlaceholder(Info.IsABIPlaceholder) {}

SDKNodeTypeWitness::SDKNodeTypeWitness(SDKNodeInitInfo Info):
  SDKNode(Info, SDKNodeKind::TypeWitness) {}

SDKNodeDeclOperator::SDKNodeDeclOperator(SDKNodeInitInfo Info):
  SDKNodeDecl(Info, SDKNodeKind::DeclOperator) {}

SDKNodeDeclTypeAlias::SDKNodeDeclTypeAlias(SDKNodeInitInfo Info):
  SDKNodeDecl(Info, SDKNodeKind::DeclTypeAlias) {}

SDKNodeDeclVar::SDKNodeDeclVar(SDKNodeInitInfo Info): 
  SDKNodeDecl(Info, SDKNodeKind::DeclVar), IsLet(Info.IsLet),
  HasStorage(Info.HasStorage) {}

SDKNodeDeclMacro::SDKNodeDeclMacro(SDKNodeInitInfo Info):
  SDKNodeDecl(Info, SDKNodeKind::DeclMacro) {}

SDKNodeDeclAbstractFunc::SDKNodeDeclAbstractFunc(SDKNodeInitInfo Info,
  SDKNodeKind Kind): SDKNodeDecl(Info, Kind), IsThrowing(Info.IsThrowing),
                     ReqNewWitnessTableEntry(Info.ReqNewWitnessTableEntry),
                     SelfIndex(Info.SelfIndex) {}

SDKNodeDeclFunction::SDKNodeDeclFunction(SDKNodeInitInfo Info):
  SDKNodeDeclAbstractFunc(Info, SDKNodeKind::DeclFunction),
  FuncSelfKind(Info.FuncSelfKind) {}

SDKNodeDeclConstructor::SDKNodeDeclConstructor(SDKNodeInitInfo Info):
  SDKNodeDeclAbstractFunc(Info, SDKNodeKind::DeclConstructor), InitKind(Info.InitKind) {}

SDKNodeDeclAccessor::SDKNodeDeclAccessor(SDKNodeInitInfo Info):
  SDKNodeDeclAbstractFunc(Info, SDKNodeKind::DeclAccessor),
  AccKind(Info.AccKind) {}

SDKNodeDeclImport::SDKNodeDeclImport(SDKNodeInitInfo Info):
  SDKNodeDecl(Info, SDKNodeKind::DeclImport) {}

SDKNodeDeclAssociatedType::SDKNodeDeclAssociatedType(SDKNodeInitInfo Info):
  SDKNodeDecl(Info, SDKNodeKind::DeclAssociatedType) {}

SDKNodeDeclSubscript::SDKNodeDeclSubscript(SDKNodeInitInfo Info):
  SDKNodeDeclAbstractFunc(Info, SDKNodeKind::DeclSubscript),
  HasStorage(Info.HasStorage) {}

StringRef SDKNodeDecl::getHeaderName() const {
  if (Location.empty())
    return StringRef();
  return llvm::sys::path::filename(Location.split(":").first);
}

static SDKNodeDeclAccessor *getAccessorInternal(ArrayRef<SDKNode*> Accessors,
                                                AccessorKind Kind) {
  for (auto *AC: Accessors) {
    if (cast<SDKNodeDeclAccessor>(AC)->getAccessorKind() == Kind) {
      return cast<SDKNodeDeclAccessor>(AC);
    }
  }
  return nullptr;
}

SDKNodeDeclAccessor *SDKNodeDeclVar::getAccessor(AccessorKind Kind) const {
  return getAccessorInternal(Accessors, Kind);
}

SDKNodeDeclAccessor *SDKNodeDeclSubscript::getAccessor(AccessorKind Kind) const {
  return getAccessorInternal(Accessors, Kind);
}

SDKNodeType *SDKNodeDeclVar::getType() const {
  return cast<SDKNodeType>(childAt(0));
}

SDKNodeType *SDKNodeDeclMacro::getType() const {
  return cast<SDKNodeType>(childAt(0));
}

NodePtr UpdatedNodesMap::findUpdateCounterpart(const SDKNode *Node) const {
  assert(Node->isAnnotatedAs(NodeAnnotation::Updated) && "Not update operation.");
  auto FoundPair = std::find_if(MapImpl.begin(), MapImpl.end(),
                      [&](std::pair<NodePtr, NodePtr> Pair) {
    return Pair.second == Node || Pair.first == Node;
  });
  assert(FoundPair != MapImpl.end() && "Cannot find update counterpart.");
  return Node == FoundPair->first ? FoundPair->second : FoundPair->first;
}

#define NODE_KIND_RANGE(ID, FIRST, LAST)                                      \
bool SDKNode##ID::classof(const SDKNode *N) {                                 \
  return N->getKind() >= SDKNodeKind::FIRST &&                                \
    N->getKind() <= SDKNodeKind::LAST;                                        \
}
#include "swift/IDE/DigesterEnums.def"

unsigned SDKNode::getChildIndex(const SDKNode* Child) const {
  auto It = std::find(Children.begin(), Children.end(), Child);
  assert(It != Children.end() && "cannot find the child");
  return It - Children.begin();
}

SDKNode* SDKNode::getOnlyChild() const {
  assert(Children.size() == 1 && "more that one child.");
  return *Children.begin();
}

SDKNodeRoot *SDKNode::getRootNode() const {
  for (auto *Root = const_cast<SDKNode*>(this); Root;) {
    if (auto Result = dyn_cast<SDKNodeRoot>(Root))
      return Result;
    if (auto *Conf = dyn_cast<SDKNodeConformance>(Root)) {
      Root = Conf->getNominalTypeDecl();
    } else if (auto *Acc = dyn_cast<SDKNodeDeclAccessor>(Root)) {
      Root = Acc->getStorage();
    } else {
      Root = Root->getParent();
    }
  }
  llvm_unreachable("Unhandled SDKNodeKind in switch.");
}

uint8_t SDKNode::getJsonFormatVersion() const {
  return getRootNode()->getJsonFormatVersion();
}

void SDKNode::addChild(SDKNode *Child) {
  Child->Parent = this;
  Children.push_back(Child);
  if (auto *Root = dyn_cast<SDKNodeRoot>(this)) {
    struct DeclCollector: public SDKNodeVisitor {
      SDKNodeRoot &Root;
      DeclCollector(SDKNodeRoot &Root): Root(Root) {}
      void visit(NodePtr Node) override {
        Root.registerDescendant(Node);
      }
    } Collector(*Root);
    SDKNode::preorderVisit(Child, Collector);
  }
}

NodePtr SDKNode::childAt(unsigned I) const {
  assert(I < getChildrenCount());
  return getChildren()[I];
}

void SDKNode::removeChild(NodePtr C) {
  Children.erase(std::find(Children.begin(), Children.end(), C));
}

void SDKNode::annotate(NodeAnnotation Anno, StringRef Comment) {
  assert(!Comment.empty());
  if(isAnnotatedAs(Anno))
    return;
  annotate(Anno);
  AnnotateComments[Anno] = Comment;
}

void SDKNode::removeAnnotate(NodeAnnotation Anno) {
  assert(isAnnotatedAs(Anno));
  Annotations.erase(Anno);
  AnnotateComments.erase(Anno);
  assert(!isAnnotatedAs(Anno));
  assert(AnnotateComments.count(Anno) == 0);
}

StringRef SDKNode::getAnnotateComment(NodeAnnotation Anno) const {
  return AnnotateComments.find(Anno)->second;
}

ArrayRef<NodeAnnotation> SDKNode::
getAnnotations(std::vector<NodeAnnotation> &Scratch) const {
  for (auto Ann : Annotations)
    Scratch.push_back(Ann);
  return llvm::makeArrayRef(Scratch);
}

bool SDKNode::isAnnotatedAs(NodeAnnotation Anno) const {
  return Annotations.find(Anno) != Annotations.end();;
}

void SDKNode::preorderVisit(NodePtr Root, SDKNodeVisitor &Visitor) {
  Visitor.visit(Root);
  Visitor.Ancestors.push_back(Root);
  for (auto Child : Root->Children)
    preorderVisit(Child, Visitor);
  Visitor.Ancestors.pop_back();
}

void SDKNode::postorderVisit(NodePtr Root, SDKNodeVisitor &Visitor) {
  Visitor.Ancestors.push_back(Root);
  for (auto Child : Root->Children)
    postorderVisit(Child, Visitor);
  Visitor.Ancestors.pop_back();
  Visitor.visit(Root);
}

SDKNodeVectorViewer::VectorIt
SDKNodeVectorViewer::getNext(VectorIt Start) {
  for (auto It = Start; It != Collection.end(); ++ It)
    if (Selector(*It))
      return It;
  return Collection.end();
}

SDKNodeVectorViewer::ViewerIterator&
SDKNodeVectorViewer::ViewerIterator::operator++() {
  P = Viewer.getNext(P + 1);
  return *this;
}

SDKNodeVectorViewer::ViewerIterator SDKNodeVectorViewer::begin() {
  return ViewerIterator(*this, getNext(Collection.begin()));
}

SDKNodeVectorViewer::ViewerIterator SDKNodeVectorViewer::end() {
  return ViewerIterator(*this, Collection.end());
}

KnownTypeKind SDKNodeType::getTypeKind() const {
#define KNOWN_TYPE(NAME) if (getName() == #NAME) return KnownTypeKind::NAME;
#include "swift/IDE/DigesterEnums.def"
  return KnownTypeKind::Unknown;
}

ArrayRef<TypeAttrKind> SDKNodeType::getTypeAttributes() const {
  return llvm::makeArrayRef(TypeAttributes.data(), TypeAttributes.size());
}

void SDKNodeType::addTypeAttribute(TypeAttrKind AttrKind) {
  TypeAttributes.push_back(AttrKind);
}

bool SDKNodeType::hasTypeAttribute(TypeAttrKind DAKind) const {
  return std::find(TypeAttributes.begin(), TypeAttributes.end(), DAKind) !=
    TypeAttributes.end();
}

StringRef SDKNodeType::getParamValueOwnership() const {
  return ParamValueOwnership.empty() ? "Default" : ParamValueOwnership;
}

StringRef SDKNodeType::getTypeRoleDescription() const {
  assert(isTopLevelType());
  auto P = getParent();
  switch(P->getKind()) {
  case SDKNodeKind::Root:
  case SDKNodeKind::TypeNominal:
  case SDKNodeKind::TypeFunc:
  case SDKNodeKind::TypeAlias:
  case SDKNodeKind::DeclType:
  case SDKNodeKind::DeclOperator:
  case SDKNodeKind::Conformance:
  case SDKNodeKind::DeclImport:
    llvm_unreachable("Type Parent is wrong");
  case SDKNodeKind::DeclFunction:
  case SDKNodeKind::DeclConstructor:
  case SDKNodeKind::DeclAccessor:
  case SDKNodeKind::DeclSubscript:
    return SDKNodeDeclAbstractFunc::getTypeRoleDescription(Ctx,
      P->getChildIndex(this));
  case SDKNodeKind::DeclVar:
  case SDKNodeKind::DeclMacro:
    return "declared";
  case SDKNodeKind::DeclTypeAlias:
    return "underlying";
  case SDKNodeKind::DeclAssociatedType:
    return "default";
  case SDKNodeKind::TypeWitness:
    return "type witness type";
  }
  llvm_unreachable("Unhandled SDKNodeKind in switch");
}

SDKNode *SDKNodeRoot::getInstance(SDKContext &Ctx) {
  SDKNodeInitInfo Info(Ctx);
  Info.Name = Ctx.buffer("TopLevel");
  Info.PrintedName = Ctx.buffer("TopLevel");
  llvm::transform(Ctx.getOpts().ToolArgs, std::back_inserter(Info.ToolArgs),
                  [&](std::string s) { return Ctx.buffer(s); });
  Info.JsonFormatVer = DIGESTER_JSON_VERSION;
  return Info.createSDKNode(SDKNodeKind::Root);
}

StringRef SDKNodeDecl::getScreenInfo() const {
  auto ModuleName = getModuleName();
  auto HeaderName = getHeaderName();
  auto &Ctx = getSDKContext();
  llvm::SmallString<64> SS;
  llvm::raw_svector_ostream OS(SS);
  if (Ctx.getOpts().CompilerStyle) {
    // Compiler style we don't need source info
    OS << (Ctx.checkingABI() ? "ABI breakage" : "API breakage");
  } else {
    // Print more source info.
    if (Ctx.getOpts().PrintModule)
      OS << ModuleName;
    if (!HeaderName.empty())
      OS << "(" << HeaderName << ")";
  }
  if (!OS.str().empty())
    OS << ": ";
  bool IsExtension = false;
  if (auto *TD = dyn_cast<SDKNodeDeclType>(this)) {
    IsExtension = TD->isExternal();
  }

  // There is no particular reasons why we don't use lower-cased keyword names
  // in non-CompilerStyle mode. This is to be backward compatible so clients
  // don't need to update existing known breakages.
  OS << getDeclKindStr(IsExtension? DeclKind::Extension : getDeclKind(),
                       getSDKContext().getOpts().CompilerStyle);
  OS << " " << getFullyQualifiedName();
  return Ctx.buffer(OS.str());
}

void SDKNodeDecl::printFullyQualifiedName(llvm::raw_ostream &OS) const {
  if (auto *ACC = dyn_cast<SDKNodeDeclAccessor>(this)) {
    ACC->getStorage()->printFullyQualifiedName(OS);
    OS << "." << getPrintedName();
    return;
  }
  std::vector<NodePtr> Parent;
  for (auto *P = getParent(); isa<SDKNodeDecl>(P); P = P->getParent())
    Parent.push_back(P);
  for (auto It = Parent.rbegin(); It != Parent.rend(); ++ It)
    OS << (*It)->getPrintedName() << ".";
  OS << getPrintedName();
}

StringRef SDKNodeDecl::getFullyQualifiedName() const {
  llvm::SmallString<32> Buffer;
  llvm::raw_svector_ostream OS(Buffer);
  printFullyQualifiedName(OS);
  return getSDKContext().buffer(OS.str());
}

bool SDKNodeDecl::isNonOptionalProtocolRequirement() const {
  return isProtocolRequirement() && !hasDeclAttribute(DAK_Optional);
}

bool SDKNodeDecl::hasDeclAttribute(DeclAttrKind DAKind) const {
  return std::find(DeclAttributes.begin(), DeclAttributes.end(), DAKind) !=
    DeclAttributes.end();
}

ArrayRef<DeclAttrKind> SDKNodeDecl::getDeclAttributes() const {
  return llvm::makeArrayRef(DeclAttributes.data(), DeclAttributes.size());
}

bool SDKNodeDecl::hasAttributeChange(const SDKNodeDecl &Another) const {
  std::set<DeclAttrKind> Left(getDeclAttributes().begin(),
                              getDeclAttributes().end());
  std::set<DeclAttrKind> Right(Another.getDeclAttributes().begin(),
                               Another.getDeclAttributes().end());
  return Left != Right;
}

bool SDKNodeType::hasAttributeChange(const SDKNodeType &Another) const {
  std::set<TypeAttrKind> Left(getTypeAttributes().begin(),
                              getTypeAttributes().end());
  std::set<TypeAttrKind> Right(Another.getTypeAttributes().begin(),
                               Another.getTypeAttributes().end());
  return Left != Right;
}

SDKNodeDecl *SDKNodeType::getClosestParentDecl() const {
  auto *Result = getParent();
  for (; !isa<SDKNodeDecl>(Result); Result = Result->getParent());
  return Result->getAs<SDKNodeDecl>();
}

void SDKNodeDeclType::addConformance(SDKNode *Conf) {
  cast<SDKNodeConformance>(Conf)->TypeDecl = this;
  Conformances.push_back(Conf);
}

void SDKNodeDeclSubscript::addAccessor(SDKNode *AC) {
  cast<SDKNodeDeclAccessor>(AC)->Owner = this;
  Accessors.push_back(AC);
}

void SDKNodeDeclVar::addAccessor(SDKNode *AC) {
  cast<SDKNodeDeclAccessor>(AC)->Owner = this;
  Accessors.push_back(AC);
}

SDKNodeType *SDKNodeTypeWitness::getUnderlyingType() const {
  return getOnlyChild()->getAs<SDKNodeType>();
}

StringRef SDKNodeTypeWitness::getWitnessedTypeName() const {
  return Ctx.buffer((llvm::Twine(getParent()->getAs<SDKNodeConformance>()->
    getName()) + "." + getName()).str());
}

llvm::Optional<SDKNodeDeclType *> SDKNodeDeclType::getSuperclass() const {
  if (SuperclassUsr.empty())
    return llvm::None;
  auto Descendants = getRootNode()->getDescendantsByUsr(SuperclassUsr);
  if (!Descendants.empty()) {
    return Descendants.front()->getAs<SDKNodeDeclType>();
  }
  return llvm::None;
}

/// Finding the node through all children, including the inherited ones,
/// whose printed name matches with the given name.
llvm::Optional<SDKNodeDecl *>
SDKNodeDeclType::lookupChildByPrintedName(StringRef Name) const {
  for (auto C : getChildren()) {
    if (C->getPrintedName() == Name)
      return C->getAs<SDKNodeDecl>();
  }
  // Finding from the inheritance chain.
  if (auto Super = getSuperclass()) {
    return (*Super)->lookupChildByPrintedName(Name);
  }
  return llvm::None;
}

SDKNodeType *SDKNodeDeclType::getRawValueType() const {
  if (isConformingTo(KnownProtocolKind::RawRepresentable)) {
    if (auto RV = lookupChildByPrintedName("rawValue")) {
      return (*(*RV)->getChildBegin())->getAs<SDKNodeType>();
    }
  }
  return nullptr;
}

bool SDKNodeDeclType::isConformingTo(swift::ide::api::KnownProtocolKind Kind) const {
  switch (Kind) {
#define KNOWN_PROTOCOL(NAME)                                                \
    case KnownProtocolKind::NAME:                                           \
      return std::find_if(Conformances.begin(), Conformances.end(),         \
        [](SDKNode *Conf) { return Conf->getName() == #NAME; }) !=          \
          Conformances.end();
#include "swift/IDE/DigesterEnums.def"
  }
  llvm_unreachable("covered switch");
}

StringRef SDKNodeDeclAbstractFunc::getTypeRoleDescription(SDKContext &Ctx,
                                                      unsigned Index) {
  if (Index == 0) {
    return Ctx.buffer("return");
  } else {
    llvm::SmallString<4> Buffer;
    Buffer += "parameter ";
    Buffer += std::to_string(Index - 1);
    return Ctx.buffer(Buffer.str());
  }
}

#define NODE_KIND(X, NAME)                                                 \
  bool SDKNode##X::classof(const SDKNode *N) {                             \
    return N->getKind() == SDKNodeKind::X;                                 \
  }
#include "swift/IDE/DigesterEnums.def"

static llvm::Optional<KeyKind> parseKeyKind(StringRef Content) {
  return llvm::StringSwitch<llvm::Optional<KeyKind>>(Content)
#define KEY(NAME) .Case(#NAME, KeyKind::KK_##NAME)
#include "swift/IDE/DigesterEnums.def"
      .Default(llvm::None);
}

static StringRef getKeyContent(SDKContext &Ctx, KeyKind Kind) {
  switch (Kind) {
#define KEY(NAME) case KeyKind::KK_##NAME: return Ctx.buffer(#NAME);
#include "swift/IDE/DigesterEnums.def"
  }
  llvm_unreachable("Unhandled KeyKind in switch.");
}

SDKNode* SDKNode::constructSDKNode(SDKContext &Ctx,
                                   llvm::yaml::MappingNode *Node) {
  static auto GetScalarString = [&](llvm::yaml::Node *N) -> StringRef {
    SmallString<64> Buffer;
    return Ctx.buffer(cast<llvm::yaml::ScalarNode>(N)->getValue(Buffer));
  };

  static auto getAsInt = [&](llvm::yaml::Node *N) -> int {
    return std::stoi(cast<llvm::yaml::ScalarNode>(N)->getRawValue().str());
  };
  static auto getAsBool = [&](llvm::yaml::Node *N) -> bool {
    auto txt = cast<llvm::yaml::ScalarNode>(N)->getRawValue();
    assert(txt.startswith("false") || txt.startswith("true"));
    return txt.startswith("true");
  };
  SDKNodeKind Kind;
  SDKNodeInitInfo Info(Ctx);
  NodeVector Children;
  NodeVector Conformances;
  NodeVector Accessors;
  SDKNode *Result = nullptr;

  for (auto &Pair : *Node) {
    auto keyString = GetScalarString(Pair.getKey()); 
    if (auto keyKind = parseKeyKind(keyString)) {
      switch(*keyKind) {
      case KeyKind::KK_kind:
        if (auto parsedKind = parseSDKNodeKind(GetScalarString(Pair.getValue()))) {
          Kind = *parsedKind;
        } else {
          Ctx.diagnose(Pair.getValue(), diag::sdk_node_unrecognized_node_kind,
                       GetScalarString(Pair.getValue()));
        }
        break;
#define KEY_UINT(X, Y)                                                        \
        case KeyKind::KK_##Y: Info.X = getAsInt(Pair.getValue()); break;
#include "swift/IDE/DigesterEnums.def"
#define KEY_STRING(X, Y)                                                      \
  case KeyKind::KK_##Y: Info.X = GetScalarString(Pair.getValue()); break;
#include "swift/IDE/DigesterEnums.def"
#define KEY_BOOL(X, Y) case KeyKind::KK_##Y: Info.X = getAsBool(Pair.getValue()); break;
#include "swift/IDE/DigesterEnums.def"
      case KeyKind::KK_children:
        for (auto &Mapping : *cast<llvm::yaml::SequenceNode>(Pair.getValue())) {
          Children.push_back(constructSDKNode(Ctx,
                                        cast<llvm::yaml::MappingNode>(&Mapping)));
        }
        break;
      case KeyKind::KK_conformances:
        for (auto &Mapping : *cast<llvm::yaml::SequenceNode>(Pair.getValue())) {
          Conformances.push_back(constructSDKNode(Ctx,
                                    cast<llvm::yaml::MappingNode>(&Mapping)));
        }
        break;
#define KEY_STRING_ARR(X, Y)                                                  \
      case KeyKind::KK_##Y:                                                   \
        assert(Info.X.empty());                                               \
        for (auto &Name : *cast<llvm::yaml::SequenceNode>(Pair.getValue())) { \
          Info.X.push_back(GetScalarString(&Name));                           \
        }                                                                     \
        break;
#include "swift/IDE/DigesterEnums.def"
      case KeyKind::KK_ownership:
        Info.ReferenceOwnership =
            swift::ReferenceOwnership(getAsInt(Pair.getValue()));
        assert(Info.ReferenceOwnership != swift::ReferenceOwnership::Strong &&
               "Strong is implied.");
        break;

      case KeyKind::KK_typeAttributes: {
        auto *Seq = cast<llvm::yaml::SequenceNode>(Pair.getValue());
        std::transform(Seq->begin(), Seq->end(),
                       std::back_inserter(Info.TypeAttrs),
          [&](llvm::yaml::Node &N) {
            auto Result = llvm::StringSwitch<TypeAttrKind>(GetScalarString(&N))
  #define TYPE_ATTR(X) .Case(#X, TypeAttrKind::TAK_##X)
  #include "swift/AST/Attr.def"
            .Default(TypeAttrKind::TAK_Count);
            if (Result == TAK_Count)
              Ctx.diagnose(&N, diag::sdk_node_unrecognized_type_attr_kind,
                           GetScalarString(&N));
            return Result;
          });
        break;
      }
      case KeyKind::KK_declAttributes: {
        auto *Seq = cast<llvm::yaml::SequenceNode>(Pair.getValue());
        std::transform(Seq->begin(), Seq->end(), std::back_inserter(Info.DeclAttrs),
          [&](llvm::yaml::Node &N) {
            auto Result = llvm::StringSwitch<DeclAttrKind>(GetScalarString(&N))
  #define DECL_ATTR(_, NAME, ...) .Case(#NAME, DeclAttrKind::DAK_##NAME)
  #include "swift/AST/Attr.def"
            .Default(DeclAttrKind::DAK_Count);
            if (Result == DAK_Count)
              Ctx.diagnose(&N, diag::sdk_node_unrecognized_decl_attr_kind,
                           GetScalarString(&N));
            return Result;
          });
        break;
      }
      case KeyKind::KK_accessors: {
        for (auto &Mapping : *cast<llvm::yaml::SequenceNode>(Pair.getValue())) {
          Accessors.push_back(constructSDKNode(Ctx,
            cast<llvm::yaml::MappingNode>(&Mapping)));
        }
        break;
      }
      case KeyKind::KK_accessorKind: {
        AccessorKind unknownKind = (AccessorKind)((uint8_t)(AccessorKind::Last) + 1);
        Info.AccKind = llvm::StringSwitch<AccessorKind>(
          GetScalarString(Pair.getValue()))
#define ACCESSOR(ID)
#define SINGLETON_ACCESSOR(ID, KEYWORD) .Case(#KEYWORD, AccessorKind::ID)
#include "swift/AST/AccessorKinds.def"
          .Default(unknownKind);
        if (Info.AccKind == unknownKind) {
          Ctx.diagnose(Pair.getValue(), diag::sdk_node_unrecognized_accessor_kind,
                       GetScalarString(Pair.getValue()));
        }
        break;
      }
      case KeyKind::KK_declKind: {
        auto dKind = llvm::StringSwitch<llvm::Optional<DeclKind>>(
                         GetScalarString(Pair.getValue()))
#define DECL(X, PARENT) .Case(#X, DeclKind::X)
  #include "swift/AST/DeclNodes.def"
                         .Default(llvm::None);
        if (dKind)
          Info.DKind = *dKind;
        else
          Ctx.diagnose(Pair.getValue(), diag::sdk_node_unrecognized_decl_kind,
                       GetScalarString(Pair.getValue()));
        break;
      }
      }
    } else if (keyString == ABIRootKey) {
      Result = constructSDKNode(Ctx,
        cast<llvm::yaml::MappingNode>(Pair.getValue()));
    } else if (keyString == ConstValuesKey) {
      // We don't need to consume the const values from the compiler-side
      Pair.skip();
    } else {
      Ctx.diagnose(Pair.getKey(), diag::sdk_node_unrecognized_key,
                              keyString);
      Pair.skip();
    }
  };
  if (Result)
    return Result;
  Result = Info.createSDKNode(Kind);
  for (auto C : Children) {
    Result->addChild(C);
  }
  for (auto *Conf: Conformances) {
    cast<SDKNodeDeclType>(Result)->addConformance(Conf);
  }
   for (auto *Acc: Accessors) {
     if (auto *SD = dyn_cast<SDKNodeDeclSubscript>(Result)) {
       SD->addAccessor(Acc);
     } else if (auto *VD = dyn_cast<SDKNodeDeclVar>(Result)) {
       VD->addAccessor(Acc);
     }
   }
  return Result;
}

bool SDKNode::hasSameChildren(const SDKNode &Other) const {
  if (Children.size() != Other.Children.size())
    return false;
  for (unsigned I = 0; I < Children.size(); ++ I) {
    if (*Children[I] != *Other.Children[I])
      return false;
  }
  return true;
}

void swift::ide::api::nodeSetDifference(ArrayRef<SDKNode*> Left,
                                        ArrayRef<SDKNode*> Right,
                                        NodeVector &LeftMinusRight,
                                        NodeVector &RightMinusLeft) {
  llvm::SmallPtrSet<NodePtr, 16> LeftToRemove;
  llvm::SmallPtrSet<NodePtr, 16> RightToRemove;
  for (auto LC: Left) {
    for (auto RC: Right) {
      if (!RightToRemove.count(RC) && *LC == *RC) {
        LeftToRemove.insert(LC);
        RightToRemove.insert(RC);
        break;
      }
    }
  }
  std::for_each(Left.begin(), Left.end(), [&] (SDKNode *N) {
    if (!LeftToRemove.count(N))
      LeftMinusRight.push_back(N);
  });
  std::for_each(Right.begin(), Right.end(), [&] (SDKNode *N) {
    if (!RightToRemove.count(N))
      RightMinusLeft.push_back(N);
  });
}


static bool hasSameContents(ArrayRef<SDKNode*> Left,
                            ArrayRef<SDKNode*> Right) {
  NodeVector LeftMinusRight, RightMinusLeft;
  nodeSetDifference(Left, Right, LeftMinusRight, RightMinusLeft);
  return LeftMinusRight.empty() && RightMinusLeft.empty();
}

static bool hasSameParameterFlags(const SDKNodeType *Left, const SDKNodeType *Right) {
  if (Left->hasDefaultArgument() != Right->hasDefaultArgument())
    return false;
  if (Left->getParamValueOwnership() != Right->getParamValueOwnership())
    return false;

  return true;
}

// Return whether a decl has been moved in/out to an extension
static llvm::Optional<bool> isFromExtensionChanged(const SDKNode &L,
                                                   const SDKNode &R) {
  assert(L.getKind() == R.getKind());
  // Version 8 starts to include whether a decl is from an extension.
  if (L.getJsonFormatVersion() + R.getJsonFormatVersion() < 2 * 8) {
    return llvm::None;
  }
  auto *Left = dyn_cast<SDKNodeDecl>(&L);
  auto *Right = dyn_cast<SDKNodeDecl>(&R);
  if (!Left) {
    return llvm::None;
  }
  if (Left->isFromExtension() == Right->isFromExtension()) {
    return llvm::None;
  } else {
    return Right->isFromExtension();
  }
}

static bool isSDKNodeEqual(SDKContext &Ctx, const SDKNode &L, const SDKNode &R) {
  auto *LeftAlias = dyn_cast<SDKNodeTypeAlias>(&L);
  auto *RightAlias = dyn_cast<SDKNodeTypeAlias>(&R);
  if (LeftAlias || RightAlias) {
    auto Left = L.getAs<SDKNodeType>();
    auto Right = R.getAs<SDKNodeType>();

    // First compare the parameter attributes.
    if (!hasSameParameterFlags(Left, Right))
      return false;

    // Comparing the underlying types if any of the inputs are alias.
    Left = LeftAlias ? LeftAlias->getUnderlyingType() : Left;
    Right = RightAlias ? RightAlias->getUnderlyingType() : Right;
    return *Left == *Right;
  }

  if (L.getKind() != R.getKind())
    return false;

  switch(L.getKind()) {
    case SDKNodeKind::TypeAlias:
      llvm_unreachable("Should be handled above.");
    case SDKNodeKind::TypeNominal:
    case SDKNodeKind::TypeFunc: {
      auto Left = L.getAs<SDKNodeType>();
      auto Right = R.getAs<SDKNodeType>();
      if (Left->hasAttributeChange(*Right))
        return false;
      if (!hasSameParameterFlags(Left, Right))
        return false;
      if (Left->getPrintedName() == Right->getPrintedName())
        return true;
      if (Ctx.checkingABI()) {
        // For abi checking where we don't have sugar types at all, the printed
        // name difference is enough to indicate these two types differ.
        return false;
      } else {
        return Left->getName() == Right->getName() &&
          Left->hasSameChildren(*Right);
      }
    }

    case SDKNodeKind::DeclFunction: {
      auto Left = L.getAs<SDKNodeDeclFunction>();
      auto Right = R.getAs<SDKNodeDeclFunction>();
      if (Left->getSelfAccessKind() != Right->getSelfAccessKind())
        return false;
      LLVM_FALLTHROUGH;
    }
    case SDKNodeKind::DeclConstructor: {
      auto Left = L.getAs<SDKNodeDeclAbstractFunc>();
      auto Right = R.getAs<SDKNodeDeclAbstractFunc>();
      if (Left->isThrowing() ^ Right->isThrowing())
        return false;
      if (Left->reqNewWitnessTableEntry() != Right->reqNewWitnessTableEntry())
        return false;
      LLVM_FALLTHROUGH;
    }
    case SDKNodeKind::DeclAccessor: {
      if (auto *LA = dyn_cast<SDKNodeDeclAccessor>(&L)) {
        if (auto *RA = dyn_cast<SDKNodeDeclAccessor>(&R)) {
          if (LA->getAccessorKind() != RA->getAccessorKind())
            return false;
        }
      }
      LLVM_FALLTHROUGH;
    }
    case SDKNodeKind::DeclVar: {
      if (auto *LV = dyn_cast<SDKNodeDeclVar>(&L)) {
        if (auto *RV = dyn_cast<SDKNodeDeclVar>(&R)) {
          if (Ctx.checkingABI()) {
            if (LV->hasStorage() != RV->hasStorage())
              return false;
          }
          if (!hasSameContents(LV->getAllAccessors(), RV->getAllAccessors()))
            return false;
        }
      }
      LLVM_FALLTHROUGH;
    }
    case SDKNodeKind::DeclType: {
      auto *Left = dyn_cast<SDKNodeDeclType>(&L);
      auto *Right = dyn_cast<SDKNodeDeclType>(&R);
      if (Left && Right) {
        if (!hasSameContents(Left->getConformances(), Right->getConformances())) {
          return false;
        }
        if (Left->getSuperClassName() != Right->getSuperClassName()) {
          return false;
        }
        if (Left->getDeclKind() != Right->getDeclKind()) {
          return false;
        }
      }
      LLVM_FALLTHROUGH;
    }
    case SDKNodeKind::DeclAssociatedType:
    case SDKNodeKind::DeclSubscript: {
      if (auto *Left = dyn_cast<SDKNodeDeclSubscript>(&L)) {
        if (auto *Right = dyn_cast<SDKNodeDeclSubscript>(&R)) {
          if (!hasSameContents(Left->getAllAccessors(), Right->getAllAccessors()))
            return false;
        }
      }
      LLVM_FALLTHROUGH;
    }
    case SDKNodeKind::DeclOperator:
    case SDKNodeKind::DeclTypeAlias: {
      auto Left = L.getAs<SDKNodeDecl>();
      auto Right = R.getAs<SDKNodeDecl>();
      if (Left->isStatic() ^ Right->isStatic())
        return false;
      if (Left->getReferenceOwnership() != Right->getReferenceOwnership())
        return false;
      if (Left->hasAttributeChange(*Right))
        return false;
      if (Left->getGenericSignature() != Right->getGenericSignature())
        return false;
      if (Left->isOpen() != Right->isOpen())
        return false;
      if (Left->isInternal() != Right->isInternal())
        return false;
      if (Left->getObjCName() != Right->getObjCName())
        return false;
      if (Left->hasFixedBinaryOrder() != Right->hasFixedBinaryOrder())
        return false;
      if (Left->hasFixedBinaryOrder()) {
        if (Left->getFixedBinaryOrder() != Right->getFixedBinaryOrder())
          return false;
      }
      if (Left->getUsr() != Right->getUsr())
        return false;
      LLVM_FALLTHROUGH;
    }
    case SDKNodeKind::DeclMacro:
    case SDKNodeKind::Conformance:
    case SDKNodeKind::TypeWitness:
    case SDKNodeKind::DeclImport:
    case SDKNodeKind::Root: {
      if (isFromExtensionChanged(L, R))
        return false;
      return L.getPrintedName() == R.getPrintedName() &&
        L.hasSameChildren(R);
    }
  }

  llvm_unreachable("Unhandled SDKNodeKind in switch.");
}

bool SDKContext::isEqual(const SDKNode &Left, const SDKNode &Right) {
  return isSDKNodeEqual(*this, Left, Right);
}

AccessLevel SDKContext::getAccessLevel(const ValueDecl *VD) const {
  return checkingABI() ? VD->getEffectiveAccess() : VD->getFormalAccess();
}

bool SDKNode::operator==(const SDKNode &Other) const {
  return Ctx.isEqual(*this, Other);
}

// The pretty printer of a tree of SDKNode
class SDKNodeDumpVisitor : public SDKNodeVisitor {
  void dumpSpace(int Num) {
    while (Num != 0) {
      llvm::outs() << "\t";
      Num --;
    }
  }
  void visit(NodePtr Node) override {
    dumpSpace(depth());
    llvm::outs() << "[" << Node->getKind() << "]" << Node->getName() << "\n";
  }
public:
  SDKNodeDumpVisitor() {};
};

static StringRef getPrintedName(SDKContext &Ctx, Type Ty,
                                bool IsImplicitlyUnwrappedOptional = false) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  PrintOptions PO = getTypePrintOpts(Ctx.getOpts());
  PO.SkipAttributes = true;
  if (IsImplicitlyUnwrappedOptional)
    PO.PrintOptionalAsImplicitlyUnwrapped = true;

  Ty.print(OS, PO);
  return Ctx.buffer(OS.str());
}

static StringRef getTypeName(SDKContext &Ctx, Type Ty,
                             bool IsImplicitlyUnwrappedOptional) {
  if (Ty->getKind() == TypeKind::Paren) {
    return Ctx.buffer("Paren");
  }
  if (Ty->isVoid()) {
    return Ctx.buffer("Void");
  }
  if (auto *NAT = dyn_cast<TypeAliasType>(Ty.getPointer())) {
    return NAT->getDecl()->getNameStr();
  }

  if (auto existential = Ty->getAs<ExistentialType>()) {
    return getTypeName(Ctx, existential->getConstraintType(),
                       IsImplicitlyUnwrappedOptional);
  }

  if (Ty->getAnyNominal()) {
    if (IsImplicitlyUnwrappedOptional) {
      assert(Ty->getOptionalObjectType());
      return StringRef("ImplicitlyUnwrappedOptional");
    }
    return Ty->getAnyNominal()->getNameStr();
  }
#define TYPE(id, parent)                                                      \
  if (Ty->getKind() == TypeKind::id) {                                        \
    return Ctx.buffer(#id);                                                   \
  }
#include "swift/AST/TypeNodes.def"
  llvm_unreachable("Unhandled type name.");
}

static StringRef calculateUsr(SDKContext &Ctx, ValueDecl *VD) {
  llvm::SmallString<64> SS;
  llvm::raw_svector_ostream OS(SS);
  if (!ide::printValueDeclUSR(VD, OS)) {
    return Ctx.buffer(SS.str());
  }
  return StringRef();
}

static StringRef calculateMangledName(SDKContext &Ctx, ValueDecl *VD) {
  if (isFromClang(VD)) {
    // Don't mangle clang symbols.
    return StringRef();
  }
  if (auto *attr = VD->getAttrs().getAttribute<SILGenNameAttr>()) {
    return Ctx.buffer(attr->Name);
  }
  Mangle::ASTMangler NewMangler;
  return Ctx.buffer(NewMangler.mangleAnyDecl(VD, true,
                                    /*bool respectOriginallyDefinedIn*/true));
}

static StringRef calculateLocation(SDKContext &SDKCtx, Decl *D) {
  if (SDKCtx.getOpts().AvoidLocation)
    return StringRef();
  auto &Ctx = D->getASTContext();
  auto &Importer = static_cast<ClangImporter &>(*Ctx.getClangModuleLoader());

  clang::SourceManager &SM = Importer.getClangPreprocessor().getSourceManager();
  if (ClangNode CN = D->getClangNode()) {
    clang::SourceLocation Loc = CN.getLocation();
    Loc = SM.getFileLoc(Loc);
    if (Loc.isValid())
      return SDKCtx.buffer(Loc.printToString(SM));
  }

  return StringRef();
}

static bool isFunctionTypeNoEscape(Type Ty) {
  if (auto *AFT = Ty->getAs<AnyFunctionType>()) {
    return AFT->getExtInfo().isNoEscape();
  }
  return false;
}

static StringRef getSimpleName(ValueDecl *VD) {
  if (VD->hasName()) {
    auto name = VD->getBaseName();
    switch (name.getKind()) {
      case DeclBaseName::Kind::Subscript:
        return "subscript";
      case DeclBaseName::Kind::Constructor:
        return "init";
      case DeclBaseName::Kind::Destructor:
        return "deinit";
      case DeclBaseName::Kind::Normal:
        return llvm::StringSwitch<StringRef>(name.getIdentifier().str())
        .Case("subscript", "`subscript`")
        .Case("init", "`init`")
        .Case("deinit", "`deinit`")
        .Default(name.getIdentifier().str());
    }
  }
  if (auto *AD = dyn_cast<AccessorDecl>(VD)) {
    switch(AD->getAccessorKind()) {
#define ACCESSOR(ID) \
case AccessorKind::ID: return #ID;
#include "swift/AST/AccessorKinds.def"
    }
  }
  return "_";
}

static StringRef getPrintedName(SDKContext &Ctx, ValueDecl *VD) {
  if (isa<AbstractFunctionDecl>(VD) || isa<SubscriptDecl>(VD)) {
    llvm::SmallString<32> Result;
    Result.append(getSimpleName(VD));
    Result.append("(");
    for (auto Arg : VD->getName().getArgumentNames()) {
      Result.append(Arg.empty() ? "_" : Arg.str());
      Result.append(":");
    }
    Result.append(")");
    return Ctx.buffer(Result.str());
  }
  return getSimpleName(VD);
}

static bool isFuncThrowing(ValueDecl *VD) {
  if (auto AF = dyn_cast<AbstractFunctionDecl>(VD)) {
    return AF->hasThrows();
  }
  return false;
}

static llvm::Optional<uint8_t> getSelfIndex(ValueDecl *VD) {
  if (auto AF = dyn_cast<AbstractFunctionDecl>(VD)) {
    if (AF->isImportAsInstanceMember())
      return AF->getSelfIndex();
  }
  return llvm::None;
}

static ReferenceOwnership getReferenceOwnership(ValueDecl *VD) {
  if (auto OA = VD->getAttrs().getAttribute<ReferenceOwnershipAttr>()) {
    return OA->get();
  }
  return ReferenceOwnership::Strong;
}

// Get a requirement with all types canonicalized.
Requirement getCanonicalRequirement(Requirement &Req) {
  auto kind = Req.getKind();
  if (kind == RequirementKind::Layout) {
    return Requirement(kind, Req.getFirstType()->getCanonicalType(),
                       Req.getLayoutConstraint());
  } else {
    return Requirement(kind, Req.getFirstType()->getCanonicalType(),
                       Req.getSecondType()->getCanonicalType());
  }
}

static
StringRef printGenericSignature(SDKContext &Ctx, ArrayRef<Requirement> AllReqs,
                                bool Canonical) {
  llvm::SmallString<32> Result;
  llvm::raw_svector_ostream OS(Result);
  if (AllReqs.empty())
    return StringRef();
  OS << "<";
  bool First = true;
  PrintOptions Opts = getTypePrintOpts(Ctx.getOpts());
  for (auto Req: AllReqs) {
    if (!First) {
      OS << ", ";
    } else {
      First = false;
    }
    if (Canonical)
      getCanonicalRequirement(Req).print(OS, Opts);
    else
      Req.print(OS, Opts);
  }
  OS << ">";
  return Ctx.buffer(OS.str());
}

static StringRef printGenericSignature(SDKContext &Ctx, Decl *D, bool Canonical) {
  llvm::SmallString<32> Result;
  llvm::raw_svector_ostream OS(Result);
  if (auto *PD = dyn_cast<ProtocolDecl>(D)) {
    return printGenericSignature(Ctx, PD->getRequirementSignature().getRequirements(),
                                 Canonical);
  }
  PrintOptions Opts = getTypePrintOpts(Ctx.getOpts());
  if (auto *GC = D->getAsGenericContext()) {
    if (auto Sig = GC->getGenericSignature()) {
      if (Canonical)
        Sig.getCanonicalSignature()->print(OS, Opts);
      else
        Sig->print(OS, Opts);
      return Ctx.buffer(OS.str());
    }
  }
  return StringRef();
}

static
StringRef printGenericSignature(SDKContext &Ctx, ProtocolConformance *Conf, bool Canonical) {
  return printGenericSignature(Ctx, Conf->getConditionalRequirements(), Canonical);
}

static llvm::Optional<uint8_t>
getSimilarMemberCount(NominalTypeDecl *NTD, ValueDecl *VD,
                      llvm::function_ref<bool(Decl *)> Check) {
  if (!Check(VD))
    return llvm::None;
  auto Members = NTD->getMembers();
  auto End = std::find(Members.begin(), Members.end(), VD);
  assert(End != Members.end());
  return std::count_if(Members.begin(), End, Check);
}

llvm::Optional<uint8_t> SDKContext::getFixedBinaryOrder(ValueDecl *VD) const {
  // We don't need fixed binary order when checking API stability.
  if (!checkingABI())
    return llvm::None;
  auto *NTD = dyn_cast_or_null<NominalTypeDecl>(VD->getDeclContext()->
    getAsDecl());

  if (!NTD || isa<ProtocolDecl>(NTD) || NTD->isResilient())
    return llvm::None;

  // The relative order of stored properties matters for non-resilient type.
  auto isStored = [](Decl *M) {
    if (auto *STD = dyn_cast<AbstractStorageDecl>(M)) {
      return STD->hasStorage() && !STD->isStatic();
    }
    return false;
  };

  switch (NTD->getKind()) {
  case DeclKind::Enum: {
    return getSimilarMemberCount(NTD, VD, [](Decl *M) {
      return isa<EnumElementDecl>(M);
    });
  }
  case DeclKind::Class:
  case DeclKind::Struct: {
    return getSimilarMemberCount(NTD, VD, isStored);
  }
  default:
    llvm_unreachable("bad nominal type kind.");
  }
}

// check for if it has @available(macOS 9999, iOS 9999, tvOS 9999, watchOS 9999, *)
static bool isABIPlaceHolder(Decl *D) {
  llvm::SmallSet<PlatformKind, 4> Platforms;
  for (auto *ATT: D->getAttrs()) {
    if (auto *AVA = dyn_cast<AvailableAttr>(ATT)) {
      if (AVA->Platform != PlatformKind::none && AVA->Introduced &&
          AVA->Introduced->getMajor() == 9999) {
        Platforms.insert(AVA->Platform);
      }
    }
  }
  return Platforms.size() == 4;
}

static bool isABIPlaceholderRecursive(Decl *D) {
  for (auto *CD = D; CD; CD = CD->getDeclContext()->getAsDecl()) {
    if (isABIPlaceHolder(CD))
      return true;
  }
  return false;
}

StringRef SDKContext::getPlatformIntroVersion(Decl *D, PlatformKind Kind) {
  if (!D)
    return StringRef();
  for (auto *ATT: D->getAttrs()) {
    if (auto *AVA = dyn_cast<AvailableAttr>(ATT)) {
      if (AVA->Platform == Kind && AVA->Introduced) {
        return buffer(AVA->Introduced->getAsString());
      }
    }
  }
  return StringRef();
}

StringRef SDKContext::getLanguageIntroVersion(Decl *D) {
  if (!D)
    return StringRef();
  for (auto *ATT: D->getAttrs()) {
    if (auto *AVA = dyn_cast<AvailableAttr>(ATT)) {
      if (AVA->isLanguageVersionSpecific() && AVA->Introduced) {
        return buffer(AVA->Introduced->getAsString());
      }
    }
  }
  return getLanguageIntroVersion(D->getDeclContext()->getAsDecl());
}

StringRef SDKContext::getObjcName(Decl *D) {
  if (auto *OC = D->getAttrs().getAttribute<ObjCAttr>()) {
    if (OC->getName().has_value()) {
      SmallString<32> Buffer;
      return buffer(OC->getName()->getString(Buffer));
    }
  }
  return StringRef();
}

SDKNodeInitInfo::SDKNodeInitInfo(SDKContext &Ctx, Type Ty, TypeInitInfo Info) :
    Ctx(Ctx), Name(getTypeName(Ctx, Ty, Info.IsImplicitlyUnwrappedOptional)),
    PrintedName(getPrintedName(Ctx, Ty, Info.IsImplicitlyUnwrappedOptional)),
    ParamValueOwnership(Info.ValueOwnership),
    HasDefaultArg(Info.hasDefaultArgument) {
  if (isFunctionTypeNoEscape(Ty))
    TypeAttrs.push_back(TypeAttrKind::TAK_noescape);
  // If this is a nominal type, get its Usr.
  if (auto *ND = Ty->getAnyNominal()) {
    Usr = calculateUsr(Ctx, ND);
    MangledName = calculateMangledName(Ctx, ND);
  }
}

static std::vector<DeclAttrKind> collectDeclAttributes(Decl *D) {
  std::vector<DeclAttrKind> Results;
  for (auto *Attr: D->getAttrs())
    Results.push_back(Attr->getKind());
  if (auto *VD = dyn_cast<ValueDecl>(D)) {
#define HANDLE(COND, KIND_NAME)                                                                   \
    if (VD->COND && !llvm::is_contained(Results, DeclAttrKind::KIND_NAME))                        \
      Results.emplace_back(DeclAttrKind::KIND_NAME);
    // These attributes may be semantically applicable to the current decl but absent from
    // the actual AST. Populating them to the nodes ensure we don't have false positives.
    HANDLE(isObjC(), DAK_ObjC)
    HANDLE(isFinal(), DAK_Final)
    HANDLE(isDynamic(), DAK_Dynamic)
#undef HANDLE
  }
  return Results;
}

CtorInitializerKind SDKNodeDeclConstructor::getInitKind() const {
#define CASE(KIND) if (InitKind == #KIND) return CtorInitializerKind::KIND;
  CASE(Designated)
  CASE(Convenience)
  CASE(ConvenienceFactory)
  CASE(Factory)
#undef CASE
  llvm_unreachable("unhandled init kind");
}

StringRef SDKContext::getInitKind(Decl *D) {
  if (auto *CD = dyn_cast<ConstructorDecl>(D)) {
    switch(CD->getInitKind()) {
#define CASE(KIND) case CtorInitializerKind::KIND: return #KIND;
    CASE(Designated)
    CASE(Convenience)
    CASE(ConvenienceFactory)
    CASE(Factory)
#undef CASE
    }
  }
  return StringRef();
}

static bool isDeclaredInExtension(Decl *D) {
  if (auto *DC = D->getDeclContext()->getAsDecl()) {
    return isa<ExtensionDecl>(DC);
  }
  return false;
}

SDKNodeInitInfo::SDKNodeInitInfo(SDKContext &Ctx, Decl *D):
      Ctx(Ctx), DKind(D->getKind()), Loc(D->getLoc()),
      Location(calculateLocation(Ctx, D)),
      ModuleName(D->getModuleContext()->getName().str()),
      GenericSig(printGenericSignature(Ctx, D, /*Canonical*/Ctx.checkingABI())),
      SugaredGenericSig(Ctx.checkingABI()?
                        printGenericSignature(Ctx, D, /*Canonical*/false):
                        StringRef()),
      IntromacOS(Ctx.getPlatformIntroVersion(D, PlatformKind::macOS)),
      IntroiOS(Ctx.getPlatformIntroVersion(D, PlatformKind::iOS)),
      IntrotvOS(Ctx.getPlatformIntroVersion(D, PlatformKind::tvOS)),
      IntrowatchOS(Ctx.getPlatformIntroVersion(D, PlatformKind::watchOS)),
      Introswift(Ctx.getLanguageIntroVersion(D)),
      ObjCName(Ctx.getObjcName(D)),
      InitKind(Ctx.getInitKind(D)),
      IsImplicit(D->isImplicit()),
      IsDeprecated(D->getAttrs().getDeprecated(D->getASTContext())),
      IsABIPlaceholder(isABIPlaceholderRecursive(D)),
      IsFromExtension(isDeclaredInExtension(D)),
      DeclAttrs(collectDeclAttributes(D)) {
  // Keep track of SPI group names
  for (auto id: D->getSPIGroups()) {
    SPIGroups.push_back(id.str());
  }
}

SDKNodeInitInfo::SDKNodeInitInfo(SDKContext &Ctx, OperatorDecl *OD):
    SDKNodeInitInfo(Ctx, cast<Decl>(OD)) {
  Name = OD->getName().str();
  PrintedName = OD->getName().str();
}

SDKNodeInitInfo::SDKNodeInitInfo(SDKContext &Ctx, ImportDecl *ID):
    SDKNodeInitInfo(Ctx, cast<Decl>(ID)) {
  std::string content;
  llvm::raw_string_ostream OS(content);
  ID->getModulePath().print(OS);
  Name = PrintedName = Ctx.buffer(content);
}

SDKNodeInitInfo::SDKNodeInitInfo(SDKContext &Ctx, ProtocolConformanceRef Conform):
    SDKNodeInitInfo(Ctx, Conform.getRequirement()) {
  // The conformance can be conditional. The generic signature keeps track of
  // the requirements.
  if (Conform.isConcrete()) {
    auto *Concrete = Conform.getConcrete();

    GenericSig = printGenericSignature(Ctx, Concrete, Ctx.checkingABI());
    SugaredGenericSig = Ctx.checkingABI() ?
      printGenericSignature(Ctx, Concrete, false): StringRef();
    // Whether this conformance is ABI placeholder depends on the decl context
    // of this conformance.
    IsABIPlaceholder = isABIPlaceholderRecursive(Concrete->getDeclContext()->
                                                 getAsDecl());
  }
}

static bool isProtocolRequirement(ValueDecl *VD) {
  if (isa<ProtocolDecl>(VD->getDeclContext()) && VD->isProtocolRequirement())
    return true;
  // If the VD is an accessor of the property declaration that is a protocol
  // requirement, we consider this accessor as a protocol requirement too.
  if (auto *AD = dyn_cast<AccessorDecl>(VD)) {
    if (auto *ST = AD->getStorage()) {
      return isProtocolRequirement(ST);
    }
  }
  return false;
}

static bool requireWitnessTableEntry(ValueDecl *VD) {
  if (auto *FD = dyn_cast<AbstractFunctionDecl>(VD)) {
    return FD->requiresNewWitnessTableEntry();
  }
  return false;
}

SDKNodeInitInfo::SDKNodeInitInfo(SDKContext &Ctx, ValueDecl *VD)
    : SDKNodeInitInfo(Ctx, cast<Decl>(VD)) {
  Name = getSimpleName(VD);
  PrintedName = getPrintedName(Ctx, VD);
  Usr = calculateUsr(Ctx, VD);
  MangledName = calculateMangledName(Ctx, VD);
  IsThrowing = isFuncThrowing(VD);
  IsStatic = VD->isStatic();
  IsOverriding = VD->getOverriddenDecl();
  IsProtocolReq = isProtocolRequirement(VD);
  IsOpen = Ctx.getAccessLevel(VD) == AccessLevel::Open;
  IsInternal = Ctx.getAccessLevel(VD) < AccessLevel::Public;
  SelfIndex = getSelfIndex(VD);
  FixedBinaryOrder = Ctx.getFixedBinaryOrder(VD);
  ReferenceOwnership = getReferenceOwnership(VD);
  ReqNewWitnessTableEntry = IsProtocolReq && requireWitnessTableEntry(VD);

  // Calculate usr for its super class.
  if (auto *CD = dyn_cast_or_null<ClassDecl>(VD)) {
    if (auto *Super = CD->getSuperclassDecl()) {
      SuperclassUsr = calculateUsr(Ctx, Super);
      for (auto T = CD->getSuperclass(); T; T = T->getSuperclass()) {
        SuperclassNames.push_back(getPrintedName(Ctx, T->getCanonicalType()));
      }
    }
    HasMissingDesignatedInitializers = CD->hasMissingDesignatedInitializers();
    InheritsConvenienceInitializers = CD->inheritsSuperclassInitializers();
  }

  if (auto *FD = dyn_cast<FuncDecl>(VD)) {
    switch(FD->getSelfAccessKind()) {
    case SelfAccessKind::Mutating:
      FuncSelfKind = "Mutating";
      break;
    case SelfAccessKind::LegacyConsuming:
      // FIXME: Stay consistent with earlier digests that had underscores here.
      FuncSelfKind = "__Consuming";
      break;
    case SelfAccessKind::NonMutating:
      FuncSelfKind = "NonMutating";
      break;
    case SelfAccessKind::Consuming:
      FuncSelfKind = "Consuming";
      break;
    case SelfAccessKind::Borrowing:
      FuncSelfKind = "Borrowing";
      break;
    }
  }

  // Get enum raw type name if this is an enum.
  if (auto *ED = dyn_cast<EnumDecl>(VD)) {
    IsEnumExhaustive = ED->isFormallyExhaustive(nullptr);
    if (auto RT = ED->getRawType()) {
      if (auto *D = RT->getNominalOrBoundGenericNominal()) {
        EnumRawTypeName = D->getName().str();
      }
    }
  }
  if (auto *VAD = dyn_cast<VarDecl>(VD)) {
    IsLet = VAD->isLet();
  }
  if (auto *VAR = dyn_cast<AbstractStorageDecl>(VD)) {
    HasStorage = VAR->hasStorage();
  }
  if (auto *ACC = dyn_cast<AccessorDecl>(VD)) {
    AccKind = ACC->getAccessorKind();
  }
}

SDKNode *SDKNodeInitInfo::createSDKNode(SDKNodeKind Kind) {
  switch(Kind) {
#define NODE_KIND(X, NAME)                                                     \
case SDKNodeKind::X:                                                           \
  return static_cast<SDKNode*>(new (Ctx.allocator().Allocate<SDKNode##X>())    \
    SDKNode##X(*this));                                                        \
  break;
#include "swift/IDE/DigesterEnums.def"
  }
  llvm_unreachable("covered switch");
}

// Recursively construct a node that represents a type, for instance,
// representing the return value type of a function decl.
SDKNode *swift::ide::api::
SwiftDeclCollector::constructTypeNode(Type T, TypeInitInfo Info) {
  if (Ctx.checkingABI()) {
    T = T->getCanonicalType();

    if (T->hasOpaqueArchetype()) {
      // When the type contains an opaque result type and we're in the ABI mode,
      // we should substitute the opaque result type to its underlying type.
      // Notice this only works if the opaque result type is from an inlinable
      // function where the function body is present in the swift module file,
      // thus allowing us to know the concrete type.
      ReplaceOpaqueTypesWithUnderlyingTypes replacer(
          /*inContext=*/nullptr, ResilienceExpansion::Maximal,
          /*isWholeModuleContext=*/false);
      T = T.subst(replacer, replacer, SubstFlags::SubstituteOpaqueArchetypes)
          ->getCanonicalType();
    }
  }

  if (auto NAT = dyn_cast<TypeAliasType>(T.getPointer())) {
    SDKNode* Root = SDKNodeInitInfo(Ctx, T, Info).createSDKNode(SDKNodeKind::TypeAlias);
    Root->addChild(constructTypeNode(NAT->getSinglyDesugaredType()));
    return Root;
  }

  if (auto Fun = T->getAs<AnyFunctionType>()) {
    SDKNode* Root = SDKNodeInitInfo(Ctx, T, Info).createSDKNode(SDKNodeKind::TypeFunc);

    // Still, return type first
    Root->addChild(constructTypeNode(Fun->getResult()));

    auto Input =
        AnyFunctionType::composeTuple(Fun->getASTContext(), Fun->getParams(),
                                      ParameterFlagHandling::IgnoreNonEmpty);
    Root->addChild(constructTypeNode(Input));
    return Root;
  }

  SDKNode* Root = SDKNodeInitInfo(Ctx, T, Info).createSDKNode(SDKNodeKind::TypeNominal);

  // Keep paren type as a stand-alone level.
  if (auto *PT = dyn_cast<ParenType>(T.getPointer())) {
    Root->addChild(constructTypeNode(PT->getSinglyDesugaredType()));
    return Root;
  }

  // Handle the case where Type has sub-types.
  if (auto BGT = T->getAs<BoundGenericType>()) {
    for (auto Arg : BGT->getGenericArgs()) {
      Root->addChild(constructTypeNode(Arg));
    }
  } else if (auto Tup = T->getAs<TupleType>()) {
    for (auto Elt : Tup->getElementTypes())
      Root->addChild(constructTypeNode(Elt));
  } else if (auto MTT = T->getAs<AnyMetatypeType>()) {
    Root->addChild(constructTypeNode(MTT->getInstanceType()));
  } else if (auto ATT = T->getAs<ArchetypeType>()) {
    for (auto Pro : ATT->getConformsTo()) {
      Root->addChild(constructTypeNode(Pro->getDeclaredInterfaceType()));
    }
  }
  return Root;
}

std::vector<SDKNode*> swift::ide::api::
SwiftDeclCollector::createParameterNodes(ParameterList *PL) {
  std::vector<SDKNode*> Result;
  for (auto param: *PL) {
    TypeInitInfo Info;
    Info.IsImplicitlyUnwrappedOptional = param->isImplicitlyUnwrappedOptional();
    Info.hasDefaultArgument = param->getDefaultArgumentKind() !=
      DefaultArgumentKind::None;
    switch (param->getValueOwnership()) {
#define CASE(KIND) case ValueOwnership::KIND: Info.ValueOwnership = #KIND; break;
    CASE(Owned)
    CASE(InOut)
    CASE(Shared)
    case ValueOwnership::Default: break;
#undef CASE
    }
    Result.push_back(constructTypeNode(param->getInterfaceType(), Info));
  }
  return Result;
}

// Construct a node for a function decl. The first child of the function decl
// is guaranteed to be the return value type of this function.
// We sometimes skip the first parameter because it can be metatype of dynamic
// this if the function is a member function.
SDKNode *swift::ide::api::
SwiftDeclCollector::constructFunctionNode(FuncDecl* FD,
                                          SDKNodeKind Kind) {
  auto Func = SDKNodeInitInfo(Ctx, FD).createSDKNode(Kind);
  TypeInitInfo Info;
  Info.IsImplicitlyUnwrappedOptional = FD->isImplicitlyUnwrappedOptional();
  Func->addChild(constructTypeNode(FD->getResultInterfaceType(), Info));
  for (auto *Node : createParameterNodes(FD->getParameters()))
    Func->addChild(Node);
  return Func;
}

SDKNode* swift::ide::api::
SwiftDeclCollector::constructInitNode(ConstructorDecl *CD) {
  auto Func = SDKNodeInitInfo(Ctx, CD).createSDKNode(SDKNodeKind::DeclConstructor);
  Func->addChild(constructTypeNode(CD->getResultInterfaceType()));
  for (auto *Node : createParameterNodes(CD->getParameters()))
    Func->addChild(Node);
  return Func;
}

bool swift::ide::api::
SDKContext::shouldIgnore(Decl *D, const Decl* Parent) const {
  // Exclude all clang nodes if we're comparing Swift decls specifically.
  // FIXME: isFromClang also excludes Swift decls with @objc. We should allow those.
  if (Opts.SwiftOnly && isFromClang(D)) {
    return true;
  }
  if (auto *ACC = dyn_cast<AccessorDecl>(D)) {
    // Only include accessors if they are part of var and subscript decl.
    if (!isa<AbstractStorageDecl>(Parent)) {
      return true;
    }
    // Only include getter/setter if we are checking source compatibility.
    if (!checkingABI()) {
      switch (ACC->getAccessorKind()) {
      case AccessorKind::Get:
      case AccessorKind::Set:
        break;
      default:
        return true;
      }
    }
  }
  if (checkingABI()) {
    if (auto *VD = dyn_cast<ValueDecl>(D)) {
      // Private vars with fixed binary orders can have ABI-impact, so we should
      // allowlist them if we're checking ABI.
      if (getFixedBinaryOrder(VD).has_value())
        return false;
      // Typealias should have no impact on ABI.
      if (isa<TypeAliasDecl>(VD))
        return true;
    }
    // Exclude decls with @_alwaysEmitIntoClient if we are checking ABI.
    // These decls are considered effectively public because they are usable
    // from inline, so we have to manually exclude them here.
    if (D->getAttrs().hasAttribute<AlwaysEmitIntoClientAttr>())
      return true;
  } else {
    if (D->isPrivateStdlibDecl(false))
      return true;
  }
  if (AvailableAttr::isUnavailable(D))
     return true;
  if (auto VD = dyn_cast<ValueDecl>(D)) {
    switch (getAccessLevel(VD)) {
    case AccessLevel::Internal:
    case AccessLevel::Private:
    case AccessLevel::FilePrivate:
      return true;
    case AccessLevel::Package:
    case AccessLevel::Public:
    case AccessLevel::Open:
      break;
    }
  }

  if (auto *ClangD = D->getClangDecl()) {
    if (isa<clang::ObjCIvarDecl>(ClangD))
      return true;
    if (isa<clang::FieldDecl>(ClangD))
      return true;
    if (ClangD->hasAttr<clang::SwiftPrivateAttr>())
      return true;

    // If this decl is a synthesized member from a conformed clang protocol, we
    // should ignore this member to reduce redundancy.
    if (Parent &&
        !isa<swift::ProtocolDecl>(Parent) &&
        isa<clang::ObjCProtocolDecl>(ClangD->getDeclContext()))
      return true;
  }
  return false;
}

SDKNode *swift::ide::api::
SwiftDeclCollector::constructTypeDeclNode(NominalTypeDecl *NTD) {
  auto TypeNode = SDKNodeInitInfo(Ctx, NTD).createSDKNode(SDKNodeKind::DeclType);
  addConformancesToTypeDecl(cast<SDKNodeDeclType>(TypeNode), NTD);
  addMembersToRoot(TypeNode, NTD);
  for (auto Ext : NTD->getExtensions()) {
    HandledExtensions.insert(Ext);
    addMembersToRoot(TypeNode, Ext);
  }
  return TypeNode;
}

/// Create a node for stand-alone extensions. In the sdk dump, we don't have
/// a specific node for extension. Members in extensions are inlined to the
/// extended types. If the extended types are from a different module, we have to
/// synthesize this type node to include those extension members, since these
/// extension members are legit members of the module.
SDKNode *swift::ide::api::
SwiftDeclCollector::constructExternalExtensionNode(NominalTypeDecl *NTD,
                                            ArrayRef<ExtensionDecl*> AllExts) {
  SDKNodeInitInfo initInfo(Ctx, NTD);
  initInfo.IsExternal = true;
  auto *TypeNode = initInfo.createSDKNode(SDKNodeKind::DeclType);
  addConformancesToTypeDecl(cast<SDKNodeDeclType>(TypeNode), NTD);

  bool anyConformancesAdded = false;
  // The members of the extensions are the only members of this synthesized type.
  for (auto *Ext: AllExts) {
    HandledExtensions.insert(Ext);
    addMembersToRoot(TypeNode, Ext);

    // Keep track if we've declared any conformances in this extension.
    // FIXME: This is too conservative. We only _really_ care if this extension
    //        declares a conformance to any public protocols outside the module
    //        where the extended type originated. Eventually this should be
    //        updated to filter extensions that declare conformances to internal
    //        protocols that either don't inherit from any protocols or only
    //        inherit from other internal protocols. It should also consider
    //        conditional conformances with internal requirements that are still
    //        part of the ABI.
    if (!Ext->getInherited().empty())
      anyConformancesAdded = true;
  }

  // If none of the extensions added any public members or conformances, don't
  // synthesize the type node.
  if (TypeNode->getChildrenCount() == 0 && !anyConformancesAdded)
    return nullptr;

  return TypeNode;
}

SDKNode *swift::ide::api::
SwiftDeclCollector::constructVarNode(ValueDecl *VD) {
  auto Var = cast<SDKNodeDeclVar>(SDKNodeInitInfo(Ctx, VD).createSDKNode(SDKNodeKind::DeclVar));
  TypeInitInfo Info;
  Info.IsImplicitlyUnwrappedOptional = VD->isImplicitlyUnwrappedOptional();
  Var->addChild(constructTypeNode(VD->getInterfaceType(), Info));
  if (auto VAD = dyn_cast<AbstractStorageDecl>(VD)) {
    llvm::SmallVector<AccessorDecl*, 4> scratch;
    for(auto *AC: VAD->getOpaqueAccessors(scratch)) {
      if (!Ctx.shouldIgnore(AC, VAD)) {
        Var->addAccessor(constructFunctionNode(AC, SDKNodeKind::DeclAccessor));
      }
    }
  }
  return Var;
}

SDKNode *swift::ide::api::
SwiftDeclCollector::constructTypeAliasNode(TypeAliasDecl *TAD) {
  auto Alias = SDKNodeInitInfo(Ctx, TAD).createSDKNode(SDKNodeKind::DeclTypeAlias);
  Alias->addChild(constructTypeNode(TAD->getUnderlyingType()));
  return Alias;
}

SDKNode *swift::ide::api::
SwiftDeclCollector::constructMacroNode(MacroDecl *MD) {
  auto Macro = SDKNodeInitInfo(Ctx, MD).createSDKNode(SDKNodeKind::DeclMacro);
  Macro->addChild(constructTypeNode(MD->getInterfaceType(), TypeInitInfo()));
  return Macro;
}

SDKNode *swift::ide::api::
SwiftDeclCollector::constructAssociatedTypeNode(AssociatedTypeDecl *ATD) {
  auto Asso = SDKNodeInitInfo(Ctx, ATD).
    createSDKNode(SDKNodeKind::DeclAssociatedType);
  if (auto DT = ATD->getDefaultDefinitionType()) {
    Asso->addChild(constructTypeNode(DT));
  }
  return Asso;
}

SDKNode *swift::ide::api::
SwiftDeclCollector::constructSubscriptDeclNode(SubscriptDecl *SD) {
  auto *Subs = cast<SDKNodeDeclSubscript>(SDKNodeInitInfo(Ctx, SD).
    createSDKNode(SDKNodeKind::DeclSubscript));
  Subs->addChild(constructTypeNode(SD->getElementInterfaceType()));
  for (auto *Node: createParameterNodes(SD->getIndices())) {
    Subs->addChild(Node);
  }
  llvm::SmallVector<AccessorDecl*, 4> scratch;
  for(auto *AC: SD->getOpaqueAccessors(scratch)) {
    if (!Ctx.shouldIgnore(AC, SD)) {
      Subs->addAccessor(constructFunctionNode(AC, SDKNodeKind::DeclAccessor));
    }
  }
  return Subs;
}

void swift::ide::api::
SwiftDeclCollector::addMembersToRoot(SDKNode *Root, IterableDeclContext *Context) {
  for (auto *Member : Context->getMembers()) {
    if (Ctx.shouldIgnore(Member, Context->getDecl()))
      continue;
    if (auto Func = dyn_cast<FuncDecl>(Member)) {
      Root->addChild(constructFunctionNode(Func, SDKNodeKind::DeclFunction));
    } else if (auto CD = dyn_cast<ConstructorDecl>(Member)) {
      Root->addChild(constructInitNode(CD));
    } else if (auto VD = dyn_cast<VarDecl>(Member)) {
      Root->addChild(constructVarNode(VD));
    } else if (auto TAD = dyn_cast<TypeAliasDecl>(Member)) {
      Root->addChild(constructTypeAliasNode(TAD));
    } else if (auto EED = dyn_cast<EnumElementDecl>(Member)) {
      Root->addChild(constructVarNode(EED));
    } else if (auto NTD = dyn_cast<NominalTypeDecl>(Member)) {
      Root->addChild(constructTypeDeclNode(NTD));
    } else if (auto ATD = dyn_cast<AssociatedTypeDecl>(Member)) {
      Root->addChild(constructAssociatedTypeNode(ATD));
    } else if (auto SD = dyn_cast<SubscriptDecl>(Member)) {
      Root->addChild(constructSubscriptDeclNode(SD));
    } else if (isa<PatternBindingDecl>(Member)) {
      // All containing variables should have been handled.
    } else if (isa<EnumCaseDecl>(Member)) {
      // All containing variables should have been handled.
    } else if (isa<IfConfigDecl>(Member)) {
      // All containing members should have been handled.
    } else if (isa<PoundDiagnosticDecl>(Member)) {
      // All containing members should have been handled.
    } else if (isa<DestructorDecl>(Member)) {
      // deinit has no impact.
    } else if (isa<MissingMemberDecl>(Member)) {
      // avoid adding MissingMemberDecl
    } else {
      llvm::errs() << "Unhandled decl:\n";
      Member->dump(llvm::errs());
    }
  }
}

SDKNode *swift::ide::api::
SwiftDeclCollector::constructTypeWitnessNode(AssociatedTypeDecl *Assoc,
                                             Type Ty) {
  auto *Witness = SDKNodeInitInfo(Ctx, Assoc).createSDKNode(SDKNodeKind::TypeWitness);
  Witness->addChild(constructTypeNode(Ty));
  return Witness;
}

SDKNode *swift::ide::api::
SwiftDeclCollector::constructConformanceNode(ProtocolConformance *Conform) {
  if (Ctx.checkingABI())
    Conform = Conform->getCanonicalConformance();
  auto ConfNode = cast<SDKNodeConformance>(SDKNodeInitInfo(Ctx,
    ProtocolConformanceRef(Conform)).createSDKNode(SDKNodeKind::Conformance));
  Conform->forEachTypeWitness(
    [&](AssociatedTypeDecl *assoc, Type ty, TypeDecl *typeDecl) -> bool {
      ConfNode->addChild(constructTypeWitnessNode(assoc, ty));
      return false;
    });
  return ConfNode;
}

void swift::ide::api::
SwiftDeclCollector::addConformancesToTypeDecl(SDKNodeDeclType *Root,
                                              NominalTypeDecl *NTD) {
  if (auto *PD = dyn_cast<ProtocolDecl>(NTD)) {
    PD->walkInheritedProtocols([&](ProtocolDecl *inherited) {
      if (PD != inherited && !Ctx.shouldIgnore(inherited)) {
        ProtocolConformanceRef Conf(inherited);
        auto ConfNode = SDKNodeInitInfo(Ctx, Conf)
            .createSDKNode(SDKNodeKind::Conformance);
        Root->addConformance(ConfNode);
      }

      return TypeWalker::Action::Continue;
    });
  } else {
    // Avoid adding the same conformance twice.
    SmallPtrSet<ProtocolConformance*, 4> Seen;
    for (auto &Conf: NTD->getAllConformances()) {
      if (!Ctx.shouldIgnore(Conf->getProtocol()) && !Seen.count(Conf))
        Root->addConformance(constructConformanceNode(Conf));
      Seen.insert(Conf);
    }
  }
}

void SwiftDeclCollector::printTopLevelNames() {
  for (auto &Node : RootNode->getChildren()) {
    llvm::outs() << Node->getKind() << ": " << Node->getName() << '\n';
  }
}

void SwiftDeclCollector::lookupVisibleDecls(ArrayRef<ModuleDecl *> Modules) {
  for (auto M: Modules) {
    llvm::SmallVector<Decl*, 512> Decls;
    swift::getTopLevelDeclsForDisplay(M, Decls);
    for (auto D : Decls) {
      if (Ctx.shouldIgnore(D))
        continue;
      if (KnownDecls.count(D))
        continue;
      KnownDecls.insert(D);
      if (auto VD = dyn_cast<ValueDecl>(D))
        foundDecl(VD, DeclVisibilityKind::DynamicLookup,
                  DynamicLookupInfo::AnyObject);
      else
        processDecl(D);
    }
  }

  // Now sort the macros before processing so that we can have deterministic
  // output.
  llvm::array_pod_sort(ClangMacros.begin(), ClangMacros.end(),
     [](ValueDecl * const *lhs,
        ValueDecl * const *rhs) -> int {
       return (*lhs)->getBaseName().userFacingName().compare(
                (*rhs)->getBaseName().userFacingName());
     });

  for (auto *VD : ClangMacros)
    processValueDecl(VD);

  // Collect extensions to types from other modules and synthesize type nodes
  // for them.
  llvm::MapVector<NominalTypeDecl*, llvm::SmallVector<ExtensionDecl*, 4>> ExtensionMap;
  for (auto *D: KnownDecls) {
    if (auto *Ext = dyn_cast<ExtensionDecl>(D)) {
      if (HandledExtensions.find(Ext) == HandledExtensions.end()) {
        if (auto *NTD = Ext->getExtendedNominal()) {
          // Check if the extension is from other modules.
          if (!llvm::is_contained(Modules, NTD->getModuleContext())) {
            ExtensionMap[NTD].push_back(Ext);
          }
        }
      }
    }
  }
  for (auto Pair: ExtensionMap) {
    if (auto child = constructExternalExtensionNode(Pair.first, Pair.second))
      RootNode->addChild(child);
  }
}

SDKNode *SwiftDeclCollector::constructOperatorDeclNode(OperatorDecl *OD) {
  return SDKNodeInitInfo(Ctx, OD).createSDKNode(SDKNodeKind::DeclOperator);
}

void SwiftDeclCollector::processDecl(Decl *D) {
  assert(!isa<ValueDecl>(D));
  if (auto *OD = dyn_cast<OperatorDecl>(D)) {
    RootNode->addChild(constructOperatorDeclNode(OD));
  }
  if (auto *IM = dyn_cast<ImportDecl>(D)) {
    RootNode->addChild(SDKNodeInitInfo(Ctx, IM)
      .createSDKNode(SDKNodeKind::DeclImport));
  }
}

void SwiftDeclCollector::processValueDecl(ValueDecl *VD) {
  if (auto FD = dyn_cast<FuncDecl>(VD)) {
    RootNode->addChild(constructFunctionNode(FD, SDKNodeKind::DeclFunction));
  } else if (auto NTD = dyn_cast<NominalTypeDecl>(VD)) {
    RootNode->addChild(constructTypeDeclNode(NTD));
  } else if (auto VAD = dyn_cast<VarDecl>(VD)) {
    RootNode->addChild(constructVarNode(VAD));
  } else if (auto TAD = dyn_cast<TypeAliasDecl>(VD)) {
    RootNode->addChild(constructTypeAliasNode(TAD));
  } else if (auto MD = dyn_cast<MacroDecl>(VD)) {
    RootNode->addChild(constructMacroNode(MD));
  } else {
    llvm_unreachable("unhandled value decl");
  }
}

void SwiftDeclCollector::foundDecl(ValueDecl *VD, DeclVisibilityKind Reason,
                                   DynamicLookupInfo) {
  if (VD->getClangMacro()) {
    // Collect macros, we will sort them afterwards.
    ClangMacros.push_back(VD);
    return;
  }

  processValueDecl(VD);
}

void SDKNode::output(json::Output &out, KeyKind Key, bool Value) {
  if (Value)
    out.mapRequired(getKeyContent(Ctx, Key).data(), Value);
}

void SDKNode::output(json::Output &out, KeyKind Key, StringRef Value) {
  if (!Value.empty())
    out.mapRequired(getKeyContent(Ctx, Key).data(), Value);
}

void SDKNode::jsonize(json::Output &out) {
  auto Kind = getKind();
  out.mapRequired(getKeyContent(Ctx, KeyKind::KK_kind).data(), Kind);
  output(out, KeyKind::KK_name, Name);
  output(out, KeyKind::KK_printedName, PrintedName);
  out.mapOptional(getKeyContent(Ctx, KeyKind::KK_children).data(), Children);
}

void SDKNodeRoot::jsonize(json::Output &out) {
  SDKNode::jsonize(out);
  out.mapRequired(getKeyContent(Ctx, KeyKind::KK_json_format_version).data(), JsonFormatVer);
  if (!Ctx.getOpts().AvoidToolArgs)
    out.mapOptional(getKeyContent(Ctx, KeyKind::KK_tool_arguments).data(), ToolArgs);
}

void SDKNodeConformance::jsonize(json::Output &out) {
  SDKNode::jsonize(out);
  output(out, KeyKind::KK_usr, Usr);
  output(out, KeyKind::KK_mangledName, MangledName);
  output(out, KeyKind::KK_isABIPlaceholder, IsABIPlaceholder);
}

void SDKNodeDecl::jsonize(json::Output &out) {
  SDKNode::jsonize(out);
  out.mapRequired(getKeyContent(Ctx, KeyKind::KK_declKind).data(), DKind);
  output(out, KeyKind::KK_usr, Usr);
  output(out, KeyKind::KK_mangledName, MangledName);
  output(out, KeyKind::KK_location, Location);
  output(out, KeyKind::KK_moduleName, ModuleName);
  output(out, KeyKind::KK_genericSig, GenericSig);
  output(out, KeyKind::KK_sugared_genericSig, SugaredGenericSig);
  output(out, KeyKind::KK_static, IsStatic);
  output(out, KeyKind::KK_deprecated,IsDeprecated);
  output(out, KeyKind::KK_protocolReq, IsProtocolReq);
  output(out, KeyKind::KK_overriding, IsOverriding);
  output(out, KeyKind::KK_implicit, IsImplicit);
  output(out, KeyKind::KK_isOpen, IsOpen);
  output(out, KeyKind::KK_isInternal, IsInternal);
  output(out, KeyKind::KK_isABIPlaceholder, IsABIPlaceholder);
  output(out, KeyKind::KK_intro_Macosx, introVersions.macos);
  output(out, KeyKind::KK_intro_iOS, introVersions.ios);
  output(out, KeyKind::KK_intro_tvOS, introVersions.tvos);
  output(out, KeyKind::KK_intro_watchOS, introVersions.watchos);
  output(out, KeyKind::KK_intro_swift, introVersions.swift);
  output(out, KeyKind::KK_objc_name, ObjCName);
  out.mapOptional(getKeyContent(Ctx, KeyKind::KK_declAttributes).data(), DeclAttributes);
  out.mapOptional(getKeyContent(Ctx, KeyKind::KK_fixedbinaryorder).data(), FixedBinaryOrder);
  // Strong reference is implied, no need for serialization.
  if (getReferenceOwnership() != ReferenceOwnership::Strong) {
    uint8_t Raw = uint8_t(getReferenceOwnership());
    out.mapRequired(getKeyContent(Ctx, KeyKind::KK_ownership).data(), Raw);
  }
  output(out, KeyKind::KK_isFromExtension, IsFromExtension);
  out.mapOptional(getKeyContent(Ctx, KeyKind::KK_spi_group_names).data(), SPIGroups);
}

void SDKNodeDeclAbstractFunc::jsonize(json::Output &out) {
  SDKNodeDecl::jsonize(out);
  output(out, KeyKind::KK_throwing, IsThrowing);
  output(out, KeyKind::KK_reqNewWitnessTableEntry, ReqNewWitnessTableEntry);
  out.mapOptional(getKeyContent(Ctx, KeyKind::KK_selfIndex).data(), SelfIndex);
}

void SDKNodeDeclFunction::jsonize(json::Output &out) {
  SDKNodeDeclAbstractFunc::jsonize(out);
  output(out, KeyKind::KK_funcSelfKind, FuncSelfKind);
}

void SDKNodeDeclConstructor::jsonize(json::Output &out) {
  SDKNodeDeclAbstractFunc::jsonize(out);
  output(out, KeyKind::KK_init_kind, InitKind);
}

void SDKNodeDeclType::jsonize(json::Output &out) {
  SDKNodeDecl::jsonize(out);
  output(out, KeyKind::KK_superclassUsr, SuperclassUsr);
  output(out, KeyKind::KK_enumRawTypeName, EnumRawTypeName);
  output(out, KeyKind::KK_isExternal, IsExternal);
  output(out, KeyKind::KK_isEnumExhaustive, IsEnumExhaustive);
  output(out, KeyKind::KK_hasMissingDesignatedInitializers,
         HasMissingDesignatedInitializers);
  output(out, KeyKind::KK_inheritsConvenienceInitializers,
         InheritsConvenienceInitializers);
  out.mapOptional(getKeyContent(Ctx, KeyKind::KK_superclassNames).data(), SuperclassNames);
  out.mapOptional(getKeyContent(Ctx, KeyKind::KK_conformances).data(), Conformances);
}

void SDKNodeType::jsonize(json::Output &out) {
  SDKNode::jsonize(out);
  out.mapOptional(getKeyContent(Ctx, KeyKind::KK_typeAttributes).data(), TypeAttributes);
  output(out, KeyKind::KK_hasDefaultArg, HasDefaultArg);
  output(out, KeyKind::KK_paramValueOwnership, ParamValueOwnership);
}

void SDKNodeTypeNominal::jsonize(json::Output &out) {
  SDKNodeType::jsonize(out);
  output(out, KeyKind::KK_usr, USR);
}

void SDKNodeDeclSubscript::jsonize(json::Output &out) {
  SDKNodeDeclAbstractFunc::jsonize(out);
  output(out, KeyKind::KK_hasStorage, HasStorage);
  out.mapOptional(getKeyContent(Ctx, KeyKind::KK_accessors).data(), Accessors);
}

void SDKNodeDeclVar::jsonize(json::Output &out) {
  SDKNodeDecl::jsonize(out);
  output(out, KeyKind::KK_isLet, IsLet);
  output(out, KeyKind::KK_hasStorage, HasStorage);
  out.mapOptional(getKeyContent(Ctx, KeyKind::KK_accessors).data(), Accessors);
}

void SDKNodeDeclAccessor::jsonize(json::Output &out) {
  SDKNodeDeclAbstractFunc::jsonize(out);
  out.mapRequired(getKeyContent(Ctx, KeyKind::KK_accessorKind).data(), AccKind);
}


namespace swift {
namespace json {
// In the namespace of swift::json, we define several functions so that the
// JSON serializer will know how to interpret and dump types defined in this
// file.
template<>
struct ScalarEnumerationTraits<TypeAttrKind> {
  static void enumeration(Output &out, TypeAttrKind &value) {
#define TYPE_ATTR(X) out.enumCase(value, #X, TypeAttrKind::TAK_##X);
#include "swift/AST/Attr.def"
  }
};

template<>
struct ScalarEnumerationTraits<DeclAttrKind> {
  static void enumeration(Output &out, DeclAttrKind &value) {
#define DECL_ATTR(_, Name, ...) out.enumCase(value, #Name, DeclAttrKind::DAK_##Name);
#include "swift/AST/Attr.def"
  }
};

template<>
struct ScalarEnumerationTraits<DeclKind> {
  static void enumeration(Output &out, DeclKind &value) {
#define DECL(X, PARENT) out.enumCase(value, #X, DeclKind::X);
#include "swift/AST/DeclNodes.def"
  }
};

template<>
struct ScalarEnumerationTraits<AccessorKind> {
  static void enumeration(Output &out, AccessorKind &value) {
#define ACCESSOR(ID)
#define SINGLETON_ACCESSOR(ID, KEYWORD) \
  out.enumCase(value, #KEYWORD, AccessorKind::ID);
#include "swift/AST/AccessorKinds.def"
  }
};

template<>
struct ObjectTraits<SDKNode *> {
  static void mapping(Output &out, SDKNode *&value) {
    value->jsonize(out);
  }
};

template<>
struct ArrayTraits<ArrayRef<SDKNode*>> {
  static size_t size(Output &out, ArrayRef<SDKNode *> &seq) {
    return seq.size();
  }
  static SDKNode *&element(Output &, ArrayRef<SDKNode *> &seq,
                                size_t index) {
    return const_cast<SDKNode *&>(seq[index]);
  }
};

template<>
struct ArrayTraits<ArrayRef<TypeAttrKind>> {
  static size_t size(Output &out, ArrayRef<TypeAttrKind> &seq) {
    return seq.size();
  }
  static TypeAttrKind& element(Output &, ArrayRef<TypeAttrKind> &seq,
                               size_t index) {
    return const_cast<TypeAttrKind&>(seq[index]);
  }
};

template<>
struct ArrayTraits<ArrayRef<DeclAttrKind>> {
  static size_t size(Output &out, ArrayRef<DeclAttrKind> &seq) {
    return seq.size();
  }
  static DeclAttrKind& element(Output &, ArrayRef<DeclAttrKind> &seq,
                               size_t index) {
    return const_cast<DeclAttrKind&>(seq[index]);
  }
};
template<>
struct ArrayTraits<ArrayRef<StringRef>> {
  static size_t size(Output &out, ArrayRef<StringRef> &seq) {
    return seq.size();
  }
  static StringRef& element(Output &, ArrayRef<StringRef> &seq,
                               size_t index) {
    return const_cast<StringRef&>(seq[index]);
  }
};
} // namespace json
} // namespace swift

namespace  {// Anonymous namespace.
// Deserialize an SDKNode tree.
std::pair<std::unique_ptr<llvm::MemoryBuffer>, SDKNode*>
static parseJsonEmit(SDKContext &Ctx, StringRef FileName) {
  namespace yaml = llvm::yaml;

  // Load the input file.
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
    vfs::getFileOrSTDIN(*Ctx.getSourceMgr().getFileSystem(), FileName,
                        /*FileSize*/-1, /*RequiresNullTerminator*/true,
                        /*IsVolatile*/false, /*RetryCount*/30);
  if (!FileBufOrErr) {
    llvm_unreachable("Failed to read JSON file");
  }
  StringRef Buffer = FileBufOrErr->get()->getBuffer();
  yaml::Stream Stream(llvm::MemoryBufferRef(Buffer, FileName),
                      Ctx.getSourceMgr().getLLVMSourceMgr());
  SDKNode *Result = nullptr;
  for (auto DI = Stream.begin(); DI != Stream.end(); ++ DI) {
    assert(DI != Stream.end() && "Failed to read a document");
    yaml::Node *N = DI->getRoot();
    assert(N && "Failed to find a root");
    Result = SDKNode::constructSDKNode(Ctx, cast<yaml::MappingNode>(N));
  }
  return {std::move(FileBufOrErr.get()), Result};
}
enum class ConstKind: uint8_t {
  StringLiteral = 0,
  IntegerLiteral,
  FloatLiteral,
  BooleanLiteral,
  Array,
  Dictionary,
};

struct ConstExprInfo {
  StringRef filePath;
  ConstKind kind;
  unsigned offset = 0;
  unsigned length = 0;
  StringRef value;
  StringRef referencedD;
  ConstExprInfo(StringRef filePath, ConstKind kind, unsigned offset,
                unsigned length, StringRef value, StringRef referencedD):
    filePath(filePath), kind(kind), offset(offset), length(length), value(value),
    referencedD(referencedD) {}
  ConstExprInfo() = default;
};

class ConstExtractor: public ASTWalker {
  SDKContext &SCtx;
  SourceManager &SM;
  std::vector<ConstExprInfo> allConsts;

  void record(Expr *E, ConstKind kind, StringRef Value, StringRef ReferencedD) {
    auto startLoc = E->getStartLoc();
    // Asserts?
    if (startLoc.isInvalid())
      return;
    auto endLoc = E->getEndLoc();
    assert(endLoc.isValid());
    endLoc = Lexer::getLocForEndOfToken(SM, endLoc);
    auto bufferId = SM.findBufferContainingLoc(startLoc);
    auto length = SM.getByteDistance(startLoc, endLoc);
    auto file = SM.getIdentifierForBuffer(bufferId);
    auto offset = SM.getLocOffsetInBuffer(startLoc, bufferId);
    allConsts.emplace_back(file, kind, offset, length, Value, ReferencedD);
  }

  void record(Expr *E, Expr *ValueProvider, StringRef ReferencedD = "") {
    std::string content;
    llvm::raw_string_ostream os(content);
    ValueProvider->printConstExprValue(&os, nullptr);
    assert(!content.empty());
    auto buffered = SCtx.buffer(content);
    switch(ValueProvider->getKind()) {
#define CASE(X) case ExprKind::X: record(E, ConstKind::X, buffered, ReferencedD); break;
      CASE(StringLiteral)
      CASE(IntegerLiteral)
      CASE(FloatLiteral)
      CASE(BooleanLiteral)
      CASE(Dictionary)
      CASE(Array)
#undef CASE
    default:
      return;
    }
  }

  StringRef getDeclName(Decl *D) {
    if (auto *VD = dyn_cast<ValueDecl>(D)) {
      std::string content;
      llvm::raw_string_ostream os(content);
      VD->getName().print(os);
      return SCtx.buffer(content);
    }
    return StringRef();
  }

  bool handleSimpleReference(Expr *E) {
    assert(E);
    Decl *ReferencedDecl = nullptr;
    if (auto *MRE = dyn_cast<MemberRefExpr>(E)) {
      ReferencedDecl = MRE->getDecl().getDecl();
    } else if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
      ReferencedDecl = DRE->getDecl();
    } else {
      return false;
    }
    assert(ReferencedDecl);
    if (auto *VAR = dyn_cast<VarDecl>(ReferencedDecl)) {
      if (!VAR->getAttrs().hasAttribute<CompileTimeConstAttr>()) {
        return false;
      }
      if (auto *PD = VAR->getParentPatternBinding()) {
        if (auto *init = PD->getInit(PD->getPatternEntryIndexForVarDecl(VAR))) {
          record(E, init, getDeclName(ReferencedDecl));
          return true;
        }
      }
    }
    return false;
  }

  MacroWalking getMacroWalkingBehavior() const override {
    return MacroWalking::ArgumentsAndExpansion;
  }

  PreWalkResult<Expr *> walkToExprPre(Expr *E) override {
    if (E->isSemanticallyConstExpr()) {
      record(E, E);
      return Action::SkipChildren(E);
    }
    if (handleSimpleReference(E)) {
      return Action::SkipChildren(E);
    }
    return Action::Continue(E);
  }

public:
  ConstExtractor(SDKContext &SCtx, ASTContext &Ctx): SCtx(SCtx),
    SM(Ctx.SourceMgr) {}
  void extract(ModuleDecl *MD) { MD->walk(*this); }
  std::vector<ConstExprInfo> &getAllConstValues() { return allConsts; }
};
} // End of anonymous namespace

template <> struct swift::json::ObjectTraits<ConstExprInfo> {
  static void mapping(Output &out, ConstExprInfo &info) {
    out.mapRequired("filePath", info.filePath);
    StringRef kind;
    switch(info.kind) {
#define CASE(X) case ConstKind::X: kind = #X; break;
    CASE(StringLiteral)
    CASE(IntegerLiteral)
    CASE(FloatLiteral)
    CASE(BooleanLiteral)
    CASE(Dictionary)
    CASE(Array)
#undef CASE
    }
    out.mapRequired("kind", kind);
    out.mapRequired("offset", info.offset);
    out.mapRequired("length", info.length);
    out.mapRequired("value", info.value);
    if (!info.referencedD.empty())
      out.mapRequired("decl", info.referencedD);
  }
};

struct swift::ide::api::PayLoad {
  std::vector<ConstExprInfo> *allContsValues = nullptr;
};

// Construct all roots vector from a given file where a forest was
// previously dumped.
void SwiftDeclCollector::deSerialize(StringRef Filename) {
  auto Pair = parseJsonEmit(Ctx, Filename);
  RootNode = std::move(Pair.second);
}

// Serialize the content of all roots to a given file using JSON format.
void SwiftDeclCollector::serialize(llvm::raw_ostream &os, SDKNode *Root,
                                   PayLoad OtherInfo) {
  std::error_code EC;
  json::Output yout(os);
  assert(Root->getKind() == SDKNodeKind::Root);
  SDKNodeRoot &root = *static_cast<SDKNodeRoot*>(Root);
  yout.beginObject();
  yout.mapRequired(ABIRootKey, root);
  if (auto *constValues = OtherInfo.allContsValues) {
    yout.mapRequired(ConstValuesKey, *constValues);
  }
  yout.endObject();
}

// Serialize the content of all roots to a given file using JSON format.
void SwiftDeclCollector::serialize(llvm::raw_ostream &os) {
  SwiftDeclCollector::serialize(os, RootNode, PayLoad());
}

SDKNodeRoot *
swift::ide::api::getEmptySDKNodeRoot(SDKContext &SDKCtx) {
  SwiftDeclCollector Collector(SDKCtx);
  return Collector.getSDKRoot();
}

SDKNodeRoot*
swift::ide::api::getSDKNodeRoot(SDKContext &SDKCtx,
                                 const CompilerInvocation &InitInvoke,
                                 const llvm::StringSet<> &ModuleNames) {
  CheckerOptions Opts = SDKCtx.getOpts();
  CompilerInvocation Invocation(InitInvoke);

  CompilerInstance &CI = SDKCtx.newCompilerInstance();
  // Display diagnostics to stderr.
  PrintingDiagnosticConsumer PrintDiags(llvm::errs());
  if (llvm::errs().has_colors())
    PrintDiags.forceColors();
  CI.addDiagnosticConsumer(&PrintDiags);
  // The PrintDiags is only responsible compiler errors, we should remove the
  // consumer immediately after importing is done.
  SWIFT_DEFER { CI.getDiags().removeConsumer(PrintDiags); };
  std::string InstanceSetupError;
  if (CI.setup(Invocation, InstanceSetupError)) {
    llvm::errs() << InstanceSetupError << '\n';
    return nullptr;
  }

  auto &Ctx = CI.getASTContext();

  // Load standard library so that Clang importer can use it.
  auto *Stdlib = Ctx.getStdlibModule(/*loadIfAbsent=*/true);
  if (!Stdlib) {
    llvm::errs() << "Failed to load Swift stdlib\n";
    return nullptr;
  }

  std::vector<ModuleDecl *> Modules;
  for (auto &Entry : ModuleNames) {
    StringRef Name = Entry.getKey();
    if (Opts.Verbose)
      llvm::errs() << "Loading module: " << Name << "...\n";
    auto *M = Ctx.getModuleByName(Name);
    if (!M || M->failedToLoad() || Ctx.Diags.hadAnyError()) {
      llvm::errs() << "Failed to load module: " << Name << '\n';
      if (Opts.AbortOnModuleLoadFailure)
        return nullptr;
    } else {
      Modules.push_back(M);
    }
  }
  if (Opts.Verbose)
    llvm::errs() << "Scanning symbols...\n";

  SwiftDeclCollector Collector(SDKCtx);
  Collector.lookupVisibleDecls(Modules);
  return Collector.getSDKRoot();
}

void swift::ide::api::dumpSDKRoot(SDKNodeRoot *Root, PayLoad load,
                                  llvm::raw_ostream &os) {
  assert(Root);
  auto Opts = Root->getSDKContext().getOpts();
  if (Opts.Verbose)
    llvm::errs() << "Dumping SDK...\n";
  SwiftDeclCollector::serialize(os, Root, load);
}

void swift::ide::api::dumpSDKRoot(SDKNodeRoot *Root, llvm::raw_ostream &os) {
  dumpSDKRoot(Root, PayLoad(), os);
}

int swift::ide::api::dumpSDKContent(const CompilerInvocation &InitInvoke,
                                    const llvm::StringSet<> &ModuleNames,
                                    llvm::raw_ostream &os, CheckerOptions Opts) {
  SDKContext SDKCtx(Opts);
  SDKNodeRoot *Root = getSDKNodeRoot(SDKCtx, InitInvoke, ModuleNames);
  if (!Root)
    return 1;
  dumpSDKRoot(Root, os);
  return 0;
}

int swift::ide::api::deserializeSDKDump(StringRef dumpPath, StringRef OutputPath,
    CheckerOptions Opts) {
  std::error_code EC;
  llvm::raw_fd_ostream FS(OutputPath, EC, llvm::sys::fs::OF_None);
  if (!fs::exists(dumpPath)) {
    llvm::errs() << dumpPath << " does not exist\n";
    return 1;
  }
  PrintingDiagnosticConsumer PDC;
  SDKContext Ctx(Opts);
  Ctx.addDiagConsumer(PDC);

  SwiftDeclCollector Collector(Ctx);
  Collector.deSerialize(dumpPath);
  Collector.serialize(FS);
  return 0;
}

void swift::ide::api::dumpModuleContent(ModuleDecl *MD, llvm::raw_ostream &os,
                                        bool ABI, bool Empty) {
  CheckerOptions opts;
  opts.ABI = ABI;
  opts.SwiftOnly = true;
  opts.AvoidLocation = true;
  opts.AvoidToolArgs = true;
  opts.Migrator = false;
  opts.SkipOSCheck = false;
  opts.SkipRemoveDeprecatedCheck = false;
  opts.Verbose = false;
  SDKContext ctx(opts);
  SwiftDeclCollector collector(ctx);
  ConstExtractor extractor(ctx, MD->getASTContext());
  PayLoad payload;
  SWIFT_DEFER {
    payload.allContsValues = &extractor.getAllConstValues();
    dumpSDKRoot(collector.getSDKRoot(), payload, os);
  };
  if (Empty) {
    return;
  }
  collector.lookupVisibleDecls({MD});
  extractor.extract(MD);
}

int swift::ide::api::findDeclUsr(StringRef dumpPath, CheckerOptions Opts) {
  std::error_code EC;
  if (!fs::exists(dumpPath)) {
    llvm::errs() << dumpPath << " does not exist\n";
    return 1;
  }
  PrintingDiagnosticConsumer PDC;
  SDKContext Ctx(Opts);
  Ctx.addDiagConsumer(PDC);

  SwiftDeclCollector Collector(Ctx);
  Collector.deSerialize(dumpPath);
  struct FinderByLocation: SDKNodeVisitor {
    StringRef Location;
    FinderByLocation(StringRef Location): Location(Location) {}
    void visit(SDKNode* Node) override {
      if (auto *D = dyn_cast<SDKNodeDecl>(Node)) {
        if (D->getLocation().contains(Location) && !D->getUsr().empty()) {
          llvm::outs() << D->getFullyQualifiedName() << ": " << D->getUsr() << "\n";
        }
      }
    }
  };
  if (!Opts.LocationFilter.empty()) {
    FinderByLocation Finder(Opts.LocationFilter);
    Collector.visitAllRoots(Finder);
  }
  return 0;
}

void swift::ide::api::SDKNodeDeclType::diagnose(SDKNode *Right) {
  SDKNodeDecl::diagnose(Right);
  auto *R = dyn_cast<SDKNodeDeclType>(Right);
  if (!R)
    return;
  auto Loc = R->getLoc();
  if (getDeclKind() != R->getDeclKind()) {
    emitDiag(Loc, diag::decl_kind_changed, getDeclKindStr(R->getDeclKind(),
      getSDKContext().getOpts().CompilerStyle));
    return;
  }

  assert(getDeclKind() == R->getDeclKind());
  auto DKind = getDeclKind();
  switch (DKind) {
  case DeclKind::Class: {
    auto LSuperClass = getSuperClassName();
    auto RSuperClass = R->getSuperClassName();
    if (!LSuperClass.empty() && LSuperClass != RSuperClass) {
      if (RSuperClass.empty()) {
        emitDiag(Loc, diag::super_class_removed, LSuperClass);
      } else if (!llvm::is_contained(R->getClassInheritanceChain(), LSuperClass)) {
        emitDiag(Loc, diag::super_class_changed, LSuperClass, RSuperClass);
      }
    }

    // Check for @_hasMissingDesignatedInitializers and
    // @_inheritsConvenienceInitializers changes.
    if (isOpen() && R->isOpen()) {
      // It's not safe to add new, invisible designated inits to open
      // classes.
      if (!hasMissingDesignatedInitializers() &&
          R->hasMissingDesignatedInitializers())
        R->emitDiag(R->getLoc(), diag::added_invisible_designated_init);
    }

    // It's not safe to stop inheriting convenience inits, it changes
    // the set of initializers that are available.
    if (!Ctx.checkingABI() &&
        inheritsConvenienceInitializers() &&
        !R->inheritsConvenienceInitializers())
      R->emitDiag(R->getLoc(), diag::not_inheriting_convenience_inits);
    break;
  }
  default:
    break;
  }
}

void swift::ide::api::SDKNodeDeclAbstractFunc::diagnose(SDKNode *Right) {
  SDKNodeDecl::diagnose(Right);
  auto *R = dyn_cast<SDKNodeDeclAbstractFunc>(Right);
  if (!R)
    return;
  auto Loc = R->getLoc();
  if (!isThrowing() && R->isThrowing()) {
    emitDiag(Loc, diag::decl_new_attr, Ctx.buffer("throwing"));
  }
  if (Ctx.checkingABI()) {
    if (reqNewWitnessTableEntry() != R->reqNewWitnessTableEntry()) {
      emitDiag(Loc, diag::decl_new_witness_table_entry, reqNewWitnessTableEntry());
    }

    // Diagnose moving a non-final class member to an extension.
    if (hasValidParentPtr(getKind())) {
      while(auto *parent = dyn_cast<SDKNodeDecl>(getParent())) {
        if (parent->getDeclKind() != DeclKind::Class) {
          break;
        }
        if (hasDeclAttribute(DeclAttrKind::DAK_Final)) {
          break;
        }
        auto result = isFromExtensionChanged(*this, *Right);
        if (result.has_value() && *result) {
          emitDiag(Loc, diag::class_member_moved_to_extension);
        }
        break;
      }
    }
  }
}

void swift::ide::api::SDKNodeDeclFunction::diagnose(SDKNode *Right) {
  SDKNodeDeclAbstractFunc::diagnose(Right);
  auto *R = dyn_cast<SDKNodeDeclFunction>(Right);
  if (!R)
    return;
  auto Loc = R->getLoc();
  if (getSelfAccessKind() != R->getSelfAccessKind()) {
    emitDiag(Loc, diag::func_self_access_change, getSelfAccessKind(),
             R->getSelfAccessKind());
  }
  if (Ctx.checkingABI()) {
    if (hasFixedBinaryOrder() != R->hasFixedBinaryOrder()) {
      emitDiag(Loc, diag::func_has_fixed_order_change, hasFixedBinaryOrder());
    }
  }
}

static StringRef getAttrName(DeclAttrKind Kind) {
  switch (Kind) {
#define DECL_ATTR(NAME, CLASS, ...)                                           \
  case DAK_##CLASS:                                                           \
      return DeclAttribute::isDeclModifier(DAK_##CLASS) ? #NAME : "@"#NAME;
#include "swift/AST/Attr.def"
  case DAK_Count:
    llvm_unreachable("unrecognized attribute kind.");
  }
  llvm_unreachable("covered switch");
}

static bool shouldDiagnoseAddingAttribute(SDKNodeDecl *D, DeclAttrKind Kind) {
  return true;
}

static bool shouldDiagnoseRemovingAttribute(SDKNodeDecl *D, DeclAttrKind Kind) {
  return true;
}

static bool isOwnershipEquivalent(ReferenceOwnership Left,
                                  ReferenceOwnership Right) {
  if (Left == Right)
    return true;
  if (Left == ReferenceOwnership::Unowned && Right == ReferenceOwnership::Weak)
    return true;
  if (Left == ReferenceOwnership::Weak && Right == ReferenceOwnership::Unowned)
    return true;
  return false;
}

void swift::ide::api::detectRename(SDKNode *L, SDKNode *R) {
  if (L->getKind() == R->getKind() && isa<SDKNodeDecl>(L) &&
      L->getPrintedName() != R->getPrintedName()) {
    L->annotate(NodeAnnotation::Rename);
    L->annotate(NodeAnnotation::RenameOldName, L->getPrintedName());
    L->annotate(NodeAnnotation::RenameNewName, R->getPrintedName());
  }
}

void swift::ide::api::SDKNodeDecl::diagnose(SDKNode *Right) {
  SDKNode::diagnose(Right);
  auto *RD = dyn_cast<SDKNodeDecl>(Right);
  if (!RD)
    return;
  detectRename(this, RD);
  auto Loc = RD->getLoc();
  if (isOpen() && !RD->isOpen()) {
    emitDiag(Loc, diag::no_longer_open);
  }

  // Diagnose static attribute change.
  if (isStatic() ^ RD->isStatic()) {
    emitDiag(Loc, diag::decl_new_attr, Ctx.buffer(isStatic() ? "not static" :
                                             "static"));
  }

  // Diagnose ownership change.
  if (!isOwnershipEquivalent(getReferenceOwnership(),
                             RD->getReferenceOwnership())) {
    auto getOwnershipDescription = [&](swift::ReferenceOwnership O) {
      if (O == ReferenceOwnership::Strong)
        return Ctx.buffer("strong");
      return keywordOf(O);
    };
    emitDiag(Loc, diag::decl_attr_change,
             getOwnershipDescription(getReferenceOwnership()),
             getOwnershipDescription(RD->getReferenceOwnership()));
  }
  // Diagnose generic signature change
  if (getGenericSignature() != RD->getGenericSignature()) {
    // Prefer sugared signature in diagnostics to be more user-friendly.
    if (Ctx.commonVersionAtLeast(2) &&
        getSugaredGenericSignature() != RD->getSugaredGenericSignature()) {
      emitDiag(Loc, diag::generic_sig_change,
               getSugaredGenericSignature(), RD->getSugaredGenericSignature());
    } else {
      emitDiag(Loc, diag::generic_sig_change,
               getGenericSignature(), RD->getGenericSignature());
    }
  }

  // ObjC name changes are considered breakage
  if (getObjCName() != RD->getObjCName()) {
    if (Ctx.commonVersionAtLeast(4)) {
      emitDiag(Loc, diag::objc_name_change, getObjCName(), RD->getObjCName());
    }
  }

  if (isOptional() != RD->isOptional()) {
    if (Ctx.checkingABI()) {
      // Both adding/removing optional is ABI-breaking.
      emitDiag(Loc, diag::optional_req_changed, isOptional());
    } else if (isOptional()) {
      // Removing optional is source-breaking.
      emitDiag(Loc, diag::optional_req_changed, isOptional());
    }
  }

  // Diagnose removing attributes.
  for (auto Kind: getDeclAttributes()) {
    if (!RD->hasDeclAttribute(Kind)) {
      if ((Ctx.checkingABI() ? DeclAttribute::isRemovingBreakingABI(Kind) :
                               DeclAttribute::isRemovingBreakingAPI(Kind)) &&
          shouldDiagnoseRemovingAttribute(this, Kind)) {
        emitDiag(Loc, diag::decl_new_attr,
                Ctx.buffer((llvm::Twine("without ") + getAttrName(Kind)).str()));
      }
    }
  }

  // Diagnose adding attributes.
  for (auto Kind: RD->getDeclAttributes()) {
    if (!hasDeclAttribute(Kind)) {
      if ((Ctx.checkingABI() ? DeclAttribute::isAddingBreakingABI(Kind) :
                               DeclAttribute::isAddingBreakingAPI(Kind)) &&
          shouldDiagnoseAddingAttribute(this, Kind)) {
        emitDiag(Loc, diag::decl_new_attr,
                Ctx.buffer((llvm::Twine("with ") + getAttrName(Kind)).str()));
      }
    }
  }

  if (Ctx.checkingABI()) {
    if (hasFixedBinaryOrder() && RD->hasFixedBinaryOrder() &&
        getFixedBinaryOrder() != RD->getFixedBinaryOrder()) {
      emitDiag(Loc, diag::decl_reorder, getFixedBinaryOrder(),
               RD->getFixedBinaryOrder());
    }
    if (getUsr() != RD->getUsr()) {
      auto left = demangleUSR(getUsr());
      auto right = demangleUSR(RD->getUsr());
      if (left != right) {
        emitDiag(Loc, diag::demangled_name_changed, left, right);
      }
    }
  }
}

void swift::ide::api::SDKNodeDeclOperator::diagnose(SDKNode *Right) {
  SDKNodeDecl::diagnose(Right);
  auto *RO = dyn_cast<SDKNodeDeclOperator>(Right);
  if (!RO)
    return;
  auto Loc = RO->getLoc();
  if (getDeclKind() != RO->getDeclKind()) {
    emitDiag(Loc, diag::decl_kind_changed, getDeclKindStr(RO->getDeclKind(),
      getSDKContext().getOpts().CompilerStyle));
  }
}

void swift::ide::api::SDKNodeDeclVar::diagnose(SDKNode *Right) {
  SDKNodeDecl::diagnose(Right);
  auto *RV = dyn_cast<SDKNodeDeclVar>(Right);
  if (!RV)
    return;
  auto Loc = RV->getLoc();
  if (Ctx.checkingABI()) {
    if (hasFixedBinaryOrder() != RV->hasFixedBinaryOrder()) {
      emitDiag(Loc, diag::var_has_fixed_order_change, hasFixedBinaryOrder());
    }
  }
}

static bool shouldDiagnoseType(SDKNodeType *T) {
  return T->isTopLevelType();
}

void swift::ide::api::SDKNodeType::diagnose(SDKNode *Right) {
  SDKNode::diagnose(Right);
  auto *RT = dyn_cast<SDKNodeType>(Right);
  if (!RT || !shouldDiagnoseType(this))
    return;
  assert(isTopLevelType());

  // Diagnose type witness changes when diagnosing ABI breakages.
  if (auto *Wit = dyn_cast<SDKNodeTypeWitness>(getParent())) {
    auto *Conform = Wit->getParent()->getAs<SDKNodeConformance>();
    if (Ctx.checkingABI() && getPrintedName() != RT->getPrintedName()) {
      auto *LD = Conform->getNominalTypeDecl();
      LD->emitDiag(SourceLoc(), diag::type_witness_change,
                   Wit->getWitnessedTypeName(),
                   getPrintedName(), RT->getPrintedName());
    }
    return;
  }

  StringRef Descriptor = getTypeRoleDescription();
  assert(isa<SDKNodeDecl>(getParent()));
  auto LParent = cast<SDKNodeDecl>(getParent());
  assert(LParent->getKind() == RT->getParent()->getAs<SDKNodeDecl>()->getKind());
  auto Loc = RT->getParent()->getAs<SDKNodeDecl>()->getLoc();
  if (getPrintedName() != RT->getPrintedName()) {
    LParent->emitDiag(Loc, diag::decl_type_change,
                      Descriptor, getPrintedName(), RT->getPrintedName());
  }

  if (hasDefaultArgument() && !RT->hasDefaultArgument()) {
    LParent->emitDiag(Loc, diag::default_arg_removed, Descriptor);
  }
  if (getParamValueOwnership() != RT->getParamValueOwnership()) {
    LParent->emitDiag(Loc, diag::param_ownership_change,
                      getTypeRoleDescription(),
                      getParamValueOwnership(),
                      RT->getParamValueOwnership());
  }
}

void swift::ide::api::SDKNodeTypeFunc::diagnose(SDKNode *Right) {
  SDKNodeType::diagnose(Right);
  auto *RT = dyn_cast<SDKNodeTypeFunc>(Right);
  if (!RT || !shouldDiagnoseType(this))
    return;
  assert(isTopLevelType());
  auto Loc = RT->getParent()->getAs<SDKNodeDecl>()->getLoc();
  if (Ctx.checkingABI() && isEscaping() != RT->isEscaping()) {
    getParent()->getAs<SDKNodeDecl>()->emitDiag(Loc,
                                                diag::func_type_escaping_changed,
                                                getTypeRoleDescription(),
                                                isEscaping());
  }
}
