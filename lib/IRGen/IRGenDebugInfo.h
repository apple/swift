//===--- IRGenDebugInfo.h - Debug Info Support ------------------*- C++ -*-===//
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
// This file defines IR codegen support for debug information.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_DEBUGINFO_H
#define SWIFT_IRGEN_DEBUGINFO_H

#include <swift/SIL/SILInstruction.h>
#include "DebugTypeInfo.h"
#include "IRGenFunction.h"

namespace llvm {
class DIBuilder;
}

namespace swift {

class ClangImporter;
class IRGenOptions;

enum class SILFunctionTypeRepresentation : uint8_t;

namespace irgen {

class IRBuilder;
class IRGenFunction;
class IRGenModule;

enum IndirectionKind {
  DirectValue,
  IndirectValue,
  CoroDirectValue,
  CoroIndirectValue
};
enum ArtificialKind : bool { RealValue = false, ArtificialValue = true };

/// Used to signal to emitDbgIntrinsic that we actually want to emit dbg.declare
/// instead of dbg.addr. By default, we now emit llvm.dbg.addr instead of
/// llvm.dbg.declare for normal variables. This is not true for metadata which
/// truly are function wide and should be llvm.dbg.declare.
enum class AddrDbgInstrKind : bool {
  DbgDeclare,
  DbgAddr,
};

/// Helper object that keeps track of the current CompileUnit, File,
/// LexicalScope, and knows how to translate a \c SILLocation into an
/// \c llvm::DebugLoc.
class IRGenDebugInfo {
public:
  static std::unique_ptr<IRGenDebugInfo>
  createIRGenDebugInfo(const IRGenOptions &Opts, ClangImporter &CI,
                       IRGenModule &IGM, llvm::Module &M,
                       StringRef MainOutputFilenameForDebugInfo,
                       StringRef PrivateDiscriminator);
  virtual ~IRGenDebugInfo();

  /// Finalize the llvm::DIBuilder owned by this object.
  void finalize();

  /// Update the IRBuilder's current debug location to the location
  /// Loc and the lexical scope DS.
  void setCurrentLoc(IRBuilder &Builder, const SILDebugScope *DS,
                     SILLocation Loc);

  /// Replace the current debug location in \p Builder with the same location,
  /// but contained in an inlined function which is named like \p failureMsg.
  ///
  /// This lets the debugger display the \p failureMsg as an inlined function
  /// frame.
  void addFailureMessageToCurrentLoc(IRBuilder &Builder, StringRef failureMsg);

  void clearLoc(IRBuilder &Builder);

  /// Push the current debug location onto a stack and initialize the
  /// IRBuilder to an empty location.
  void pushLoc();

  /// Restore the current debug location from the stack.
  void popLoc();

  /// If we are not emitting CodeView, this does nothing since the ``llvm.trap``
  /// instructions should already have an artificial location of zero.
  /// In CodeView, since zero is not an artificial location, we emit the
  /// location of the unified trap block at the end of the function as an
  /// artificial inline location pointing to the user's instruction.
  void setInlinedTrapLocation(IRBuilder &Builder, const SILDebugScope *Scope);

  /// Set the location for entry point function (main by default).
  void setEntryPointLoc(IRBuilder &Builder);

  /// Return the scope for the entry point function (main by default).
  llvm::DIScope *getEntryPointFn();

  /// Translate a SILDebugScope into an llvm::DIDescriptor.
  llvm::DIScope *getOrCreateScope(const SILDebugScope *DS);

  
  /// Emit debug info for an import declaration.
  ///
  /// The DWARF output for import decls is similar to that of a using
  /// directive in C++:
  ///   import Foundation
  ///   -->
  ///   0: DW_TAG_imported_module
  ///        DW_AT_import(*1)
  ///   1: DW_TAG_module // instead of DW_TAG_namespace.
  ///        DW_AT_name("Foundation")
  ///
  void emitImport(ImportDecl *D);

  /// Emit debug info for the given function.
  /// \param DS The parent scope of the function.
  /// \param Fn The IR representation of the function.
  /// \param Rep The calling convention of the function.
  /// \param Ty The signature of the function.
  llvm::DISubprogram *emitFunction(const SILDebugScope *DS, llvm::Function *Fn,
                                   SILFunctionTypeRepresentation Rep,
                                   SILType Ty, DeclContext *DeclCtx = nullptr,
                                   GenericEnvironment *GE = nullptr);

  /// Emit debug info for a given SIL function.
  llvm::DISubprogram *emitFunction(SILFunction &SILFn, llvm::Function *Fn);

  /// Convenience function useful for functions without any source
  /// location. Internally calls emitFunction, emits a debug
  /// scope, and finally sets it using setCurrentLoc.
  inline void emitArtificialFunction(IRGenFunction &IGF, llvm::Function *Fn,
                                     SILType SILTy = SILType()) {
    emitArtificialFunction(IGF.Builder, Fn, SILTy);
  }

  void emitArtificialFunction(IRBuilder &Builder,
                              llvm::Function *Fn, SILType SILTy = SILType());

  inline void emitOutlinedFunction(IRGenFunction &IGF,
                                   llvm::Function *Fn,
                                   StringRef outlinedFromName) {
    emitOutlinedFunction(IGF.Builder, Fn, outlinedFromName);
  }

  void emitOutlinedFunction(IRBuilder &Builder,
                            llvm::Function *Fn,
                            StringRef outlinedFromName);

  /// Emit a dbg.declare intrinsic at the current insertion point and
  /// the Builder's current debug location.
  void emitVariableDeclaration(IRBuilder &Builder,
                               ArrayRef<llvm::Value *> Storage,
                               DebugTypeInfo Ty, const SILDebugScope *DS,
                               llvm::Optional<SILLocation> VarLoc,
                               SILDebugVariable VarInfo,
                               IndirectionKind Indirection = DirectValue,
                               ArtificialKind Artificial = RealValue,
                               AddrDbgInstrKind = AddrDbgInstrKind::DbgDeclare);

  /// Emit a dbg.addr or dbg.value intrinsic, depending on Storage. If \p
  /// ForceDbgDeclare is set to Yes, then instead of emitting a dbg.addr, we
  /// will insert a dbg.declare. Please only use that if you know that the given
  /// value can never be moved and have its lifetime ended early (e.x.: type
  /// metadata).
  void emitDbgIntrinsic(IRBuilder &Builder, llvm::Value *Storage,
                        llvm::DILocalVariable *Var, llvm::DIExpression *Expr,
                        unsigned Line, unsigned Col, llvm::DILocalScope *Scope,
                        const SILDebugScope *DS, bool InCoroContext = false,
                        AddrDbgInstrKind = AddrDbgInstrKind::DbgDeclare);

  /// Create debug metadata for a global variable.
  void emitGlobalVariableDeclaration(llvm::GlobalVariable *Storage,
                                     StringRef Name, StringRef LinkageName,
                                     DebugTypeInfo DebugType,
                                     bool IsLocalToUnit,
                                     llvm::Optional<SILLocation> Loc);

  /// Emit debug metadata for type metadata (for generic types). So meta.
  void emitTypeMetadata(IRGenFunction &IGF, llvm::Value *Metadata,
                        unsigned Depth, unsigned Index, StringRef Name);

  /// Emit debug info for the IR function parameter holding the size of one or
  /// more parameter / type packs.
  void emitPackCountParameter(IRGenFunction &IGF, llvm::Value *Metadata,
                              SILDebugVariable VarInfo);

  /// Return the DIBuilder.
  llvm::DIBuilder &getBuilder();

  /// Decode (and cache) a SourceLoc.
  SILLocation::FilenameAndLocation decodeSourceLoc(SourceLoc SL);
};

/// An RAII object that autorestores the debug location.
class AutoRestoreLocation {
  IRGenDebugInfo *DI;
  IRBuilder &Builder;
  llvm::DebugLoc SavedLocation;

public:
  AutoRestoreLocation(IRGenDebugInfo *DI, IRBuilder &Builder);
  /// Autorestore everything back to normal.
  ~AutoRestoreLocation();
};

/// An RAII object that temporarily switches to an artificial debug
/// location that has a valid scope, but no line information. This is
/// useful when emitting compiler-generated instructions (e.g.,
/// ARC-inserted calls to release()) that have no source location
/// associated with them. The DWARF specification allows the compiler
/// to use the special line number 0 to indicate code that cannot be
/// attributed to any source location.
class ArtificialLocation : public AutoRestoreLocation {
public:
  /// Set the current location to line 0, but within scope DS.
  ArtificialLocation(const SILDebugScope *DS, IRGenDebugInfo *DI,
                     IRBuilder &Builder);
};

/// An RAII object that temporarily switches to an empty
/// location. This is how the function prologue is represented.
class PrologueLocation : public AutoRestoreLocation {
public:
  /// Set the current location to an empty location.
  PrologueLocation(IRGenDebugInfo *DI, IRBuilder &Builder);
};

} // irgen
} // swift

#endif
