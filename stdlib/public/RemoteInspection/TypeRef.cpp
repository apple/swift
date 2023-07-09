//===--- TypeRef.cpp - Swift Type References for Reflection ---------------===//
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
// Implements the structures of type references for property and enum
// case reflection.
//
//===----------------------------------------------------------------------===//

#if SWIFT_ENABLE_REFLECTION

#include "swift/Basic/Range.h"
#include "swift/Demangling/Demangle.h"
#include "swift/RemoteInspection/TypeRef.h"
#include "swift/RemoteInspection/TypeRefBuilder.h"
#include <iostream>

using namespace swift;
using namespace reflection;

class PrintTypeRef : public TypeRefVisitor<PrintTypeRef, void> {
  std::ostream &stream;
  unsigned Indent;

  std::ostream &indent(unsigned Amount) {
    for (unsigned i = 0; i < Amount; ++i)
      stream << " ";
    return stream;
  }

  std::ostream &printHeader(std::string Name) {
    indent(Indent) << "(" << Name;
    return stream;
  }

  std::ostream &printField(std::string name, std::string value) {
    if (!name.empty())
      stream << " " << name << "=" << value;
    else
      stream << " " << value;
    return stream;
  }

  void printRec(const TypeRef *typeRef) {
    stream << "\n";

    Indent += 2;
    visit(typeRef);
    Indent -= 2;
  }

public:
  PrintTypeRef(std::ostream &stream, unsigned Indent)
      : stream(stream), Indent(Indent) {}

  void visitBuiltinTypeRef(const BuiltinTypeRef *B) {
    printHeader("builtin");
    auto demangled = Demangle::demangleTypeAsString(B->getMangledName());
    printField("", demangled);
    stream << ")";
  }

  void visitNominalTypeRef(const NominalTypeRef *N) {
    StringRef mangledName = N->getMangledName();
    if (N->isStruct())
      printHeader("struct");
    else if (N->isEnum())
      printHeader("enum");
    else if (N->isClass())
      printHeader("class");
    else if (N->isProtocol()) {
      printHeader("protocol");
      mangledName = Demangle::dropSwiftManglingPrefix(mangledName);
    } else if (N->isAlias())
      printHeader("alias");
    else
      printHeader("nominal");
    auto demangled = Demangle::demangleTypeAsString(mangledName);
    printField("", demangled);
    if (auto parent = N->getParent())
      printRec(parent);
    stream << ")";
  }

  void visitBoundGenericTypeRef(const BoundGenericTypeRef *BG) {
    if (BG->isStruct())
      printHeader("bound_generic_struct");
    else if (BG->isEnum())
      printHeader("bound_generic_enum");
    else if (BG->isClass())
      printHeader("bound_generic_class");
    else
      printHeader("bound_generic");

    auto demangled = Demangle::demangleTypeAsString(BG->getMangledName());
    printField("", demangled);
    for (auto param : BG->getGenericParams())
      printRec(param);
    if (auto parent = BG->getParent())
      printRec(parent);
    stream << ")";
  }

  void visitTupleTypeRef(const TupleTypeRef *T) {
    printHeader("tuple");

    auto Labels = T->getLabels();
    for (auto NameElement : llvm::zip_first(Labels, T->getElements())) {
      auto Label = std::get<0>(NameElement);
      if (!Label.empty())
        stream << Label << " = ";
      printRec(std::get<1>(NameElement));
    }
    stream << ")";
  }

  void visitFunctionTypeRef(const FunctionTypeRef *F) {
    printHeader("function");

    switch (F->getFlags().getConvention()) {
    case FunctionMetadataConvention::Swift:
      break;
    case FunctionMetadataConvention::Block:
      printField("convention", "block");
      break;
    case FunctionMetadataConvention::Thin:
      printField("convention", "thin");
      break;
    case FunctionMetadataConvention::CFunctionPointer:
      printField("convention", "c");
      break;
    }

    switch (F->getDifferentiabilityKind().Value) {
    case FunctionMetadataDifferentiabilityKind::NonDifferentiable:
      break;

    case FunctionMetadataDifferentiabilityKind::Forward:
      printField("differentiable", "forward");
      break;

    case FunctionMetadataDifferentiabilityKind::Reverse:
      printField("differentiable", "reverse");
      break;

    case FunctionMetadataDifferentiabilityKind::Normal:
      printField("differentiable", "normal");
      break;

    case FunctionMetadataDifferentiabilityKind::Linear:
      printField("differentiable", "linear");
      break;
    }

    if (auto globalActor = F->getGlobalActor()) {
      stream << "\n";
      Indent += 2;
      printHeader("global-actor");
      {
        Indent += 2;
        printRec(globalActor);
        stream << ")";
        Indent -= 2;
      }
      Indent += 2;
    }

    stream << "\n";
    Indent += 2;
    printHeader("parameters");

    auto &parameters = F->getParameters();
    for (const auto &param : parameters) {
      auto flags = param.getFlags();

      if (!flags.isNone()) {
        Indent += 2;
        stream << "\n";
      }

      switch (flags.getValueOwnership()) {
      case ValueOwnership::Default:
        /* nothing */
        break;
      case ValueOwnership::InOut:
        printHeader("inout");
        break;
      case ValueOwnership::Shared:
        printHeader("shared");
        break;
      case ValueOwnership::Owned:
        printHeader("owned");
        break;
      }

      if (flags.isIsolated())
        printHeader("isolated");

      if (flags.isVariadic())
        printHeader("variadic");

      printRec(param.getType());

      if (!flags.isNone()) {
        Indent -= 2;
        stream << ")";
      }
    }

    if (parameters.empty())
      stream << ")";

    stream << "\n";
    printHeader("result");
    printRec(F->getResult());
    stream << ")";

    Indent -= 2;
  }

  void visitProtocolCompositionTypeRef(const ProtocolCompositionTypeRef *PC) {
    printHeader("protocol_composition");
    if (PC->hasExplicitAnyObject())
      stream << " any_object";
    if (auto superclass = PC->getSuperclass())
      printRec(superclass);
    for (auto protocol : PC->getProtocols())
      printRec(protocol);
    stream << ")";
  }

  void
  visitConstrainedExistentialTypeRef(const ConstrainedExistentialTypeRef *CET) {
    printHeader("constrained_existential_type");
    printRec(CET->getBase());
    for (auto req : CET->getRequirements())
      visitTypeRefRequirement(req);
    stream << ")";
  }

  void visitMetatypeTypeRef(const MetatypeTypeRef *M) {
    printHeader("metatype");
    if (M->wasAbstract())
      printField("", "was_abstract");
    printRec(M->getInstanceType());
    stream << ")";
  }

  void visitExistentialMetatypeTypeRef(const ExistentialMetatypeTypeRef *EM) {
    printHeader("existential_metatype");
    printRec(EM->getInstanceType());
    stream << ")";
  }

  void
  visitGenericTypeParameterTypeRef(const GenericTypeParameterTypeRef *GTP) {
    printHeader("generic_type_parameter");
    printField("depth", std::to_string(GTP->getDepth()));
    printField("index", std::to_string(GTP->getIndex()));
    stream << ")";
  }

  void visitDependentMemberTypeRef(const DependentMemberTypeRef *DM) {
    printHeader("dependent_member");
    printField("protocol", DM->getProtocol());
    printRec(DM->getBase());
    printField("member", DM->getMember());
    stream << ")";
  }

  void visitForeignClassTypeRef(const ForeignClassTypeRef *F) {
    printHeader("foreign");
    if (!F->getName().empty())
      printField("name", F->getName());
    stream << ")";
  }

  void visitObjCClassTypeRef(const ObjCClassTypeRef *OC) {
    printHeader("objective_c_class");
    if (!OC->getName().empty())
      printField("name", OC->getName());
    stream << ")";
  }

  void visitObjCProtocolTypeRef(const ObjCProtocolTypeRef *OC) {
    printHeader("objective_c_protocol");
    if (!OC->getName().empty())
      printField("name", OC->getName());
    stream << ")";
  }

#define REF_STORAGE(Name, name, ...)                                           \
  void visit##Name##StorageTypeRef(const Name##StorageTypeRef *US) {           \
    printHeader(#name "_storage");                                             \
    printRec(US->getType());                                                   \
    stream << ")";                                                             \
  }
#include "swift/AST/ReferenceStorage.def"

  void visitSILBoxTypeRef(const SILBoxTypeRef *SB) {
    printHeader("sil_box");
    printRec(SB->getBoxedType());
    stream << ")";
  }

  void visitTypeRefRequirement(const TypeRefRequirement &req) {
    printHeader("requirement ");
    switch (req.getKind()) {
    case RequirementKind::SameShape:
      printRec(req.getFirstType());
      stream << ".shape == ";
      printRec(req.getSecondType());
      stream << ".shape";
      break;
    case RequirementKind::Conformance:
    case RequirementKind::Superclass:
      printRec(req.getFirstType());
      stream << " : ";
      printRec(req.getSecondType());
      break;
    case RequirementKind::SameType:
      printRec(req.getFirstType());
      stream << " == ";
      printRec(req.getSecondType());
      break;
    case RequirementKind::Layout:
      stream << "layout requirement";
      break;
    }
    stream << ")";
  }

  void visitSILBoxTypeWithLayoutTypeRef(const SILBoxTypeWithLayoutTypeRef *SB) {
    printHeader("sil_box_with_layout\n");
    Indent += 2;
    printHeader("layout\n");
    Indent += 2;
    for (auto &f : SB->getFields()) {
      printHeader(f.isMutable() ? "var" : "let");
      printRec(f.getType());
      stream << ")";
    }
    Indent -= 2;
    stream << ")\n";
    printHeader("generic_signature\n");
    Indent += 2;
    for (auto &subst : SB->getSubstitutions()) {
      printHeader("substitution");
      printRec(subst.first);
      printRec(subst.second);
      stream << ")";
    }
    Indent -= 2;
    for (auto &req : SB->getRequirements()) {
      visitTypeRefRequirement(req);
    }
    stream << ")";
    stream << ")";
  }

  void visitOpaqueArchetypeTypeRef(const OpaqueArchetypeTypeRef *O) {
    printHeader("opaque_archetype");
    printField("id", O->getID().str());
    printField("description", O->getDescription().str());
    stream << " ordinal " << O->getOrdinal() << " ";
    for (auto argList : O->getArgumentLists()) {
      stream << "\n";
      indent(Indent + 2) << "args: <";
      for (auto arg : argList) {
        printRec(arg);
      }
      stream << ">";
    }
    stream << ")";
  }

  void visitOpaqueTypeRef(const OpaqueTypeRef *O) {
    printHeader("opaque");
    stream << ")";
  }
};

struct TypeRefIsConcrete
  : public TypeRefVisitor<TypeRefIsConcrete, bool> {
  const GenericArgumentMap &Subs;

  TypeRefIsConcrete(const GenericArgumentMap &Subs) : Subs(Subs) {}

  bool visitBuiltinTypeRef(const BuiltinTypeRef *B) {
    return true;
  }

  bool visitNominalTypeRef(const NominalTypeRef *N) {
    if (N->getParent())
      return visit(N->getParent());
    return true;
  }

  bool visitBoundGenericTypeRef(const BoundGenericTypeRef *BG) {
    if (BG->getParent())
      if (!visit(BG->getParent()))
        return false;
    for (auto Param : BG->getGenericParams())
      if (!visit(Param))
        return false;
    return true;
  }

  bool visitTupleTypeRef(const TupleTypeRef *T) {
    for (auto Element : T->getElements()) {
      if (!visit(Element))
        return false;
    }
    return true;
  }

  bool visitFunctionTypeRef(const FunctionTypeRef *F) {
    for (const auto &Param : F->getParameters())
      if (!visit(Param.getType()))
        return false;
    return visit(F->getResult());
  }

  bool
  visitProtocolCompositionTypeRef(const ProtocolCompositionTypeRef *PC) {
    for (auto Protocol : PC->getProtocols())
      if (!visit(Protocol))
        return false;
    if (auto Superclass = PC->getSuperclass())
      if (!visit(Superclass))
        return false;
    return true;
  }

  bool
  visitConstrainedExistentialTypeRef(const ConstrainedExistentialTypeRef *CET) {
    return visit(CET->getBase());
  }

  bool visitMetatypeTypeRef(const MetatypeTypeRef *M) {
    return visit(M->getInstanceType());
  }

  bool
  visitExistentialMetatypeTypeRef(const ExistentialMetatypeTypeRef *EM) {
    return visit(EM->getInstanceType());
  }

  bool
  visitGenericTypeParameterTypeRef(const GenericTypeParameterTypeRef *GTP) {
    return Subs.find({GTP->getDepth(), GTP->getIndex()}) != Subs.end();
  }

  bool
  visitDependentMemberTypeRef(const DependentMemberTypeRef *DM) {
    return visit(DM->getBase());
  }

  bool visitForeignClassTypeRef(const ForeignClassTypeRef *F) {
    return true;
  }

  bool visitObjCClassTypeRef(const ObjCClassTypeRef *OC) {
    return true;
  }

  bool visitObjCProtocolTypeRef(const ObjCProtocolTypeRef *OC) {
    return true;
  }
  
  bool visitOpaqueTypeRef(const OpaqueTypeRef *O) {
    return true;
  }
    
  bool visitOpaqueArchetypeTypeRef(const OpaqueArchetypeTypeRef *O) {
    return false;
  }

#define REF_STORAGE(Name, name, ...) \
  bool visit##Name##StorageTypeRef(const Name##StorageTypeRef *US) { \
    return visit(US->getType()); \
  }
#include "swift/AST/ReferenceStorage.def"

  bool visitSILBoxTypeRef(const SILBoxTypeRef *SB) {
    return visit(SB->getBoxedType());
  }

  bool visitSILBoxTypeWithLayoutTypeRef(const SILBoxTypeWithLayoutTypeRef *SB) {
    return true;
  }
};

const OpaqueTypeRef *
OpaqueTypeRef::Singleton = new OpaqueTypeRef();

const OpaqueTypeRef *OpaqueTypeRef::get() {
  return Singleton;
}

void TypeRef::dump() const { dump(std::cerr); }

void TypeRef::dump(std::ostream &stream, unsigned Indent) const {
  PrintTypeRef(stream, Indent).visit(this);
  stream << "\n";
}

class DemanglingForTypeRef
    : public TypeRefVisitor<DemanglingForTypeRef, Demangle::NodePointer> {
  Demangle::Demangler &Dem;

  /// Demangle a type and dive into the outermost Type node.
  Demangle::NodePointer demangleAndUnwrapType(llvm::StringRef mangledName) {
    auto node = Dem.demangleType(mangledName);
    if (node && node->getKind() == Node::Kind::Type && node->getNumChildren())
      node = node->getFirstChild();
    return node;
  }

public:
  DemanglingForTypeRef(Demangle::Demangler &Dem) : Dem(Dem) {}

  Demangle::NodePointer visit(const TypeRef *typeRef) {
    auto node = TypeRefVisitor<DemanglingForTypeRef,
                                Demangle::NodePointer>::visit(typeRef);

    // Wrap all nodes in a Type node, as consumers generally expect.
    auto typeNode = Dem.createNode(Node::Kind::Type);
    typeNode->addChild(node, Dem);
    return typeNode;
  }

  Demangle::NodePointer visitBuiltinTypeRef(const BuiltinTypeRef *B) {
    return demangleAndUnwrapType(B->getMangledName());
  }

  Demangle::NodePointer visitNominalTypeRef(const NominalTypeRef *N) {
    auto node = demangleAndUnwrapType(N->getMangledName());
    if (!node || node->getNumChildren() != 2)
      return node;

    auto parent = N->getParent();
    if (!parent)
      return node;

    // Swap in the richer parent that is stored in the NominalTypeRef
    // instead of what is encoded in the mangled name. The mangled name's
    // context has been "unspecialized" by NodeBuilder.
    auto parentNode = visit(parent);
    if (!parentNode)
      return node;
    if (parentNode->getKind() == Node::Kind::Type &&
        parentNode->getNumChildren())
      parentNode = parentNode->getFirstChild();

    auto contextualizedNode = Dem.createNode(node->getKind());
    contextualizedNode->addChild(parentNode, Dem);
    contextualizedNode->addChild(node->getChild(1), Dem);
    return contextualizedNode;
  }

  Demangle::NodePointer
  visitBoundGenericTypeRef(const BoundGenericTypeRef *BG) {
    Node::Kind genericNodeKind;
    if (BG->isStruct()) {
      genericNodeKind = Node::Kind::BoundGenericStructure;
    } else if (BG->isEnum()) {
      genericNodeKind = Node::Kind::BoundGenericEnum;
    } else if (BG->isClass()) {
      genericNodeKind = Node::Kind::BoundGenericClass;
    } else {
      genericNodeKind = Node::Kind::BoundGenericOtherNominalType;
    }
    auto unspecializedType = Dem.demangleType(BG->getMangledName());

    auto genericArgsList = Dem.createNode(Node::Kind::TypeList);
    for (auto param : BG->getGenericParams())
      genericArgsList->addChild(visit(param), Dem);

    auto genericNode = Dem.createNode(genericNodeKind);
    genericNode->addChild(unspecializedType, Dem);
    genericNode->addChild(genericArgsList, Dem);

    auto parent = BG->getParent();
    if (!parent)
      return genericNode;

    auto parentNode = visit(parent);
    if (!parentNode || !parentNode->hasChildren() ||
        parentNode->getKind() != Node::Kind::Type ||
        !unspecializedType->hasChildren())
      return genericNode;

    // Peel off the "Type" node.
    parentNode = parentNode->getFirstChild();

    auto nominalNode = unspecializedType->getFirstChild();

    if (nominalNode->getNumChildren() != 2)
      return genericNode;

    // Save identifier for reinsertion later, we have to remove it
    // so we can insert the parent node as the first child.
    auto identifierNode = nominalNode->getLastChild();

    // Remove all children.
    nominalNode->removeChildAt(1);
    nominalNode->removeChildAt(0);

    // Add the parent we just visited back in, followed by the identifier.
    nominalNode->addChild(parentNode, Dem);
    nominalNode->addChild(identifierNode, Dem);

    return genericNode;
  }

  Demangle::NodePointer visitTupleTypeRef(const TupleTypeRef *T) {
    auto tuple = Dem.createNode(Node::Kind::Tuple);

    auto Labels = T->getLabels();
    for (auto LabelElement : llvm::zip(Labels, T->getElements())) {
      auto tupleElt = Dem.createNode(Node::Kind::TupleElement);
      auto Label = std::get<0>(LabelElement);
      if (!Label.empty()) {
        auto name = Dem.createNode(Node::Kind::TupleElementName, Label);
        tupleElt->addChild(name, Dem);
      }
      tupleElt->addChild(visit(std::get<1>(LabelElement)), Dem);
      tuple->addChild(tupleElt, Dem);
    }
    return tuple;
  }

  Demangle::NodePointer visitFunctionTypeRef(const FunctionTypeRef *F) {
    Node::Kind kind;
    switch (F->getFlags().getConvention()) {
    case FunctionMetadataConvention::Swift:
      kind = !F->getFlags().isEscaping() ? Node::Kind::NoEscapeFunctionType
                                         : Node::Kind::FunctionType;
      break;
    case FunctionMetadataConvention::Block:
      kind = Node::Kind::ObjCBlock;
      break;
    case FunctionMetadataConvention::Thin:
      kind = Node::Kind::ThinFunctionType;
      break;
    case FunctionMetadataConvention::CFunctionPointer:
      kind = Node::Kind::CFunctionPointer;
      break;
    }

    llvm::SmallVector<std::pair<NodePointer, bool>, 8> inputs;
    for (const auto &param : F->getParameters()) {
      auto flags = param.getFlags();
      auto input = visit(param.getType());

      auto wrapInput = [&](Node::Kind kind) {
        auto parent = Dem.createNode(kind);
        parent->addChild(input, Dem);
        input = parent;
      };
      if (flags.isNoDerivative()) {
        wrapInput(Node::Kind::NoDerivative);
      }
      switch (flags.getValueOwnership()) {
      case ValueOwnership::Default:
        /* nothing */
        break;
      case ValueOwnership::InOut:
        wrapInput(Node::Kind::InOut);
        break;
      case ValueOwnership::Shared:
        wrapInput(Node::Kind::Shared);
        break;
      case ValueOwnership::Owned:
        wrapInput(Node::Kind::Owned);
        break;
      }
      if (flags.isIsolated()) {
        wrapInput(Node::Kind::Isolated);
      }

      inputs.push_back({input, flags.isVariadic()});
    }
    NodePointer totalInput = nullptr;
    // FIXME: this is copy&paste from Demangle.cpp
    switch (inputs.size()) {
    case 1: {
      auto singleParam = inputs.front();

      // If the sole unlabeled parameter has a non-tuple type, encode
      // the parameter list as a single type.
      if (!singleParam.second) {
        auto singleType = singleParam.first;
        if (singleType->getKind() == Node::Kind::Type)
          singleType = singleType->getFirstChild();
        if (singleType->getKind() != Node::Kind::Tuple) {
          totalInput = singleParam.first;
          break;
        }
      }

      // Otherwise it requires a tuple wrapper.
      SWIFT_FALLTHROUGH;
    }

    // This covers both none and multiple parameters.
    default:
      auto tuple = Dem.createNode(Node::Kind::Tuple);
      for (auto &input : inputs) {
        NodePointer eltType;
        bool isVariadic;
        std::tie(eltType, isVariadic) = input;

        // Tuple element := variadic-marker label? type
        auto tupleElt = Dem.createNode(Node::Kind::TupleElement);

        if (isVariadic)
          tupleElt->addChild(Dem.createNode(Node::Kind::VariadicMarker), Dem);

        if (eltType->getKind() == Node::Kind::Type) {
          tupleElt->addChild(eltType, Dem);
        } else {
          auto type = Dem.createNode(Node::Kind::Type);
          type->addChild(eltType, Dem);
          tupleElt->addChild(type, Dem);
        }

        tuple->addChild(tupleElt, Dem);
      }
      totalInput = tuple;
      break;
    }

    NodePointer parameters = Dem.createNode(Node::Kind::ArgumentTuple);
    NodePointer paramType = Dem.createNode(Node::Kind::Type);

    paramType->addChild(totalInput, Dem);
    parameters->addChild(paramType, Dem);

    NodePointer resultTy = visit(F->getResult());
    NodePointer result = Dem.createNode(Node::Kind::ReturnType);
    result->addChild(resultTy, Dem);

    auto funcNode = Dem.createNode(kind);
    if (auto globalActor = F->getGlobalActor()) {
      auto node = Dem.createNode(Node::Kind::GlobalActorFunctionType);
      auto globalActorNode = visit(globalActor);
      node->addChild(globalActorNode, Dem);
      funcNode->addChild(node, Dem);
    }

    if (F->getFlags().isDifferentiable()) {
      MangledDifferentiabilityKind mangledKind;
      switch (F->getDifferentiabilityKind().Value) {
#define CASE(X) case FunctionMetadataDifferentiabilityKind::X: \
        mangledKind = MangledDifferentiabilityKind::X; break;

      CASE(NonDifferentiable)
      CASE(Forward)
      CASE(Reverse)
      CASE(Normal)
      CASE(Linear)
#undef CASE
      }

      funcNode->addChild(
          Dem.createNode(
            Node::Kind::DifferentiableFunctionType,
            (Node::IndexType)mangledKind),
          Dem);
    }

    if (F->getFlags().isThrowing())
      funcNode->addChild(Dem.createNode(Node::Kind::ThrowsAnnotation), Dem);
    if (F->getFlags().isSendable()) {
      funcNode->addChild(
          Dem.createNode(Node::Kind::ConcurrentFunctionType), Dem);
    }
    if (F->getFlags().isAsync())
      funcNode->addChild(Dem.createNode(Node::Kind::AsyncAnnotation), Dem);
    funcNode->addChild(parameters, Dem);
    funcNode->addChild(result, Dem);
    return funcNode;
  }

  Demangle::NodePointer
  visitProtocolCompositionTypeRef(const ProtocolCompositionTypeRef *PC) {
    auto type_list = Dem.createNode(Node::Kind::TypeList);
    for (auto protocol : PC->getProtocols())
      type_list->addChild(visit(protocol), Dem);

    auto proto_list = Dem.createNode(Node::Kind::ProtocolList);
    proto_list->addChild(type_list, Dem);

    auto node = proto_list;
    if (auto superclass = PC->getSuperclass()) {
      node = Dem.createNode(Node::Kind::ProtocolListWithClass);
      node->addChild(proto_list, Dem);
      node->addChild(visit(superclass), Dem);
    } else if (PC->hasExplicitAnyObject()) {
      node = Dem.createNode(Node::Kind::ProtocolListWithAnyObject);
      node->addChild(proto_list, Dem);
    }
    return node;
  }

  Demangle::NodePointer
  visitConstrainedExistentialTypeRef(const ConstrainedExistentialTypeRef *CET) {
    auto node = Dem.createNode(Node::Kind::ConstrainedExistential);
    node->addChild(visit(CET->getBase()), Dem);
    auto constraintList =
        Dem.createNode(Node::Kind::ConstrainedExistentialRequirementList);
    for (auto req : CET->getRequirements())
      constraintList->addChild(visitTypeRefRequirement(req), Dem);
    node->addChild(constraintList, Dem);
    return node;
  }

  Demangle::NodePointer visitMetatypeTypeRef(const MetatypeTypeRef *M) {
    auto node = Dem.createNode(Node::Kind::Metatype);
    // FIXME: This is lossy. @objc_metatype is also abstract.
    auto repr = Dem.createNode(Node::Kind::MetatypeRepresentation,
                               M->wasAbstract() ? "@thick" : "@thin");
    node->addChild(repr, Dem);
    node->addChild(visit(M->getInstanceType()), Dem);
    return node;
  }

  Demangle::NodePointer
  visitExistentialMetatypeTypeRef(const ExistentialMetatypeTypeRef *EM) {
    auto node = Dem.createNode(Node::Kind::Metatype);
    node->addChild(visit(EM->getInstanceType()), Dem);
    return node;
  }

  Demangle::NodePointer
  visitGenericTypeParameterTypeRef(const GenericTypeParameterTypeRef *GTP) {
    auto node = Dem.createNode(Node::Kind::DependentGenericParamType);
    node->addChild(Dem.createNode(Node::Kind::Index, GTP->getDepth()), Dem);
    node->addChild(Dem.createNode(Node::Kind::Index, GTP->getIndex()), Dem);
    return node;
  }

  Demangle::NodePointer
  visitDependentMemberTypeRef(const DependentMemberTypeRef *DM) {

    auto node = Dem.createNode(Node::Kind::DependentMemberType);
    auto Base = visit(DM->getBase());
    node->addChild(Base, Dem);

    auto MemberId = Dem.createNode(Node::Kind::Identifier, DM->getMember());

    auto MangledProtocol = DM->getProtocol();
    if (MangledProtocol.empty()) {
      // If there's no protocol, add the Member as an Identifier node
      node->addChild(MemberId, Dem);
    } else {
      // Otherwise, build up a DependentAssociatedTR node with
      // the member Identifer and protocol
      auto AssocTy = Dem.createNode(Node::Kind::DependentAssociatedTypeRef);
      AssocTy->addChild(MemberId, Dem);
      auto Proto = Dem.demangleType(MangledProtocol);
      assert(Proto && "Failed to demangle");
      assert(Proto->getKind() == Node::Kind::Type && "Protocol type is not a type?!");
      AssocTy->addChild(Proto, Dem);
      node->addChild(AssocTy, Dem);
    }
    return node;
  }

  Demangle::NodePointer visitForeignClassTypeRef(const ForeignClassTypeRef *F) {
    return demangleAndUnwrapType(F->getName());
  }

  Demangle::NodePointer visitObjCClassTypeRef(const ObjCClassTypeRef *OC) {
    auto module = Dem.createNode(Node::Kind::Module, MANGLING_MODULE_OBJC);
    auto node = Dem.createNode(Node::Kind::Class);
    node->addChild(module, Dem);
    node->addChild(Dem.createNode(Node::Kind::Identifier, OC->getName()), Dem);
    return node;
  }

  Demangle::NodePointer
  visitObjCProtocolTypeRef(const ObjCProtocolTypeRef *OC) {
    auto module = Dem.createNode(Node::Kind::Module, MANGLING_MODULE_OBJC);
    auto node = Dem.createNode(Node::Kind::Protocol);
    node->addChild(module, Dem);
    node->addChild(Dem.createNode(Node::Kind::Identifier, OC->getName()), Dem);
    return node;
  }

#define REF_STORAGE(Name, name, ...)                                           \
  Demangle::NodePointer visit##Name##StorageTypeRef(                           \
      const Name##StorageTypeRef *US) {                                        \
    auto node = Dem.createNode(Node::Kind::Name);                              \
    node->addChild(visit(US->getType()), Dem);                                 \
    return node;                                                               \
  }
#include "swift/AST/ReferenceStorage.def"

  Demangle::NodePointer visitSILBoxTypeRef(const SILBoxTypeRef *SB) {
    auto node = Dem.createNode(Node::Kind::SILBoxType);
    node->addChild(visit(SB->getBoxedType()), Dem);
    return node;
  }

  Demangle::NodePointer visitTypeRefRequirement(const TypeRefRequirement &req) {
    switch (req.getKind()) {
    case RequirementKind::SameShape: {
      // Not implemented.
      return nullptr;
    }
    case RequirementKind::Conformance: {
      auto r = Dem.createNode(Node::Kind::DependentGenericConformanceRequirement);
      r->addChild(visit(req.getFirstType()), Dem);
      r->addChild(visit(req.getSecondType()), Dem);
      return r;
    }
    case RequirementKind::Superclass: {
      auto r = Dem.createNode(Node::Kind::DependentGenericConformanceRequirement);
      r->addChild(visit(req.getFirstType()), Dem);
      r->addChild(visit(req.getSecondType()), Dem);
      return r;
    }
    case RequirementKind::SameType: {
      auto r = Dem.createNode(Node::Kind::DependentGenericSameTypeRequirement);
      r->addChild(visit(req.getFirstType()), Dem);
      r->addChild(visit(req.getSecondType()), Dem);
      return r;
    }
    case RequirementKind::Layout:
      // Not implemented.
      return nullptr;
    }
  }

  Demangle::NodePointer
  visitSILBoxTypeWithLayoutTypeRef(const SILBoxTypeWithLayoutTypeRef *SB) {
    auto node = Dem.createNode(Node::Kind::SILBoxTypeWithLayout);
    auto layout = Dem.createNode(Node::Kind::SILBoxLayout);
    for (auto &f : SB->getFields()) {
      auto field =
          Dem.createNode(f.isMutable() ? Node::Kind::SILBoxMutableField
                                       : Node::Kind::SILBoxImmutableField);
      field->addChild(visit(f.getType()), Dem);
      layout->addChild(field, Dem);
    }
    node->addChild(layout, Dem);

    auto signature = Dem.createNode(Node::Kind::DependentGenericSignature);
    auto addCount = [&](unsigned count) {
      signature->addChild(
          Dem.createNode(Node::Kind::DependentGenericParamCount, count), Dem);
    };
    unsigned depth = 0;
    unsigned index = 0;
    for (auto &s : SB->getSubstitutions())
      if (auto *param = dyn_cast<GenericTypeParameterTypeRef>(s.first)) {
        while (param->getDepth() > depth) {
          addCount(index);
          ++depth, index = 0;
        }
        assert(index == param->getIndex() && "generic params out of order");
        ++index;
      }
    for (auto &req : SB->getRequirements()) {
      auto *r = visitTypeRefRequirement(req);
      if (!r)
        continue;
      signature->addChild(r, Dem);
    }
    node->addChild(signature, Dem);
    auto list = Dem.createNode(Node::Kind::TypeList);
    for (auto &subst : SB->getSubstitutions())
      list->addChild(visit(subst.second), Dem);
    node->addChild(list, Dem);
    return node;
  }

  Demangle::NodePointer visitOpaqueTypeRef(const OpaqueTypeRef *O) {
    return Dem.createNode(Node::Kind::OpaqueType);
  }
      
  Demangle::NodePointer visitOpaqueArchetypeTypeRef(const OpaqueArchetypeTypeRef *O) {
    auto decl = Dem.demangleType(O->getID());
    if (!decl)
      return nullptr;
    
    auto index = Dem.createNode(Node::Kind::Index, O->getOrdinal());
    
    auto argNodeLists = Dem.createNode(Node::Kind::TypeList);
    for (auto argList : O->getArgumentLists()) {
      auto argNodeList = Dem.createNode(Node::Kind::TypeList);
      
      for (auto arg : argList) {
        auto argNode = visit(arg);
        if (!argNode)
          return nullptr;
        
        argNodeList->addChild(argNode, Dem);
      }
      
      argNodeLists->addChild(argNodeList, Dem);
    }
    
    auto node = Dem.createNode(Node::Kind::OpaqueType);
    node->addChild(decl, Dem);
    node->addChild(index, Dem);
    node->addChild(argNodeLists, Dem);
    
    return node;
  }
};

Demangle::NodePointer TypeRef::getDemangling(Demangle::Demangler &Dem) const {
  return DemanglingForTypeRef(Dem).visit(this);
}

bool TypeRef::isConcrete() const {
  GenericArgumentMap Subs;
  return TypeRefIsConcrete(Subs).visit(this);
}

bool TypeRef::isConcreteAfterSubstitutions(
    const GenericArgumentMap &Subs) const {
  return TypeRefIsConcrete(Subs).visit(this);
}

unsigned NominalTypeTrait::getDepth() const {
  if (auto P = Parent) {
    switch (P->getKind()) {
    case TypeRefKind::Nominal:
      return 1 + cast<NominalTypeRef>(P)->getDepth();
    case TypeRefKind::BoundGeneric:
      return 1 + cast<BoundGenericTypeRef>(P)->getDepth();
    default:
      break;
    }
  }
  return 0;
}

llvm::Optional<GenericArgumentMap> TypeRef::getSubstMap() const {
  GenericArgumentMap Substitutions;
  switch (getKind()) {
    case TypeRefKind::Nominal: {
      auto Nom = cast<NominalTypeRef>(this);
      if (auto Parent = Nom->getParent())
        return Parent->getSubstMap();
      return GenericArgumentMap();
    }
    case TypeRefKind::BoundGeneric: {
      auto BG = cast<BoundGenericTypeRef>(this);
      auto Depth = BG->getDepth();
      unsigned Index = 0;
      for (auto Param : BG->getGenericParams()) {
        if (!Param->isConcrete())
          return llvm::None;
        Substitutions.insert({{Depth, Index++}, Param});
      }
      if (auto Parent = BG->getParent()) {
        auto ParentSubs = Parent->getSubstMap();
        if (!ParentSubs)
          return llvm::None;
        Substitutions.insert(ParentSubs->begin(), ParentSubs->end());
      }
      break;
    }
    default:
      break;
  }
  return Substitutions;
}

bool NominalTypeTrait::isStruct() const {
  return Demangle::isStruct(MangledName);
}

bool NominalTypeTrait::isEnum() const { return Demangle::isEnum(MangledName); }

bool NominalTypeTrait::isClass() const {
  return Demangle::isClass(MangledName);
}

bool NominalTypeTrait::isProtocol() const {
  return Demangle::isProtocol(MangledName);
}

bool NominalTypeTrait::isAlias() const {
  return Demangle::isAlias(MangledName);
}

/// Visitor class to set the WasAbstract flag of any MetatypeTypeRefs
/// contained in the given type.
class ThickenMetatype
  : public TypeRefVisitor<ThickenMetatype, const TypeRef *> {
  TypeRefBuilder &Builder;
public:
  using TypeRefVisitor<ThickenMetatype, const TypeRef *>::visit;

  ThickenMetatype(TypeRefBuilder &Builder) : Builder(Builder) {}

  const TypeRef *visitBuiltinTypeRef(const BuiltinTypeRef *B) {
    return B;
  }

  const TypeRef *visitNominalTypeRef(const NominalTypeRef *N) {
    return N;
  }

  const TypeRef *visitBoundGenericTypeRef(const BoundGenericTypeRef *BG) {
    std::vector<const TypeRef *> GenericParams;
    for (auto Param : BG->getGenericParams())
      GenericParams.push_back(visit(Param));
    auto parent = BG->getParent();
    if (parent) {
      parent = ThickenMetatype(Builder).visit(parent);
    }
    return BoundGenericTypeRef::create(Builder, BG->getMangledName(),
                                       GenericParams, parent);
  }

  const TypeRef *visitTupleTypeRef(const TupleTypeRef *T) {
    std::vector<const TypeRef *> Elements;
    for (auto Element : T->getElements())
      Elements.push_back(visit(Element));
    auto Labels = T->getLabels();
    return TupleTypeRef::create(Builder, Elements, Labels);
  }

  const TypeRef *visitFunctionTypeRef(const FunctionTypeRef *F) {
    std::vector<remote::FunctionParam<const TypeRef *>> SubstitutedParams;
    for (const auto &Param : F->getParameters()) {
      auto typeRef = Param.getType();
      SubstitutedParams.push_back(Param.withType(visit(typeRef)));
    }

    const TypeRef *globalActorType = nullptr;
    if (F->getGlobalActor())
      globalActorType = visit(F->getGlobalActor());

    auto SubstitutedResult = visit(F->getResult());

    return FunctionTypeRef::create(Builder, SubstitutedParams,
                                   SubstitutedResult, F->getFlags(),
                                   F->getDifferentiabilityKind(),
                                   globalActorType);
  }

  const TypeRef *
  visitProtocolCompositionTypeRef(const ProtocolCompositionTypeRef *PC) {
    return PC;
  }

  const TypeRef *
  visitConstrainedExistentialTypeRef(const ConstrainedExistentialTypeRef *CET) {
    return ConstrainedExistentialTypeRef::create(Builder, CET->getBase(),
                                                 CET->getRequirements());
  }

  const TypeRef *visitMetatypeTypeRef(const MetatypeTypeRef *M) {
    return MetatypeTypeRef::create(Builder, visit(M->getInstanceType()),
                                   /*WasAbstract=*/true);
  }

  const TypeRef *
  visitExistentialMetatypeTypeRef(const ExistentialMetatypeTypeRef *EM) {
    return EM;
  }

  const TypeRef *
  visitGenericTypeParameterTypeRef(const GenericTypeParameterTypeRef *GTP) {
    return GTP;
  }

  const TypeRef *visitDependentMemberTypeRef(const DependentMemberTypeRef *DM) {
    return DM;
  }

  const TypeRef *visitForeignClassTypeRef(const ForeignClassTypeRef *F) {
    return F;
  }

  const TypeRef *visitObjCClassTypeRef(const ObjCClassTypeRef *OC) {
    return OC;
  }

  const TypeRef *visitObjCProtocolTypeRef(const ObjCProtocolTypeRef *OP) {
    return OP;
  }

#define REF_STORAGE(Name, name, ...) \
  const TypeRef *visit##Name##StorageTypeRef(const Name##StorageTypeRef *US) { \
    return US; \
  }
#include "swift/AST/ReferenceStorage.def"

  const TypeRef *visitSILBoxTypeRef(const SILBoxTypeRef *SB) {
    return SILBoxTypeRef::create(Builder, visit(SB->getBoxedType()));
  }

  const TypeRef *
  visitSILBoxTypeWithLayoutTypeRef(const SILBoxTypeWithLayoutTypeRef *SB) {
    return SB;
  }

  const TypeRef *visitOpaqueTypeRef(const OpaqueTypeRef *O) {
    return O;
  }

  const TypeRef *visitOpaqueArchetypeTypeRef(const OpaqueArchetypeTypeRef *O) {
    return O;
  }

};

static const TypeRef *
thickenMetatypes(TypeRefBuilder &Builder, const TypeRef *TR) {
  return ThickenMetatype(Builder).visit(TR);
}

class TypeRefSubstitution
  : public TypeRefVisitor<TypeRefSubstitution, const TypeRef *> {
  TypeRefBuilder &Builder;
  GenericArgumentMap Substitutions;
  // Set true iff the Substitution map was actually used
  bool DidSubstitute;
public:
  using TypeRefVisitor<TypeRefSubstitution, const TypeRef *>::visit;

  TypeRefSubstitution(TypeRefBuilder &Builder, GenericArgumentMap Substitutions)
      : Builder(Builder), Substitutions(Substitutions), DidSubstitute(false) {}

  bool didSubstitute() const { return DidSubstitute; }

  const TypeRef *visitBuiltinTypeRef(const BuiltinTypeRef *B) {
    return B;
  }

  const TypeRef *visitNominalTypeRef(const NominalTypeRef *N) {
    if (N->getParent())
      return NominalTypeRef::create(Builder, N->getMangledName(),
                                    visit(N->getParent()));
    return N;
  }

  const TypeRef *visitBoundGenericTypeRef(const BoundGenericTypeRef *BG) {
    auto *Parent = BG->getParent();
    if (Parent != nullptr)
      Parent = visit(Parent);
    std::vector<const TypeRef *> GenericParams;
    for (auto Param : BG->getGenericParams())
      GenericParams.push_back(visit(Param));
    return BoundGenericTypeRef::create(Builder, BG->getMangledName(),
                                       GenericParams, Parent);
  }

  const TypeRef *visitTupleTypeRef(const TupleTypeRef *T) {
    std::vector<const TypeRef *> Elements;
    for (auto Element : T->getElements())
      Elements.push_back(visit(Element));
    auto Labels = T->getLabels();
    return TupleTypeRef::create(Builder, Elements, Labels);
  }

  const TypeRef *visitFunctionTypeRef(const FunctionTypeRef *F) {
    std::vector<remote::FunctionParam<const TypeRef *>> SubstitutedParams;
    for (const auto &Param : F->getParameters()) {
      auto typeRef = Param.getType();
      SubstitutedParams.push_back(Param.withType(visit(typeRef)));
    }

    auto SubstitutedResult = visit(F->getResult());

    const TypeRef *globalActorType = nullptr;
    if (F->getGlobalActor())
      globalActorType = visit(F->getGlobalActor());

    return FunctionTypeRef::create(Builder, SubstitutedParams,
                                   SubstitutedResult, F->getFlags(),
                                   F->getDifferentiabilityKind(),
                                   globalActorType);
  }

  const TypeRef *
  visitProtocolCompositionTypeRef(const ProtocolCompositionTypeRef *PC) {
    return PC;
  }

  const TypeRef *visitMetatypeTypeRef(const MetatypeTypeRef *M) {
    // If the metatype's instance type does not contain any type parameters,
    // substitution does not alter anything, and the empty representation
    // can still be used.
    if (M->isConcrete())
      return M;

    // When substituting a concrete type into a type parameter inside
    // of a metatype's instance type, (eg; T.Type, T := C), we must
    // represent the metatype at runtime as a value, even if the
    // metatype naturally has an empty representation.
    return MetatypeTypeRef::create(Builder, visit(M->getInstanceType()),
                                   /*WasAbstract=*/true);
  }

  const TypeRef *
  visitExistentialMetatypeTypeRef(const ExistentialMetatypeTypeRef *EM) {
    // Existential metatypes do not contain type parameters.
    assert(EM->getInstanceType()->isConcrete());
    return EM;
  }

  llvm::Optional<TypeRefRequirement>
  visitTypeRefRequirement(const TypeRefRequirement &req) {
    auto newFirst = visit(req.getFirstType());
    if (!newFirst)
      return llvm::None;

    switch (req.getKind()) {
    case RequirementKind::SameShape:
    case RequirementKind::Conformance:
    case RequirementKind::Superclass:
    case RequirementKind::SameType: {
      auto newSecond = visit(req.getFirstType());
      if (!newSecond)
        return llvm::None;
      return TypeRefRequirement(req.getKind(), newFirst, newSecond);
    }
    case RequirementKind::Layout:
      return TypeRefRequirement(req.getKind(), newFirst,
                                req.getLayoutConstraint());
    }

    llvm_unreachable("Unhandled RequirementKind in switch.");
  }

  const TypeRef *
  visitConstrainedExistentialTypeRef(const ConstrainedExistentialTypeRef *CET) {
    std::vector<TypeRefRequirement> constraints;
    for (auto Req : CET->getRequirements()) {
      auto substReq = visitTypeRefRequirement(Req);
      if (!substReq)
        continue;
      constraints.emplace_back(*substReq);
    }
    return ConstrainedExistentialTypeRef::create(Builder, CET->getBase(),
                                                 constraints);
  }

  const TypeRef *
  visitGenericTypeParameterTypeRef(const GenericTypeParameterTypeRef *GTP) {
    auto found = Substitutions.find({GTP->getDepth(), GTP->getIndex()});
    if (found == Substitutions.end())
      return GTP;
    assert(found->second->isConcrete());
    DidSubstitute = true; // We actually used the Substitutions

    // When substituting a concrete type containing a metatype into a
    // type parameter, (eg: T, T := C.Type), we must also represent
    // the metatype as a value.
    return thickenMetatypes(Builder, found->second);
  }

  const TypeRef *visitDependentMemberTypeRef(const DependentMemberTypeRef *DM) {
    // Substitute type parameters in the base type to get a fully concrete
    // type.
    auto SubstBase = visit(DM->getBase());

    const TypeRef *TypeWitness = nullptr;

    while (TypeWitness == nullptr) {
      auto &Member = DM->getMember();
      const auto &Protocol = DM->getProtocol();

      // Get the original type of the witness from the conformance.
      if (auto *Nominal = dyn_cast<NominalTypeRef>(SubstBase)) {
        TypeWitness = Builder.lookupTypeWitness(Nominal->getMangledName(),
                                                Member, Protocol);
      } else if (auto *BG = dyn_cast<BoundGenericTypeRef>(SubstBase)) {
        TypeWitness = Builder.lookupTypeWitness(BG->getMangledName(),
                                                Member, Protocol);
      }

      if (TypeWitness != nullptr)
        break;

      // If we didn't find the member type, check the superclass.
      auto *Superclass = Builder.lookupSuperclass(SubstBase);
      if (Superclass == nullptr)
        break;

      SubstBase = Superclass;
    }

    auto Protocol = std::make_pair(DM->getProtocol(), false);

    // We didn't find the member type, so return something to let the
    // caller know we're dealing with incomplete metadata.
    if (TypeWitness == nullptr)
      return Builder.createDependentMemberType(DM->getMember(),
                                               SubstBase,
                                               Protocol);

    // Likewise if we can't get the substitution map.
    auto SubstMap = SubstBase->getSubstMap();
    if (!SubstMap)
      return Builder.createDependentMemberType(DM->getMember(),
                                               SubstBase,
                                               Protocol);

    // Apply base type substitutions to get the fully-substituted nested type.
    auto *Subst = TypeWitness->subst(Builder, *SubstMap);

    // Same as above.
    return thickenMetatypes(Builder, Subst);
  }

  const TypeRef *visitForeignClassTypeRef(const ForeignClassTypeRef *F) {
    return F;
  }

  const TypeRef *visitObjCClassTypeRef(const ObjCClassTypeRef *OC) {
    return OC;
  }

  const TypeRef *visitObjCProtocolTypeRef(const ObjCProtocolTypeRef *OP) {
    return OP;
  }

#define REF_STORAGE(Name, name, ...) \
  const TypeRef *visit##Name##StorageTypeRef(const Name##StorageTypeRef *US) { \
    return Name##StorageTypeRef::create(Builder, visit(US->getType())); \
  }
#include "swift/AST/ReferenceStorage.def"

  const TypeRef *visitSILBoxTypeRef(const SILBoxTypeRef *SB) {
    return SILBoxTypeRef::create(Builder, visit(SB->getBoxedType()));
  }

  const TypeRef *
  visitSILBoxTypeWithLayoutTypeRef(const SILBoxTypeWithLayoutTypeRef *SB) {
    return SB;
  }

  const TypeRef *visitOpaqueTypeRef(const OpaqueTypeRef *O) { return O; }

  const TypeRef *visitOpaqueArchetypeTypeRef(const OpaqueArchetypeTypeRef *O) {
    std::vector<const TypeRef *> newArgsBuffer;
    for (auto argList : O->getArgumentLists()) {
      for (auto arg : argList) {
        newArgsBuffer.push_back(visit(arg));
      }
    }

    std::vector<llvm::ArrayRef<const TypeRef *>> newArgLists;

    return OpaqueArchetypeTypeRef::create(Builder, O->getID(), O->getDescription(),
                                          O->getOrdinal(),
                                          newArgLists);
  }
};

const TypeRef *TypeRef::subst(TypeRefBuilder &Builder,
                              const GenericArgumentMap &Subs) const {
  return TypeRefSubstitution(Builder, Subs).visit(this);
}

const TypeRef *TypeRef::subst(TypeRefBuilder &Builder,
                              const GenericArgumentMap &Subs,
			      bool &DidSubstitute) const {
  auto subst = TypeRefSubstitution(Builder, Subs);
  auto TR = subst.visit(this);
  DidSubstitute = subst.didSubstitute();
  return TR;
}

bool TypeRef::deriveSubstitutions(GenericArgumentMap &Subs,
                                  const TypeRef *OrigTR,
                                  const TypeRef *SubstTR) {

  // Walk into parent types of concrete nominal types.
  if (auto *O = dyn_cast<NominalTypeRef>(OrigTR)) {
    if (auto *S = dyn_cast<NominalTypeRef>(SubstTR)) {
      if (!!O->getParent() != !!S->getParent() ||
          O->getMangledName() != S->getMangledName())
        return false;

      if (O->getParent() &&
          !deriveSubstitutions(Subs,
                               O->getParent(),
                               S->getParent()))
        return false;

      return true;
    }
  }

  // Decompose arguments of bound generic types in parallel.
  if (auto *O = dyn_cast<BoundGenericTypeRef>(OrigTR)) {
    if (auto *S = dyn_cast<BoundGenericTypeRef>(SubstTR)) {
      if (!!O->getParent() != !!S->getParent() ||
          O->getMangledName() != S->getMangledName() ||
          O->getGenericParams().size() != S->getGenericParams().size())
        return false;

      if (O->getParent() &&
          !deriveSubstitutions(Subs,
                               O->getParent(),
                               S->getParent()))
        return false;

      for (unsigned i = 0, e = O->getGenericParams().size(); i < e; ++i) {
        if (!deriveSubstitutions(Subs,
                                 O->getGenericParams()[i],
                                 S->getGenericParams()[i]))
          return false;
      }

      return true;
    }
  }

  // Decompose tuple element types in parallel.
  if (auto *O = dyn_cast<TupleTypeRef>(OrigTR)) {
    if (auto *S = dyn_cast<TupleTypeRef>(SubstTR)) {
      if (O->getElements().size() != S->getElements().size())
        return false;

      for (unsigned i = 0, e = O->getElements().size(); i < e; ++i) {
        if (!deriveSubstitutions(Subs,
                                 O->getElements()[i],
                                 S->getElements()[i]))
          return false;
      }

      return true;
    }
  }

  // Decompose parameter and result types in parallel.
  if (auto *O = dyn_cast<FunctionTypeRef>(OrigTR)) {
    if (auto *S = dyn_cast<FunctionTypeRef>(SubstTR)) {
      auto oParams = O->getParameters();
      auto sParams = S->getParameters();

      if (oParams.size() != sParams.size())
        return false;

      for (auto index : indices(oParams)) {
        if (!deriveSubstitutions(Subs, oParams[index].getType(),
                                 sParams[index].getType()))
          return false;
      }

      if (!deriveSubstitutions(Subs,
                               O->getResult(),
                               S->getResult()))
        return false;

      return true;
    }
  }

  // Walk down into the instance type.
  if (auto *O = dyn_cast<MetatypeTypeRef>(OrigTR)) {
    if (auto *S = dyn_cast<MetatypeTypeRef>(SubstTR)) {

      if (!deriveSubstitutions(Subs,
                               O->getInstanceType(),
                               S->getInstanceType()))
        return false;

      return true;
    }
  }

  // Walk down into the referent storage type.
  if (auto *O = dyn_cast<ReferenceStorageTypeRef>(OrigTR)) {
    if (auto *S = dyn_cast<ReferenceStorageTypeRef>(SubstTR)) {

      if (O->getKind() != S->getKind())
        return false;

      if (!deriveSubstitutions(Subs,
                               O->getType(),
                               S->getType()))
        return false;

      return true;
    }
  }

  if (isa<DependentMemberTypeRef>(OrigTR)) {
    // FIXME: Do some validation here?
    return true;
  }

  // If the original type is a generic type parameter, just make
  // sure the substituted type matches anything we've already
  // seen.
  if (auto *O = dyn_cast<GenericTypeParameterTypeRef>(OrigTR)) {
    DepthAndIndex key = {O->getDepth(), O->getIndex()};
    auto found = Subs.find(key);
    if (found == Subs.end()) {
      Subs[key] = SubstTR;
      return true;
    }

    return (found->second == SubstTR);
  }

  // Anything else must be concrete and the two types must match
  // exactly.
  return (OrigTR == SubstTR);
}

#endif
