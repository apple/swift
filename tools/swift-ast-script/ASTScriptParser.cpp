//===--- ASTScriptParser.cpp ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// AST script parsing.
///
//===----------------------------------------------------------------------===//

#include "swift/Basic/QuotedString.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/Token.h"
#include "llvm/Support/MemoryBuffer.h"

#include "ASTScript.h"
#include "ASTScriptConfiguration.h"

using namespace swift;
using namespace scripting;

namespace {

class ASTScriptParser {
  ASTScriptConfiguration &Config;
  ASTContext &Context;
  DiagnosticEngine &Diags;
  std::unique_ptr<llvm::MemoryBuffer> Buffer;
  llvm::Optional<Lexer> TheLexer;
  Token Tok;

public:
  ASTScriptParser(ASTScriptConfiguration &config)
    : Config(config), Context(config.Compiler.getASTContext()),
      Diags(config.Compiler.getDiags()) {

    auto &compiler = config.Compiler;

    // Open the file and give it to the source manager.
    auto bufferOrErr = llvm::MemoryBuffer::getFile(config.ScriptFile);
    if (!bufferOrErr) {
      llvm::errs() << "error opening file " << QuotedString(config.ScriptFile)
                   << ": " << bufferOrErr.getError().message() << "\n";
      return;
    }
    auto &sourceManager = compiler.getSourceMgr();
    auto bufferID = sourceManager.addNewSourceBuffer(std::move(*bufferOrErr));

    // Build and prime the lexer.
    TheLexer.emplace(Context.LangOpts, sourceManager, bufferID,
                     &Diags, LexerMode::Swift);
    TheLexer->lex(Tok);
  }

  std::unique_ptr<ASTScript> parseFile() {
    if (!TheLexer) return nullptr;
    return parseTopLevel();
  }

private:
  /***************************************************************************/
  /*** Parsing primitives ****************************************************/
  /***************************************************************************/

  void consume(tok kind) {
    assert(Tok.is(kind));
    TheLexer->lex(Tok);
  }

  bool consumeIf(tok kind) {
    if (Tok.isNot(kind)) return false;
    consume(kind);
    return true;
  }

  bool consumeIfExactly(StringRef literal) {
    if (Tok.isNot(tok::identifier) || Tok.getText() != literal)
      return false;
    consume(tok::identifier);
    return true;
  }

  bool consumeIfIdentifier(StringRef &ident) {
    if (Tok.isNot(tok::identifier)) return false;
    ident = Tok.getText();
    consume(tok::identifier);
    return true;
  }

  llvm::Optional<StringRef> consumeIfIdentifier() {
    StringRef ident;
    return consumeIfIdentifier(ident) ? llvm::Optional<StringRef>(ident)
                                      : llvm::None;
  }

  /***************************************************************************/
  /*** ASTScript parsing *****************************************************/
  /***************************************************************************/

  std::unique_ptr<ASTScript> parseTopLevel();
};

} // end anonymous namespace

/// ast-script ::= ???
std::unique_ptr<ASTScript> ASTScriptParser::parseTopLevel() {
  return std::unique_ptr<ASTScript>(new ASTScript(Config));
}

std::unique_ptr<ASTScript> ASTScript::parse(ASTScriptConfiguration &config) {
  return ASTScriptParser(config).parseFile();
}
