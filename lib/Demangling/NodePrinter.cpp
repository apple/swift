//===--- NodePrinter.cpp - Swift Demangling Node Printer ------------------===//
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
//  This file implements the node printer for demangle node trees.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Ownership.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Strings.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace swift;
using namespace Demangle;
using llvm::StringRef;

DemanglerPrinter &DemanglerPrinter::operator<<(unsigned long long n) & {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%llu", n);
  Stream.append(buffer);
  return *this;
}
DemanglerPrinter &DemanglerPrinter::writeHex(unsigned long long n) & {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%llX", n);
  Stream.append(buffer);
  return *this;
}
DemanglerPrinter &DemanglerPrinter::operator<<(long long n) & {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%lld",n);
  Stream.append(buffer);
  return *this;
}

#if SWIFT_STDLIB_HAS_TYPE_PRINTING

[[noreturn]]
static void printer_unreachable(const char *Message) {
  fprintf(stderr, "fatal error: %s\n", Message);
  std::abort();
}

std::string Demangle::genericParameterName(uint64_t depth, uint64_t index) {
  DemanglerPrinter name;
  do {
    name << (char)('A' + (index % 26));
    index /= 26;
  } while (index);
  if (depth != 0)
    name << depth;
  return std::move(name).str();
}

namespace {

struct QuotedString {
  std::string Value;

  explicit QuotedString(std::string Value) : Value(Value) {}
};

static DemanglerPrinter &operator<<(DemanglerPrinter &printer,
                                    const QuotedString &QS) {
  printer << '"';
  for (auto C : QS.Value) {
    switch (C) {
    case '\\': printer << "\\\\"; break;
    case '\t': printer << "\\t"; break;
    case '\n': printer << "\\n"; break;
    case '\r': printer << "\\r"; break;
    case '"': printer << "\\\""; break;
    case '\0': printer << "\\0"; break;
    default:
      auto c = static_cast<unsigned char>(C);
      // Other control or high-bit characters should get escaped.
      if (c < 0x20 || c >= 0x7F) {
        static const char Hexdigit[] = {
          '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
          'A', 'B', 'C', 'D', 'E', 'F'
        };
        printer << "\\x" << Hexdigit[c >> 4] << Hexdigit[c & 0xF];
      } else {
        printer << (char)c;
      }
      break;
    }
  }
  printer << '"';
  return printer;
}

static StringRef toString(Directness d) {
  switch (d) {
  case Directness::Direct:
    return "direct";
  case Directness::Indirect:
    return "indirect";
  }
  printer_unreachable("bad directness");
}

static StringRef toString(ValueWitnessKind k) {
  switch (k) {
  case ValueWitnessKind::AllocateBuffer:
    return "allocateBuffer";
  case ValueWitnessKind::AssignWithCopy:
    return "assignWithCopy";
  case ValueWitnessKind::AssignWithTake:
    return "assignWithTake";
  case ValueWitnessKind::DeallocateBuffer:
    return "deallocateBuffer";
  case ValueWitnessKind::Destroy:
    return "destroy";
  case ValueWitnessKind::DestroyBuffer:
    return "destroyBuffer";
  case ValueWitnessKind::InitializeBufferWithCopyOfBuffer:
    return "initializeBufferWithCopyOfBuffer";
  case ValueWitnessKind::InitializeBufferWithCopy:
    return "initializeBufferWithCopy";
  case ValueWitnessKind::InitializeWithCopy:
      return "initializeWithCopy";
  case ValueWitnessKind::InitializeBufferWithTake:
    return "initializeBufferWithTake";
  case ValueWitnessKind::InitializeWithTake:
    return "initializeWithTake";
  case ValueWitnessKind::ProjectBuffer:
    return "projectBuffer";
  case ValueWitnessKind::InitializeBufferWithTakeOfBuffer:
    return "initializeBufferWithTakeOfBuffer";
  case ValueWitnessKind::DestroyArray:
    return "destroyArray";
  case ValueWitnessKind::InitializeArrayWithCopy:
    return "initializeArrayWithCopy";
  case ValueWitnessKind::InitializeArrayWithTakeFrontToBack:
    return "initializeArrayWithTakeFrontToBack";
  case ValueWitnessKind::InitializeArrayWithTakeBackToFront:
    return "initializeArrayWithTakeBackToFront";
  case ValueWitnessKind::StoreExtraInhabitant:
    return "storeExtraInhabitant";
  case ValueWitnessKind::GetExtraInhabitantIndex:
    return "getExtraInhabitantIndex";
  case ValueWitnessKind::GetEnumTag:
    return "getEnumTag";
  case ValueWitnessKind::DestructiveProjectEnumData:
    return "destructiveProjectEnumData";
  case ValueWitnessKind::DestructiveInjectEnumTag:
    return "destructiveInjectEnumTag";
  case ValueWitnessKind::GetEnumTagSinglePayload:
    return "getEnumTagSinglePayload";
  case ValueWitnessKind::StoreEnumTagSinglePayload:
    return "storeEnumTagSinglePayload";
  }
  printer_unreachable("bad value witness kind");
}

class NodePrinter {
private:
  DemanglerPrinter Printer;
  DemangleOptions Options;
  bool SpecializationPrefixPrinted = false;
  bool isValid = true;

public:
  NodePrinter(DemangleOptions options) : Options(options) {}

  std::string printRoot(NodePointer root) {
    isValid = true;
    print(root, 0);
    if (isValid)
      return std::move(Printer).str();
    return "";
  }

private:
  static const unsigned MaxDepth = 768;

  /// Called when the node tree in valid.
  ///
  /// The demangler already catches most error cases and mostly produces valid
  /// node trees. But some cases are difficult to catch in the demangler and
  /// instead the NodePrinter bails.
  void setInvalid() { isValid = false; }

  void printChildren(Node::iterator begin, Node::iterator end, unsigned depth,
                     const char *sep = nullptr) {
    for (; begin != end;) {
      print(*begin, depth + 1);
      ++begin;
      if (sep && begin != end)
        Printer << sep;
    }
  }

  void printChildren(NodePointer Node, unsigned depth,
                     const char *sep = nullptr) {
    if (!Node)
      return;
    Node::iterator begin = Node->begin(), end = Node->end();
    printChildren(begin, end, depth, sep);
  }

  NodePointer getFirstChildOfKind(NodePointer Node, Node::Kind kind) {
    if (!Node)
      return nullptr;
    for (NodePointer child : *Node) {
      if (child && child->getKind() == kind)
        return child;
    }
    return nullptr;
  }

  void printBoundGenericNoSugar(NodePointer Node, unsigned depth) {
    if (Node->getNumChildren() < 2)
      return;
    NodePointer typelist = Node->getChild(1);
    print(Node->getChild(0), depth + 1);
    Printer << "<";
    printChildren(typelist, depth, ", ");
    Printer << ">";
  }

  void printOptionalIndex(NodePointer node) {
    assert(node->getKind() == Node::Kind::Index ||
           node->getKind() == Node::Kind::UnknownIndex);
    if (node->hasIndex())
      Printer << "#" << node->getIndex() << " ";
  }

  static bool isSwiftModule(NodePointer node) {
    return (node->getKind() == Node::Kind::Module &&
            node->getText() == STDLIB_NAME);
  }
  
  bool printContext(NodePointer Context) {
    if (!Options.QualifyEntities)
      return false;

    if (Context->getKind() == Node::Kind::Module) {
      if (Context->getText() == swift::STDLIB_NAME)
        return Options.DisplayStdlibModule;
      if (Context->getText() == swift::MANGLING_MODULE_OBJC)
        return Options.DisplayObjCModule;
      if (Context->getText() == Options.HidingCurrentModule)
        return false;
      if (Context->getText().startswith(LLDB_EXPRESSIONS_MODULE_NAME_PREFIX))
        return Options.DisplayDebuggerGeneratedModule;
    }
    return true;
  }

  static bool isIdentifier(NodePointer node, StringRef desired) {
    return (node->getKind() == Node::Kind::Identifier &&
            node->getText() == desired);
  }
  
  enum class SugarType {
    None,
    Optional,
    ImplicitlyUnwrappedOptional,
    Array,
    Dictionary
  };

  enum class TypePrinting {
    NoType,
    WithColon,
    FunctionStyle
  };

  /// Determine whether this is a "simple" type, from the type-simple
  /// production.
  bool isSimpleType(NodePointer Node) {
    switch (Node->getKind()) {
    case Node::Kind::AssociatedType:
    case Node::Kind::AssociatedTypeRef:
    case Node::Kind::BoundGenericClass:
    case Node::Kind::BoundGenericEnum:
    case Node::Kind::BoundGenericStructure:
    case Node::Kind::BoundGenericProtocol:
    case Node::Kind::BoundGenericOtherNominalType:
    case Node::Kind::BoundGenericTypeAlias:
    case Node::Kind::BoundGenericFunction:
    case Node::Kind::BuiltinTypeName:
    case Node::Kind::Class:
    case Node::Kind::DependentGenericType:
    case Node::Kind::DependentMemberType:
    case Node::Kind::DependentGenericParamType:
    case Node::Kind::DynamicSelf:
    case Node::Kind::Enum:
    case Node::Kind::ErrorType:
    case Node::Kind::ExistentialMetatype:
    case Node::Kind::Metatype:
    case Node::Kind::MetatypeRepresentation:
    case Node::Kind::Module:
    case Node::Kind::Tuple:
    case Node::Kind::Pack:
    case Node::Kind::SILPackDirect:
    case Node::Kind::SILPackIndirect:
    case Node::Kind::ConstrainedExistentialRequirementList:
    case Node::Kind::ConstrainedExistentialSelf:
    case Node::Kind::Protocol:
    case Node::Kind::ProtocolSymbolicReference:
    case Node::Kind::ReturnType:
    case Node::Kind::SILBoxType:
    case Node::Kind::SILBoxTypeWithLayout:
    case Node::Kind::Structure:
    case Node::Kind::OtherNominalType:
    case Node::Kind::TupleElementName:
    case Node::Kind::TypeAlias:
    case Node::Kind::TypeList:
    case Node::Kind::LabelList:
    case Node::Kind::TypeSymbolicReference:
    case Node::Kind::SugaredOptional:
    case Node::Kind::SugaredArray:
    case Node::Kind::SugaredDictionary:
    case Node::Kind::SugaredParen:
      return true;

    case Node::Kind::Type:
      return isSimpleType(Node->getChild(0));

    case Node::Kind::ProtocolList:
      return Node->getChild(0)->getNumChildren() <= 1;

    case Node::Kind::ProtocolListWithAnyObject:
      return Node->getChild(0)->getChild(0)->getNumChildren() == 0;

    case Node::Kind::ConstrainedExistential:
    case Node::Kind::PackElement:
    case Node::Kind::PackElementLevel:
    case Node::Kind::PackExpansion:
    case Node::Kind::ProtocolListWithClass:
    case Node::Kind::AccessorAttachedMacroExpansion:
    case Node::Kind::AccessorFunctionReference:
    case Node::Kind::Allocator:
    case Node::Kind::ArgumentTuple:
    case Node::Kind::AssociatedConformanceDescriptor:
    case Node::Kind::AssociatedTypeDescriptor:
    case Node::Kind::AssociatedTypeMetadataAccessor:
    case Node::Kind::AssociatedTypeWitnessTableAccessor:
    case Node::Kind::AutoClosureType:
    case Node::Kind::BaseConformanceDescriptor:
    case Node::Kind::BaseWitnessTableAccessor:
    case Node::Kind::ClangType:
    case Node::Kind::ClassMetadataBaseOffset:
    case Node::Kind::CFunctionPointer:
    case Node::Kind::ConformanceAttachedMacroExpansion:
    case Node::Kind::Constructor:
    case Node::Kind::CoroutineContinuationPrototype:
    case Node::Kind::CurryThunk:
    case Node::Kind::DispatchThunk:
    case Node::Kind::Deallocator:
    case Node::Kind::DeclContext:
    case Node::Kind::DefaultArgumentInitializer:
    case Node::Kind::DefaultAssociatedTypeMetadataAccessor:
    case Node::Kind::DefaultAssociatedConformanceAccessor:
    case Node::Kind::DependentAssociatedTypeRef:
    case Node::Kind::DependentGenericSignature:
    case Node::Kind::DependentGenericParamPackMarker:
    case Node::Kind::DependentGenericParamCount:
    case Node::Kind::DependentGenericConformanceRequirement:
    case Node::Kind::DependentGenericLayoutRequirement:
    case Node::Kind::DependentGenericSameTypeRequirement:
    case Node::Kind::DependentGenericSameShapeRequirement:
    case Node::Kind::DependentPseudogenericSignature:
    case Node::Kind::Destructor:
    case Node::Kind::DidSet:
    case Node::Kind::DirectMethodReferenceAttribute:
    case Node::Kind::Directness:
    case Node::Kind::DynamicAttribute:
    case Node::Kind::EscapingAutoClosureType:
    case Node::Kind::EscapingObjCBlock:
    case Node::Kind::NoEscapeFunctionType:
    case Node::Kind::ExplicitClosure:
    case Node::Kind::Extension:
    case Node::Kind::EnumCase:
    case Node::Kind::FieldOffset:
    case Node::Kind::FreestandingMacroExpansion:
    case Node::Kind::FullObjCResilientClassStub:
    case Node::Kind::FullTypeMetadata:
    case Node::Kind::Function:
    case Node::Kind::FunctionSignatureSpecialization:
    case Node::Kind::FunctionSignatureSpecializationParam:
    case Node::Kind::FunctionSignatureSpecializationReturn:
    case Node::Kind::FunctionSignatureSpecializationParamKind:
    case Node::Kind::FunctionSignatureSpecializationParamPayload:
    case Node::Kind::FunctionType:
    case Node::Kind::GenericProtocolWitnessTable:
    case Node::Kind::GenericProtocolWitnessTableInstantiationFunction:
    case Node::Kind::GenericPartialSpecialization:
    case Node::Kind::GenericPartialSpecializationNotReAbstracted:
    case Node::Kind::GenericSpecialization:
    case Node::Kind::GenericSpecializationNotReAbstracted:
    case Node::Kind::GenericSpecializationInResilienceDomain:
    case Node::Kind::GenericSpecializationParam:
    case Node::Kind::GenericSpecializationPrespecialized:
    case Node::Kind::InlinedGenericFunction:
    case Node::Kind::GenericTypeMetadataPattern:
    case Node::Kind::Getter:
    case Node::Kind::Global:
    case Node::Kind::GlobalGetter:
    case Node::Kind::Identifier:
    case Node::Kind::Index:
    case Node::Kind::InitAccessor:
    case Node::Kind::IVarInitializer:
    case Node::Kind::IVarDestroyer:
    case Node::Kind::ImplDifferentiabilityKind:
    case Node::Kind::ImplEscaping:
    case Node::Kind::ImplConvention:
    case Node::Kind::ImplParameterResultDifferentiability:
    case Node::Kind::ImplFunctionAttribute:
    case Node::Kind::ImplFunctionConvention:
    case Node::Kind::ImplFunctionConventionName:
    case Node::Kind::ImplFunctionType:
    case Node::Kind::ImplInvocationSubstitutions:
    case Node::Kind::ImplPatternSubstitutions:
    case Node::Kind::ImplicitClosure:
    case Node::Kind::ImplParameter:
    case Node::Kind::ImplResult:
    case Node::Kind::ImplYield:
    case Node::Kind::ImplErrorResult:
    case Node::Kind::InOut:
    case Node::Kind::InfixOperator:
    case Node::Kind::Initializer:
    case Node::Kind::Isolated:
    case Node::Kind::CompileTimeConst:
    case Node::Kind::PropertyWrapperBackingInitializer:
    case Node::Kind::PropertyWrapperInitFromProjectedValue:
    case Node::Kind::KeyPathGetterThunkHelper:
    case Node::Kind::KeyPathSetterThunkHelper:
    case Node::Kind::KeyPathEqualsThunkHelper:
    case Node::Kind::KeyPathHashThunkHelper:
    case Node::Kind::LazyProtocolWitnessTableAccessor:
    case Node::Kind::LazyProtocolWitnessTableCacheVariable:
    case Node::Kind::LocalDeclName:
    case Node::Kind::Macro:
    case Node::Kind::MacroExpansionUniqueName:
    case Node::Kind::MaterializeForSet:
    case Node::Kind::MemberAttributeAttachedMacroExpansion:
    case Node::Kind::MemberAttachedMacroExpansion:
    case Node::Kind::MergedFunction:
    case Node::Kind::Metaclass:
    case Node::Kind::MethodDescriptor:
    case Node::Kind::MethodLookupFunction:
    case Node::Kind::ModifyAccessor:
    case Node::Kind::NativeOwningAddressor:
    case Node::Kind::NativeOwningMutableAddressor:
    case Node::Kind::NativePinningAddressor:
    case Node::Kind::NativePinningMutableAddressor:
    case Node::Kind::NominalTypeDescriptor:
    case Node::Kind::NominalTypeDescriptorRecord:
    case Node::Kind::NonObjCAttribute:
    case Node::Kind::Number:
    case Node::Kind::ObjCAsyncCompletionHandlerImpl:
    case Node::Kind::ObjCAttribute:
    case Node::Kind::ObjCBlock:
    case Node::Kind::ObjCMetadataUpdateFunction:
    case Node::Kind::ObjCResilientClassStub:
    case Node::Kind::OpaqueTypeDescriptor:
    case Node::Kind::OpaqueTypeDescriptorRecord:
    case Node::Kind::OpaqueTypeDescriptorAccessor:
    case Node::Kind::OpaqueTypeDescriptorAccessorImpl:
    case Node::Kind::OpaqueTypeDescriptorAccessorKey:
    case Node::Kind::OpaqueTypeDescriptorAccessorVar:
    case Node::Kind::Owned:
    case Node::Kind::OwningAddressor:
    case Node::Kind::OwningMutableAddressor:
    case Node::Kind::PartialApplyForwarder:
    case Node::Kind::PartialApplyObjCForwarder:
    case Node::Kind::PeerAttachedMacroExpansion:
    case Node::Kind::PostfixOperator:
    case Node::Kind::PredefinedObjCAsyncCompletionHandlerImpl:
    case Node::Kind::PrefixOperator:
    case Node::Kind::PrivateDeclName:
    case Node::Kind::PropertyDescriptor:
    case Node::Kind::ProtocolConformance:
    case Node::Kind::ProtocolConformanceDescriptor:
    case Node::Kind::ProtocolConformanceDescriptorRecord:
    case Node::Kind::MetadataInstantiationCache:
    case Node::Kind::ProtocolDescriptor:
    case Node::Kind::ProtocolDescriptorRecord:
    case Node::Kind::ProtocolRequirementsBaseDescriptor:
    case Node::Kind::ProtocolSelfConformanceDescriptor:
    case Node::Kind::ProtocolSelfConformanceWitness:
    case Node::Kind::ProtocolSelfConformanceWitnessTable:
    case Node::Kind::ProtocolWitness:
    case Node::Kind::ProtocolWitnessTable:
    case Node::Kind::ProtocolWitnessTableAccessor:
    case Node::Kind::ProtocolWitnessTablePattern:
    case Node::Kind::ReabstractionThunk:
    case Node::Kind::ReabstractionThunkHelper:
    case Node::Kind::ReabstractionThunkHelperWithSelf:
    case Node::Kind::ReabstractionThunkHelperWithGlobalActor:
    case Node::Kind::ReadAccessor:
    case Node::Kind::RelatedEntityDeclName:
    case Node::Kind::RetroactiveConformance:
    case Node::Kind::Setter:
    case Node::Kind::Shared:
    case Node::Kind::SILBoxLayout:
    case Node::Kind::SILBoxMutableField:
    case Node::Kind::SILBoxImmutableField:
    case Node::Kind::IsSerialized:
    case Node::Kind::MetatypeParamsRemoved:
    case Node::Kind::SpecializationPassID:
    case Node::Kind::Static:
    case Node::Kind::Subscript:
    case Node::Kind::Suffix:
    case Node::Kind::ThinFunctionType:
    case Node::Kind::TupleElement:
    case Node::Kind::TypeMangling:
    case Node::Kind::TypeMetadata:
    case Node::Kind::TypeMetadataAccessFunction:
    case Node::Kind::TypeMetadataCompletionFunction:
    case Node::Kind::TypeMetadataInstantiationCache:
    case Node::Kind::TypeMetadataInstantiationFunction:
    case Node::Kind::TypeMetadataSingletonInitializationCache:
    case Node::Kind::TypeMetadataDemanglingCache:
    case Node::Kind::TypeMetadataLazyCache:
    case Node::Kind::UncurriedFunctionType:
#define REF_STORAGE(Name, ...) \
    case Node::Kind::Name:
#include "swift/AST/ReferenceStorage.def"
    case Node::Kind::UnknownIndex:
    case Node::Kind::UnsafeAddressor:
    case Node::Kind::UnsafeMutableAddressor:
    case Node::Kind::ValueWitness:
    case Node::Kind::ValueWitnessTable:
    case Node::Kind::Variable:
    case Node::Kind::VTableAttribute:
    case Node::Kind::VTableThunk:
    case Node::Kind::WillSet:
    case Node::Kind::ReflectionMetadataBuiltinDescriptor:
    case Node::Kind::ReflectionMetadataFieldDescriptor:
    case Node::Kind::ReflectionMetadataAssocTypeDescriptor:
    case Node::Kind::ReflectionMetadataSuperclassDescriptor:
    case Node::Kind::ResilientProtocolWitnessTable:
    case Node::Kind::GenericTypeParamDecl:
    case Node::Kind::ConcurrentFunctionType:
    case Node::Kind::DifferentiableFunctionType:
    case Node::Kind::GlobalActorFunctionType:
    case Node::Kind::AsyncAnnotation:
    case Node::Kind::ThrowsAnnotation:
    case Node::Kind::EmptyList:
    case Node::Kind::FirstElementMarker:
    case Node::Kind::VariadicMarker:
    case Node::Kind::OutlinedBridgedMethod:
    case Node::Kind::OutlinedCopy:
    case Node::Kind::OutlinedConsume:
    case Node::Kind::OutlinedRetain:
    case Node::Kind::OutlinedRelease:
    case Node::Kind::OutlinedInitializeWithTake:
    case Node::Kind::OutlinedInitializeWithCopy:
    case Node::Kind::OutlinedAssignWithTake:
    case Node::Kind::OutlinedAssignWithCopy:
    case Node::Kind::OutlinedDestroy:
    case Node::Kind::OutlinedVariable:
    case Node::Kind::OutlinedReadOnlyObject:
    case Node::Kind::AssocTypePath:
    case Node::Kind::ModuleDescriptor:
    case Node::Kind::AnonymousDescriptor:
    case Node::Kind::AssociatedTypeGenericParamRef:
    case Node::Kind::ExtensionDescriptor:
    case Node::Kind::AnonymousContext:
    case Node::Kind::AnyProtocolConformanceList:
    case Node::Kind::ConcreteProtocolConformance:
    case Node::Kind::DependentAssociatedConformance:
    case Node::Kind::DependentProtocolConformanceAssociated:
    case Node::Kind::DependentProtocolConformanceInherited:
    case Node::Kind::DependentProtocolConformanceRoot:
    case Node::Kind::ProtocolConformanceRefInTypeModule:
    case Node::Kind::ProtocolConformanceRefInProtocolModule:
    case Node::Kind::ProtocolConformanceRefInOtherModule:
    case Node::Kind::DistributedThunk:
    case Node::Kind::DistributedAccessor:
    case Node::Kind::DynamicallyReplaceableFunctionKey:
    case Node::Kind::DynamicallyReplaceableFunctionImpl:
    case Node::Kind::DynamicallyReplaceableFunctionVar:
    case Node::Kind::OpaqueType:
    case Node::Kind::OpaqueTypeDescriptorSymbolicReference:
    case Node::Kind::OpaqueReturnType:
    case Node::Kind::OpaqueReturnTypeIndex:
    case Node::Kind::OpaqueReturnTypeParent:
    case Node::Kind::OpaqueReturnTypeOf:
    case Node::Kind::CanonicalSpecializedGenericMetaclass:
    case Node::Kind::CanonicalSpecializedGenericTypeMetadataAccessFunction:
    case Node::Kind::NoncanonicalSpecializedGenericTypeMetadata:
    case Node::Kind::NoncanonicalSpecializedGenericTypeMetadataCache:
    case Node::Kind::GlobalVariableOnceDeclList:
    case Node::Kind::GlobalVariableOnceFunction:
    case Node::Kind::GlobalVariableOnceToken:
    case Node::Kind::CanonicalPrespecializedGenericTypeCachingOnceToken:
    case Node::Kind::AsyncFunctionPointer:
    case Node::Kind::AutoDiffFunction:
    case Node::Kind::AutoDiffDerivativeVTableThunk:
    case Node::Kind::AutoDiffSelfReorderingReabstractionThunk:
    case Node::Kind::AutoDiffSubsetParametersThunk:
    case Node::Kind::AutoDiffFunctionKind:
    case Node::Kind::DifferentiabilityWitness:
    case Node::Kind::NoDerivative:
    case Node::Kind::IndexSubset:
    case Node::Kind::AsyncAwaitResumePartialFunction:
    case Node::Kind::AsyncSuspendResumePartialFunction:
    case Node::Kind::AccessibleFunctionRecord:
    case Node::Kind::BackDeploymentThunk:
    case Node::Kind::BackDeploymentFallback:
    case Node::Kind::ExtendedExistentialTypeShape:
    case Node::Kind::Uniquable:
    case Node::Kind::UniqueExtendedExistentialTypeShapeSymbolicReference:
    case Node::Kind::NonUniqueExtendedExistentialTypeShapeSymbolicReference:
    case Node::Kind::SymbolicExtendedExistentialType:
    case Node::Kind::HasSymbolQuery:
    case Node::Kind::RuntimeDiscoverableAttributeRecord:
    case Node::Kind::RuntimeAttributeGenerator:
      return false;
    }
    printer_unreachable("bad node kind");
  }

  void printWithParens(NodePointer type, unsigned depth) {
    bool needs_parens = !isSimpleType(type);
    if (needs_parens)
      Printer << "(";
    print(type, depth + 1);
    if (needs_parens)
      Printer << ")";
  }

  SugarType findSugar(NodePointer Node) {
    if (Node->getNumChildren() == 1 &&
        Node->getKind() == Node::Kind::Type)
      return findSugar(Node->getChild(0));
    
    if (Node->getNumChildren() != 2)
      return SugarType::None;
    
    if (Node->getKind() != Node::Kind::BoundGenericEnum &&
        Node->getKind() != Node::Kind::BoundGenericStructure)
      return SugarType::None;

    auto unboundType = Node->getChild(0)->getChild(0); // drill through Type
    auto typeArgs = Node->getChild(1);
    
    if (Node->getKind() == Node::Kind::BoundGenericEnum) {
      // Swift.Optional
      if (isIdentifier(unboundType->getChild(1), "Optional") &&
          typeArgs->getNumChildren() == 1 &&
          isSwiftModule(unboundType->getChild(0))) {
        return SugarType::Optional;
      }

      // Swift.ImplicitlyUnwrappedOptional
      if (isIdentifier(unboundType->getChild(1), 
                       "ImplicitlyUnwrappedOptional") &&
          typeArgs->getNumChildren() == 1 &&
          isSwiftModule(unboundType->getChild(0))) {
        return SugarType::ImplicitlyUnwrappedOptional;
      }

      return SugarType::None;
    }

    assert(Node->getKind() == Node::Kind::BoundGenericStructure);

    // Array
    if (isIdentifier(unboundType->getChild(1), "Array") &&
        typeArgs->getNumChildren() == 1 &&
        isSwiftModule(unboundType->getChild(0))) {
      return SugarType::Array;
    }

    // Dictionary
    if (isIdentifier(unboundType->getChild(1), "Dictionary") &&
        typeArgs->getNumChildren() == 2 &&
        isSwiftModule(unboundType->getChild(0))) {
      return SugarType::Dictionary;
    }

    return SugarType::None;
  }

  void printBoundGeneric(NodePointer Node, unsigned depth) {
    if (Node->getNumChildren() < 2)
      return;
    if (Node->getNumChildren() != 2) {
      printBoundGenericNoSugar(Node, depth);
      return;
    }

    if (!Options.SynthesizeSugarOnTypes ||
        Node->getKind() == Node::Kind::BoundGenericClass)
    {
      // no sugar here
      printBoundGenericNoSugar(Node, depth);
      return;
    }

    // Print the conforming type for a "bound" protocol node "as" the protocol
    // type.
    if (Node->getKind() == Node::Kind::BoundGenericProtocol) {
      printChildren(Node->getChild(1), depth);
      Printer << " as ";
      print(Node->getChild(0), depth + 1);
      return;
    }

    SugarType sugarType = findSugar(Node);
    
    switch (sugarType) {
      case SugarType::None:
        printBoundGenericNoSugar(Node, depth);
        break;
      case SugarType::Optional:
      case SugarType::ImplicitlyUnwrappedOptional: {
        NodePointer type = Node->getChild(1)->getChild(0);
        printWithParens(type, depth);
        Printer << (sugarType == SugarType::Optional ? "?" : "!");
        break;
      }
      case SugarType::Array: {
        NodePointer type = Node->getChild(1)->getChild(0);
        Printer << "[";
        print(type, depth + 1);
        Printer << "]";
        break;
      }
      case SugarType::Dictionary: {
        NodePointer keyType = Node->getChild(1)->getChild(0);
        NodePointer valueType = Node->getChild(1)->getChild(1);
        Printer << "[";
        print(keyType, depth + 1);
        Printer << " : ";
        print(valueType, depth + 1);
        Printer << "]";
        break;
      }
    }
  }

  NodePointer getChildIf(NodePointer Node, Node::Kind Kind) {
    auto result =
        std::find_if(Node->begin(), Node->end(), [&](NodePointer child) {
          return child->getKind() == Kind;
        });
    return result != Node->end() ? *result : nullptr;
  }

  void printFunctionParameters(NodePointer LabelList, NodePointer ParameterType,
                               unsigned depth, bool showTypes) {
    if (ParameterType->getKind() != Node::Kind::ArgumentTuple) {
      setInvalid();
      return;
    }

    NodePointer Parameters = ParameterType->getFirstChild();
    assert(Parameters->getKind() == Node::Kind::Type);
    Parameters = Parameters->getFirstChild();
    if (Parameters->getKind() != Node::Kind::Tuple) {
      // only a single not-named parameter
      if (showTypes) {
        Printer << '(';
        print(Parameters, depth + 1);
        Printer << ')';
      } else {
        Printer << "(_:)";
      }
      return;
    }

    auto getLabelFor = [&](NodePointer Param, unsigned Index) -> std::string {
      auto Label = LabelList->getChild(Index);
      assert(Label && (Label->getKind() == Node::Kind::Identifier ||
                       Label->getKind() == Node::Kind::FirstElementMarker));
      return Label->getKind() == Node::Kind::Identifier ? Label->getText().str()
                                                        : "_";
    };

    unsigned ParamIndex = 0;
    bool hasLabels = LabelList && LabelList->getNumChildren() > 0;

    Printer << '(';
    llvm::interleave(
        Parameters->begin(), Parameters->end(),
        [&](NodePointer Param) {
          assert(Param->getKind() == Node::Kind::TupleElement);

          if (hasLabels) {
            Printer << getLabelFor(Param, ParamIndex) << ':';
          } else if (!showTypes) {
            if (auto Label = getChildIf(Param, Node::Kind::TupleElementName))
              Printer << Label->getText() << ":";
            else
              Printer << "_:";
          }

          if (hasLabels && showTypes)
            Printer << ' ';

          ++ParamIndex;

          if (showTypes)
            print(Param, depth + 1);
        },
        [&]() { Printer << (showTypes ? ", " : ""); });
    Printer << ')';
  }

  void printFunctionType(NodePointer LabelList, NodePointer node,
                         unsigned depth) {
    if (node->getNumChildren() < 2) {
      setInvalid();
      return;
    }

    auto printConventionWithMangledCType = [this, node,
                                            depth](const char *convention) {
      Printer << "@convention(" << convention;
      if (node->getFirstChild()->getKind() == Node::Kind::ClangType) {
        Printer << ", mangledCType: \"";
        print(node->getFirstChild(), depth + 1);
        Printer << '"';
      }
      Printer << ") ";
    };

    switch (node->getKind()) {
    case Node::Kind::FunctionType:
    case Node::Kind::UncurriedFunctionType:
    case Node::Kind::NoEscapeFunctionType:
      break;
    case Node::Kind::AutoClosureType:
    case Node::Kind::EscapingAutoClosureType:
      Printer << "@autoclosure "; break;
    case Node::Kind::ThinFunctionType:
      Printer << "@convention(thin) "; break;
    case Node::Kind::CFunctionPointer:
      printConventionWithMangledCType("c");
      break;
    case Node::Kind::EscapingObjCBlock:
      Printer << "@escaping ";
      LLVM_FALLTHROUGH;
    case Node::Kind::ObjCBlock:
      printConventionWithMangledCType("block");
      break;
    default:
      assert(false && "Unhandled function type in printFunctionType!");
    }

    unsigned argIndex = node->getNumChildren() - 2;
    unsigned startIndex = 0;
    bool isSendable = false, isAsync = false, isThrows = false;
    auto diffKind = MangledDifferentiabilityKind::NonDifferentiable;
    if (node->getChild(startIndex)->getKind() == Node::Kind::ClangType) {
      // handled earlier
      ++startIndex;
    }
    if (node->getChild(startIndex)->getKind() ==
          Node::Kind::GlobalActorFunctionType) {
      print(node->getChild(startIndex), depth + 1);
      ++startIndex;
    }
    if (node->getChild(startIndex)->getKind() ==
        Node::Kind::DifferentiableFunctionType) {
      diffKind =
          (MangledDifferentiabilityKind)node->getChild(startIndex)->getIndex();
      ++startIndex;
    }
    if (node->getChild(startIndex)->getKind() == Node::Kind::ThrowsAnnotation) {
      ++startIndex;
      isThrows = true;
    }
    if (node->getChild(startIndex)->getKind()
            == Node::Kind::ConcurrentFunctionType) {
      ++startIndex;
      isSendable = true;
    }
    if (node->getChild(startIndex)->getKind() == Node::Kind::AsyncAnnotation) {
      ++startIndex;
      isAsync = true;
    }

    switch (diffKind) {
    case MangledDifferentiabilityKind::Forward:
      Printer << "@differentiable(_forward) ";
      break;
    case MangledDifferentiabilityKind::Reverse:
      Printer << "@differentiable(reverse) ";
      break;
    case MangledDifferentiabilityKind::Linear:
      Printer << "@differentiable(_linear) ";
      break;
    case MangledDifferentiabilityKind::Normal:
      Printer << "@differentiable ";
      break;
    case MangledDifferentiabilityKind::NonDifferentiable:
      break;
    }

    if (isSendable)
      Printer << "@Sendable ";

    printFunctionParameters(LabelList, node->getChild(argIndex), depth,
                            Options.ShowFunctionArgumentTypes);

    if (!Options.ShowFunctionArgumentTypes)
      return;

    if (isAsync)
      Printer << " async";

    if (isThrows)
      Printer << " throws";

    print(node->getChild(argIndex + 1), depth + 1);
  }

  void printImplFunctionType(NodePointer fn, unsigned depth) {
    NodePointer patternSubs = nullptr;
    NodePointer invocationSubs = nullptr;
    enum State { Attrs, Inputs, Results } curState = Attrs;
    auto transitionTo = [&](State newState) {
      assert(newState >= curState);
      for (; curState != newState; curState = State(curState + 1)) {
        switch (curState) {
        case Attrs:
          if (patternSubs) {
            Printer << "@substituted ";
            print(patternSubs->getChild(0), depth + 1);
            Printer << ' ';
          }
          Printer << '(';
          continue;
        case Inputs: Printer << ") -> ("; continue;
        case Results: printer_unreachable("no state after Results");
        }
        printer_unreachable("bad state");
      }
    };

    for (auto &child : *fn) {
      if (child->getKind() == Node::Kind::ImplParameter) {
        if (curState == Inputs) Printer << ", ";
        transitionTo(Inputs);
        print(child, depth + 1);
      } else if (child->getKind() == Node::Kind::ImplResult
                 || child->getKind() == Node::Kind::ImplYield
                 || child->getKind() == Node::Kind::ImplErrorResult) {
        if (curState == Results) Printer << ", ";
        transitionTo(Results);
        print(child, depth + 1);
      } else if (child->getKind() == Node::Kind::ImplPatternSubstitutions) {
        patternSubs = child;
      } else if (child->getKind() == Node::Kind::ImplInvocationSubstitutions) {
        invocationSubs = child;
      } else {
        assert(curState == Attrs);
        print(child, depth + 1);
        Printer << ' ';
      }
    }
    transitionTo(Results);
    Printer << ')';

    if (patternSubs) {
      Printer << " for <";
      printChildren(patternSubs->getChild(1), depth);
      Printer << '>';
    }
    if (invocationSubs) {
      Printer << " for <";
      printChildren(invocationSubs->getChild(0), depth);
      Printer << '>';
    }
  }

  void printGenericSignature(NodePointer Node, unsigned depth) {
    Printer << '<';

    unsigned numChildren = Node->getNumChildren();

    unsigned numGenericParams = 0;
    for (; numGenericParams < numChildren; ++numGenericParams) {
      if (Node->getChild(numGenericParams)->getKind()
               != Node::Kind::DependentGenericParamCount) {
        break;
      }
    }

    unsigned firstRequirement = numGenericParams;
    for (; firstRequirement < numChildren; ++firstRequirement) {
      auto child = Node->getChild(firstRequirement);
      if (child->getKind() == Node::Kind::Type)
        child = child->getChild(0);
      if (child->getKind() != Node::Kind::DependentGenericParamPackMarker) {
        break;
      }
    }

    auto isGenericParamPack = [&](unsigned depth, unsigned index) {
      for (unsigned i = numGenericParams; i < firstRequirement; ++i) {
        auto child = Node->getChild(i);
        if (child->getKind() != Node::Kind::DependentGenericParamPackMarker)
          continue;
        child = child->getChild(0);

        if (child->getKind() != Node::Kind::Type)
          continue;

        child = child->getChild(0);
        if (child->getKind() != Node::Kind::DependentGenericParamType)
          continue;

        if (index == child->getChild(0)->getIndex() &&
            depth == child->getChild(1)->getIndex()) {
          return true;
        }
      }

      return false;
    };

    unsigned gpDepth = 0;
    for (; gpDepth < numGenericParams; ++gpDepth) {
      if (gpDepth != 0)
        Printer << "><";

      unsigned count = Node->getChild(gpDepth)->getIndex();
      for (unsigned index = 0; index < count; ++index) {
        if (index != 0)
          Printer << ", ";

        // Limit the number of printed generic parameters. In practice this
        // it will never be exceeded. The limit is only important for malformed
        // symbols where count can be really huge.
        if (index >= 128) {
          Printer << "...";
          break;
        }

        if (isGenericParamPack(gpDepth, index))
          Printer << "each ";

        // FIXME: Depth won't match when a generic signature applies to a
        // method in generic type context.
        Printer << Options.GenericParameterName(gpDepth, index);
      }
    }

    if (firstRequirement != numChildren) {
      if (Options.DisplayWhereClauses) {
        Printer << " where ";
        for (unsigned i = firstRequirement; i < numChildren; ++i) {
          if (i > firstRequirement)
            Printer << ", ";
          print(Node->getChild(i), depth + 1);
        }
      }
    }
    Printer << '>';
  }

  void printFunctionSigSpecializationParams(NodePointer Node, unsigned depth);

  void printSpecializationPrefix(NodePointer node, StringRef Description,
                                 unsigned depth,
                                 StringRef ParamPrefix = StringRef());

  /// The main big print function.
  NodePointer print(NodePointer Node, unsigned depth,
                    bool asPrefixContext = false);

  NodePointer printAbstractStorage(NodePointer Node, unsigned depth,
                                   bool asPrefixContent, StringRef ExtraName);

  /// Utility function to print entities.
  ///
  /// \param Entity The entity node to print
  /// \param depth The depth in the print() call tree.
  /// \param asPrefixContext Should the entity printed as a context which as a
  ///        prefix to another entity, e.g. the Abc in Abc.def()
  /// \param TypePr How should the type of the entity be printed, if at all.
  ///        E.g. with a colon for properties or as a function type.
  /// \param hasName Does the entity has a name, e.g. a function in contrast to
  ///        an initializer.
  /// \param ExtraName An extra name added to the entity name (if any).
  /// \param ExtraIndex An extra index added to the entity name (if any),
  ///        e.g. closure #1
  /// \param OverwriteName If non-empty, print this name instead of the one
  ///        provided by the node. Gets printed even if hasName is false.
  /// \return If a non-null node is returned it's a context which must be
  ///         printed in postfix-form after the entity: "<entity> in <context>".
  NodePointer printEntity(NodePointer Entity, unsigned depth,
                          bool asPrefixContext, TypePrinting TypePr,
                          bool hasName, StringRef ExtraName = "",
                          int ExtraIndex = -1, StringRef OverwriteName = "");

  /// Print the type of an entity.
  ///
  /// \param Entity The entity.
  /// \param type The type of the entity.
  /// \param genericFunctionTypeList If not null, the generic argument types
  ///           which is printed in the generic signature.
  /// \param depth The depth in the print() call tree.
  void printEntityType(NodePointer Entity, NodePointer type,
                       NodePointer genericFunctionTypeList, unsigned depth);
};
} // end anonymous namespace

static bool isExistentialType(NodePointer node) {
  return (node->getKind() == Node::Kind::ExistentialMetatype ||
          node->getKind() == Node::Kind::ProtocolList ||
          node->getKind() == Node::Kind::ProtocolListWithClass ||
          node->getKind() == Node::Kind::ProtocolListWithAnyObject);
}

/// Print the relevant parameters and return the new index.
void NodePrinter::printFunctionSigSpecializationParams(NodePointer Node,
                                                       unsigned depth) {
  unsigned Idx = 0;
  unsigned End = Node->getNumChildren();
  while (Idx < End) {
    NodePointer firstChild = Node->getChild(Idx);
    unsigned V = firstChild->getIndex();
    auto K = FunctionSigSpecializationParamKind(V);
    switch (K) {
    case FunctionSigSpecializationParamKind::BoxToValue:
    case FunctionSigSpecializationParamKind::BoxToStack:
    case FunctionSigSpecializationParamKind::InOutToOut:
      print(Node->getChild(Idx++), depth + 1);
      break;
    case FunctionSigSpecializationParamKind::ConstantPropFunction:
    case FunctionSigSpecializationParamKind::ConstantPropGlobal: {
      Printer << "[";
      print(Node->getChild(Idx++), depth + 1);
      Printer << " : ";
      const auto &text = Node->getChild(Idx++)->getText();
      std::string demangledName = demangleSymbolAsString(text);
      if (demangledName.empty()) {
        Printer << text;
      } else {
        Printer << demangledName;
      }
      Printer << "]";
      break;
    }
    case FunctionSigSpecializationParamKind::ConstantPropInteger:
    case FunctionSigSpecializationParamKind::ConstantPropFloat:
      Printer << "[";
      print(Node->getChild(Idx++), depth + 1);
      Printer << " : ";
      print(Node->getChild(Idx++), depth + 1);
      Printer << "]";
      break;
    case FunctionSigSpecializationParamKind::ConstantPropString:
      Printer << "[";
      print(Node->getChild(Idx++), depth + 1);
      Printer << " : ";
      print(Node->getChild(Idx++), depth + 1);
      Printer << "'";
      print(Node->getChild(Idx++), depth + 1);
      Printer << "'";
      Printer << "]";
      break;
    case FunctionSigSpecializationParamKind::ConstantPropKeyPath:
      Printer << "[";
      print(Node->getChild(Idx++), depth + 1);
      Printer << " : ";
      print(Node->getChild(Idx++), depth + 1);
      Printer << "<";
      print(Node->getChild(Idx++), depth + 1);
      Printer << ",";
      print(Node->getChild(Idx++), depth + 1);
      Printer << ">]";
      break;
    case FunctionSigSpecializationParamKind::ClosureProp:
      Printer << "[";
      print(Node->getChild(Idx++), depth + 1);
      Printer << " : ";
      print(Node->getChild(Idx++), depth + 1);
      Printer << ", Argument Types : [";
      for (unsigned e = Node->getNumChildren(); Idx < e;) {
        NodePointer child = Node->getChild(Idx);
        // Until we no longer have a type node, keep demangling.
        if (child->getKind() != Node::Kind::Type)
          break;
        print(child, depth + 1);
        ++Idx;

        // If we are not done, print the ", ".
        if (Idx < e && Node->getChild(Idx)->hasText())
          Printer << ", ";
      }
      Printer << "]";
      break;
    default:
      assert(
       ((V & unsigned(FunctionSigSpecializationParamKind::OwnedToGuaranteed)) ||
        (V & unsigned(FunctionSigSpecializationParamKind::GuaranteedToOwned)) ||
        (V & unsigned(FunctionSigSpecializationParamKind::SROA)) ||
        (V & unsigned(FunctionSigSpecializationParamKind::Dead))||
        (V & unsigned(
                  FunctionSigSpecializationParamKind::ExistentialToGeneric))) &&
       "Invalid OptionSet");
      print(Node->getChild(Idx++), depth + 1);
    }
  }
}

void NodePrinter::printSpecializationPrefix(NodePointer node,
                                            StringRef Description,
                                            unsigned depth,
                                            StringRef ParamPrefix) {
  if (!Options.DisplayGenericSpecializations) {
    if (!SpecializationPrefixPrinted) {
      Printer << "specialized ";
      SpecializationPrefixPrinted = true;
    }
    return;
  }
  Printer << Description << " <";
  const char *Separator = "";
  int argNum = 0;
  for (NodePointer child : *node) {
    switch (child->getKind()) {
      case Node::Kind::SpecializationPassID:
      case Node::Kind::MetatypeParamsRemoved:
        // We skip those nodes since it does not contain any
        // information that is useful to our users.
        break;

      case Node::Kind::IsSerialized:
        Printer << Separator;
        Separator = ", ";
        print(child, depth + 1);
        break;

      default:
        // Ignore empty specializations.
        if (child->hasChildren()) {
          Printer << Separator << ParamPrefix;
          Separator = ", ";
          switch (child->getKind()) {
          case Node::Kind::FunctionSignatureSpecializationParam:
            Printer << "Arg[" << argNum << "] = ";
            printFunctionSigSpecializationParams(child, depth);
            break;
          case Node::Kind::FunctionSignatureSpecializationReturn:
            Printer << "Return = ";
            printFunctionSigSpecializationParams(child, depth);
            break;
          default:
            print(child, depth + 1);
          }
        }
        ++argNum;
        break;
    }
  }
  Printer << "> of ";
}

static bool isClassType(NodePointer Node) {
  return Node->getKind() == Node::Kind::Class;
}

static bool needSpaceBeforeType(NodePointer Type) {
  switch (Type->getKind()) {
    case Node::Kind::Type:
      return needSpaceBeforeType(Type->getFirstChild());
    case Node::Kind::FunctionType:
    case Node::Kind::NoEscapeFunctionType:
    case Node::Kind::UncurriedFunctionType:
    case Node::Kind::DependentGenericType:
      return false;
    default:
      return true;
  }
}

NodePointer NodePrinter::print(NodePointer Node, unsigned depth,
                               bool asPrefixContext) {
  if (depth > NodePrinter::MaxDepth) {
    Printer << "<<too complex>>";
    return nullptr;
  }

  if (!Node) {
    Printer << "<null node pointer>";
    return nullptr;
  }

  switch (auto kind = Node->getKind()) {
  case Node::Kind::Static:
    Printer << "static ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::CurryThunk:
    Printer << "curry thunk of ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::DispatchThunk:
    Printer << "dispatch thunk of ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::MethodDescriptor:
    Printer << "method descriptor for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::MethodLookupFunction:
    Printer << "method lookup function for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::ObjCMetadataUpdateFunction:
    Printer << "ObjC metadata update function for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::ObjCResilientClassStub:
    Printer << "ObjC resilient class stub for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::FullObjCResilientClassStub:
    Printer << "full ObjC resilient class stub for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::OutlinedBridgedMethod:
    Printer << "outlined bridged method (" << Node->getText() << ") of ";
    return nullptr;
  case Node::Kind::OutlinedCopy:
    Printer << "outlined copy of ";
    print(Node->getChild(0), depth + 1);
    if (Node->getNumChildren() > 1)
      print(Node->getChild(1), depth + 1);
    return nullptr;
  case Node::Kind::OutlinedConsume:
    Printer << "outlined consume of ";
    print(Node->getChild(0), depth + 1);
    if (Node->getNumChildren() > 1)
      print(Node->getChild(1), depth + 1);
    return nullptr;
  case Node::Kind::OutlinedRetain:
    Printer << "outlined retain of ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::OutlinedRelease:
    Printer << "outlined release of ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::OutlinedInitializeWithTake:
    Printer << "outlined init with take of ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::OutlinedInitializeWithCopy:
    Printer << "outlined init with copy of ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::OutlinedAssignWithTake:
    Printer << "outlined assign with take of ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::OutlinedAssignWithCopy:
    Printer << "outlined assign with copy of ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::OutlinedDestroy:
    Printer << "outlined destroy of ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::OutlinedVariable:
    Printer << "outlined variable #" << Node->getIndex() << " of ";
    return nullptr;
  case Node::Kind::OutlinedReadOnlyObject:
    Printer << "outlined read-only object #" << Node->getIndex() << " of ";
    return nullptr;
  case Node::Kind::Directness:
    Printer << toString(Directness(Node->getIndex())) << " ";
    return nullptr;
  case Node::Kind::AnonymousContext:
    if (Options.QualifyEntities && Options.DisplayExtensionContexts) {
      print(Node->getChild(1), depth + 1);
      Printer << ".(unknown context at ";
      print(Node->getChild(0), depth + 1);
      Printer << ")";
      if (Node->getNumChildren() >= 3 &&
          Node->getChild(2)->getNumChildren() > 0) {
        Printer << '<';
        print(Node->getChild(2), depth + 1);
        Printer << '>';
      }
    }
    return nullptr;
  case Node::Kind::Extension:
    assert((Node->getNumChildren() == 2 || Node->getNumChildren() == 3)
           && "Extension expects 2 or 3 children.");
    if (Options.QualifyEntities && Options.DisplayExtensionContexts) {
      Printer << "(extension in ";
      // Print the module where extension is defined.
      print(Node->getChild(0), depth + 1, true);
      Printer << "):";
    }
    print(Node->getChild(1), depth + 1);
    if (Node->getNumChildren() == 3) {
      // Currently the runtime does not mangle the generic signature.
      // This is an open to-do in swift::_buildDemanglingForContext().
      if (!Options.PrintForTypeName)
        print(Node->getChild(2), depth + 1);
    }
    return nullptr;
  case Node::Kind::Variable:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::WithColon,
                       /*hasName*/ true);
  case Node::Kind::Function:
  case Node::Kind::BoundGenericFunction:
    return printEntity(Node, depth, asPrefixContext,
                       TypePrinting::FunctionStyle,
                       /*hasName*/ true);
  case Node::Kind::Subscript:
    return printEntity(
        Node, depth, asPrefixContext, TypePrinting::FunctionStyle,
        /*hasName*/ false, /*ExtraName*/ "", /*ExtraIndex*/ -1, "subscript");
  case Node::Kind::Macro:
    return printEntity(Node, depth, asPrefixContext,
                       Node->getNumChildren() == 3? TypePrinting::WithColon
                                                  : TypePrinting::FunctionStyle,
                       /*hasName*/ true);
  case Node::Kind::AccessorAttachedMacroExpansion:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/true,
                       ("accessor macro @" +
                        nodeToString(Node->getChild(2)) + " expansion #"),
                       (int)Node->getChild(3)->getIndex() + 1);
  case Node::Kind::FreestandingMacroExpansion:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/true, "freestanding macro expansion #",
                       (int)Node->getChild(2)->getIndex() + 1);
  case Node::Kind::MemberAttributeAttachedMacroExpansion:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/true,
                       ("member attribute macro @" +
                        nodeToString(Node->getChild(2)) + " expansion #"),
                       (int)Node->getChild(3)->getIndex() + 1);
  case Node::Kind::MemberAttachedMacroExpansion:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/true,
                       ("member macro @" + nodeToString(Node->getChild(2)) +
                        " expansion #"),
                       (int)Node->getChild(3)->getIndex() + 1);
  case Node::Kind::PeerAttachedMacroExpansion:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/true,
                       ("peer macro @" + nodeToString(Node->getChild(2)) +
                        " expansion #"),
                       (int)Node->getChild(3)->getIndex() + 1);
  case Node::Kind::ConformanceAttachedMacroExpansion:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/true,
                       ("conformance macro @" + nodeToString(Node->getChild(2)) +
                        " expansion #"),
                       (int)Node->getChild(3)->getIndex() + 1);
  case Node::Kind::MacroExpansionUniqueName:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/true, "unique name #",
                       (int)Node->getChild(2)->getIndex() + 1);
  case Node::Kind::GenericTypeParamDecl:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/ true);
  case Node::Kind::ExplicitClosure:
    return printEntity(
        Node, depth, asPrefixContext,
        Options.ShowFunctionArgumentTypes ? TypePrinting::FunctionStyle
                                          : TypePrinting::NoType,
        /*hasName*/ false, "closure #", (int)Node->getChild(1)->getIndex() + 1);
  case Node::Kind::ImplicitClosure:
    return printEntity(Node, depth, asPrefixContext,
                       Options.ShowFunctionArgumentTypes
                           ? TypePrinting::FunctionStyle
                           : TypePrinting::NoType,
                       /*hasName*/ false, "implicit closure #",
                       (int)Node->getChild(1)->getIndex() + 1);
  case Node::Kind::Global:
    printChildren(Node, depth);
    return nullptr;
  case Node::Kind::Suffix:
    if (Options.DisplayUnmangledSuffix) {
      Printer << " with unmangled suffix "
              << QuotedString(Node->getText().str());
    }
    return nullptr;
  case Node::Kind::Initializer:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/ false, "variable initialization expression");
  case Node::Kind::PropertyWrapperBackingInitializer:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/ false,
                       "property wrapper backing initializer");
  case Node::Kind::PropertyWrapperInitFromProjectedValue:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/ false,
                       "property wrapper init from projected value");
  case Node::Kind::DefaultArgumentInitializer:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/ false, "default argument ",
                       (int)Node->getChild(1)->getIndex());
  case Node::Kind::DeclContext:
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::Type:
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::TypeMangling:
    if (Node->getChild(0)->getKind() == Node::Kind::LabelList) {
      printFunctionType(Node->getChild(0), Node->getChild(1)->getFirstChild(),
                        depth);
    } else {
      print(Node->getChild(0), depth + 1);
    }
    return nullptr;
  case Node::Kind::Class:
  case Node::Kind::Structure:
  case Node::Kind::Enum:
  case Node::Kind::Protocol:
  case Node::Kind::TypeAlias:
  case Node::Kind::OtherNominalType:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/ true);
  case Node::Kind::LocalDeclName:
    print(Node->getChild(1), depth + 1);
    if (Options.DisplayLocalNameContexts)
      Printer << " #" << (Node->getChild(0)->getIndex() + 1);
    return nullptr;
  case Node::Kind::PrivateDeclName:
    if (Node->getNumChildren() > 1) {
      if (Options.ShowPrivateDiscriminators)
        Printer << '(';

      print(Node->getChild(1), depth + 1);

      if (Options.ShowPrivateDiscriminators)
        Printer << " in " << Node->getChild(0)->getText() << ')';
    } else {
      if (Options.ShowPrivateDiscriminators) {
        Printer << "(in " << Node->getChild(0)->getText() << ')';
      }
    }
    return nullptr;
  case Node::Kind::RelatedEntityDeclName:
    Printer << "related decl '" << Node->getFirstChild()->getText() << "' for ";
    print(Node->getChild(1), depth + 1);
    return nullptr;
  case Node::Kind::Module:
    if (Options.DisplayModuleNames)
      Printer << Node->getText();
    return nullptr;
  case Node::Kind::Identifier:
    Printer << Node->getText();
    return nullptr;
  case Node::Kind::Index:
    Printer << Node->getIndex();
    return nullptr;
  case Node::Kind::UnknownIndex:
    Printer << "unknown index";
    return nullptr;
  case Node::Kind::FunctionType:
  case Node::Kind::UncurriedFunctionType:
  case Node::Kind::NoEscapeFunctionType:
  case Node::Kind::AutoClosureType:
  case Node::Kind::EscapingAutoClosureType:
  case Node::Kind::ThinFunctionType:
  case Node::Kind::CFunctionPointer:
  case Node::Kind::ObjCBlock:
  case Node::Kind::EscapingObjCBlock:
    printFunctionType(nullptr, Node, depth);
    return nullptr;
  case Node::Kind::ClangType:
    Printer << Node->getText();
    return nullptr;
  case Node::Kind::ArgumentTuple:
    printFunctionParameters(nullptr, Node, depth,
                            Options.ShowFunctionArgumentTypes);
    return nullptr;
  case Node::Kind::Tuple: {
    Printer << "(";
    printChildren(Node, depth, ", ");
    Printer << ")";
    return nullptr;
  }
  case Node::Kind::TupleElement: {
    if (auto Label = getChildIf(Node, Node::Kind::TupleElementName))
      Printer << Label->getText() << ": ";

    auto Type = getChildIf(Node, Node::Kind::Type);
    assert(Type && "malformed Node::Kind::TupleElement");

    print(Type, depth + 1);

    if (getChildIf(Node, Node::Kind::VariadicMarker))
      Printer << "...";
    return nullptr;
  }
  case Node::Kind::TupleElementName:
    Printer << Node->getText() << ": ";
    return nullptr;
  case Node::Kind::Pack: {
    Printer << "Pack{";
    printChildren(Node, depth, ", ");
    Printer << "}";
    return nullptr;
  }
  case Node::Kind::SILPackDirect:
  case Node::Kind::SILPackIndirect: {
    Printer << (kind == Node::Kind::SILPackDirect ? "@direct" : "@indirect");
    Printer << " Pack{";
    printChildren(Node, depth, ", ");
    Printer << "}";
    return nullptr;
  }
  case Node::Kind::PackExpansion: {
    Printer << "repeat ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  }
  case Node::Kind::PackElement: {
    Printer << "/* level: " << Node->getChild(1)->getIndex() << " */ ";
    Printer << "each ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  }
  case Node::Kind::PackElementLevel:
    printer_unreachable("should be handled in Node::Kind::PackElement");

  case Node::Kind::ReturnType:
    if (Node->getNumChildren() == 0)
      Printer << " -> " << Node->getText();
    else {
      Printer << " -> ";
      printChildren(Node, depth);
    }
    return nullptr;
  case Node::Kind::RetroactiveConformance:
    if (Node->getNumChildren() != 2)
      return nullptr;

    Printer << "retroactive @ ";
    print(Node->getChild(0), depth + 1);
    print(Node->getChild(1), depth + 1);
    return nullptr;
#define REF_STORAGE(Name, ...)                                                 \
  case Node::Kind::Name:                                                       \
    Printer << keywordOf(ReferenceOwnership::Name) << " ";                     \
    print(Node->getChild(0), depth + 1);                                       \
    return nullptr;
#include "swift/AST/ReferenceStorage.def"
  case Node::Kind::InOut:
    Printer << "inout ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::Isolated:
    Printer << "isolated ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::CompileTimeConst:
    Printer << "_const ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::Shared:
    Printer << "__shared ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::Owned:
    Printer << "__owned ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::NoDerivative:
    Printer << "@noDerivative ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::NonObjCAttribute:
    Printer << "@nonobjc ";
    return nullptr;
  case Node::Kind::ObjCAttribute:
    Printer << "@objc ";
    return nullptr;
  case Node::Kind::DirectMethodReferenceAttribute:
    Printer << "super ";
    return nullptr;
  case Node::Kind::DynamicAttribute:
    Printer << "dynamic ";
    return nullptr;
  case Node::Kind::VTableAttribute:
    Printer << "override ";
    return nullptr;
  case Node::Kind::FunctionSignatureSpecialization:
    printSpecializationPrefix(Node, "function signature specialization", depth);
    return nullptr;
  case Node::Kind::GenericPartialSpecialization:
    printSpecializationPrefix(Node, "generic partial specialization", depth,
                              "Signature = ");
    return nullptr;
  case Node::Kind::GenericPartialSpecializationNotReAbstracted:
    printSpecializationPrefix(Node,
                              "generic not-reabstracted partial specialization",
                              depth, "Signature = ");
    return nullptr;
  case Node::Kind::GenericSpecialization:
  case Node::Kind::GenericSpecializationInResilienceDomain:
    printSpecializationPrefix(Node, "generic specialization", depth);
    return nullptr;
  case Node::Kind::GenericSpecializationPrespecialized:
    printSpecializationPrefix(Node, "generic pre-specialization", depth);
    return nullptr;
  case Node::Kind::GenericSpecializationNotReAbstracted:
    printSpecializationPrefix(Node, "generic not re-abstracted specialization",
                              depth);
    return nullptr;
  case Node::Kind::InlinedGenericFunction:
    printSpecializationPrefix(Node, "inlined generic function", depth);
    return nullptr;
  case Node::Kind::IsSerialized:
    Printer << "serialized";
    return nullptr;
  case Node::Kind::MetatypeParamsRemoved:
    Printer << "metatypes-removed";
    return nullptr;
  case Node::Kind::GenericSpecializationParam:
    print(Node->getChild(0), depth + 1);
    for (unsigned i = 1, e = Node->getNumChildren(); i < e; ++i) {
      if (i == 1)
        Printer << " with ";
      else
        Printer << " and ";
      print(Node->getChild(i), depth + 1);
    }
    return nullptr;
  case Node::Kind::FunctionSignatureSpecializationReturn:
  case Node::Kind::FunctionSignatureSpecializationParam:
    printer_unreachable("should be handled in printSpecializationPrefix");
  case Node::Kind::FunctionSignatureSpecializationParamPayload: {
    std::string demangledName = demangleSymbolAsString(Node->getText());
    if (demangledName.empty()) {
      Printer << Node->getText();
    } else {
      Printer << demangledName;
    }
    return nullptr;
  }
  case Node::Kind::FunctionSignatureSpecializationParamKind: {
    uint64_t raw = Node->getIndex();

    bool printedOptionSet = false;
    if (raw &
        uint64_t(FunctionSigSpecializationParamKind::ExistentialToGeneric)) {
      printedOptionSet = true;
      Printer << "Existential To Protocol Constrained Generic";
    }

    if (raw & uint64_t(FunctionSigSpecializationParamKind::Dead)) {
      if (printedOptionSet)
        Printer << " and ";
      printedOptionSet = true;
      Printer << "Dead";
    }
    if (raw & uint64_t(FunctionSigSpecializationParamKind::OwnedToGuaranteed)) {
      if (printedOptionSet)
        Printer << " and ";
      printedOptionSet = true;
      Printer << "Owned To Guaranteed";
    }

    if (raw & uint64_t(FunctionSigSpecializationParamKind::GuaranteedToOwned)) {
      if (printedOptionSet)
        Printer << " and ";
      printedOptionSet = true;
      Printer << "Guaranteed To Owned";
    }

    if (raw & uint64_t(FunctionSigSpecializationParamKind::SROA)) {
      if (printedOptionSet)
        Printer << " and ";
      Printer << "Exploded";
      return nullptr;
    }

    if (printedOptionSet)
      return nullptr;

    switch (FunctionSigSpecializationParamKind(raw)) {
    case FunctionSigSpecializationParamKind::BoxToValue:
      Printer << "Value Promoted from Box";
      return nullptr;
    case FunctionSigSpecializationParamKind::BoxToStack:
      Printer << "Stack Promoted from Box";
      return nullptr;
    case FunctionSigSpecializationParamKind::InOutToOut:
      Printer << "InOut Converted to Out";
      return nullptr;
    case FunctionSigSpecializationParamKind::ConstantPropFunction:
      Printer << "Constant Propagated Function";
      return nullptr;
    case FunctionSigSpecializationParamKind::ConstantPropGlobal:
      Printer << "Constant Propagated Global";
      return nullptr;
    case FunctionSigSpecializationParamKind::ConstantPropInteger:
      Printer << "Constant Propagated Integer";
      return nullptr;
    case FunctionSigSpecializationParamKind::ConstantPropFloat:
      Printer << "Constant Propagated Float";
      return nullptr;
    case FunctionSigSpecializationParamKind::ConstantPropString:
      Printer << "Constant Propagated String";
      return nullptr;
    case FunctionSigSpecializationParamKind::ConstantPropKeyPath:
      Printer << "Constant Propagated KeyPath";
      return nullptr;
    case FunctionSigSpecializationParamKind::ClosureProp:
      Printer << "Closure Propagated";
      return nullptr;
    case FunctionSigSpecializationParamKind::ExistentialToGeneric:
    case FunctionSigSpecializationParamKind::Dead:
    case FunctionSigSpecializationParamKind::OwnedToGuaranteed:
    case FunctionSigSpecializationParamKind::GuaranteedToOwned:
    case FunctionSigSpecializationParamKind::SROA:
      printer_unreachable("option sets should have been handled earlier");
    }
    return nullptr;
  }
  case Node::Kind::SpecializationPassID:
    Printer << Node->getIndex();
    return nullptr;
  case Node::Kind::BuiltinTypeName:
    Printer << Node->getText();
    return nullptr;
  case Node::Kind::Number:
    Printer << Node->getIndex();
    return nullptr;
  case Node::Kind::InfixOperator:
    Printer << Node->getText() << " infix";
    return nullptr;
  case Node::Kind::PrefixOperator:
    Printer << Node->getText() << " prefix";
    return nullptr;
  case Node::Kind::PostfixOperator:
    Printer << Node->getText() << " postfix";
    return nullptr;
  case Node::Kind::LazyProtocolWitnessTableAccessor:
    Printer << "lazy protocol witness table accessor for type ";
    print(Node->getChild(0), depth + 1);
    Printer << " and conformance ";
    print(Node->getChild(1), depth + 1);
    return nullptr;
  case Node::Kind::LazyProtocolWitnessTableCacheVariable:
    Printer << "lazy protocol witness table cache variable for type ";
    print(Node->getChild(0), depth + 1);
    Printer << " and conformance ";
    print(Node->getChild(1), depth + 1);
    return nullptr;
  case Node::Kind::ProtocolSelfConformanceWitnessTable:
    Printer << "protocol self-conformance witness table for ";
    print(Node->getFirstChild(), depth + 1);
    return nullptr;
  case Node::Kind::ProtocolWitnessTableAccessor:
    Printer << "protocol witness table accessor for ";
    print(Node->getFirstChild(), depth + 1);
    return nullptr;
  case Node::Kind::ProtocolWitnessTable:
    Printer << "protocol witness table for ";
    print(Node->getFirstChild(), depth + 1);
    return nullptr;
  case Node::Kind::ProtocolWitnessTablePattern:
    Printer << "protocol witness table pattern for ";
    print(Node->getFirstChild(), depth + 1);
    return nullptr;
  case Node::Kind::GenericProtocolWitnessTable:
    Printer << "generic protocol witness table for ";
    print(Node->getFirstChild(), depth + 1);
    return nullptr;
  case Node::Kind::GenericProtocolWitnessTableInstantiationFunction:
    Printer << "instantiation function for generic protocol witness table for ";
    print(Node->getFirstChild(), depth + 1);
    return nullptr;
  case Node::Kind::ResilientProtocolWitnessTable:
    Printer << "resilient protocol witness table for ";
    print(Node->getFirstChild(), depth + 1);
    return nullptr;
  case Node::Kind::VTableThunk: {
    Printer << "vtable thunk for ";
    print(Node->getChild(1), depth + 1);
    Printer << " dispatching to ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  }
  case Node::Kind::ProtocolSelfConformanceWitness: {
    Printer << "protocol self-conformance witness for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  }
  case Node::Kind::ProtocolWitness: {
    Printer << "protocol witness for ";
    print(Node->getChild(1), depth + 1);
    Printer << " in conformance ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  }
  case Node::Kind::PartialApplyForwarder:
    if (Options.ShortenPartialApply)
      Printer << "partial apply";
    else
      Printer << "partial apply forwarder";

    if (Node->hasChildren()) {
      Printer << " for ";
      printChildren(Node, depth);
    }
    return nullptr;
  case Node::Kind::PartialApplyObjCForwarder:
    if (Options.ShortenPartialApply)
      Printer << "partial apply";
    else
      Printer << "partial apply ObjC forwarder";

    if (Node->hasChildren()) {
      Printer << " for ";
      printChildren(Node, depth);
    }
    return nullptr;
  case Node::Kind::KeyPathGetterThunkHelper:
  case Node::Kind::KeyPathSetterThunkHelper:
    if (Node->getKind() == Node::Kind::KeyPathGetterThunkHelper)
      Printer << "key path getter for ";
    else
      Printer << "key path setter for ";

    print(Node->getChild(0), depth + 1);
    Printer << " : ";
    for (unsigned index = 1; index < Node->getNumChildren(); ++index) {
      auto Child = Node->getChild(index);
      if (Child->getKind() == Node::Kind::IsSerialized)
        Printer << ", ";
      print(Child, depth + 1);
    }
    return nullptr;
  case Node::Kind::KeyPathEqualsThunkHelper:
  case Node::Kind::KeyPathHashThunkHelper: {
    Printer << "key path index "
         << (Node->getKind() == Node::Kind::KeyPathEqualsThunkHelper
               ? "equality" : "hash")
         << " operator for ";

    unsigned lastChildIndex = Node->getNumChildren();
    auto lastChild = Node->getChild(lastChildIndex - 1);
    if (lastChild->getKind() == Node::Kind::IsSerialized) {
      --lastChildIndex;
      lastChild = Node->getChild(lastChildIndex - 1);
    }

    if (lastChild->getKind() == Node::Kind::DependentGenericSignature) {
      print(lastChild, depth + 1);
      --lastChildIndex;
    }

    Printer << "(";
    for (unsigned i = 0; i < lastChildIndex; ++i) {
      if (i != 0)
        Printer << ", ";
      print(Node->getChild(i), depth + 1);
    }
    Printer << ")";
    return nullptr;
  }
  case Node::Kind::FieldOffset: {
    print(Node->getChild(0), depth + 1); // directness
    Printer << "field offset for ";
    auto entity = Node->getChild(1);
    print(entity, depth + 1, /*asContext*/ false);
    return nullptr;
  }
  case Node::Kind::EnumCase: {
    Printer << "enum case for ";
    auto entity = Node->getChild(0);
    print(entity, depth + 1, /*asContext*/ false);
    return nullptr;
  }
  case Node::Kind::ReabstractionThunk:
  case Node::Kind::ReabstractionThunkHelper: {
    if (Options.ShortenThunk) {
      Printer << "thunk for ";
      print(Node->getChild(Node->getNumChildren() - 1), depth + 1);
      return nullptr;
    }
    Printer << "reabstraction thunk ";
    if (Node->getKind() == Node::Kind::ReabstractionThunkHelper)
      Printer << "helper ";
    unsigned idx = 0;
    if (Node->getNumChildren() == 3) {
      auto generics = Node->getChild(0);
      idx = 1;
      print(generics, depth + 1);
      Printer << " ";
    }
    Printer << "from ";
    print(Node->getChild(idx + 1), depth + 1);
    Printer << " to ";
    print(Node->getChild(idx), depth + 1);
    return nullptr;
  }
  case Node::Kind::ReabstractionThunkHelperWithGlobalActor: {
    print(Node->getChild(0), depth + 1);
    Printer << " with global actor constraint ";
    print(Node->getChild(1), depth + 1);
    return nullptr;
  }
  case Node::Kind::ReabstractionThunkHelperWithSelf: {
    Printer << "reabstraction thunk ";
    unsigned idx = 0;
    if (Node->getNumChildren() == 4) {
      auto generics = Node->getChild(0);
      idx = 1;
      print(generics, depth + 1);
      Printer << " ";
    }
    Printer << "from ";
    print(Node->getChild(idx + 2), depth + 1);
    Printer << " to ";
    print(Node->getChild(idx + 1), depth + 1);
    Printer << " self ";
    print(Node->getChild(idx), depth + 1);
    return nullptr;
  }
  case Node::Kind::AutoDiffFunction:
  case Node::Kind::AutoDiffDerivativeVTableThunk: {
    unsigned prefixEndIndex = 0;
    while (prefixEndIndex != Node->getNumChildren() &&
           Node->getChild(prefixEndIndex)->getKind()
              != Node::Kind::AutoDiffFunctionKind)
      ++prefixEndIndex;
    auto funcKind = Node->getChild(prefixEndIndex);
    auto paramIndices = Node->getChild(prefixEndIndex + 1);
    auto resultIndices = Node->getChild(prefixEndIndex + 2);
    if (kind == Node::Kind::AutoDiffDerivativeVTableThunk)
      Printer << "vtable thunk for ";
    print(funcKind, depth + 1);
    Printer << " of ";
    NodePointer optionalGenSig = nullptr;
    for (unsigned i = 0; i < prefixEndIndex; ++i) {
      // The last node may be a generic signature. If so, print it later.
      if (i == prefixEndIndex - 1 &&
          Node->getChild(i)->getKind()
              == Node::Kind::DependentGenericSignature) {
        optionalGenSig = Node->getChild(i);
        break;
      }
      print(Node->getChild(i), depth + 1);
    }
    if (Options.ShortenThunk)
      return nullptr;
    Printer << " with respect to parameters ";
    print(paramIndices, depth + 1);
    Printer << " and results ";
    print(resultIndices, depth + 1);
    if (optionalGenSig && Options.DisplayWhereClauses) {
      Printer << " with ";
      print(optionalGenSig, depth + 1);
    }
    return nullptr;
  }
  case Node::Kind::AutoDiffSelfReorderingReabstractionThunk: {
    Printer << "autodiff self-reordering reabstraction thunk ";
    auto childIt = Node->begin();
    auto fromType = *childIt++;
    auto toType = *childIt++;
    if (Options.ShortenThunk) {
      Printer << "for ";
      print(fromType, depth + 1);
      return nullptr;
    }
    NodePointer optionalGenSig =
        (*childIt)->getKind() == Node::Kind::DependentGenericSignature
            ? *childIt++ : nullptr;
    Printer << "for ";
    print(*childIt++, depth + 1); // kind
    if (optionalGenSig) {
      print(optionalGenSig, depth + 1);
      Printer << ' ';
    }
    Printer << " from ";
    print(fromType, depth + 1);
    Printer << " to ";
    print(toType, depth + 1);
    return nullptr;
  }
  case Node::Kind::AutoDiffSubsetParametersThunk: {
    Printer << "autodiff subset parameters thunk for ";
    auto currentIndex = Node->getNumChildren() - 1;
    auto toParamIndices = Node->getChild(currentIndex--);
    auto resultIndices = Node->getChild(currentIndex--);
    auto paramIndices = Node->getChild(currentIndex--);
    auto kind = Node->getChild(currentIndex--);
    print(kind, depth + 1);
    Printer << " from ";
    // Print the "from" thing.
    if (currentIndex == 0) {
      print(Node->getFirstChild(), depth + 1); // the "from" type
    } else {
      for (unsigned i = 0; i < currentIndex; ++i) // the "from" global
        print(Node->getChild(i), depth + 1);
    }
    if (Options.ShortenThunk)
      return nullptr;
    Printer << " with respect to parameters ";
    print(paramIndices, depth + 1);
    Printer << " and results ";
    print(resultIndices, depth + 1);
    Printer << " to parameters ";
    print(toParamIndices, depth + 1);
    if (currentIndex > 0) {
      Printer << " of type ";
      print(Node->getChild(currentIndex), depth + 1); // "to" type
    }
    return nullptr;
  }
  case Node::Kind::AutoDiffFunctionKind: {
    auto kind = (AutoDiffFunctionKind)Node->getIndex();
    switch (kind) {
    case AutoDiffFunctionKind::JVP:
      Printer << "forward-mode derivative";
      break;
    case AutoDiffFunctionKind::VJP:
      Printer << "reverse-mode derivative";
      break;
    case AutoDiffFunctionKind::Differential:
      Printer << "differential";
      break;
    case AutoDiffFunctionKind::Pullback:
      Printer << "pullback";
      break;
    }
    return nullptr;
  }
  case Node::Kind::DifferentiabilityWitness: {
    auto kindNodeIndex = Node->getNumChildren() - (
        Node->getLastChild()->getKind() == Node::Kind::DependentGenericSignature
            ? 4 : 3);
    auto kind =
        (MangledDifferentiabilityKind)Node->getChild(kindNodeIndex)->getIndex();
    switch (kind) {
    case MangledDifferentiabilityKind::Forward:
      Printer << "forward-mode";
      break;
    case MangledDifferentiabilityKind::Reverse:
      Printer << "reverse-mode";
      break;
    case MangledDifferentiabilityKind::Normal:
      Printer << "normal";
      break;
    case MangledDifferentiabilityKind::Linear:
      Printer << "linear";
      break;
    case MangledDifferentiabilityKind::NonDifferentiable:
      assert(false && "Impossible case");
    }
    Printer << " differentiability witness for ";
    unsigned idx = 0;
    for (auto numChildren = Node->getNumChildren();
         idx < numChildren &&
             Node->getChild(idx)->getKind() != Node::Kind::Index; ++idx)
      print(Node->getChild(idx), depth + 1);
    ++idx; // kind (handled earlier)
    Printer << " with respect to parameters ";
    print(Node->getChild(idx++), depth + 1); // parameter indices
    Printer << " and results ";
    print(Node->getChild(idx++), depth + 1);
    if (idx < Node->getNumChildren()) {
      auto *genSig = Node->getChild(idx);
      assert(genSig->getKind() == Node::Kind::DependentGenericSignature);
      Printer << " with ";
      print(genSig, depth + 1);
    }
    return nullptr;
  }
  case Node::Kind::IndexSubset: {
    Printer << '{';
        auto text = Node->getText();
    bool printedAnyIndex = false;
    for (unsigned i = 0, n = text.size(); i < n; ++i) {
      if (text[i] != 'S') {
        assert(text[i] == 'U');
        continue;
      }
      if (printedAnyIndex)
        Printer << ", ";
      Printer << i;
      printedAnyIndex = true;
    }
    Printer << '}';
    return nullptr;
  }
  case Node::Kind::MergedFunction:
    if (!Options.ShortenThunk) {
      Printer << "merged ";
    }
    return nullptr;
  case Node::Kind::TypeSymbolicReference:
    Printer << "type symbolic reference 0x";
    Printer.writeHex(Node->getIndex());
    return nullptr;
  case Node::Kind::OpaqueTypeDescriptorSymbolicReference:
    Printer << "opaque type symbolic reference 0x";
    Printer.writeHex(Node->getIndex());
    return nullptr;
  case Node::Kind::DistributedThunk:
    if (!Options.ShortenThunk) {
      Printer << "distributed thunk ";
    }
    return nullptr;
  case Node::Kind::DistributedAccessor:
    if (!Options.ShortenThunk) {
      Printer << "distributed accessor for ";
    }
    return nullptr;
  case Node::Kind::AccessibleFunctionRecord:
    if (!Options.ShortenThunk) {
      Printer << "accessible function runtime record for ";
    }
    return nullptr;
  case Node::Kind::RuntimeDiscoverableAttributeRecord:
    if (!Options.ShortenThunk) {
      Printer << "runtime discoverable attribute record for ";
    }
    return nullptr;
  case Node::Kind::DynamicallyReplaceableFunctionKey:
    if (!Options.ShortenThunk) {
      Printer << "dynamically replaceable key for ";
    }
    return nullptr;
  case Node::Kind::DynamicallyReplaceableFunctionImpl:
    if (!Options.ShortenThunk) {
      Printer << "dynamically replaceable thunk for ";
    }
    return nullptr;
  case Node::Kind::DynamicallyReplaceableFunctionVar:
    if (!Options.ShortenThunk) {
      Printer << "dynamically replaceable variable for ";
    }
    return nullptr;
  case Node::Kind::BackDeploymentThunk:
    if (!Options.ShortenThunk) {
      Printer << "back deployment thunk for ";
    }
    return nullptr;
  case Node::Kind::BackDeploymentFallback:
    Printer << "back deployment fallback for ";
    return nullptr;
  case Node::Kind::ProtocolSymbolicReference:
    Printer << "protocol symbolic reference 0x";
    Printer.writeHex(Node->getIndex());
    return nullptr;
  case Node::Kind::GenericTypeMetadataPattern:
    Printer << "generic type metadata pattern for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::Metaclass:
    Printer << "metaclass for ";
    print(Node->getFirstChild(), depth + 1);
    return nullptr;
  case Node::Kind::ProtocolSelfConformanceDescriptor:
    Printer << "protocol self-conformance descriptor for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::ProtocolConformanceDescriptor:
    Printer << "protocol conformance descriptor for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::ProtocolConformanceDescriptorRecord:
    Printer << "protocol conformance descriptor runtime record for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::ProtocolDescriptor:
    Printer << "protocol descriptor for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::ProtocolDescriptorRecord:
    Printer << "protocol descriptor runtime record for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::ProtocolRequirementsBaseDescriptor:
    Printer << "protocol requirements base descriptor for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::FullTypeMetadata:
    Printer << "full type metadata for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::TypeMetadata:
    Printer << "type metadata for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::TypeMetadataAccessFunction:
    Printer << "type metadata accessor for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::TypeMetadataInstantiationCache:
    Printer << "type metadata instantiation cache for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::TypeMetadataInstantiationFunction:
    Printer << "type metadata instantiation function for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::TypeMetadataSingletonInitializationCache:
    Printer << "type metadata singleton initialization cache for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::TypeMetadataCompletionFunction:
    Printer << "type metadata completion function for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::TypeMetadataDemanglingCache:
    Printer << "demangling cache variable for type metadata for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::TypeMetadataLazyCache:
    Printer << "lazy cache variable for type metadata for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::AssociatedConformanceDescriptor:
    Printer << "associated conformance descriptor for ";
    print(Node->getChild(0), depth + 1);
    Printer << ".";
    print(Node->getChild(1), depth + 1);
    Printer << ": ";
    print(Node->getChild(2), depth + 1);
    return nullptr;
  case Node::Kind::DefaultAssociatedConformanceAccessor:
    Printer << "default associated conformance accessor for ";
    print(Node->getChild(0), depth + 1);
    Printer << ".";
    print(Node->getChild(1), depth + 1);
    Printer << ": ";
    print(Node->getChild(2), depth + 1);
    return nullptr;
  case Node::Kind::AssociatedTypeDescriptor:
    Printer << "associated type descriptor for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::AssociatedTypeMetadataAccessor:
    Printer << "associated type metadata accessor for ";
    print(Node->getChild(1), depth + 1);
    Printer << " in ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::BaseConformanceDescriptor:
    Printer << "base conformance descriptor for ";
    print(Node->getChild(0), depth + 1);
    Printer << ": ";
    print(Node->getChild(1), depth + 1);
    return nullptr;
  case Node::Kind::DefaultAssociatedTypeMetadataAccessor:
    Printer << "default associated type metadata accessor for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::AssociatedTypeWitnessTableAccessor:
    Printer << "associated type witness table accessor for ";
    print(Node->getChild(1), depth + 1);
    Printer << " : ";
    print(Node->getChild(2), depth + 1);
    Printer << " in ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::BaseWitnessTableAccessor:
    Printer << "base witness table accessor for ";
    print(Node->getChild(1), depth + 1);
    Printer << " in ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::ClassMetadataBaseOffset:
    Printer << "class metadata base offset for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::PropertyDescriptor:
    Printer << "property descriptor for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::NominalTypeDescriptor:
    Printer << "nominal type descriptor for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::NominalTypeDescriptorRecord:
    Printer << "nominal type descriptor runtime record for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::OpaqueTypeDescriptor:
    Printer << "opaque type descriptor for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::OpaqueTypeDescriptorRecord:
    Printer << "opaque type descriptor runtime record for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::OpaqueTypeDescriptorAccessor:
    Printer << "opaque type descriptor accessor for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::OpaqueTypeDescriptorAccessorImpl:
    Printer << "opaque type descriptor accessor impl for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::OpaqueTypeDescriptorAccessorKey:
    Printer << "opaque type descriptor accessor key for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::OpaqueTypeDescriptorAccessorVar:
    Printer << "opaque type descriptor accessor var for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::CoroutineContinuationPrototype:
    Printer << "coroutine continuation prototype for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::ValueWitness:
    Printer << toString(ValueWitnessKind(Node->getFirstChild()->getIndex()));
    if (Options.ShortenValueWitness) Printer << " for ";
    else Printer << " value witness for ";
    print(Node->getChild(1), depth + 1);
    return nullptr;
  case Node::Kind::ValueWitnessTable:
    Printer << "value witness table for ";
    print(Node->getFirstChild(), depth + 1);
    return nullptr;
  case Node::Kind::BoundGenericClass:
  case Node::Kind::BoundGenericStructure:
  case Node::Kind::BoundGenericEnum:
  case Node::Kind::BoundGenericProtocol:
  case Node::Kind::BoundGenericOtherNominalType:
  case Node::Kind::BoundGenericTypeAlias:
    printBoundGeneric(Node, depth);
    return nullptr;
  case Node::Kind::DynamicSelf:
    Printer << "Self";
    return nullptr;
  case Node::Kind::SILBoxType: {
    Printer << "@box ";
    NodePointer type = Node->getChild(0);
    print(type, depth + 1);
    return nullptr;
  }
  case Node::Kind::Metatype: {
    unsigned Idx = 0;
    if (Node->getNumChildren() == 2) {
      NodePointer repr = Node->getChild(Idx);
      print(repr, depth + 1);
      Printer << " ";
      ++Idx;
    }
    NodePointer type = Node->getChild(Idx)->getChild(0);
    printWithParens(type, depth);
    if (isExistentialType(type)) {
      Printer << ".Protocol";
    } else {
      Printer << ".Type";
    }
    return nullptr;
  }
  case Node::Kind::ConstrainedExistential: {
    Printer << "any ";
    print(Node->getChild(0), depth + 1);
    Printer << "<";
    print(Node->getChild(1), depth + 1);
    Printer << ">";
    return nullptr;
  }
  case Node::Kind::ConstrainedExistentialRequirementList: {
    printChildren(Node, depth, ", ");
    return nullptr;
  }
  case Node::Kind::ExistentialMetatype: {
    unsigned Idx = 0;
    if (Node->getNumChildren() == 2) {
      NodePointer repr = Node->getChild(Idx);
      print(repr, depth + 1);
      Printer << " ";
      ++Idx;
    }

    NodePointer type = Node->getChild(Idx);
    print(type, depth + 1);
    Printer << ".Type";
    return nullptr;
  }
  case Node::Kind::ConstrainedExistentialSelf:
    Printer << "Self";
    return nullptr;
  case Node::Kind::MetatypeRepresentation: {
    Printer << Node->getText();
    return nullptr;
  }
  case Node::Kind::AssociatedTypeRef:
    print(Node->getChild(0), depth + 1);
    Printer << '.' << Node->getChild(1)->getText();
    return nullptr;
  case Node::Kind::ProtocolList: {
    NodePointer type_list = Node->getChild(0);
    if (!type_list)
      return nullptr;
    if (type_list->getNumChildren() == 0)
      Printer << "Any";
    else
      printChildren(type_list, depth, " & ");
    return nullptr;
  }
  case Node::Kind::ProtocolListWithClass: {
    if (Node->getNumChildren() < 2)
      return nullptr;
    NodePointer protocols = Node->getChild(0);
    NodePointer superclass = Node->getChild(1);
    print(superclass, depth + 1);
    Printer << " & ";
    if (protocols->getNumChildren() < 1)
      return nullptr;
    NodePointer type_list = protocols->getChild(0);
    printChildren(type_list, depth, " & ");
    return nullptr;
  }
  case Node::Kind::ProtocolListWithAnyObject: {
    if (Node->getNumChildren() < 1)
      return nullptr;
    NodePointer protocols = Node->getChild(0);
    if (protocols->getNumChildren() < 1)
      return nullptr;
    NodePointer type_list = protocols->getChild(0);
    if (type_list->getNumChildren() > 0) {
      printChildren(type_list, depth, " & ");
      Printer << " & ";
    }
    if (Options.QualifyEntities && Options.DisplayStdlibModule)
      Printer << swift::STDLIB_NAME << ".";
    Printer << "AnyObject";
    return nullptr;
  }
  case Node::Kind::AssociatedType:
    // Don't print for now.
    return nullptr;
  case Node::Kind::OwningAddressor:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "owningAddressor");
  case Node::Kind::OwningMutableAddressor:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "owningMutableAddressor");
  case Node::Kind::NativeOwningAddressor:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "nativeOwningAddressor");
  case Node::Kind::NativeOwningMutableAddressor:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "nativeOwningMutableAddressor");
  case Node::Kind::NativePinningAddressor:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "nativePinningAddressor");
  case Node::Kind::NativePinningMutableAddressor:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "nativePinningMutableAddressor");
  case Node::Kind::UnsafeAddressor:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "unsafeAddressor");
  case Node::Kind::UnsafeMutableAddressor:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "unsafeMutableAddressor");
  case Node::Kind::GlobalGetter:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "getter");
  case Node::Kind::Getter:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "getter");
  case Node::Kind::Setter:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "setter");
  case Node::Kind::MaterializeForSet:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "materializeForSet");
  case Node::Kind::WillSet:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "willset");
  case Node::Kind::DidSet:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "didset");
  case Node::Kind::ReadAccessor:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "read");
  case Node::Kind::ModifyAccessor:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "modify");
  case Node::Kind::InitAccessor:
    return printAbstractStorage(Node->getFirstChild(), depth, asPrefixContext,
                                "init");
  case Node::Kind::Allocator:
    return printEntity(
        Node, depth, asPrefixContext, TypePrinting::FunctionStyle,
        /*hasName*/ false,
        isClassType(Node->getChild(0)) ? "__allocating_init" : "init");
  case Node::Kind::Constructor:
    return printEntity(Node, depth, asPrefixContext,
                       TypePrinting::FunctionStyle,
                       /*hasName*/ Node->getNumChildren() > 2, "init");
  case Node::Kind::Destructor:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/ false, "deinit");
  case Node::Kind::Deallocator:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/ false,
                       isClassType(Node->getChild(0)) ? "__deallocating_deinit"
                                                      : "deinit");
  case Node::Kind::IVarInitializer:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/ false, "__ivar_initializer");
  case Node::Kind::IVarDestroyer:
    return printEntity(Node, depth, asPrefixContext, TypePrinting::NoType,
                       /*hasName*/ false, "__ivar_destroyer");
  case Node::Kind::ProtocolConformance: {
    NodePointer child0 = Node->getChild(0);
    NodePointer child1 = Node->getChild(1);
    NodePointer child2 = Node->getChild(2);
    if (Node->getNumChildren() == 4) {
      // TODO: check if this is correct
      Printer << "property behavior storage of ";
      print(child2, depth + 1);
      Printer << " in ";
      print(child0, depth + 1);
      Printer << " : ";
      print(child1, depth + 1);
    } else {
      print(child0, depth + 1);
      if (Options.DisplayProtocolConformances) {
        Printer << " : ";
        print(child1, depth + 1);
        Printer << " in ";
        print(child2, depth + 1);
      }
    }
    return nullptr;
  }
  case Node::Kind::TypeList:
    printChildren(Node, depth);
    return nullptr;
  case Node::Kind::LabelList:
    return nullptr;
  case Node::Kind::ImplDifferentiabilityKind:
    Printer << "@differentiable";
    switch ((MangledDifferentiabilityKind)Node->getIndex()) {
    case MangledDifferentiabilityKind::Normal:
      break;
    case MangledDifferentiabilityKind::Linear:
      Printer << "(_linear)";
      break;
    case MangledDifferentiabilityKind::Forward:
      Printer << "(_forward)";
      break;
    case MangledDifferentiabilityKind::Reverse:
      Printer << "(reverse)";
      break;
    case MangledDifferentiabilityKind::NonDifferentiable:
      assert(false && "Impossible case 'NonDifferentiable'");
    }
    return nullptr;
  case Node::Kind::ImplEscaping:
    Printer << "@escaping";
    return nullptr;
  case Node::Kind::ImplConvention:
    Printer << Node->getText();
    return nullptr;
  case Node::Kind::ImplParameterResultDifferentiability:
    // Skip if text is empty.
    if (Node->getText().empty())
      return nullptr;
    // Otherwise, print with trailing space.
    Printer << Node->getText() << ' ';
    return nullptr;
  case Node::Kind::ImplFunctionAttribute:
    Printer << Node->getText();
    return nullptr;
  case Node::Kind::ImplFunctionConvention:
    Printer << "@convention(";
    switch (Node->getNumChildren()) {
    case 1:
      Printer << Node->getChild(0)->getText();
      break;
    case 2:
      Printer << Node->getChild(0)->getText() << ", mangledCType: \"";
      print(Node->getChild(1), depth + 1);
      Printer << '"';
      break;
    default:
      assert(false && "Unexpected numChildren for ImplFunctionConvention");
    }
    Printer << ')';
    return nullptr;
  case Node::Kind::ImplFunctionConventionName:
    assert(false && "Already handled in ImplFunctionConvention");
    return nullptr;
  case Node::Kind::ImplErrorResult:
    Printer << "@error ";
    printChildren(Node, depth, " ");
    return nullptr;
  case Node::Kind::ImplYield:
    Printer << "@yields ";
    printChildren(Node, depth, " ");
    return nullptr;
  case Node::Kind::ImplParameter:
  case Node::Kind::ImplResult:
    // Children: `convention, differentiability?, type`
    // Print convention.
    print(Node->getChild(0), depth + 1);
    Printer << " ";
    // Print differentiability, if it exists.
    if (Node->getNumChildren() == 3)
      print(Node->getChild(1), depth + 1);
    // Print type.
    print(Node->getLastChild(), depth + 1);
    return nullptr;
  case Node::Kind::ImplFunctionType:
    printImplFunctionType(Node, depth);
    return nullptr;
  case Node::Kind::ImplInvocationSubstitutions:
    Printer << "for <";
    printChildren(Node->getChild(0), depth, ", ");
    Printer << '>';
    return nullptr;
  case Node::Kind::ImplPatternSubstitutions:
    Printer << "@substituted ";
    print(Node->getChild(0), depth + 1);
    Printer << " for <";
    printChildren(Node->getChild(1), depth, ", ");
    Printer << '>';
    return nullptr;
  case Node::Kind::ErrorType:
    Printer << "<ERROR TYPE>";
    return nullptr;
      
  case Node::Kind::DependentPseudogenericSignature:
  case Node::Kind::DependentGenericSignature: {
    printGenericSignature(Node, depth);
    return nullptr;
  }
  case Node::Kind::DependentGenericParamCount:
  case Node::Kind::DependentGenericParamPackMarker:
    printer_unreachable("should be printed as a child of a "
                        "DependentGenericSignature");
  case Node::Kind::DependentGenericConformanceRequirement: {
    NodePointer type = Node->getChild(0);
    NodePointer reqt = Node->getChild(1);
    print(type, depth + 1);
    Printer << ": ";
    print(reqt, depth + 1);
    return nullptr;
  }
  case Node::Kind::DependentGenericLayoutRequirement: {
    NodePointer type = Node->getChild(0);
    NodePointer layout = Node->getChild(1);
    print(type, depth + 1);
    Printer << ": ";
    assert(layout->getKind() == Node::Kind::Identifier);
    assert(layout->getText().size() == 1);
    char c = layout->getText()[0];
    StringRef name;
    if (c == 'U') {
      name = "_UnknownLayout";
    } else if (c == 'R') {
      name = "_RefCountedObject";
    } else if (c == 'N') {
      name = "_NativeRefCountedObject";
    } else if (c == 'C') {
      name = "AnyObject";
    } else if (c == 'D') {
      name = "_NativeClass";
    } else if (c == 'T') {
      name = "_Trivial";
    } else if (c == 'E' || c == 'e') {
      name = "_Trivial";
    } else if (c == 'M' || c == 'm') {
      name = "_TrivialAtMost";
    }
    Printer << name;
    if (Node->getNumChildren() > 2) {
      Printer << "(";
      print(Node->getChild(2), depth + 1);
      if (Node->getNumChildren() > 3) {
        Printer << ", ";
        print(Node->getChild(3), depth + 1);
      }
      Printer << ")";
    }
    return nullptr;
  }
  case Node::Kind::DependentGenericSameTypeRequirement: {
    NodePointer fst = Node->getChild(0);
    NodePointer snd = Node->getChild(1);

    print(fst, depth + 1);
    Printer << " == ";
    print(snd, depth + 1);
    return nullptr;
  }
  case Node::Kind::DependentGenericSameShapeRequirement: {
    NodePointer fst = Node->getChild(0);
    NodePointer snd = Node->getChild(1);

    print(fst, depth + 1);
    Printer << ".shape == ";
    print(snd, depth + 1);
    Printer << ".shape";
    return nullptr;
  }
  case Node::Kind::DependentGenericParamType: {
    unsigned index = Node->getChild(1)->getIndex();
    unsigned depth = Node->getChild(0)->getIndex();
    Printer << Options.GenericParameterName(depth, index);
    return nullptr;
  }
  case Node::Kind::DependentGenericType: {
    NodePointer sig = Node->getChild(0);
    NodePointer depTy = Node->getChild(1);
    print(sig, depth + 1);
    if (needSpaceBeforeType(depTy))
      Printer << ' ';
    print(depTy, depth + 1);
    return nullptr;
  }
  case Node::Kind::DependentMemberType: {
    NodePointer base = Node->getChild(0);
    print(base, depth + 1);
    Printer << '.';
    NodePointer assocTy = Node->getChild(1);
    print(assocTy, depth + 1);
    return nullptr;
  }
  case Node::Kind::DependentAssociatedTypeRef: {
    if (Node->getNumChildren() > 1) {
      print(Node->getChild(1), depth + 1);
      Printer << '.';
    }
    print(Node->getChild(0), depth + 1);
    return nullptr;
  }
  case Node::Kind::ReflectionMetadataBuiltinDescriptor:
    Printer << "reflection metadata builtin descriptor ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::ReflectionMetadataFieldDescriptor:
    Printer << "reflection metadata field descriptor ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::ReflectionMetadataAssocTypeDescriptor:
    Printer << "reflection metadata associated type descriptor ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::ReflectionMetadataSuperclassDescriptor:
    Printer << "reflection metadata superclass descriptor ";
    print(Node->getChild(0), depth + 1);
    return nullptr;

  case Node::Kind::ConcurrentFunctionType:
    Printer << "@Sendable ";
    return nullptr;
  case Node::Kind::DifferentiableFunctionType: {
    Printer << "@differentiable";
    auto kind = (MangledDifferentiabilityKind)Node->getIndex();
    switch (kind) {
    case MangledDifferentiabilityKind::Forward:
      Printer << "(_forward)";
      break;
    case MangledDifferentiabilityKind::Reverse:
      Printer << "(reverse)";
      break;
    case MangledDifferentiabilityKind::Linear:
      Printer << "(_linear)";
      break;
    case MangledDifferentiabilityKind::Normal:
      break;
    case MangledDifferentiabilityKind::NonDifferentiable:
      assert(false && "Unexpected case NonDifferentiable");
    }
    Printer << ' ';
    return nullptr;
  }
  case Node::Kind::GlobalActorFunctionType: {
    if (Node->getNumChildren() > 0) {
      Printer << '@';
      print(Node->getChild(0), depth + 1);
      Printer << ' ';
    }
    return nullptr;
  }
  case Node::Kind::AsyncAnnotation:
    Printer << " async ";
    return nullptr;
  case Node::Kind::ThrowsAnnotation:
    Printer << " throws ";
    return nullptr;
  case Node::Kind::EmptyList:
    Printer << " empty-list ";
    return nullptr;
  case Node::Kind::FirstElementMarker:
    Printer << " first-element-marker ";
    return nullptr;
  case Node::Kind::VariadicMarker:
    Printer << " variadic-marker ";
    return nullptr;
  case Node::Kind::SILBoxTypeWithLayout: {
    assert(Node->getNumChildren() == 1 || Node->getNumChildren() == 3);
    NodePointer layout = Node->getChild(0);
    assert(layout->getKind() == Node::Kind::SILBoxLayout);
    NodePointer signature, genericArgs = nullptr;
    if (Node->getNumChildren() == 3) {
      signature = Node->getChild(1);
      assert(signature->getKind() == Node::Kind::DependentGenericSignature);
      genericArgs = Node->getChild(2);
      assert(genericArgs->getKind() == Node::Kind::TypeList);

      print(signature, depth + 1);
      Printer << ' ';
    }
    print(layout, depth + 1);
    if (genericArgs) {
      Printer << " <";
      for (unsigned i = 0, e = genericArgs->getNumChildren(); i < e; ++i) {
        if (i > 0)
          Printer << ", ";
        print(genericArgs->getChild(i), depth + 1);
      }
      Printer << '>';
    }
    return nullptr;
  }
  case Node::Kind::SILBoxLayout:
    Printer << '{';
    for (unsigned i = 0; i < Node->getNumChildren(); ++i) {
      if (i > 0)
        Printer << ',';
      Printer << ' ';
      print(Node->getChild(i), depth + 1);
    }
    Printer << " }";
    return nullptr;
  case Node::Kind::SILBoxImmutableField:
  case Node::Kind::SILBoxMutableField:
    Printer << (Node->getKind() == Node::Kind::SILBoxImmutableField
      ? "let "
      : "var ");
    assert(Node->getNumChildren() == 1
           && Node->getChild(0)->getKind() == Node::Kind::Type);
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::AssocTypePath:
    printChildren(Node->begin(), Node->end(), depth, ".");
    return nullptr;
  case Node::Kind::ModuleDescriptor:
    Printer << "module descriptor ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::AnonymousDescriptor:
    Printer << "anonymous descriptor ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::ExtensionDescriptor:
    Printer << "extension descriptor ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::AssociatedTypeGenericParamRef:
    Printer << "generic parameter reference for associated type ";
    printChildren(Node, depth);
    return nullptr;
  case Node::Kind::AnyProtocolConformanceList:
    printChildren(Node, depth);
    return nullptr;
  case Node::Kind::ConcreteProtocolConformance:
    Printer << "concrete protocol conformance ";
    if (Node->hasIndex())
      Printer << "#" << Node->getIndex() << " ";
    printChildren(Node, depth);
    return nullptr;
  case Node::Kind::DependentAssociatedConformance:
    Printer << "dependent associated conformance ";
    printChildren(Node, depth);
    return nullptr;
  case Node::Kind::DependentProtocolConformanceAssociated:
    Printer << "dependent associated protocol conformance ";
    printOptionalIndex(Node->getChild(2));
    print(Node->getChild(0), depth + 1);
    print(Node->getChild(1), depth + 1);
    return nullptr;
  case Node::Kind::DependentProtocolConformanceInherited:
    Printer << "dependent inherited protocol conformance ";
    printOptionalIndex(Node->getChild(2));
    print(Node->getChild(0), depth + 1);
    print(Node->getChild(1), depth + 1);
    return nullptr;
  case Node::Kind::DependentProtocolConformanceRoot:
    Printer << "dependent root protocol conformance ";
    printOptionalIndex(Node->getChild(2));
    print(Node->getChild(0), depth + 1);
    print(Node->getChild(1), depth + 1);
    return nullptr;
  case Node::Kind::ProtocolConformanceRefInTypeModule:
    Printer << "protocol conformance ref (type's module) ";
    printChildren(Node, depth);
    return nullptr;
  case Node::Kind::ProtocolConformanceRefInProtocolModule:
    Printer << "protocol conformance ref (protocol's module) ";
    printChildren(Node, depth);
    return nullptr;
  case Node::Kind::ProtocolConformanceRefInOtherModule:
    Printer << "protocol conformance ref (retroactive) ";
    printChildren(Node, depth);
    return nullptr;
  case Node::Kind::SugaredOptional:
    printWithParens(Node->getChild(0), depth);
    Printer << "?";
    return nullptr;
  case Node::Kind::SugaredArray:
    Printer << "[";
    print(Node->getChild(0), depth + 1);
    Printer << "]";
    return nullptr;
  case Node::Kind::SugaredDictionary:
    Printer << "[";
    print(Node->getChild(0), depth + 1);
    Printer << " : ";
    print(Node->getChild(1), depth + 1);
    Printer << "]";
    return nullptr;
  case Node::Kind::SugaredParen:
    Printer << "(";
    print(Node->getChild(0), depth + 1);
    Printer << ")";
    return nullptr;
  case Node::Kind::OpaqueReturnType:
    Printer << "some";
    return nullptr;
  case Node::Kind::OpaqueReturnTypeOf:
    Printer << "<<opaque return type of ";
    printChildren(Node, depth);
    Printer << ">>";
    return nullptr;
  case Node::Kind::OpaqueType:
    print(Node->getChild(0), depth + 1);
    Printer << '.';
    print(Node->getChild(1), depth + 1);
    return nullptr;
  case Node::Kind::AccessorFunctionReference:
    Printer << "accessor function at " << Node->getIndex();
    return nullptr;
  case Node::Kind::CanonicalSpecializedGenericMetaclass:
    Printer << "specialized generic metaclass for ";
    print(Node->getFirstChild(), depth + 1);
    return nullptr;
  case Node::Kind::CanonicalSpecializedGenericTypeMetadataAccessFunction:
    Printer << "canonical specialized generic type metadata accessor for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::MetadataInstantiationCache:
    Printer << "metadata instantiation cache for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::NoncanonicalSpecializedGenericTypeMetadata:
    Printer << "noncanonical specialized generic type metadata for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::NoncanonicalSpecializedGenericTypeMetadataCache:
    Printer << "cache variable for noncanonical specialized generic type metadata for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::GlobalVariableOnceToken:
  case Node::Kind::GlobalVariableOnceFunction:
    Printer << (kind == Node::Kind::GlobalVariableOnceToken
                  ? "one-time initialization token for "
                  : "one-time initialization function for ");
    printContext(Node->getChild(0));
    print(Node->getChild(1), depth + 1);
    return nullptr;
  case Node::Kind::GlobalVariableOnceDeclList:
    if (Node->getNumChildren() == 1) {
      print(Node->getChild(0), depth + 1);
    } else {
      Printer << '(';
      for (unsigned i = 0, e = Node->getNumChildren(); i < e; ++i) {
        if (i != 0) {
          Printer << ", ";
        }
        print(Node->getChild(i), depth + 1);
      }
      Printer << ')';
    }
    return nullptr;
  case Node::Kind::PredefinedObjCAsyncCompletionHandlerImpl:
    Printer << "predefined ";
    LLVM_FALLTHROUGH;
  case Node::Kind::ObjCAsyncCompletionHandlerImpl:
    Printer << "@objc completion handler block implementation for ";
    if (Node->getNumChildren() >= 4)
      print(Node->getChild(3), depth + 1);
    print(Node->getChild(0), depth + 1);
    Printer << " with result type ";
    print(Node->getChild(1), depth + 1);
    switch (Node->getChild(2)->getIndex()) {
    case 0:
      break;
    case 1:
      Printer << " nonzero on error";
      break;
    case 2:
      Printer << " zero on error";
      break;
    default:
      Printer << " <invalid error flag>";
      break;
    }
    return nullptr;
  case Node::Kind::CanonicalPrespecializedGenericTypeCachingOnceToken:
    Printer << "flag for loading of canonical specialized generic type "
               "metadata for ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::AsyncFunctionPointer:
    Printer << "async function pointer to ";
    return nullptr;
  case Node::Kind::AsyncAwaitResumePartialFunction:
    if (Options.ShowAsyncResumePartial) {
      Printer << "(";
      print(Node->getChild(0), depth + 1);
      Printer << ")";
      Printer << " await resume partial function for ";
    }
    return nullptr;
  case Node::Kind::AsyncSuspendResumePartialFunction:
    if (Options.ShowAsyncResumePartial) {
      Printer << "(";
      print(Node->getChild(0), depth + 1);
      Printer << ")";
      Printer << " suspend resume partial function for ";
    }
    return nullptr;
  case Node::Kind::Uniquable:
    Printer << "uniquable ";
    print(Node->getChild(0), depth + 1);
    return nullptr;
  case Node::Kind::ExtendedExistentialTypeShape: {
    // Printing the requirement signature is pretty useless if we
    // don't print `where` clauses.
    auto savedDisplayWhereClauses = Options.DisplayWhereClauses;
    Options.DisplayWhereClauses = true;

    NodePointer genSig = nullptr, type = nullptr;
    if (Node->getNumChildren() == 2) {
      genSig = Node->getChild(1);
      type = Node->getChild(2);
    } else {
      type = Node->getChild(1);
    }

    Printer << "existential shape for ";
    if (genSig) {
      print(genSig, depth + 1);
      Printer << " ";
    }
    Printer << "any ";
    print(type, depth + 1);

    Options.DisplayWhereClauses = savedDisplayWhereClauses;
    return nullptr;
  }
  case Node::Kind::UniqueExtendedExistentialTypeShapeSymbolicReference:
    Printer << "unique existential shape symbolic reference 0x";
    Printer.writeHex(Node->getIndex());
    return nullptr;
  case Node::Kind::NonUniqueExtendedExistentialTypeShapeSymbolicReference:
    Printer << "non-unique existential shape symbolic reference 0x";
    Printer.writeHex(Node->getIndex());
    return nullptr;
  case Node::Kind::SymbolicExtendedExistentialType: {
    auto shape = Node->getChild(0);
    bool isUnique =
      (shape->getKind() ==
         Node::Kind::UniqueExtendedExistentialTypeShapeSymbolicReference);
    Printer << "symbolic existential type ("
            << (isUnique ? "" : "non-")
            << "unique) 0x";
    Printer.writeHex(shape->getIndex());
    Printer << " <";
    print(Node->getChild(1), depth + 1);
    if (Node->getNumChildren() > 2) {
      Printer << ", ";
      print(Node->getChild(2), depth + 1);
    }
    Printer << ">";

    return nullptr;
  }
  case Node::Kind::HasSymbolQuery:
    Printer << "#_hasSymbol query for ";
    return nullptr;
  case Node::Kind::RuntimeAttributeGenerator:
    Printer << "runtime attribute generator of ";

    print(Node->getChild(1), depth);

    printEntity(Node, depth, asPrefixContext,
                TypePrinting::NoType, /*hasName*/ false,
                " for attribute");

    return nullptr;
  case Node::Kind::OpaqueReturnTypeIndex:
  case Node::Kind::OpaqueReturnTypeParent:
    return nullptr;
  }

  printer_unreachable("bad node kind!");
}

NodePointer NodePrinter::printAbstractStorage(NodePointer Node, unsigned depth,
                                              bool asPrefixContent,
                                              StringRef ExtraName) {
  switch (Node->getKind()) {
    case Node::Kind::Variable:
      return printEntity(Node, depth, asPrefixContent, TypePrinting::WithColon,
                         /*hasName*/ true, ExtraName);
    case Node::Kind::Subscript:
      return printEntity(Node, depth, asPrefixContent, TypePrinting::WithColon,
                         /*hasName*/ false, ExtraName, /*ExtraIndex*/ -1,
                         "subscript");
    default:
      printer_unreachable("Not an abstract storage node");
  }
}

NodePointer NodePrinter::printEntity(NodePointer Entity, unsigned depth,
                                     bool asPrefixContext, TypePrinting TypePr,
                                     bool hasName, StringRef ExtraName,
                                     int ExtraIndex, StringRef OverwriteName) {

  NodePointer genericFunctionTypeList = nullptr;
  if (Entity->getKind() == Node::Kind::BoundGenericFunction) {
    genericFunctionTypeList = Entity->getChild(1);
    Entity = Entity->getFirstChild();
  }

  // Either we print the context in prefix form "<context>.<name>" or in
  // suffix form "<name> in <context>".
  bool MultiWordName = ExtraName.contains(' ');
  // Also a local name (e.g. Mystruct #1) does not look good if its context is
  // printed in prefix form.
  bool LocalName =
      hasName && Entity->getChild(1)->getKind() == Node::Kind::LocalDeclName;
  if (LocalName && Options.DisplayLocalNameContexts)
    MultiWordName = true;

  if (asPrefixContext && (TypePr != TypePrinting::NoType || MultiWordName)) {
      // If the context has a type to be printed, we can't use the prefix form.
      return Entity;
  }

  NodePointer PostfixContext = nullptr;
  NodePointer Context = Entity->getChild(0);
  if (printContext(Context)) {
    if (MultiWordName) {
      // If the name contains some spaces we don't print the context now but
      // later in suffix form.
      PostfixContext = Context;
    } else {
      size_t CurrentPos = Printer.getStringRef().size();
      PostfixContext = print(Context, depth + 1, /*asPrefixContext*/ true);

      // Was the context printed as prefix?
      if (Printer.getStringRef().size() != CurrentPos)
        Printer << '.';
    }
  }

  if (hasName || !OverwriteName.empty()) {
    if (!ExtraName.empty() && MultiWordName) {
      Printer << ExtraName;
      if (ExtraIndex >= 0)
        Printer << ExtraIndex;

      Printer << " of ";
      ExtraName = "";
      ExtraIndex = -1;
    }
    size_t CurrentPos = Printer.getStringRef().size();
    if (!OverwriteName.empty()) {
      Printer << OverwriteName;
    } else {
      auto Name = Entity->getChild(1);
      if (Name->getKind() != Node::Kind::PrivateDeclName)
        print(Name, depth + 1);

      if (auto PrivateName = getChildIf(Entity, Node::Kind::PrivateDeclName))
        print(PrivateName, depth + 1);
    }
    if (Printer.getStringRef().size() != CurrentPos && !ExtraName.empty())
      Printer << '.';
  }
  if (!ExtraName.empty()) {
    Printer << ExtraName;
    if (ExtraIndex >= 0)
      Printer << ExtraIndex;
  }
  if (TypePr != TypePrinting::NoType) {
    NodePointer type = getChildIf(Entity, Node::Kind::Type);
    assert(type && "malformed entity");
    if (!type) {
      setInvalid();
      return nullptr;
    }
    type = type->getChild(0);
    if (TypePr == TypePrinting::FunctionStyle) {
      // We expect to see a function type here, but if we don't, use the colon.
      NodePointer t = type;
      while (t->getKind() == Node::Kind::DependentGenericType)
        t = t->getChild(1)->getChild(0);
      if (t->getKind() != Node::Kind::FunctionType &&
          t->getKind() != Node::Kind::NoEscapeFunctionType &&
          t->getKind() != Node::Kind::UncurriedFunctionType &&
          t->getKind() != Node::Kind::CFunctionPointer &&
          t->getKind() != Node::Kind::ThinFunctionType) {
        TypePr = TypePrinting::WithColon;
      }
    }

    if (TypePr == TypePrinting::WithColon) {
      if (Options.DisplayEntityTypes) {
        Printer << " : ";
        printEntityType(Entity, type, genericFunctionTypeList, depth);
      }
    } else {
      assert(TypePr == TypePrinting::FunctionStyle);
      if (MultiWordName || needSpaceBeforeType(type))
        Printer << ' ';
      printEntityType(Entity, type, genericFunctionTypeList, depth);
    }
  }
  if (!asPrefixContext && PostfixContext &&
      (!LocalName || Options.DisplayLocalNameContexts)) {
    // Print any left over context which couldn't be printed in prefix form.
    if (Entity->getKind() == Node::Kind::DefaultArgumentInitializer ||
        Entity->getKind() == Node::Kind::Initializer ||
        Entity->getKind() == Node::Kind::PropertyWrapperBackingInitializer ||
        Entity->getKind() == Node::Kind::PropertyWrapperInitFromProjectedValue ||
        Entity->getKind() == Node::Kind::RuntimeAttributeGenerator) {
      Printer << " of ";
    } else {
      Printer << " in ";
    }
    print(PostfixContext, depth + 1);
    PostfixContext = nullptr;
  }
  return PostfixContext;
}

void NodePrinter::printEntityType(NodePointer Entity, NodePointer type,
                                  NodePointer genericFunctionTypeList,
                                  unsigned depth) {
  NodePointer labelList = getChildIf(Entity, Node::Kind::LabelList);
  if (labelList || genericFunctionTypeList) {
    if (genericFunctionTypeList) {
      Printer << "<";
      printChildren(genericFunctionTypeList, depth, ", ");
      Printer << ">";
    }
    if (type->getKind() == Node::Kind::DependentGenericType) {
      if (!genericFunctionTypeList)
        print(type->getChild(0), depth + 1); // generic signature

      auto dependentType = type->getChild(1);
      if (needSpaceBeforeType(dependentType))
        Printer << ' ';
      type = dependentType->getFirstChild();
    }
    printFunctionType(labelList, type, depth);
  } else {
    print(type, depth + 1);
  }
}

NodePointer
matchSequenceOfKinds(NodePointer start,
                     std::vector<std::tuple<Node::Kind, size_t>> pattern) {
  if (start != nullptr) {
    NodePointer current = start;
    size_t idx = 0;
    while (idx < pattern.size()) {
      std::tuple<Node::Kind, size_t> next = pattern[idx];
      idx += 1;
      NodePointer nextChild = current->getChild(std::get<1>(next));
      if (nextChild != nullptr && nextChild->getKind() == std::get<0>(next)) {
        current = nextChild;
      } else {
        return nullptr;
      }
    }
    if (idx == pattern.size()) {
      return current;
    } else {
      return nullptr;
    }
  } else {
    return nullptr;
  }
}

std::string Demangle::keyPathSourceString(const char *MangledName,
                                          size_t MangledNameLength) {
  std::string invalid = "";
  std::string unlabelledArg = "_: ";
  Context ctx;
  NodePointer root =
      ctx.demangleSymbolAsNode(StringRef(MangledName, MangledNameLength));
  if (!root)
    return invalid;
  if (root->getNumChildren() >= 1) {
    NodePointer firstChild = root->getChild(0);
    if (firstChild->getKind() == Node::Kind::KeyPathGetterThunkHelper) {
      NodePointer child = firstChild->getChild(0);
      switch (child->getKind()) {
      case Node::Kind::Subscript: {
        std::string subscriptText = "subscript(";
        std::vector<std::string> argumentTypeNames;
        auto getArgumentTypeName = [&argumentTypeNames](size_t i) {
          if (i < argumentTypeNames.size())
            return argumentTypeNames[i];
          return std::string("<unknown>");
        };
        // Multiple arguments case
        NodePointer argList = matchSequenceOfKinds(
            child, {
                       std::make_pair(Node::Kind::Type, 2),
                       std::make_pair(Node::Kind::FunctionType, 0),
                       std::make_pair(Node::Kind::ArgumentTuple, 0),
                       std::make_pair(Node::Kind::Type, 0),
                       std::make_pair(Node::Kind::Tuple, 0),
                   });
        if (argList != nullptr) {
          size_t numArgumentTypes = argList->getNumChildren();
          size_t idx = 0;
          while (idx < numArgumentTypes) {
            NodePointer argumentType = argList->getChild(idx);
            idx += 1;
            if (argumentType->getKind() == Node::Kind::TupleElement) {
              argumentType =
                  argumentType->getChild(0)->getChild(0)->getChild(1);
              if (argumentType->getKind() == Node::Kind::Identifier) {
                argumentTypeNames.push_back(
                    std::string(argumentType->getText()));
                continue;
              }
            }
            argumentTypeNames.push_back("<Unknown>");
          }
        } else {
          // Case where there is a single argument
          argList = matchSequenceOfKinds(
              child, {
                         std::make_pair(Node::Kind::Type, 2),
                         std::make_pair(Node::Kind::FunctionType, 0),
                         std::make_pair(Node::Kind::ArgumentTuple, 0),
                         std::make_pair(Node::Kind::Type, 0),
                     });
          if (argList != nullptr) {
            argumentTypeNames.push_back(
                std::string(argList->getChild(0)->getChild(1)->getText()));
          }
        }
        child = child->getChild(1);
        size_t idx = 0;
        // There is an argument label:
        if (child != nullptr) {
          if (child->getKind() == Node::Kind::LabelList) {
            size_t numChildren = child->getNumChildren();
            if (numChildren == 0) {
              subscriptText += unlabelledArg + getArgumentTypeName(0);
            } else {
              while (idx < numChildren) {
                Node *argChild = child->getChild(idx);
                idx += 1;
                if (argChild->getKind() == Node::Kind::Identifier) {
                  subscriptText += std::string(argChild->getText()) + ": " +
                                   getArgumentTypeName(idx - 1);
                  if (idx != numChildren) {
                    subscriptText += ", ";
                  }
                } else if (argChild->getKind() ==
                               Node::Kind::FirstElementMarker ||
                           argChild->getKind() == Node::Kind::VariadicMarker) {
                  subscriptText += unlabelledArg + getArgumentTypeName(idx - 1);
                }
              }
            }
          }
        } else {
          subscriptText += unlabelledArg + getArgumentTypeName(0);
        }
        return subscriptText + ")";
      }
      case Node::Kind::Variable: {
        child = child->getChild(1);
        if (child == nullptr) {
          return invalid;
        }
        if (child->getKind() == Node::Kind::PrivateDeclName) {
          child = child->getChild(1);
          if (child == nullptr) {
            return invalid;
          }
          if (child->getKind() == Node::Kind::Identifier) {
            return std::string(child->getText());
          }
        } else if (child->getKind() == Node::Kind::Identifier) {
          return std::string(child->getText());
        }
        break;
      }
      default:
        return invalid;
      }
    }
  }
  return invalid;
}

std::string Demangle::nodeToString(NodePointer root,
                                   const DemangleOptions &options) {
  if (!root)
    return "";

  return NodePrinter(options).printRoot(root);
}

#endif
