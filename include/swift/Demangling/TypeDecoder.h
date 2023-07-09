//===--- TypeDecoder.h - Decode mangled type names --------------*- C++ -*-===//
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
// This file provides facilities to \c TypeDecoder, which decodes a mangled
// type name into a structured type representation.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_DEMANGLING_TYPEDECODER_H
#define SWIFT_DEMANGLING_TYPEDECODER_H

#include "TypeLookupError.h"
#include "swift/Basic/LLVM.h"

#include "swift/ABI/MetadataValues.h"
#include "swift/AST/LayoutConstraintKind.h"
#include "swift/AST/RequirementKind.h"
#include "swift/Basic/Unreachable.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Demangling/NamespaceMacros.h"
#include "swift/Runtime/Portability.h"
#include "swift/Strings.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/StringSwitch.h"
#include <vector>

namespace swift {
namespace Demangle {
SWIFT_BEGIN_INLINE_NAMESPACE

enum class ImplMetatypeRepresentation {
  Thin,
  Thick,
  ObjC,
};

/// Describe a function parameter, parameterized on the type
/// representation.
template <typename BuiltType>
class FunctionParam {
  StringRef Label;
  BuiltType Type;
  ParameterFlags Flags;

  FunctionParam(StringRef label, BuiltType type, ParameterFlags flags)
      : Label(label), Type(type), Flags(flags) {}

public:
  explicit FunctionParam() {}

  FunctionParam(BuiltType type) : Type(type) {}

  StringRef getLabel() const { return Label; }
  BuiltType getType() const { return Type; }
  ParameterFlags getFlags() const { return Flags; }

  void setLabel(StringRef label) { Label = label; }
  void setType(BuiltType type) { Type = type; }

  void setVariadic() { Flags = Flags.withVariadic(true); }
  void setAutoClosure() { Flags = Flags.withAutoClosure(true); }
  void setValueOwnership(ValueOwnership ownership) {
    Flags = Flags.withValueOwnership(ownership);
  }
  void setNoDerivative() { Flags = Flags.withNoDerivative(true); }
  void setIsolated() { Flags = Flags.withIsolated(true); }
  void setFlags(ParameterFlags flags) { Flags = flags; };

  FunctionParam withLabel(StringRef label) const {
    return FunctionParam(label, Type, Flags);
  }

  FunctionParam withType(BuiltType type) const {
    return FunctionParam(Label, type, Flags);
  }

  FunctionParam withFlags(ParameterFlags flags) const {
    return FunctionParam(Label, Type, flags);
  }
};

enum class ImplParameterConvention {
  Indirect_In,
  Indirect_In_Constant,
  Indirect_In_Guaranteed,
  Indirect_Inout,
  Indirect_InoutAliasable,
  Direct_Owned,
  Direct_Unowned,
  Direct_Guaranteed,
  Pack_Owned,
  Pack_Guaranteed,
  Pack_Inout,
};

enum class ImplParameterDifferentiability {
  DifferentiableOrNotApplicable,
  NotDifferentiable
};

/// Describe a lowered function parameter, parameterized on the type
/// representation.
template <typename BuiltType>
class ImplFunctionParam {
  BuiltType Type;
  ImplParameterConvention Convention;
  ImplParameterDifferentiability Differentiability;

public:
  using ConventionType = ImplParameterConvention;
  using DifferentiabilityType = ImplParameterDifferentiability;

  static llvm::Optional<ConventionType>
  getConventionFromString(StringRef conventionString) {
    if (conventionString == "@in")
      return ConventionType::Indirect_In;
    if (conventionString == "@in_constant")
      return ConventionType::Indirect_In_Constant;
    if (conventionString == "@in_guaranteed")
      return ConventionType::Indirect_In_Guaranteed;
    if (conventionString == "@inout")
      return ConventionType::Indirect_Inout;
    if (conventionString == "@inout_aliasable")
      return ConventionType::Indirect_InoutAliasable;
    if (conventionString == "@owned")
      return ConventionType::Direct_Owned;
    if (conventionString == "@unowned")
      return ConventionType::Direct_Unowned;
    if (conventionString == "@guaranteed")
      return ConventionType::Direct_Guaranteed;
    if (conventionString == "@pack_owned")
      return ConventionType::Pack_Owned;
    if (conventionString == "@pack_guaranteed")
      return ConventionType::Pack_Guaranteed;
    if (conventionString == "@pack_inout")
      return ConventionType::Pack_Inout;

    return llvm::None;
  }

  static llvm::Optional<DifferentiabilityType>
  getDifferentiabilityFromString(StringRef string) {
    if (string.empty())
      return DifferentiabilityType::DifferentiableOrNotApplicable;
    if (string == "@noDerivative")
      return DifferentiabilityType::NotDifferentiable;
    return llvm::None;
  }

  ImplFunctionParam(BuiltType type, ImplParameterConvention convention,
                    ImplParameterDifferentiability diffKind)
      : Type(type), Convention(convention), Differentiability(diffKind) {}

  ImplParameterConvention getConvention() const { return Convention; }

  ImplParameterDifferentiability getDifferentiability() const {
    return Differentiability;
  }

  BuiltType getType() const { return Type; }
};

enum class ImplResultConvention {
  Indirect,
  Owned,
  Unowned,
  UnownedInnerPointer,
  Autoreleased,
  Pack,
};

enum class ImplResultDifferentiability {
  DifferentiableOrNotApplicable,
  NotDifferentiable
};

/// Describe a lowered function result, parameterized on the type
/// representation.
template <typename BuiltType>
class ImplFunctionResult {
  BuiltType Type;
  ImplResultConvention Convention;
  ImplResultDifferentiability Differentiability;

public:
  using ConventionType = ImplResultConvention;
  using DifferentiabilityType = ImplResultDifferentiability;

  static llvm::Optional<ConventionType>
  getConventionFromString(StringRef conventionString) {
    if (conventionString == "@out")
      return ConventionType::Indirect;
    if (conventionString == "@owned")
      return ConventionType::Owned;
    if (conventionString == "@unowned")
      return ConventionType::Unowned;
    if (conventionString == "@unowned_inner_pointer")
      return ConventionType::UnownedInnerPointer;
    if (conventionString == "@autoreleased")
      return ConventionType::Autoreleased;
    if (conventionString == "@pack_out")
      return ConventionType::Pack;

    return llvm::None;
  }

  static llvm::Optional<DifferentiabilityType>
  getDifferentiabilityFromString(StringRef string) {
    if (string.empty())
      return DifferentiabilityType::DifferentiableOrNotApplicable;
    if (string == "@noDerivative")
      return DifferentiabilityType::NotDifferentiable;
    return llvm::None;
  }

  ImplFunctionResult(
      BuiltType type, ImplResultConvention convention,
      ImplResultDifferentiability diffKind =
          ImplResultDifferentiability::DifferentiableOrNotApplicable)
      : Type(type), Convention(convention), Differentiability(diffKind) {}

  ImplResultConvention getConvention() const { return Convention; }

  ImplResultDifferentiability getDifferentiability() const {
    return Differentiability;
  }

  BuiltType getType() const { return Type; }
};

enum class ImplFunctionRepresentation {
  Thick,
  Block,
  Thin,
  CFunctionPointer,
  Method,
  ObjCMethod,
  WitnessMethod,
  Closure
};

enum class ImplFunctionDifferentiabilityKind {
  NonDifferentiable,
  Forward,
  Reverse,
  Normal,
  Linear,
};

class ImplFunctionTypeFlags {
  unsigned Rep : 3;
  unsigned Pseudogeneric : 1;
  unsigned Escaping : 1;
  unsigned Concurrent : 1;
  unsigned Async : 1;
  unsigned DifferentiabilityKind : 3;

public:
  ImplFunctionTypeFlags()
      : Rep(0), Pseudogeneric(0), Escaping(0), Concurrent(0), Async(0),
        DifferentiabilityKind(0) {}

  ImplFunctionTypeFlags(ImplFunctionRepresentation rep, bool pseudogeneric,
                        bool noescape, bool concurrent, bool async,
                        ImplFunctionDifferentiabilityKind diffKind)
      : Rep(unsigned(rep)), Pseudogeneric(pseudogeneric), Escaping(noescape),
        Concurrent(concurrent), Async(async),
        DifferentiabilityKind(unsigned(diffKind)) {}

  ImplFunctionTypeFlags
  withRepresentation(ImplFunctionRepresentation rep) const {
    return ImplFunctionTypeFlags(
        rep, Pseudogeneric, Escaping, Concurrent, Async,
        ImplFunctionDifferentiabilityKind(DifferentiabilityKind));
  }

  ImplFunctionTypeFlags
  withConcurrent() const {
    return ImplFunctionTypeFlags(
        ImplFunctionRepresentation(Rep), Pseudogeneric, Escaping, true,
        Async, ImplFunctionDifferentiabilityKind(DifferentiabilityKind));
  }

  ImplFunctionTypeFlags
  withAsync() const {
    return ImplFunctionTypeFlags(
        ImplFunctionRepresentation(Rep), Pseudogeneric, Escaping, Concurrent,
        true, ImplFunctionDifferentiabilityKind(DifferentiabilityKind));
  }

  ImplFunctionTypeFlags
  withEscaping() const {
    return ImplFunctionTypeFlags(
        ImplFunctionRepresentation(Rep), Pseudogeneric, true, Concurrent, Async,
        ImplFunctionDifferentiabilityKind(DifferentiabilityKind));
  }
  
  ImplFunctionTypeFlags
  withPseudogeneric() const {
    return ImplFunctionTypeFlags(
        ImplFunctionRepresentation(Rep), true, Escaping, Concurrent, Async,
        ImplFunctionDifferentiabilityKind(DifferentiabilityKind));
  }

  ImplFunctionTypeFlags
  withDifferentiabilityKind(ImplFunctionDifferentiabilityKind diffKind) const {
    return ImplFunctionTypeFlags(ImplFunctionRepresentation(Rep), Pseudogeneric,
                                 Escaping, Concurrent, Async, diffKind);
  }

  ImplFunctionRepresentation getRepresentation() const {
    return ImplFunctionRepresentation(Rep);
  }

  bool isAsync() const { return Async; }

  bool isEscaping() const { return Escaping; }

  bool isSendable() const { return Concurrent; }

  bool isPseudogeneric() const { return Pseudogeneric; }

  bool isDifferentiable() const {
    return getDifferentiabilityKind() !=
        ImplFunctionDifferentiabilityKind::NonDifferentiable;
  }

  ImplFunctionDifferentiabilityKind getDifferentiabilityKind() const {
    return ImplFunctionDifferentiabilityKind(DifferentiabilityKind);
  }
};

#if SWIFT_OBJC_INTEROP
/// For a mangled node that refers to an Objective-C class or protocol,
/// return the class or protocol name.
static inline llvm::Optional<StringRef>
getObjCClassOrProtocolName(NodePointer node) {
  if (node->getKind() != Demangle::Node::Kind::Class &&
      node->getKind() != Demangle::Node::Kind::Protocol)
    return llvm::None;

  if (node->getNumChildren() != 2)
    return llvm::None;

  // Check whether we have the __ObjC module.
  auto moduleNode = node->getChild(0);
  if (moduleNode->getKind() != Demangle::Node::Kind::Module ||
      moduleNode->getText() != MANGLING_MODULE_OBJC)
    return llvm::None;

  // Check whether we have an identifier.
  auto nameNode = node->getChild(1);
  if (nameNode->getKind() != Demangle::Node::Kind::Identifier)
    return llvm::None;

  return nameNode->getText();
}
#endif

template <typename BuiltType, typename BuiltRequirement,
          typename BuiltLayoutConstraint, typename BuilderType>
void decodeRequirement(NodePointer node,
                       llvm::SmallVectorImpl<BuiltRequirement> &requirements,
                       BuilderType &Builder) {
  for (auto &child : *node) {
    if (child->getKind() == Demangle::Node::Kind::DependentGenericParamCount)
      continue;

    if (child->getNumChildren() != 2)
      return;
    auto subjectType = Builder.decodeMangledType(child->getChild(0));
    if (!subjectType)
      return;

    BuiltType constraintType;
    if (child->getKind() ==
        Demangle::Node::Kind::DependentGenericConformanceRequirement) {
      constraintType = Builder.decodeMangledType(child->getChild(1));
      if (!constraintType)
        return;
    } else if (child->getKind() ==
               Demangle::Node::Kind::DependentGenericSameTypeRequirement) {
      constraintType = Builder.decodeMangledType(
          child->getChild(1), /*forRequirement=*/false);
      if (!constraintType)
        return;
    }

    switch (child->getKind()) {
    case Demangle::Node::Kind::DependentGenericConformanceRequirement: {
      requirements.push_back(BuiltRequirement(
          Builder.isExistential(constraintType) ? RequirementKind::Conformance
                                                : RequirementKind::Superclass,
          subjectType, constraintType));
      break;
    }
    case Demangle::Node::Kind::DependentGenericSameTypeRequirement: {
      requirements.push_back(BuiltRequirement(RequirementKind::SameType,
                                              subjectType, constraintType));
      break;
    }
    case Demangle::Node::Kind::DependentGenericLayoutRequirement: {
      auto kindChild = child->getChild(1);
      if (kindChild->getKind() != Demangle::Node::Kind::Identifier)
        return;

      auto kind =
          llvm::StringSwitch<llvm::Optional<LayoutConstraintKind>>(
              kindChild->getText())
              .Case("U", LayoutConstraintKind::UnknownLayout)
              .Case("R", LayoutConstraintKind::RefCountedObject)
              .Case("N", LayoutConstraintKind::NativeRefCountedObject)
              .Case("C", LayoutConstraintKind::Class)
              .Case("D", LayoutConstraintKind::NativeClass)
              .Case("T", LayoutConstraintKind::Trivial)
              .Cases("E", "e", LayoutConstraintKind::TrivialOfExactSize)
              .Cases("M", "m", LayoutConstraintKind::TrivialOfAtMostSize)
              .Default(llvm::None);

      if (!kind)
        return;

      BuiltLayoutConstraint layout;

      if (kind != LayoutConstraintKind::TrivialOfExactSize &&
          kind != LayoutConstraintKind::TrivialOfAtMostSize) {
        layout = Builder.getLayoutConstraint(*kind);
      } else {
        auto size = child->getChild(2)->getIndex();
        auto alignment = 0;

        if (child->getNumChildren() == 4)
          alignment = child->getChild(3)->getIndex();

        layout =
            Builder.getLayoutConstraintWithSizeAlign(*kind, size, alignment);
      }

      requirements.push_back(BuiltRequirement(RequirementKind::Layout, subjectType, layout));
      break;
    }
    default:
      break;
    }
  }
}

#define MAKE_NODE_TYPE_ERROR(Node, Fmt, ...)                                   \
  TYPE_LOOKUP_ERROR_FMT("TypeDecoder.h:%u: Node kind %u \"%.*s\" - " Fmt,      \
                        __LINE__, (unsigned)Node->getKind(),                   \
                        Node->hasText() ? (int)Node->getText().size() : 0,     \
                        Node->hasText() ? Node->getText().data() : "",         \
                        __VA_ARGS__)

#define MAKE_NODE_TYPE_ERROR0(Node, Str) MAKE_NODE_TYPE_ERROR(Node, "%s", Str)

/// Decode a mangled type to construct an abstract type, forming such
/// types by invoking a custom builder.
template <typename BuilderType>
class TypeDecoder {
  using BuiltType = typename BuilderType::BuiltType;
  using BuiltTypeDecl = typename BuilderType::BuiltTypeDecl;
  using BuiltProtocolDecl = typename BuilderType::BuiltProtocolDecl;
  using Field = typename BuilderType::BuiltSILBoxField;
  using BuiltSubstitution = typename BuilderType::BuiltSubstitution;
  using BuiltRequirement = typename BuilderType::BuiltRequirement;
  using BuiltLayoutConstraint = typename BuilderType::BuiltLayoutConstraint;
  using BuiltGenericSignature = typename BuilderType::BuiltGenericSignature;
  using BuiltSubstitutionMap = typename BuilderType::BuiltSubstitutionMap;
  using NodeKind = Demangle::Node::Kind;

  BuilderType &Builder;

public:
  explicit TypeDecoder(BuilderType &Builder) : Builder(Builder) {}

  /// Given a demangle tree, attempt to turn it into a type.
  TypeLookupErrorOr<BuiltType> decodeMangledType(NodePointer Node,
                                                 bool forRequirement = true) {
    return decodeMangledType(Node, 0, forRequirement);
  }

protected:
  static const unsigned MaxDepth = 1024;

  TypeLookupErrorOr<BuiltType> decodeMangledType(NodePointer Node,
                                                 unsigned depth,
                                                 bool forRequirement = true) {
    if (depth > TypeDecoder::MaxDepth)
      return TypeLookupError("Mangled type is too complex");

    if (!Node)
      return TypeLookupError("Node is NULL");

    using NodeKind = Demangle::Node::Kind;
    switch (Node->getKind()) {
    case NodeKind::Global:
      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children.");

      return decodeMangledType(Node->getChild(0), depth + 1);
    case NodeKind::TypeMangling:
      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children.");

      return decodeMangledType(Node->getChild(0), depth + 1);
    case NodeKind::Type:
      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children.");

      return decodeMangledType(Node->getChild(0), depth + 1,
                               forRequirement);
    case NodeKind::Class:
    {
#if SWIFT_OBJC_INTEROP
      if (auto mangledName = getObjCClassOrProtocolName(Node))
        return Builder.createObjCClassType(mangledName->str());
#endif
      LLVM_FALLTHROUGH;
    }
    case NodeKind::Enum:
    case NodeKind::Structure:
    case NodeKind::TypeAlias:
    case NodeKind::TypeSymbolicReference:
    {
      BuiltTypeDecl typeDecl = BuiltTypeDecl();
      BuiltType parent = BuiltType();
      bool typeAlias = false;
      if (auto error =
              decodeMangledTypeDecl(Node, depth, typeDecl, parent, typeAlias))
        return *error;

      if (typeAlias)
        return Builder.createTypeAliasType(typeDecl, parent);

      return Builder.createNominalType(typeDecl, parent);
    }

    case NodeKind::BoundGenericEnum:
    case NodeKind::BoundGenericStructure:
    case NodeKind::BoundGenericClass:
    case NodeKind::BoundGenericTypeAlias:
    case NodeKind::BoundGenericOtherNominalType: {
      if (Node->getNumChildren() < 2)
        return MAKE_NODE_TYPE_ERROR(Node,
                                    "fewer children (%zu) than required (2)",
                                    Node->getNumChildren());

      llvm::SmallVector<BuiltType, 8> args;
      if (auto error = decodeGenericArgs(Node->getChild(1), depth+1, args))
        return *error;

      auto ChildNode = Node->getChild(0);
      if (ChildNode->getKind() == NodeKind::Type &&
          ChildNode->getNumChildren() > 0)
        ChildNode = ChildNode->getChild(0);

#if SWIFT_OBJC_INTEROP
      if (auto mangledName = getObjCClassOrProtocolName(ChildNode))
        return Builder.createBoundGenericObjCClassType(mangledName->str(),
                                                       args);
#endif

      BuiltTypeDecl typeDecl = BuiltTypeDecl();
      BuiltType parent = BuiltType();
      bool typeAlias = false;
      if (auto error = decodeMangledTypeDecl(ChildNode, depth, typeDecl, parent,
                                             typeAlias))
        return *error;

      return Builder.createBoundGenericType(typeDecl, args, parent);
    }
    case NodeKind::BoundGenericProtocol: {
      // This is a special case. When you write a protocol typealias with a
      // concrete type base, for example:
      //
      // protocol P { typealias A<T> = ... }
      // struct S : P {}
      // let x: S.A<Int> = ...
      //
      // The mangling tree looks like this:
      //
      // BoundGenericProtocol ---> BoundGenericTypeAlias
      // |                         |
      // |                         |
      // --> Protocol: P           --> TypeAlias: A
      // |                         |
      // --> TypeList:             --> TypeList:
      //     |                         |
      //     --> Structure: S          --> Structure: Int
      //
      // When resolving the mangling tree to a decl, we strip off the
      // BoundGenericProtocol's *argument*, leaving behind only the
      // protocol reference.
      //
      // But when resolving it to a type, we want to *keep* the argument
      // so that the parent type becomes 'S' and not 'P'.
      if (Node->getNumChildren() < 2)
        return MAKE_NODE_TYPE_ERROR(Node,
                                    "fewer children (%zu) than required (2)",
                                    Node->getNumChildren());

      const auto &genericArgs = Node->getChild(1);
      if (genericArgs->getNumChildren() != 1)
        return MAKE_NODE_TYPE_ERROR(genericArgs,
                                    "expected 1 generic argument, saw %zu",
                                    genericArgs->getNumChildren());

      return decodeMangledType(genericArgs->getChild(0), depth + 1);
    }
    case NodeKind::BuiltinTypeName: {
      auto mangling = Demangle::mangleNode(Node);
      if (!mangling.isSuccess()) {
        return MAKE_NODE_TYPE_ERROR(Node,
                                    "failed to mangle node (%d:%u)",
                                    mangling.error().code,
                                    mangling.error().line);
      }
      return Builder.createBuiltinType(Node->getText().str(), mangling.result());
    }
    case NodeKind::Metatype:
    case NodeKind::ExistentialMetatype: {
      unsigned i = 0;
      llvm::Optional<ImplMetatypeRepresentation> repr;

      // Handle lowered metatypes in a hackish way. If the representation
      // was not thin, force the resulting typeref to have a non-empty
      // representation.
      if (Node->getNumChildren() >= 2) {
        auto reprNode = Node->getChild(i++);
        if (reprNode->getKind() != NodeKind::MetatypeRepresentation ||
            !reprNode->hasText())
          return MAKE_NODE_TYPE_ERROR0(reprNode, "wrong node kind or no text");
        if (reprNode->getText() == "@thin")
          repr = ImplMetatypeRepresentation::Thin;
        else if (reprNode->getText() == "@thick")
          repr = ImplMetatypeRepresentation::Thick;
        else if (reprNode->getText() == "@objc_metatype")
          repr = ImplMetatypeRepresentation::ObjC;
      } else if (Node->getNumChildren() < 1) {
        return MAKE_NODE_TYPE_ERROR0(Node, "no children");
      }

      auto instance = decodeMangledType(Node->getChild(i), depth + 1);
      if (instance.isError())
        return instance;
      if (Node->getKind() == NodeKind::Metatype) {
        return Builder.createMetatypeType(instance.getType(), repr);
      } else if (Node->getKind() == NodeKind::ExistentialMetatype) {
        return Builder.createExistentialMetatypeType(instance.getType(), repr);
      } else {
        assert(false);
        return MAKE_NODE_TYPE_ERROR0(Node,
                                     "Metatype/ExistentialMetatype Node "
                                     "had a different kind when re-checked");
      }
    }
    case NodeKind::SymbolicExtendedExistentialType: {
      if (Node->getNumChildren() < 2)
        return MAKE_NODE_TYPE_ERROR0(Node, "not enough children");

      auto shapeNode = Node->getChild(0);
      llvm::SmallVector<BuiltType, 8> args;
      if (auto error = decodeGenericArgs(Node->getChild(1), depth + 1, args))
        return *error;

      return Builder.createSymbolicExtendedExistentialType(shapeNode, args);
    }

    case NodeKind::ProtocolList:
    case NodeKind::ProtocolListWithAnyObject:
    case NodeKind::ProtocolListWithClass: {
      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children");

      // Find the protocol list.
      llvm::SmallVector<BuiltProtocolDecl, 8> Protocols;
      auto TypeList = Node->getChild(0);
      if (TypeList->getKind() == NodeKind::ProtocolList &&
          TypeList->getNumChildren() >= 1) {
        TypeList = TypeList->getChild(0);
      }

      // Demangle the protocol list.
      for (auto componentType : *TypeList) {
        if (auto Protocol = decodeMangledProtocolType(componentType, depth + 1))
          Protocols.push_back(Protocol);
        else
          return MAKE_NODE_TYPE_ERROR0(componentType,
                                       "failed to decode protocol type");
      }

      // Superclass or AnyObject, if present.
      bool IsClassBound = false;
      auto Superclass = BuiltType();
      if (Node->getKind() == NodeKind::ProtocolListWithClass) {
        if (Node->getNumChildren() < 2)
          return MAKE_NODE_TYPE_ERROR(Node,
                                      "fewer children (%zu) than required (2)",
                                      Node->getNumChildren());

        auto superclassNode = Node->getChild(1);
        auto result = decodeMangledType(superclassNode, depth + 1);
        if (result.isError())
          return result;
        Superclass = result.getType();

        IsClassBound = true;
      } else if (Node->getKind() == NodeKind::ProtocolListWithAnyObject) {
        IsClassBound = true;
      }

      return Builder.createProtocolCompositionType(Protocols, Superclass,
                                                   IsClassBound,
                                                   forRequirement);
    }

    case NodeKind::ConstrainedExistential: {
      if (Node->getNumChildren() < 2)
        return MAKE_NODE_TYPE_ERROR(Node,
                                    "fewer children (%zu) than required (2)",
                                    Node->getNumChildren());

      auto protocolType = decodeMangledType(Node->getChild(0), depth + 1);
      if (protocolType.isError())
        return protocolType;

      llvm::SmallVector<BuiltRequirement, 8> requirements;

      auto *reqts = Node->getChild(1);
      if (reqts->getKind() != NodeKind::ConstrainedExistentialRequirementList)
        return MAKE_NODE_TYPE_ERROR0(reqts, "is not requirement list");

      decodeRequirement<BuiltType, BuiltRequirement, BuiltLayoutConstraint,
                        BuilderType>(reqts, requirements, Builder);

      return Builder.createConstrainedExistentialType(protocolType.getType(),
                                                      requirements);
    }
    case NodeKind::ConstrainedExistentialSelf:
      return Builder.createGenericTypeParameterType(/*depth*/ 0, /*index*/ 0);

    case NodeKind::Protocol:
    case NodeKind::ProtocolSymbolicReference: {
      if (auto Proto = decodeMangledProtocolType(Node, depth + 1)) {
        return Builder.createProtocolCompositionType(Proto, BuiltType(),
                                                     /*IsClassBound=*/false,
                                                     forRequirement);
      }

      return MAKE_NODE_TYPE_ERROR0(Node, "failed to decode protocol type");
    }
    case NodeKind::DynamicSelf: {
      if (Node->getNumChildren() != 1)
        return MAKE_NODE_TYPE_ERROR(Node, "expected 1 child, saw %zu",
                                    Node->getNumChildren());

      auto selfType = decodeMangledType(Node->getChild(0), depth + 1);
      if (selfType.isError())
        return selfType;

      return Builder.createDynamicSelfType(selfType.getType());
    }
    case NodeKind::DependentGenericParamType: {
      auto depth = Node->getChild(0)->getIndex();
      auto index = Node->getChild(1)->getIndex();
      return Builder.createGenericTypeParameterType(depth, index);
    }
    case NodeKind::EscapingObjCBlock:
    case NodeKind::ObjCBlock:
    case NodeKind::CFunctionPointer:
    case NodeKind::ThinFunctionType:
    case NodeKind::NoEscapeFunctionType:
    case NodeKind::AutoClosureType:
    case NodeKind::EscapingAutoClosureType:
    case NodeKind::FunctionType: {
      if (Node->getNumChildren() < 2)
        return MAKE_NODE_TYPE_ERROR(Node,
                                    "fewer children (%zu) than required (2)",
                                    Node->getNumChildren());

      FunctionTypeFlags flags;
      if (Node->getKind() == NodeKind::ObjCBlock ||
          Node->getKind() == NodeKind::EscapingObjCBlock) {
        flags = flags.withConvention(FunctionMetadataConvention::Block);
      } else if (Node->getKind() == NodeKind::CFunctionPointer) {
        flags =
          flags.withConvention(FunctionMetadataConvention::CFunctionPointer);
      } else if (Node->getKind() == NodeKind::ThinFunctionType) {
        flags = flags.withConvention(FunctionMetadataConvention::Thin);
      }

      unsigned firstChildIdx = 0;
      if (Node->getChild(firstChildIdx)->getKind() == NodeKind::ClangType) {
        // [TODO: synthesize-Clang-type-from-mangled-name] Use the first child
        // to create a ClangTypeInfo.
        ++firstChildIdx;
      }

      BuiltType globalActorType = BuiltType();
      if (Node->getChild(firstChildIdx)->getKind() ==
          NodeKind::GlobalActorFunctionType) {
        auto child = Node->getChild(firstChildIdx);
        if (child->getNumChildren() < 1) {
          return MAKE_NODE_TYPE_ERROR0(child,
                                       "Global actor node is missing child");
        }

        auto globalActorResult =
            decodeMangledType(child->getChild(0), depth + 1);
        if (globalActorResult.isError())
          return globalActorResult;

        globalActorType = globalActorResult.getType();
        ++firstChildIdx;
      }

      FunctionMetadataDifferentiabilityKind diffKind;
      if (Node->getChild(firstChildIdx)->getKind() ==
            NodeKind::DifferentiableFunctionType) {
        auto mangledDiffKind = (MangledDifferentiabilityKind)
            Node->getChild(firstChildIdx)->getIndex();
        switch (mangledDiffKind) {
        case MangledDifferentiabilityKind::NonDifferentiable:
          assert(false && "Unexpected case NonDifferentiable");
          break;
        case MangledDifferentiabilityKind::Forward:
          diffKind = FunctionMetadataDifferentiabilityKind::Forward;
          break;
        case MangledDifferentiabilityKind::Reverse:
          diffKind = FunctionMetadataDifferentiabilityKind::Reverse;
          break;
        case MangledDifferentiabilityKind::Normal:
          diffKind = FunctionMetadataDifferentiabilityKind::Normal;
          break;
        case MangledDifferentiabilityKind::Linear:
          diffKind = FunctionMetadataDifferentiabilityKind::Linear;
          break;
        }
        ++firstChildIdx;
      }

      bool isThrow = false;
      if (Node->getChild(firstChildIdx)->getKind()
            == NodeKind::ThrowsAnnotation) {
        isThrow = true;
        ++firstChildIdx;
      }

      bool isSendable = false;
      if (Node->getChild(firstChildIdx)->getKind()
            == NodeKind::ConcurrentFunctionType) {
        isSendable = true;
        ++firstChildIdx;
      }

      bool isAsync = false;
      if (Node->getChild(firstChildIdx)->getKind()
            == NodeKind::AsyncAnnotation) {
        isAsync = true;
        ++firstChildIdx;
      }

      flags = flags.withConcurrent(isSendable)
          .withAsync(isAsync).withThrows(isThrow)
          .withDifferentiable(diffKind.isDifferentiable());

      if (Node->getNumChildren() < firstChildIdx + 2)
        return MAKE_NODE_TYPE_ERROR(Node,
                                    "fewer children (%zu) than required (%u)",
                                    Node->getNumChildren(), firstChildIdx + 2);

      bool hasParamFlags = false;
      llvm::SmallVector<FunctionParam<BuiltType>, 8> parameters;
      auto optError = decodeMangledFunctionInputType(Node->getChild(firstChildIdx),
                                                     depth + 1, parameters, hasParamFlags);
      if (optError)
        return *optError;

      flags =
          flags.withNumParameters(parameters.size())
              .withParameterFlags(hasParamFlags)
              .withEscaping(
                          Node->getKind() == NodeKind::FunctionType ||
                          Node->getKind() == NodeKind::EscapingAutoClosureType ||
                          Node->getKind() == NodeKind::EscapingObjCBlock);

      auto result =
          decodeMangledType(Node->getChild(firstChildIdx + 1), depth + 1,
                            /*forRequirement=*/false);
      if (result.isError())
        return result;
      return Builder.createFunctionType(
          parameters, result.getType(), flags, diffKind, globalActorType);
    }
    case NodeKind::ImplFunctionType: {
      auto calleeConvention = ImplParameterConvention::Direct_Unowned;
      llvm::SmallVector<ImplFunctionParam<BuiltType>, 8> parameters;
      llvm::SmallVector<ImplFunctionResult<BuiltType>, 8> results;
      llvm::SmallVector<ImplFunctionResult<BuiltType>, 8> errorResults;
      ImplFunctionTypeFlags flags;

      for (unsigned i = 0; i < Node->getNumChildren(); i++) {
        auto child = Node->getChild(i);

        if (child->getKind() == NodeKind::ImplConvention) {
          if (!child->hasText())
            return MAKE_NODE_TYPE_ERROR0(child, "expected text");

          if (child->getText() == "@convention(thin)") {
            flags =
              flags.withRepresentation(ImplFunctionRepresentation::Thin);
          } else if (child->getText() == "@callee_guaranteed") {
            calleeConvention = ImplParameterConvention::Direct_Guaranteed;
          }
        } else if (child->getKind() == NodeKind::ImplFunctionConvention) {
          if (child->getNumChildren() == 0)
            return MAKE_NODE_TYPE_ERROR0(child, "expected grandchildren");
          if ((child->getFirstChild()->getKind() !=
               NodeKind::ImplFunctionConventionName) ||
              !child->getFirstChild()->hasText())
            return MAKE_NODE_TYPE_ERROR0(child, "expected convention name");

          // [TODO: synthesize-Clang-type-from-mangled-name] If there are two
          // grand-children, the second is going to be the mangled Clang type.
          StringRef text = child->getFirstChild()->getText();
          if (text == "c") {
            flags =
              flags.withRepresentation(ImplFunctionRepresentation::CFunctionPointer);
          } else if (text == "block") {
            flags =
              flags.withRepresentation(ImplFunctionRepresentation::Block);
          }
        } else if (child->getKind() == NodeKind::ImplFunctionAttribute) {
          if (!child->hasText())
            return MAKE_NODE_TYPE_ERROR0(child, "expected text");
          if (child->getText() == "@Sendable") {
            flags = flags.withConcurrent();
          } else if (child->getText() == "@async") {
            flags = flags.withAsync();
          }
        } else if (child->getKind() == NodeKind::ImplDifferentiabilityKind) {
          ImplFunctionDifferentiabilityKind implDiffKind;
          switch ((MangledDifferentiabilityKind)child->getIndex()) {
          #define SIMPLE_CASE(CASE) \
              case MangledDifferentiabilityKind::CASE: \
                implDiffKind = ImplFunctionDifferentiabilityKind::CASE; break;
          SIMPLE_CASE(NonDifferentiable)
          SIMPLE_CASE(Normal)
          SIMPLE_CASE(Linear)
          SIMPLE_CASE(Forward)
          SIMPLE_CASE(Reverse)
          #undef SIMPLE_CASE
          }
          flags = flags.withDifferentiabilityKind(implDiffKind);
        } else if (child->getKind() == NodeKind::ImplEscaping) {
          flags = flags.withEscaping();
        } else if (child->getKind() == NodeKind::ImplParameter) {
          if (decodeImplFunctionParam(child, depth + 1, parameters))
            return MAKE_NODE_TYPE_ERROR0(child,
                                         "failed to decode function parameter");
        } else if (child->getKind() == NodeKind::ImplResult) {
          if (decodeImplFunctionParam(child, depth + 1, results))
            return MAKE_NODE_TYPE_ERROR0(child,
                                         "failed to decode function parameter");
        } else if (child->getKind() == NodeKind::ImplErrorResult) {
          if (decodeImplFunctionPart(child, depth + 1, errorResults))
            return MAKE_NODE_TYPE_ERROR0(child,
                                         "failed to decode function part");
        } else {
          return MAKE_NODE_TYPE_ERROR0(child, "unexpected kind");
        }
      }

      llvm::Optional<ImplFunctionResult<BuiltType>> errorResult;
      switch (errorResults.size()) {
      case 0:
        break;
      case 1:
        errorResult = errorResults.front();
        break;
      default:
        return MAKE_NODE_TYPE_ERROR(Node, "got %zu errors",
                                    errorResults.size());
      }

      // TODO: Some cases not handled above, but *probably* they cannot
      // appear as the types of values in SIL (yet?):
      // - functions with yield returns
      // - functions with generic signatures
      // - foreign error conventions
      return Builder.createImplFunctionType(calleeConvention,
                                            parameters, results,
                                            errorResult, flags);
    }

    case NodeKind::ArgumentTuple:
      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children");

      return decodeMangledType(Node->getChild(0), depth + 1);

    case NodeKind::ReturnType:
      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children");

      return decodeMangledType(Node->getChild(0), depth + 1,
                               /*forRequirement=*/false);

    case NodeKind::Tuple: {
      llvm::SmallVector<BuiltType, 8> elements;
      llvm::SmallVector<StringRef, 8> labels;

      for (auto &element : *Node) {
        if (element->getKind() != NodeKind::TupleElement)
          return MAKE_NODE_TYPE_ERROR0(Node, "unexpected kind");

        // If the tuple element is labeled, add its label to 'labels'.
        unsigned typeChildIndex = 0;
        if (element->getChild(typeChildIndex)->getKind() == NodeKind::VariadicMarker) {
          return MAKE_NODE_TYPE_ERROR0(element->getChild(typeChildIndex),
                                       "no children");
        }

        StringRef label;
        if (element->getChild(typeChildIndex)->getKind() == NodeKind::TupleElementName) {
          label = element->getChild(typeChildIndex)->getText();
          typeChildIndex++;
        }

        // Decode the element type.
        auto optError = decodeTypeSequenceElement(
            element->getChild(typeChildIndex), depth + 1,
            [&](BuiltType type) {
              elements.push_back(type);
              labels.push_back(label);
            });
        if (optError)
          return *optError;
      }

      // Unwrap unlabeled one-element tuples.
      //
      // FIXME: The behavior of one-element labeled tuples is inconsistent throughout
      // the different re-implementations of type substitution and pack expansion.
      if (elements.size() == 1 && labels[0].empty())
        return elements[0];

      return Builder.createTupleType(elements, labels);
    }
    case NodeKind::TupleElement:
      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children");

      if (Node->getChild(0)->getKind() == NodeKind::TupleElementName) {
        if (Node->getNumChildren() < 2)
          return MAKE_NODE_TYPE_ERROR(Node,
                                      "fewer children (%zu) than required (2)",
                                      Node->getNumChildren());

        return decodeMangledType(Node->getChild(1), depth + 1,
                                 /*forRequirement=*/false);
      }
      return decodeMangledType(Node->getChild(0), depth + 1,
                               /*forRequirement=*/false);

    case NodeKind::Pack:
    case NodeKind::SILPackDirect:
    case NodeKind::SILPackIndirect: {
      llvm::SmallVector<BuiltType, 8> elements;

      for (auto &element : *Node) {
        // Decode the element type.
        auto optError = decodeTypeSequenceElement(
            element, depth + 1,
            [&](BuiltType elementType) {
              elements.push_back(elementType);
            });
        if (optError)
          return *optError;
      }

      switch (Node->getKind()) {
      case NodeKind::Pack:
        return Builder.createPackType(elements);
      case NodeKind::SILPackDirect:
        return Builder.createSILPackType(elements, /*isElementAddress=*/false);
      case NodeKind::SILPackIndirect:
        return Builder.createSILPackType(elements, /*isElementAddress=*/true);
      default:
        llvm_unreachable("Bad kind");
      }
    }

    case NodeKind::PackExpansion: {
      return MAKE_NODE_TYPE_ERROR0(Node,
                                   "pack expansion type in unsupported position");
    }

    case NodeKind::DependentGenericType: {
      if (Node->getNumChildren() < 2)
        return MAKE_NODE_TYPE_ERROR(Node,
                                    "fewer children (%zu) than required (2)",
                                    Node->getNumChildren());

      return decodeMangledType(Node->getChild(1), depth + 1);
    }
    case NodeKind::DependentMemberType: {
      if (Node->getNumChildren() < 2)
        return MAKE_NODE_TYPE_ERROR(Node,
                                    "fewer children (%zu) than required (2)",
                                    Node->getNumChildren());

      auto base = decodeMangledType(Node->getChild(0), depth + 1);
      if (base.isError())
        return base;
      auto assocTypeChild = Node->getChild(1);
      auto member = assocTypeChild->getFirstChild()->getText();
      if (assocTypeChild->getNumChildren() < 2)
        return Builder.createDependentMemberType(member.str(), base.getType());

      auto protocol =
          decodeMangledProtocolType(assocTypeChild->getChild(1), depth + 1);
      if (!protocol)
        return BuiltType();
      return Builder.createDependentMemberType(member.str(), base.getType(),
                                               protocol);
    }
    case NodeKind::DependentAssociatedTypeRef: {
      if (Node->getNumChildren() < 2)
        return MAKE_NODE_TYPE_ERROR(Node,
                                    "fewer children (%zu) than required (2)",
                                    Node->getNumChildren());

      return decodeMangledType(Node->getChild(1), depth + 1);
    }
    case NodeKind::Unowned: {
      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children");

      auto base = decodeMangledType(Node->getChild(0), depth + 1);
      if (base.isError())
        return base;
      return Builder.createUnownedStorageType(base.getType());
    }
    case NodeKind::Unmanaged: {
      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children");

      auto base = decodeMangledType(Node->getChild(0), depth + 1);
      if (base.isError())
        return base;
      return Builder.createUnmanagedStorageType(base.getType());
    }
    case NodeKind::Weak: {
      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children");

      auto base = decodeMangledType(Node->getChild(0), depth + 1);
      if (base.isError())
        return base;
      return Builder.createWeakStorageType(base.getType());
    }
    case NodeKind::SILBoxType: {
      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children");

      auto base = decodeMangledType(Node->getChild(0), depth + 1);
      if (base.isError())
        return base;
      return Builder.createSILBoxType(base.getType());
    }
    case NodeKind::SILBoxTypeWithLayout: {
      llvm::SmallVector<Field, 4> fields;
      llvm::SmallVector<BuiltSubstitution, 4> substitutions;
      llvm::SmallVector<BuiltRequirement, 4> requirements;

      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children");

      auto fieldsNode = Node->getChild(0);
      if (fieldsNode->getKind() != NodeKind::SILBoxLayout)
        return MAKE_NODE_TYPE_ERROR0(fieldsNode, "expected layout");
      for (auto *fieldNode : *fieldsNode) {
        bool isMutable;
        switch (fieldNode->getKind()) {
        case NodeKind::SILBoxMutableField: isMutable = true; break;
        case NodeKind::SILBoxImmutableField: isMutable = false; break;
        default:
          return MAKE_NODE_TYPE_ERROR0(fieldNode, "unhandled field type");
        }
        if (fieldNode->getNumChildren() < 1)
          return MAKE_NODE_TYPE_ERROR0(fieldNode, "no children");
        auto type = decodeMangledType(fieldNode->getChild(0), depth + 1);
        if (type.isError())
          return type;
        fields.emplace_back(type.getType(), isMutable);
      }

      if (Node->getNumChildren() > 1) {
        auto *substNode = Node->getChild(2);
        if (substNode->getKind() != NodeKind::TypeList)
          return MAKE_NODE_TYPE_ERROR0(substNode, "expected type list");

        auto *dependentGenericSignatureNode = Node->getChild(1);
        if (dependentGenericSignatureNode->getKind() !=
            NodeKind::DependentGenericSignature)
          return MAKE_NODE_TYPE_ERROR0(dependentGenericSignatureNode,
                                       "expected dependent generic signature");
        if (dependentGenericSignatureNode->getNumChildren() < 1)
          return MAKE_NODE_TYPE_ERROR(
              dependentGenericSignatureNode,
              "fewer children (%zu) than required (1)",
              dependentGenericSignatureNode->getNumChildren());
        decodeRequirement<BuiltType, BuiltRequirement, BuiltLayoutConstraint,
                          BuilderType>(dependentGenericSignatureNode,
                                       requirements,
                                       Builder /*,
[&](NodePointer Node) -> BuiltType {
return decodeMangledType(Node, depth + 1).getType();
},
[&](LayoutConstraintKind Kind) -> BuiltLayoutConstraint {
return {}; // Not implemented!
},
[&](LayoutConstraintKind Kind, unsigned SizeInBits,
unsigned Alignment) -> BuiltLayoutConstraint {
return {}; // Not Implemented!
}*/);
        // The number of generic parameters at each depth are in a mini
        // state machine and come first.
        llvm::SmallVector<unsigned, 4> genericParamsAtDepth;
        for (auto *reqNode : *dependentGenericSignatureNode)
          if (reqNode->getKind() == NodeKind::DependentGenericParamCount)
            if (reqNode->hasIndex())
              genericParamsAtDepth.push_back(reqNode->getIndex());
        unsigned paramDepth = 0;
        unsigned index = 0;
        for (auto *subst : *substNode) {
          if (paramDepth >= genericParamsAtDepth.size())
            return MAKE_NODE_TYPE_ERROR0(
                dependentGenericSignatureNode,
                "more substitutions than generic params");
          while (index >= genericParamsAtDepth[paramDepth])
            ++paramDepth, index = 0;
          auto substTy = decodeMangledType(subst, depth + 1,
                                           /*forRequirement=*/false);
          if (substTy.isError())
            return substTy;
          auto paramTy = Builder.createGenericTypeParameterType(
              paramDepth, index);
          substitutions.emplace_back(paramTy, substTy.getType());
          ++index;
        }
      }

      return Builder.createSILBoxTypeWithLayout(fields, substitutions,
                                                requirements);
    }
    case NodeKind::SugaredOptional: {
      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children");

      auto base = decodeMangledType(Node->getChild(0), depth + 1);
      if (base.isError())
        return base;

      return Builder.createOptionalType(base.getType());
    }
    case NodeKind::SugaredArray: {
      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children");

      auto base = decodeMangledType(Node->getChild(0), depth + 1);
      if (base.isError())
        return base;

      return Builder.createArrayType(base.getType());
    }
    case NodeKind::SugaredDictionary: {
      if (Node->getNumChildren() < 2)
        return MAKE_NODE_TYPE_ERROR(Node,
                                    "fewer children (%zu) than required (2)",
                                    Node->getNumChildren());

      auto key = decodeMangledType(Node->getChild(0), depth + 1);
      if (key.isError())
        return key;

      auto value = decodeMangledType(Node->getChild(1), depth + 1);
      if (value.isError())
        return value;

      return Builder.createDictionaryType(key.getType(), value.getType());
    }
    case NodeKind::SugaredParen: {
      if (Node->getNumChildren() < 1)
        return MAKE_NODE_TYPE_ERROR0(Node, "no children");

      auto base = decodeMangledType(Node->getChild(0), depth + 1);
      if (base.isError())
        return base;

      return Builder.createParenType(base.getType());
    }
    case NodeKind::OpaqueType: {
      if (Node->getNumChildren() < 3)
        return MAKE_NODE_TYPE_ERROR(Node,
                                    "fewer children (%zu) than required (3)",
                                    Node->getNumChildren());
      auto descriptor = Node->getChild(0);
      auto ordinalNode = Node->getChild(1);

      if (ordinalNode->getKind() != NodeKind::Index
          || !ordinalNode->hasIndex())
        return MAKE_NODE_TYPE_ERROR0(ordinalNode,
                                     "unexpected kind or no index");
      auto ordinal = ordinalNode->getIndex();

      std::vector<BuiltType> genericArgsBuf;
      std::vector<unsigned> genericArgsLevels;
      auto boundGenerics = Node->getChild(2);
      for (unsigned i = 0; i < boundGenerics->getNumChildren(); ++i) {
        genericArgsLevels.push_back(genericArgsBuf.size());
        auto genericsNode = boundGenerics->getChild(i);
        if (genericsNode->getKind() != NodeKind::TypeList)
          break;
        for (auto argNode : *genericsNode) {
          auto arg = decodeMangledType(argNode, depth + 1,
                                       /*forRequirement=*/false);
          if (arg.isError())
            return arg;
          genericArgsBuf.push_back(arg.getType());
        }
      }
      genericArgsLevels.push_back(genericArgsBuf.size());
      std::vector<llvm::ArrayRef<BuiltType>> genericArgs;
      for (unsigned i = 0; i < genericArgsLevels.size() - 1; ++i) {
        auto start = genericArgsLevels[i], end = genericArgsLevels[i+1];
        genericArgs.emplace_back(genericArgsBuf.data() + start,
                                 end - start);
      }
      
      return Builder.resolveOpaqueType(descriptor, genericArgs, ordinal);
    }
    // TODO: Handle OpaqueReturnType, when we're in the middle of reconstructing
    // the defining decl
    default:

      return MAKE_NODE_TYPE_ERROR0(Node, "unexpected kind");
    }
  }

private:
  template<typename Fn>
  llvm::Optional<TypeLookupError>
  decodeTypeSequenceElement(Demangle::NodePointer node, unsigned depth,
                            Fn resultCallback) {
    if (node->getKind() == NodeKind::Type)
      node = node->getChild(0);

    if (node->getKind() == NodeKind::PackExpansion) {
      if (node->getNumChildren() < 2)
        return MAKE_NODE_TYPE_ERROR(node,
                                    "fewer children (%zu) than required (2)",
                                    node->getNumChildren());

      auto patternType = node->getChild(0);

      // Decode the shape pack first, to form a metadata pack.
      auto countType = decodeMangledType(node->getChild(1), depth);
      if (countType.isError())
        return *countType.getError();

      // Push the pack expansion on the active expansion stack inside the
      // builder concept.
      size_t numElements = Builder.beginPackExpansion(countType.getType());

      for (size_t i = 0; i < numElements; ++i) {
        // Advance the lane index inside the builder concept.
        Builder.advancePackExpansion(i);

        // Decode the pattern type, taking the ith element of each pack
        // referenced therein.
        auto expandedElementType = decodeMangledType(patternType, depth);
        if (expandedElementType.isError())
          return *expandedElementType.getError();

        resultCallback(Builder.createExpandedPackElement(
            expandedElementType.getType()));
      }

      // Pop the active expansion stack inside the builder concept.
      Builder.endPackExpansion();
    } else {
      auto elementType =
          decodeMangledType(node, depth, /*forRequirement=*/false);
      if (elementType.isError())
        return *elementType.getError();

      resultCallback(elementType.getType());
    }

    return llvm::None;
  }

  template <typename T>
  bool decodeImplFunctionPart(Demangle::NodePointer node, unsigned depth,
                              llvm::SmallVectorImpl<T> &results) {
    if (depth > TypeDecoder::MaxDepth)
      return true;

    if (node->getNumChildren() != 2)
      return true;
    
    if (node->getChild(0)->getKind() != Node::Kind::ImplConvention ||
        node->getChild(1)->getKind() != Node::Kind::Type)
      return true;

    StringRef conventionString = node->getChild(0)->getText();
    llvm::Optional<typename T::ConventionType> convention =
        T::getConventionFromString(conventionString);
    if (!convention)
      return true;
    auto type = decodeMangledType(node->getChild(1), depth + 1);
    if (type.isError())
      return true;

    results.emplace_back(type.getType(), *convention);
    return false;
  }

  template <typename T>
  bool decodeImplFunctionParam(Demangle::NodePointer node, unsigned depth,
                               llvm::SmallVectorImpl<T> &results) {
    if (depth > TypeDecoder::MaxDepth)
      return true;

    // Children: `convention, differentiability?, type`
    if (node->getNumChildren() != 2 && node->getNumChildren() != 3)
      return true;

    auto *conventionNode = node->getChild(0);
    auto *typeNode = node->getLastChild();
    if (conventionNode->getKind() != Node::Kind::ImplConvention ||
        typeNode->getKind() != Node::Kind::Type)
      return true;

    StringRef conventionString = conventionNode->getText();
    auto convention = T::getConventionFromString(conventionString);
    if (!convention)
      return true;
    auto result = decodeMangledType(typeNode, depth + 1);
    if (result.isError())
      return true;

    auto diffKind = T::DifferentiabilityType::DifferentiableOrNotApplicable;
    if (node->getNumChildren() == 3) {
      auto diffKindNode = node->getChild(1);
      if (diffKindNode->getKind() !=
          Node::Kind::ImplParameterResultDifferentiability)
        return true;
      auto optDiffKind =
          T::getDifferentiabilityFromString(diffKindNode->getText());
      if (!optDiffKind)
        return true;
      diffKind = *optDiffKind;
    }

    results.emplace_back(result.getType(), *convention, diffKind);
    return false;
  }

  llvm::Optional<TypeLookupError>
  decodeGenericArgs(Demangle::NodePointer node, unsigned depth,
                    llvm::SmallVectorImpl<BuiltType> &args) {
    if (node->getKind() != NodeKind::TypeList)
      return MAKE_NODE_TYPE_ERROR0(node, "is not TypeList");

    for (auto genericArg : *node) {
      auto paramType = decodeMangledType(genericArg, depth,
                                         /*forRequirement=*/false);
      if (paramType.isError())
        return *paramType.getError();
      args.push_back(paramType.getType());
    }
    return llvm::None;
  }

  llvm::Optional<TypeLookupError>
  decodeMangledTypeDecl(Demangle::NodePointer node, unsigned depth,
                        BuiltTypeDecl &typeDecl, BuiltType &parent,
                        bool &typeAlias) {
    if (depth > TypeDecoder::MaxDepth)
      return TypeLookupError("Mangled type is too complex");

    if (node->getKind() == NodeKind::Type)
      return decodeMangledTypeDecl(node->getChild(0), depth + 1, typeDecl,
                                   parent, typeAlias);

    Demangle::NodePointer declNode;
    if (node->getKind() == NodeKind::TypeSymbolicReference) {
      // A symbolic reference can be directly resolved to a nominal type.
      declNode = node;
    } else {
      if (node->getNumChildren() < 2)
        return MAKE_NODE_TYPE_ERROR(
            node, "Number of node children (%zu) less than required (2)",
            node->getNumChildren());

      auto parentContext = node->getChild(0);

      // Nested types are handled a bit funny here because a
      // nominal typeref always stores its full mangled name,
      // in addition to a reference to the parent type. The
      // mangled name already includes the module and parent
      // types, if any.
      declNode = node;
      switch (parentContext->getKind()) {
      case Node::Kind::Module:
        break;
      case Node::Kind::Extension:
        // Decode the type being extended.
        if (parentContext->getNumChildren() < 2)
          return MAKE_NODE_TYPE_ERROR(parentContext,
                                      "Number of parentContext children (%zu) "
                                      "less than required (2)",
                                      node->getNumChildren());
        parentContext = parentContext->getChild(1);
        LLVM_FALLTHROUGH;
      default:
        parent = decodeMangledType(parentContext, depth + 1).getType();
        // Remove any generic arguments from the context node, producing a
        // node that references the nominal type declaration.
        auto unspec =
            Demangle::getUnspecialized(node, Builder.getNodeFactory());
        if (!unspec.isSuccess())
          return TypeLookupError("Failed to unspecialize type");
        declNode = unspec.result();
        break;
      }
    }
    typeDecl = Builder.createTypeDecl(declNode, typeAlias);
    if (!typeDecl)
      return TypeLookupError("Failed to create type decl");

    return llvm::None;
  }

  BuiltProtocolDecl decodeMangledProtocolType(Demangle::NodePointer node,
                                              unsigned depth) {
    if (depth > TypeDecoder::MaxDepth)
      return BuiltProtocolDecl();

    if (node->getKind() == NodeKind::Type)
      return decodeMangledProtocolType(node->getChild(0), depth + 1);

    if ((node->getNumChildren() < 2 || node->getKind() != NodeKind::Protocol)
        && node->getKind() != NodeKind::ProtocolSymbolicReference)
      return BuiltProtocolDecl();

#if SWIFT_OBJC_INTEROP
    if (auto objcProtocolName = getObjCClassOrProtocolName(node))
      return Builder.createObjCProtocolDecl(objcProtocolName->str());
#endif

    return Builder.createProtocolDecl(node);
  }

  llvm::Optional<TypeLookupError> decodeMangledFunctionInputType(
      Demangle::NodePointer node, unsigned depth,
      llvm::SmallVectorImpl<FunctionParam<BuiltType>> &params,
      bool &hasParamFlags) {
    if (depth > TypeDecoder::MaxDepth)
      return llvm::None;

    // Look through a couple of sugar nodes.
    if (node->getKind() == NodeKind::Type ||
        node->getKind() == NodeKind::ArgumentTuple) {
      return decodeMangledFunctionInputType(node->getFirstChild(), depth + 1,
                                            params, hasParamFlags);
    }

    auto decodeParamTypeAndFlags =
        [&](Demangle::NodePointer typeNode,
            FunctionParam<BuiltType> &param) -> llvm::Optional<TypeLookupError> {
      Demangle::NodePointer node = typeNode;

      bool recurse = true;
      while (recurse) {
        switch (node->getKind()) {
        case NodeKind::InOut:
          param.setValueOwnership(ValueOwnership::InOut);
          node = node->getFirstChild();
          hasParamFlags = true;
          break;

        case NodeKind::Shared:
          param.setValueOwnership(ValueOwnership::Shared);
          node = node->getFirstChild();
          hasParamFlags = true;
          break;

        case NodeKind::Owned:
          param.setValueOwnership(ValueOwnership::Owned);
          node = node->getFirstChild();
          hasParamFlags = true;
          break;

        case NodeKind::NoDerivative:
          param.setNoDerivative();
          node = node->getFirstChild();
          hasParamFlags = true;
          break;

        case NodeKind::Isolated:
          param.setIsolated();
          node = node->getFirstChild();
          hasParamFlags = true;
          break;

        case NodeKind::AutoClosureType:
        case NodeKind::EscapingAutoClosureType:
          param.setAutoClosure();
          hasParamFlags = true;
          recurse = false;
          break;

        default:
          recurse = false;
          break;
        }
      }

      return decodeTypeSequenceElement(node, depth + 1,
                                       [&](BuiltType paramType) {
                                         param.setType(paramType);
                                         params.push_back(param);
                                       });
    };

    auto decodeParam =
        [&](NodePointer paramNode) -> llvm::Optional<TypeLookupError> {
      if (paramNode->getKind() != NodeKind::TupleElement)
        return llvm::None;

      FunctionParam<BuiltType> param;
      for (const auto &child : *paramNode) {
        switch (child->getKind()) {
        case NodeKind::TupleElementName:
          param.setLabel(child->getText());
          break;

        case NodeKind::VariadicMarker:
          param.setVariadic();
          hasParamFlags = true;
          break;

        case NodeKind::Type: {
          auto optError = decodeParamTypeAndFlags(
              child->getFirstChild(), param);
          if (optError)
            return optError;
          break;
        }

        default:
          return TYPE_LOOKUP_ERROR_FMT("unknown node");
        }
      }

      return llvm::None;
    };

    // Expand a single level of tuple.
    if (node->getKind() == NodeKind::Tuple) {
      // Decode all the elements as separate arguments.
      for (const auto &elt : *node) {
        auto optError = decodeParam(elt);
        if (optError)
          return *optError;
      }

      return llvm::None;
    }

    // Otherwise, handle the type as a single argument.
    FunctionParam<BuiltType> param;
    auto optError = decodeParamTypeAndFlags(node, param);
    if (optError)
      return *optError;

    return llvm::None;
  }
};

template <typename BuilderType>
inline TypeLookupErrorOr<typename BuilderType::BuiltType>
decodeMangledType(BuilderType &Builder, NodePointer Node,
                  bool forRequirement = false) {
  return TypeDecoder<BuilderType>(Builder)
      .decodeMangledType(Node, forRequirement);
}

SWIFT_END_INLINE_NAMESPACE
} // end namespace Demangle
} // end namespace swift

#endif // SWIFT_DEMANGLING_TYPEDECODER_H
