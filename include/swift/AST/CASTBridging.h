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
  const void *_Nullable data;
  long numElements;
} BridgedArrayRef;

typedef struct {
  void *start;
  SwiftInt byteLength;
} BridgedCharSourceRange;

typedef void *BridgedIdentifier;

typedef struct {
  BridgedIdentifier _Nullable Name;
  void *_Nullable NameLoc;
  BridgedIdentifier _Nullable SecondName;
  void *_Nullable SecondNameLoc;
  void *_Nullable UnderscoreLoc;
  void *_Nullable ColonLoc;
  void *Type;
  void *_Nullable TrailingCommaLoc;
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
  void *_Nullable SeparatorLoc;
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

typedef void* BridgedDiagnostic;

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
BridgedDiagnostic SwiftDiagnostic_create(void *diagnosticEngine,
                                         BridgedDiagnosticSeverity severity,
                                         const void *_Nullable sourceLoc,
                                         const unsigned char *_Nullable text,
                                         long textLen);

/// Highlight a source range as part of the diagnostic.
void SwiftDiagnostic_highlight(
    BridgedDiagnostic diag, const void *_Nullable startLoc, const void *_Nullable endLoc);

/// Add a Fix-It to replace a source range as part of the diagnostic.
void SwiftDiagnostic_fixItReplace(BridgedDiagnostic diag,
                                  const void *_Nullable replaceStartLoc,
                                  const void *_Nullable replaceEndLoc,
                                  const unsigned char *_Nullable newText,
                                  long newTextLen);

/// Finish the given diagnostic and emit it.
void SwiftDiagnostic_finish(BridgedDiagnostic diag);

BridgedIdentifier
SwiftASTContext_getIdentifier(void *ctx, const unsigned char *_Nullable str,
                              long len);

void *SwiftImportDecl_create(void *, void *, void *, char, void *,
                             BridgedArrayRef, BridgedArrayRef);

void *SwiftTopLevelCodeDecl_createStmt(void *ctx, void *DC, void *startLoc,
                                       void *element, void *endLoc);
void *SwiftTopLevelCodeDecl_createExpr(void *ctx, void *DC, void *startLoc,
                                       void *element, void *endLoc);

void *ReturnStmt_create(void *ctx, void *loc, void *_Nullable expr);

void *SwiftSequenceExpr_create(void *ctx, BridgedArrayRef exprs);

void *SwiftTupleExpr_create(void *ctx, void *lparen, BridgedArrayRef subs,
                            BridgedArrayRef names,
                            BridgedArrayRef nameLocs,
                            void *rparen);

void *SwiftFunctionCallExpr_create(void *ctx, void *fn, void *args);

void *SwiftIdentifierExpr_create(void *ctx, BridgedIdentifier base, void *loc);

void *SwiftStringLiteralExpr_create(void *ctx,
                                    const unsigned char *_Nullable string,
                                    long len, void *TokenLoc);

void *SwiftIntegerLiteralExpr_create(void *ctx,
                                     const unsigned char *_Nullable string,
                                     long len, void *TokenLoc);

void *SwiftBooleanLiteralExpr_create(void *ctx, _Bool value, void *TokenLoc);

void *ArrayExpr_create(void *ctx, void *lLoc, BridgedArrayRef elements,
                       BridgedArrayRef commas, void *rLoc);

void *SwiftVarDecl_create(void *ctx, BridgedIdentifier _Nullable name,
                          void *initExpr, void *loc, _Bool isStatic,
                          _Bool isLet, void *dc);

void *SingleValueStmtExpr_createWithWrappedBranches(void *ctx, void *S,
                                                    void *DC, _Bool mustBeExpr);

void *IfStmt_create(void *ctx, void *ifLoc, void *cond, void *_Nullable then,
                    void *_Nullable elseLoc, void *_Nullable elseStmt);

typedef enum ENUM_EXTENSIBILITY_ATTR(open) ASTNodeKind : long {
  ASTNodeKindExpr,
  ASTNodeKindStmt,
  ASTNodeKindDecl
} ASTNodeKind;

struct ASTNodeBridged {
  void *ptr;
  ASTNodeKind kind;
};

void *BraceStmt_create(void *ctx, void *lbloc, BridgedArrayRef elements,
                       void *rbloc);

void *BridgedSourceLoc_advanced(void *loc, long len);

void *ParamDecl_create(void *ctx, void *loc, void *_Nullable argLoc,
                       void *_Nullable argName, void *_Nullable paramLoc,
                       void *_Nullable paramName, void *_Nullable type,
                       void *declContext);
struct FuncDeclBridged {
  void *declContext;
  void *funcDecl;
  void *decl;
};

struct FuncDeclBridged
FuncDecl_create(void *ctx, void *staticLoc, _Bool isStatic, void *funcLoc,
                BridgedIdentifier name, void *nameLoc, _Bool isAsync,
                void *_Nullable asyncLoc, _Bool throws,
                void *_Nullable throwsLoc, void *paramLLoc,
                BridgedArrayRef params, void *paramRLoc,
                void *_Nullable returnType, void *declContext);
void FuncDecl_setBody(void *fn, void *body);

void *SimpleIdentTypeRepr_create(void *ctx, void *loc, BridgedIdentifier id);

void *UnresolvedDotExpr_create(void *ctx, void *base, void *dotLoc,
                               BridgedIdentifier name, void *nameLoc);

void *ClosureExpr_create(void *ctx, void *body, void *dc);

void NominalTypeDecl_setMembers(void *decl, BridgedArrayRef members);

struct DeclContextAndDecl {
  void *declContext;
  void *nominalDecl;
  void *decl;
};

struct DeclContextAndDecl
StructDecl_create(void *ctx, void *loc, BridgedIdentifier name, void *nameLoc,
                  void *_Nullable genericParams, void *dc);
struct DeclContextAndDecl ClassDecl_create(void *ctx, void *loc,
                                           BridgedIdentifier name,
                                           void *nameLoc, void *dc);

void *ArrayTypeRepr_create(void *ctx, void *base, void *lsquareLoc,
                           void *rsquareLoc);
void *DictionaryTypeRepr_create(void *ctx, void *keyType, void *valueType,
                                void *lsquareLoc, void *colonloc,
                                void *rsquareLoc);
void *OptionalTypeRepr_create(void *ctx, void *base, void *questionLoc);
void *ImplicitlyUnwrappedOptionalTypeRepr_create(void *ctx, void *base,
                                                 void *exclamationLoc);
void *MetatypeTypeRepr_create(void *ctx, void *baseType, void *typeLoc);
void *ProtocolTypeRepr_create(void *ctx, void *baseType, void *protoLoc);

BridgedTypeAttrKind getBridgedTypeAttrKindFromString(
    const unsigned char * _Nullable str, long len);

typedef void *BridgedTypeAttributes;
BridgedTypeAttributes BridgedTypeAttributes_create(void);
void BridgedTypeAttributes_addSimpleAttr(
    BridgedTypeAttributes typeAttributes, BridgedTypeAttrKind kind, void *atLoc, void *attrLoc);
void *AttributedTypeRepr_create(void *ctx, void *base, BridgedTypeAttributes typeAttributes);

void *AttributedTypeSpecifierRepr_create(
    void *ctx, void *base, BridgedAttributedTypeSpecifier specifier, void *specifierLoc);
void *VarargTypeRepr_create(void *ctx, void *base, void *ellipsisLocPtr);
void *PackExpansionTypeRepr_create(void *ctx, void *base, void *repeatLoc);
void *TupleTypeRepr_create(void *ctx, BridgedArrayRef elements, void *lParenLoc,
                           void *rParenLoc);
void *MemberTypeRepr_create(void *ctx, void *baseComponent,
                            BridgedArrayRef bridgedMemberComponents);
void *GenericIdentTypeRepr_create(void *ctx, BridgedIdentifier name,
                                  void *nameLoc, BridgedArrayRef genericArgs,
                                  void *lAngle, void *rAngle);
void *EmptyCompositionTypeRepr_create(void *ctx, void *anyLoc);
void *CompositionTypeRepr_create(void *ctx, BridgedArrayRef types,
                                 void *firstTypeLoc, void *firstAmpLoc);
void *FunctionTypeRepr_create(void *ctx, void *argsTy, void *_Nullable asyncLoc,
                              void *_Nullable throwsLoc, void *arrowLoc,
                              void *returnType);
void *NamedOpaqueReturnTypeRepr_create(void *ctx, void *baseTy);
void *OpaqueReturnTypeRepr_create(void *ctx, void *opaqueLoc, void *baseTy);
void *ExistentialTypeRepr_create(void *ctx, void *anyLoc, void *baseTy);
void *GenericParamList_create(void *ctx, void *lAngleLoc,
                              BridgedArrayRef params, void *_Nullable whereLoc,
                              BridgedArrayRef reqs, void *rAngleLoc);
void *GenericTypeParamDecl_create(void *ctx, void *declContext,
                                  BridgedIdentifier name, void *nameLoc,
                                  void *_Nullable eachLoc, long index,
                                  _Bool isParameterPack);
void GenericTypeParamDecl_setInheritedType(void *ctx, void *Param, void *ty);

struct DeclContextAndDecl TypeAliasDecl_create(void *ctx, void *declContext,
                                               void *aliasLoc, void *equalLoc,
                                               BridgedIdentifier name,
                                               void *nameLoc,
                                               void *_Nullable genericParams);
void TypeAliasDecl_setUnderlyingTypeRepr(void *decl, void *underlyingType);

void TopLevelCodeDecl_dump(void *);
void Expr_dump(void *);
void Decl_dump(void *);
void Stmt_dump(void *);
void Type_dump(void *);

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
