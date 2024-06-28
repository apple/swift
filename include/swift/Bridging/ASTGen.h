//===--- ASTGen.h -----------------------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTBridging.h"
#include "swift/Parse/ParseBridging.h"

#ifdef __cplusplus
extern "C" {
#endif

void *_Nonnull swift_ASTGen_createQueuedDiagnostics();
void swift_ASTGen_destroyQueuedDiagnostics(void *_Nonnull queued);
void swift_ASTGen_addQueuedSourceFile(
    void *_Nonnull queuedDiagnostics, ssize_t bufferID,
    void *_Nonnull sourceFile, const uint8_t *_Nonnull displayNamePtr,
    intptr_t displayNameLength, ssize_t parentID, ssize_t positionInParent);
void swift_ASTGen_addQueuedDiagnostic(
    void *_Nonnull queued, const char *_Nonnull text, ptrdiff_t textLength,
    BridgedDiagnosticSeverity severity, const void *_Nullable sourceLoc,
    const void *_Nullable *_Nullable highlightRanges,
    ptrdiff_t numHighlightRanges);
void swift_ASTGen_renderQueuedDiagnostics(
    void *_Nonnull queued, ssize_t contextSize, ssize_t colorize,
    BridgedStringRef *_Nonnull renderedString);

// FIXME: Hack because we cannot easily get to the already-parsed source
// file from here. Fix this egregious oversight!
void *_Nullable swift_ASTGen_parseSourceFile(const char *_Nonnull buffer,
                                             size_t bufferLength,
                                             const char *_Nonnull moduleName,
                                             const char *_Nonnull filename,
                                             void *_Nullable ctx);
void swift_ASTGen_destroySourceFile(void *_Nonnull sourceFile);

/// Check whether the given source file round-trips correctly. Returns 0 if
/// round-trip succeeded, non-zero otherwise.
int swift_ASTGen_roundTripCheck(void *_Nonnull sourceFile);

/// Emit parser diagnostics for given source file.. Returns non-zero if any
/// diagnostics were emitted.
int swift_ASTGen_emitParserDiagnostics(
    void *_Nonnull diagEngine, void *_Nonnull sourceFile, int emitOnlyErrors,
    int downgradePlaceholderErrorsToWarnings);

// Build AST nodes for the top-level entities in the syntax.
void swift_ASTGen_buildTopLevelASTNodes(
    BridgedDiagnosticEngine diagEngine, void *_Nonnull sourceFile,
    BridgedDeclContext declContext, BridgedASTContext astContext,
    BridgedLegacyParser legacyParser, void *_Nonnull outputContext,
    void (*_Nonnull)(void *_Nonnull, void *_Nonnull));

void swift_ASTGen_freeBridgedString(BridgedStringRef);

void *_Nonnull swift_ASTGen_resolveExternalMacro(
    const char *_Nonnull moduleName, const char *_Nonnull typeName,
    void *_Nonnull opaquePluginHandle);
void swift_ASTGen_destroyExternalMacro(void *_Nonnull macro);

bool swift_ASTGen_checkDefaultArgumentMacroExpression(
    void *_Nonnull diagEngine, void *_Nonnull sourceFile,
    const void *_Nonnull macroSourceLocation);

ptrdiff_t swift_ASTGen_checkMacroDefinition(
    void *_Nonnull diagEngine, BridgedStringRef sourceFileBuffer,
    BridgedStringRef macroDeclText,
    BridgedStringRef *_Nonnull expansionSourceOutPtr,
    ptrdiff_t *_Nullable *_Nonnull replacementsPtr,
    ptrdiff_t *_Nonnull numReplacements,
    ptrdiff_t *_Nullable *_Nonnull genericReplacementsPtr,
    ptrdiff_t *_Nonnull numGenericReplacements);
void swift_ASTGen_freeExpansionReplacements(
    ptrdiff_t *_Nullable replacementsPtr, ptrdiff_t numReplacements);

ptrdiff_t swift_ASTGen_expandFreestandingMacro(
    void *_Nonnull diagEngine, const void *_Nonnull macro,
    const char *_Nonnull discriminator, uint8_t rawMacroRole,
    void *_Nonnull sourceFile, const void *_Nullable sourceLocation,
    BridgedStringRef *_Nonnull evaluatedSourceOut);

ptrdiff_t swift_ASTGen_expandAttachedMacro(
    void *_Nonnull diagEngine, const void *_Nonnull macro,
    const char *_Nonnull discriminator, const char *_Nonnull qualifiedType,
    const char *_Nonnull conformances, uint8_t rawMacroRole,
    void *_Nonnull customAttrSourceFile,
    const void *_Nullable customAttrSourceLocation,
    void *_Nonnull declarationSourceFile,
    const void *_Nullable declarationSourceLocation,
    void *_Nullable parentDeclSourceFile,
    const void *_Nullable parentDeclSourceLocation,
    BridgedStringRef *_Nonnull evaluatedSourceOut);

bool swift_ASTGen_initializePlugin(void *_Nonnull handle,
                                   void *_Nullable diagEngine);
void swift_ASTGen_deinitializePlugin(void *_Nonnull handle);
bool swift_ASTGen_pluginServerLoadLibraryPlugin(
    void *_Nonnull handle, const char *_Nonnull libraryPath,
    const char *_Nonnull moduleName, BridgedStringRef *_Nullable errorOut);

/// Build a TypeRepr for AST node for the type at the given source location in
/// the specified file.
swift::TypeRepr *_Nullable swift_ASTGen_buildTypeRepr(
    BridgedDiagnosticEngine diagEngine, void *_Nonnull sourceFile,
    BridgedSourceLoc sourceLoc, BridgedDeclContext declContext,
    BridgedASTContext astContext, BridgedLegacyParser legacyParser,
    BridgedSourceLoc *_Nonnull endSourceLoc);

/// Build a Decl for AST node for the type at the given source location in the
/// specified file.
swift::Decl *_Nullable swift_ASTGen_buildDecl(
    BridgedDiagnosticEngine diagEngine, void *_Nonnull sourceFile,
    BridgedSourceLoc sourceLoc, BridgedDeclContext declContext,
    BridgedASTContext astContext, BridgedLegacyParser legacyParser,
    BridgedSourceLoc *_Nonnull endSourceLoc);

/// Build a Expr for AST node for the type at the given source location in the
/// specified file.
swift::Expr *_Nullable swift_ASTGen_buildExpr(
    BridgedDiagnosticEngine diagEngine, void *_Nonnull sourceFile,
    BridgedSourceLoc sourceLoc, BridgedDeclContext declContext,
    BridgedASTContext astContext, BridgedLegacyParser legacyParser,
    BridgedSourceLoc *_Nonnull endSourceLoc);

/// Build a Stmt for AST node for the type at the given source location in the
/// specified file.
swift::Stmt *_Nullable swift_ASTGen_buildStmt(
    BridgedDiagnosticEngine diagEngine, void *_Nonnull sourceFile,
    BridgedSourceLoc sourceLoc, BridgedDeclContext declContext,
    BridgedASTContext astContext, BridgedLegacyParser legacyParser,
    BridgedSourceLoc *_Nonnull endSourceLoc);

// MARK: - Regex parsing

bool swift_ASTGen_lexRegexLiteral(const char *_Nonnull *_Nonnull curPtrPtr,
                                  const char *_Nonnull bufferEndPtr,
                                  bool mustBeRegex,
                                  BridgedNullableDiagnosticEngine diagEngine);

bool swift_ASTGen_parseRegexLiteral(BridgedStringRef inputPtr,
                                    size_t *_Nonnull versionOut,
                                    void *_Nonnull UnsafeMutableRawPointer,
                                    size_t captureStructureSize,
                                    BridgedSourceLoc diagLoc,
                                    BridgedDiagnosticEngine diagEngine);

#ifdef __cplusplus
}
#endif
