//===--- ASTTypeIDs.h - AST Type Ids ----------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file defines TypeID support for AST types.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_ASTTYPEIDS_H
#define SWIFT_AST_ASTTYPEIDS_H

#include "swift/Basic/LLVM.h"
#include "swift/Basic/TypeID.h"
#include "llvm/ADT/Optional.h"

namespace swift {

class AbstractFunctionDecl;
class ActorIsolation;
class ApplyExpr;
enum class BodyInitKind;
struct BodyInitKindAndExpr;
class BraceStmt;
class ClosureExpr;
class IDEInspectionCallbacksFactory;
class ConstructorDecl;
class CustomAttr;
class Decl;
class EnumDecl;
class Fingerprint;
class FuncDecl;
enum class ResultBuilderBodyPreCheck : uint8_t;
class GenericParamList;
class GenericSignature;
class GenericTypeParamType;
class InfixOperatorDecl;
class IsSingleValueStmtResult;
class IterableDeclContext;
class ModuleDecl;
struct ImplicitImportList;
class NamedPattern;
class NominalTypeDecl;
class OperatorDecl;
class OpaqueTypeDecl;
class PatternBindingEntry;
class ParamDecl;
enum class ParamSpecifier : uint8_t;
class PostfixOperatorDecl;
class PrecedenceGroupDecl;
class PrefixOperatorDecl;
struct PropertyWrapperAuxiliaryVariables;
class PropertyWrapperInitializerInfo;
struct PropertyWrapperTypeInfo;
enum class CtorInitializerKind;
struct PropertyWrapperLValueness;
struct PropertyWrapperMutability;
class ProtocolConformance;
class ProtocolDecl;
class Requirement;
enum class ResilienceExpansion : unsigned;
struct FragileFunctionKind;
enum class PolymorphicEffectKind : uint8_t;
class PolymorphicEffectRequirementList;
class SourceFile;
class SymbolSourceMap;
struct TangentPropertyInfo;
class Type;
class TypeAliasDecl;
struct TypePair;
struct TypeWitnessAndDecl;
class ValueDecl;
class VarDecl;
class Witness;
enum class AncestryFlags : uint8_t;
enum class ImplicitMemberAction : uint8_t;
struct FingerprintAndMembers;
class Identifier;
class BodyAndFingerprint;
struct ConstValueTypeInfo;

// Define the AST type zone (zone 1)
#define SWIFT_TYPEID_ZONE AST
#define SWIFT_TYPEID_HEADER "swift/AST/ASTTypeIDZone.def"
#include "swift/Basic/DefineTypeIDZone.h"

} // end namespace swift

#endif // SWIFT_AST_ASTTYPEIDS_H
