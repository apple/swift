//===--- ASTBridging.h - header for the swift SILBridging module ----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_C_AST_ASTBRIDGING_H
#define SWIFT_C_AST_ASTBRIDGING_H

#include "swift/Basic/CBasicBridging.h"
#include "swift/Basic/Compiler.h"

// NOTE: DO NOT #include any stdlib headers here. e.g. <stdint.h>. Those are
// part of "Darwin"/"Glibc" module, so when a Swift file imports this header,
// it causes importing the "Darwin"/"Glibc" overlay module. That violates
// layering. i.e. Darwin overlay is created by Swift compiler.

#if __clang__
// Provide macros to temporarily suppress warning about the use of
// _Nullable and _Nonnull.
#define SWIFT_BEGIN_NULLABILITY_ANNOTATIONS                                    \
  _Pragma("clang diagnostic push")                                             \
      _Pragma("clang diagnostic ignored \"-Wnullability-extension\"")          \
          _Pragma("clang assume_nonnull begin")

#define SWIFT_END_NULLABILITY_ANNOTATIONS                                      \
  _Pragma("clang diagnostic pop") _Pragma("clang assume_nonnull end")
#else
#define SWIFT_BEGIN_NULLABILITY_ANNOTATIONS
#define SWIFT_END_NULLABILITY_ANNOTATIONS
#define _Nullable
#endif

SWIFT_BEGIN_NULLABILITY_ANNOTATIONS

typedef long SwiftInt;
typedef unsigned long SwiftUInt;

typedef struct {
  const unsigned char *_Nullable data;
  long length;
} BridgedString;

typedef struct {
  const void *_Nullable data;
  long numElements;
} BridgedArrayRef;

typedef struct BridgedASTContext {
  void *raw;
} BridgedASTContext;

typedef struct BridgedDeclContext {
  void *raw;
} BridgedDeclContext;

typedef struct BridgedSourceLoc {
  const void *_Nullable raw;
} BridgedSourceLoc;

typedef struct BridgedIdentifier {
  const void *_Nullable raw;
} BridgedIdentifier;

typedef struct {
  void *start;
  SwiftInt byteLength;
} BridgedCharSourceRange;

typedef struct {
  BridgedIdentifier Name;
  BridgedSourceLoc NameLoc;
  BridgedIdentifier SecondName;
  BridgedSourceLoc SecondNameLoc;
  BridgedSourceLoc UnderscoreLoc;
  BridgedSourceLoc ColonLoc;
  void *Type;
  BridgedSourceLoc TrailingCommaLoc;
} BridgedTupleTypeElement;

typedef enum ENUM_EXTENSIBILITY_ATTR(open) BridgedRequirementReprKind : long {
  /// A type bound T : P, where T is a type that depends on a generic
  /// parameter and P is some type that should bound T, either as a concrete
  /// supertype or a protocol to which T must conform.
  BridgedRequirementReprKindTypeConstraint,

  /// A same-type requirement T == U, where T and U are types that shall be
  /// equivalent.
  BridgedRequirementReprKindSameType,

  /// A layout bound T : L, where T is a type that depends on a generic
  /// parameter and L is some layout specification that should bound T.
  BridgedRequirementReprKindLayoutConstraint,

  // Note: there is code that packs this enum in a 2-bit bitfield.  Audit users
  // when adding enumerators.
} BridgedRequirementReprKind;

typedef struct {
  BridgedSourceLoc SeparatorLoc;
  BridgedRequirementReprKind Kind;
  void *FirstType;
  void *SecondType;
  // FIXME: Handle Layout Requirements
} BridgedRequirementRepr;

/// Diagnostic severity when reporting diagnostics.
typedef enum ENUM_EXTENSIBILITY_ATTR(open) BridgedDiagnosticSeverity : long {
  BridgedFatalError,
  BridgedError,
  BridgedWarning,
  BridgedRemark,
  BridgedNote,
} BridgedDiagnosticSeverity;

typedef struct BridgedDiagnostic {
  void *raw;
} BridgedDiagnostic;

typedef struct BridgedDiagnosticEngine {
  void *raw;
} BridgedDiagnosticEngine;

typedef enum ENUM_EXTENSIBILITY_ATTR(open) BridgedMacroDefinitionKind : long {
  /// An expanded macro.
  BridgedExpandedMacro = 0,
  /// An external macro, spelled with either the old spelling (Module.Type)
  /// or the new spelling `#externalMacro(module: "Module", type: "Type")`.
  BridgedExternalMacro,
  /// The builtin definition for "externalMacro".
  BridgedBuiltinExternalMacro
} BridgedMacroDefinitionKind;

/// Bridged parameter specifiers
typedef enum ENUM_EXTENSIBILITY_ATTR(open) BridgedAttributedTypeSpecifier : long {
  BridgedAttributedTypeSpecifierInOut,
  BridgedAttributedTypeSpecifierBorrowing,
  BridgedAttributedTypeSpecifierConsuming,
  BridgedAttributedTypeSpecifierLegacyShared,
  BridgedAttributedTypeSpecifierLegacyOwned,
  BridgedAttributedTypeSpecifierConst,
  BridgedAttributedTypeSpecifierIsolated,
} BridgedAttributedTypeSpecifier;

// Bridged type attribute kinds, which mirror TypeAttrKind exactly.
typedef enum ENUM_EXTENSIBILITY_ATTR(closed) BridgedTypeAttrKind : long {
  BridgedTypeAttrKind_autoclosure,
  BridgedTypeAttrKind_convention,
  BridgedTypeAttrKind_noescape,
  BridgedTypeAttrKind_escaping,
  BridgedTypeAttrKind_differentiable,
  BridgedTypeAttrKind_noDerivative,
  BridgedTypeAttrKind_async,
  BridgedTypeAttrKind_Sendable,
  BridgedTypeAttrKind_unchecked,
  BridgedTypeAttrKind__local,
  BridgedTypeAttrKind__noMetadata,
  BridgedTypeAttrKind__opaqueReturnTypeOf,
  BridgedTypeAttrKind_block_storage,
  BridgedTypeAttrKind_box,
  BridgedTypeAttrKind_dynamic_self,
  BridgedTypeAttrKind_sil_weak,
  BridgedTypeAttrKind_sil_unowned,
  BridgedTypeAttrKind_sil_unmanaged,
  BridgedTypeAttrKind_error,
  BridgedTypeAttrKind_out,
  BridgedTypeAttrKind_direct,
  BridgedTypeAttrKind_in,
  BridgedTypeAttrKind_inout,
  BridgedTypeAttrKind_inout_aliasable,
  BridgedTypeAttrKind_in_guaranteed,
  BridgedTypeAttrKind_in_constant,
  BridgedTypeAttrKind_pack_owned,
  BridgedTypeAttrKind_pack_guaranteed,
  BridgedTypeAttrKind_pack_inout,
  BridgedTypeAttrKind_pack_out,
  BridgedTypeAttrKind_owned,
  BridgedTypeAttrKind_unowned_inner_pointer,
  BridgedTypeAttrKind_guaranteed,
  BridgedTypeAttrKind_autoreleased,
  BridgedTypeAttrKind_callee_owned,
  BridgedTypeAttrKind_callee_guaranteed,
  BridgedTypeAttrKind_objc_metatype,
  BridgedTypeAttrKind_opened,
  BridgedTypeAttrKind_pack_element,
  BridgedTypeAttrKind_pseudogeneric,
  BridgedTypeAttrKind_yields,
  BridgedTypeAttrKind_yield_once,
  BridgedTypeAttrKind_yield_many,
  BridgedTypeAttrKind_captures_generics,
  BridgedTypeAttrKind_moveOnly,
  BridgedTypeAttrKind_thin,
  BridgedTypeAttrKind_thick,
  BridgedTypeAttrKind_Count
} BridgedTypeAttrKind;

typedef enum ENUM_EXTENSIBILITY_ATTR(open) ASTNodeKind : long {
  ASTNodeKindExpr,
  ASTNodeKindStmt,
  ASTNodeKindDecl
} ASTNodeKind;

typedef struct BridgedASTNode {
  void *ptr;
  ASTNodeKind kind;
} BridgedASTNode;

typedef struct BridgedFuncDecl {
  BridgedDeclContext declContext;
  void *funcDecl;
  void *decl;
} BridgedFuncDecl;

typedef struct BridgedDeclContextAndDecl {
  BridgedDeclContext declContext;
  void *nominalDecl;
  void *decl;
} BridgedDeclContextAndDecl;

typedef struct BridgedTypeAttributes {
  void *raw;
} BridgedTypeAttributes;

#ifdef __cplusplus
extern "C" {

#define _Bool bool
#endif

// Diagnostics

/// Create a new diagnostic with the given severity, location, and diagnostic
/// text.
///
/// \returns a diagnostic instance that can be extended with additional
/// information and then must be finished via \c SwiftDiagnostic_finish.
BridgedDiagnostic Diagnostic_create(BridgedDiagnosticEngine cDiags,
                                    BridgedDiagnosticSeverity severity,
                                    BridgedSourceLoc cLoc, BridgedString cText);

/// Highlight a source range as part of the diagnostic.
void Diagnostic_highlight(BridgedDiagnostic cDiag, BridgedSourceLoc cStartLoc,
                          BridgedSourceLoc cEndLoc);

/// Add a Fix-It to replace a source range as part of the diagnostic.
void Diagnostic_fixItReplace(BridgedDiagnostic cDiag,
                             BridgedSourceLoc cStartLoc,
                             BridgedSourceLoc cEndLoc,
                             BridgedString cReplaceText);

/// Finish the given diagnostic and emit it.
void Diagnostic_finish(BridgedDiagnostic cDiag);

BridgedIdentifier ASTContext_getIdentifier(BridgedASTContext cContext,
                                           BridgedString cStr);

void *ImportDecl_create(BridgedASTContext cContext,
                        BridgedDeclContext cDeclContext,
                        BridgedSourceLoc cImportLoc, char kind,
                        BridgedSourceLoc cKindLoc, BridgedArrayRef path,
                        BridgedArrayRef cPathLocs);

void *TopLevelCodeDecl_createStmt(BridgedASTContext cContext,
                                  BridgedDeclContext cDeclContext,
                                  BridgedSourceLoc cStartLoc, void *element,
                                  BridgedSourceLoc cEndLoc);
void *TopLevelCodeDecl_createExpr(BridgedASTContext cContext,
                                  BridgedDeclContext cDeclContext,
                                  BridgedSourceLoc cStartLoc, void *element,
                                  BridgedSourceLoc cEndLoc);

void *ReturnStmt_create(BridgedASTContext cContext, BridgedSourceLoc cLoc,
                        void *_Nullable expr);

void *SequenceExpr_create(BridgedASTContext cContext, BridgedArrayRef exprs);

void *TupleExpr_create(BridgedASTContext cContext, BridgedSourceLoc cLParen,
                       BridgedArrayRef subs, BridgedArrayRef names,
                       BridgedArrayRef cNameLocs, BridgedSourceLoc cRParen);

void *FunctionCallExpr_create(BridgedASTContext cContext, void *fn, void *args);

void *IdentifierExpr_create(BridgedASTContext cContext, BridgedIdentifier base,
                            BridgedSourceLoc cLoc);

void *StringLiteralExpr_create(BridgedASTContext cContext, BridgedString cStr,
                               BridgedSourceLoc cTokenLoc);

void *IntegerLiteralExpr_create(BridgedASTContext cContext, BridgedString cStr,
                                BridgedSourceLoc cTokenLoc);

void *BooleanLiteralExpr_create(BridgedASTContext cContext, _Bool value,
                                BridgedSourceLoc cTokenLoc);

void *ArrayExpr_create(BridgedASTContext cContext, BridgedSourceLoc cLLoc,
                       BridgedArrayRef elements, BridgedArrayRef commas,
                       BridgedSourceLoc cRLoc);

void *VarDecl_create(BridgedASTContext cContext, void *nameExpr, void *initExpr,
                     BridgedSourceLoc cLoc, _Bool isStatic, _Bool isLet,
                     BridgedDeclContext cDeclContext);

void *SingleValueStmtExpr_createWithWrappedBranches(
    BridgedASTContext cContext, void *S, BridgedDeclContext cDeclContext,
    _Bool mustBeExpr);

void *IfStmt_create(BridgedASTContext cContext, BridgedSourceLoc cIfLoc,
                    void *cond, void *_Nullable then, BridgedSourceLoc cElseLoc,
                    void *_Nullable elseStmt);

void *BraceStmt_create(BridgedASTContext cContext, BridgedSourceLoc cLBLoc,
                       BridgedArrayRef elements, BridgedSourceLoc cRBLoc);

BridgedSourceLoc SourceLoc_advanced(BridgedSourceLoc cLoc, long len);

void *ParamDecl_create(BridgedASTContext cContext, BridgedSourceLoc cLoc,
                       BridgedSourceLoc cArgLoc, BridgedIdentifier argName,
                       BridgedSourceLoc cParamLoc, BridgedIdentifier paramName,
                       void *_Nullable type, BridgedDeclContext cDeclContext);

struct BridgedFuncDecl
FuncDecl_create(BridgedASTContext cContext, BridgedSourceLoc cStaticLoc,
                _Bool isStatic, BridgedSourceLoc cFuncLoc,
                BridgedIdentifier name, BridgedSourceLoc cNameLoc,
                _Bool isAsync, BridgedSourceLoc cAsyncLoc, _Bool throws,
                BridgedSourceLoc cThrowsLoc, BridgedSourceLoc cParamLLoc,
                BridgedArrayRef params, BridgedSourceLoc cParamRLoc,
                void *_Nullable returnType, BridgedDeclContext cDeclContext);
void FuncDecl_setBody(void *fn, void *body);

void *SimpleIdentTypeRepr_create(BridgedASTContext cContext,
                                 BridgedSourceLoc cLoc, BridgedIdentifier id);

void *UnresolvedDotExpr_create(BridgedASTContext cContext, void *base,
                               BridgedSourceLoc cDotLoc, BridgedIdentifier name,
                               BridgedSourceLoc cNameLoc);

void *ClosureExpr_create(BridgedASTContext cContext, void *body,
                         BridgedDeclContext cDeclContext);

void NominalTypeDecl_setMembers(void *decl, BridgedArrayRef members);

BridgedDeclContextAndDecl StructDecl_create(BridgedASTContext cContext,
                                            BridgedSourceLoc cLoc,
                                            BridgedIdentifier name,
                                            BridgedSourceLoc cNameLoc,
                                            void *_Nullable genericParams,
                                            BridgedDeclContext cDeclContext);
BridgedDeclContextAndDecl ClassDecl_create(BridgedASTContext cContext,
                                           BridgedSourceLoc cLoc,
                                           BridgedIdentifier name,
                                           BridgedSourceLoc cNameLoc,
                                           BridgedDeclContext cDeclContext);

void *GenericParamList_create(BridgedASTContext cContext,
                              BridgedSourceLoc cLAngleLoc,
                              BridgedArrayRef params,
                              BridgedSourceLoc cWhereLoc, BridgedArrayRef reqs,
                              BridgedSourceLoc cRAngleLoc);
void *GenericTypeParamDecl_create(BridgedASTContext cContext,
                                  BridgedDeclContext cDeclContext,
                                  BridgedIdentifier name,
                                  BridgedSourceLoc cNameLoc,
                                  BridgedSourceLoc cEachLoc, long index,
                                  _Bool isParameterPack);
void GenericTypeParamDecl_setInheritedType(BridgedASTContext cContext,
                                           void *Param, void *ty);

BridgedDeclContextAndDecl TypeAliasDecl_create(BridgedASTContext cContext,
                                               BridgedDeclContext cDeclContext,
                                               BridgedSourceLoc cAliasLoc,
                                               BridgedSourceLoc cEqualLoc,
                                               BridgedIdentifier name,
                                               BridgedSourceLoc cNameLoc,
                                               void *_Nullable genericParams);
void TypeAliasDecl_setUnderlyingTypeRepr(void *decl, void *underlyingType);

BridgedTypeAttrKind TypeAttrKind_fromString(BridgedString cStr);
BridgedTypeAttributes TypeAttributes_create(void);
void TypeAttributes_addSimpleAttr(BridgedTypeAttributes cAttributes,
                                  BridgedTypeAttrKind kind,
                                  BridgedSourceLoc cAtLoc,
                                  BridgedSourceLoc cAttrLoc);

void *ArrayTypeRepr_create(BridgedASTContext cContext, void *base,
                           BridgedSourceLoc cLSquareLoc,
                           BridgedSourceLoc cRSquareLoc);
void *AttributedTypeRepr_create(BridgedASTContext cContext, void *base,
                                BridgedTypeAttributes cAttributes);
void *
AttributedTypeSpecifierRepr_create(BridgedASTContext cContext, void *base,
                                   BridgedAttributedTypeSpecifier specifier,
                                   BridgedSourceLoc cSpecifierLoc);
void *CompositionTypeRepr_create(BridgedASTContext cContext,
                                 BridgedArrayRef types,
                                 BridgedSourceLoc cFirstTypeLoc,
                                 BridgedSourceLoc cFirstAmpLoc);
void *DictionaryTypeRepr_create(BridgedASTContext cContext, void *keyType,
                                void *valueType, BridgedSourceLoc cLSquareLoc,
                                BridgedSourceLoc cColonloc,
                                BridgedSourceLoc cRSquareLoc);
void *EmptyCompositionTypeRepr_create(BridgedASTContext cContext,
                                      BridgedSourceLoc cAnyLoc);
void *FunctionTypeRepr_create(BridgedASTContext cContext, void *argsTy,
                              BridgedSourceLoc cAsyncLoc,
                              BridgedSourceLoc cThrowsLoc,
                              BridgedSourceLoc cArrowLoc, void *returnType);
void *GenericIdentTypeRepr_create(BridgedASTContext cContext,
                                  BridgedIdentifier name,
                                  BridgedSourceLoc cNameLoc,
                                  BridgedArrayRef genericArgs,
                                  BridgedSourceLoc cLAngleLoc,
                                  BridgedSourceLoc cRAngleLoc);
void *OptionalTypeRepr_create(BridgedASTContext cContext, void *base,
                              BridgedSourceLoc cQuestionLoc);
void *ImplicitlyUnwrappedOptionalTypeRepr_create(
    BridgedASTContext cContext, void *base, BridgedSourceLoc cExclamationLoc);
void *MemberTypeRepr_create(BridgedASTContext cContext, void *baseComponent,
                            BridgedArrayRef bridgedMemberComponents);
void *MetatypeTypeRepr_create(BridgedASTContext cContext, void *baseType,
                              BridgedSourceLoc cTypeLoc);
void *ProtocolTypeRepr_create(BridgedASTContext cContext, void *baseType,
                              BridgedSourceLoc cProtoLoc);
void *PackExpansionTypeRepr_create(BridgedASTContext cContext, void *base,
                                   BridgedSourceLoc cRepeatLoc);
void *TupleTypeRepr_create(BridgedASTContext cContext, BridgedArrayRef elements,
                           BridgedSourceLoc cLParenLoc,
                           BridgedSourceLoc cRParenLoc);
void *NamedOpaqueReturnTypeRepr_create(BridgedASTContext cContext,
                                       void *baseTy);
void *OpaqueReturnTypeRepr_create(BridgedASTContext cContext,
                                  BridgedSourceLoc cOpaqueLoc, void *baseTy);
void *ExistentialTypeRepr_create(BridgedASTContext cContext,
                                 BridgedSourceLoc cAnyLoc, void *baseTy);
void *VarargTypeRepr_create(BridgedASTContext cContext, void *base,
                            BridgedSourceLoc cEllipsisLoc);

void TopLevelCodeDecl_dump(void *decl);
void Expr_dump(void *expr);
void Decl_dump(void *decl);
void Stmt_dump(void *statement);
void Type_dump(void *type);

//===----------------------------------------------------------------------===//
// Plugins
//===----------------------------------------------------------------------===//

typedef void *PluginHandle;
typedef const void *PluginCapabilityPtr;

/// Set a capability data to the plugin object. Since the data is just a opaque
/// pointer, it's not used in AST at all.
void Plugin_setCapability(PluginHandle handle, PluginCapabilityPtr data);

/// Get a capability data set by \c Plugin_setCapability .
PluginCapabilityPtr _Nullable Plugin_getCapability(PluginHandle handle);

/// Get the executable file path of the plugin.
const char *Plugin_getExecutableFilePath(PluginHandle handle);

/// Lock the plugin. Clients should lock it during sending and recving the
/// response.
void Plugin_lock(PluginHandle handle);

/// Unlock the plugin.
void Plugin_unlock(PluginHandle handle);

/// Launch the plugin if it's not running.
_Bool Plugin_spawnIfNeeded(PluginHandle handle);

/// Sends the message to the plugin, returns true if there was an error.
/// Clients should receive the response  by \c Plugin_waitForNextMessage .
_Bool Plugin_sendMessage(PluginHandle handle, const BridgedData data);

/// Receive a message from the plugin.
_Bool Plugin_waitForNextMessage(PluginHandle handle, BridgedData *data);

#ifdef __cplusplus
}
#endif

SWIFT_END_NULLABILITY_ANNOTATIONS

#undef SWIFT_BEGIN_NULLABILITY_ANNOTATIONS
#undef SWIFT_END_NULLABILITY_ANNOTATIONS

#endif // SWIFT_C_AST_ASTBRIDGING_H
