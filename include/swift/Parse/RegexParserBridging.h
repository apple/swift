//===-- RegexParserBridging.h --- Regex parser interface -*- C++ -*-===//
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


#ifndef REGEX_PARSER_BRIDGING
#define REGEX_PARSER_BRIDGING

#include "swift/AST/ASTBridging.h"
#include "swift/Basic/Nullability.h"
#include <stdbool.h>

/// Attempt to lex a regex literal string. Takes the following arguments:
///
/// - CurPtrPtr: A pointer to the current pointer of lexer, which should be the
///              start of the literal. This will be advanced to the point at
///              which the lexer should resume, or will remain the same if this
///              is not a regex literal.
/// - BufferEnd: A pointer to the end of the buffer, which should not be lexed
///              past.
/// - MustBeRegex: whether an error during lexing should be considered a regex
///                literal, or some thing else.
/// - BridgedOptionalDiagnosticEngine: RegexLiteralLexingFn should diagnose the
///                                    token using this engine.
///
/// Returns: A bool indicating whether lexing was completely erroneous, and
///          cannot be recovered from, or false if there either was no error,
///          or there was a recoverable error.
typedef bool (*RegexLiteralLexingFn)(
    /*CurPtrPtr*/ const char *_Nonnull *_Nonnull,
    /*BufferEnd*/ const char *_Nonnull,
    /*MustBeRegex*/ bool, BridgedOptionalDiagnosticEngine);
void Parser_registerRegexLiteralLexingFn(RegexLiteralLexingFn _Nullable fn);

/// Parse a regex literal string. Takes the following arguments:
///
/// - InputPtr: A null-terminated C string of the regex literal.
/// - VersionOut: A buffer accepting a regex literal format version.
/// - CaptureStructureOut: A buffer accepting a byte sequence representing the
///                        capture structure of the literal.
/// - CaptureStructureSize: The size of the capture structure buffer. Must be
///                         greater than or equal to `strlen(InputPtr) + 3`.
/// - DiagnosticBaseLoc: Start location of the regex literal.
/// - BridgedDiagnosticEngine: RegexLiteralParsingFn should diagnose the
///                            parsing errors using this engine.
///
/// Returns: A bool value indicating if there was an error while parsing.
typedef bool (*RegexLiteralParsingFn)(/*InputPtr*/ const char *_Nonnull,
                                      /*VersionOut*/ unsigned *_Nonnull,
                                      /*CaptureStructureOut*/ void *_Nonnull,
                                      /*CaptureStructureSize*/ unsigned,
                                      /*DiagnosticBaseLoc*/ swift::SourceLoc,
                                      BridgedDiagnosticEngine);
void Parser_registerRegexLiteralParsingFn(RegexLiteralParsingFn _Nullable fn);

#endif // REGEX_PARSER_BRIDGING
