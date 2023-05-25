//===--- SILGlobalVariable.h - Defines SILGlobalVariable class --*- C++ -*-===//
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
// This file defines the SILGlobalVariable class.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILGLOBALVARIABLE_H
#define SWIFT_SIL_SILGLOBALVARIABLE_H

#include "swift/Basic/SwiftObjectHeader.h"
#include "swift/SIL/SILLinkage.h"
#include "swift/SIL/SILLocation.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILType.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/ilist.h"

namespace swift {

class ASTContext;
class SILFunction;
class SILInstruction;
class SILModule;
class VarDecl;
  
/// A global variable that has been referenced in SIL.
class SILGlobalVariable
  : public llvm::ilist_node<SILGlobalVariable>,
    public SILAllocated<SILGlobalVariable>,
    public SwiftObjectHeader
{
  static SwiftMetatype registeredMetatype;
    
public:
  using const_iterator = SILBasicBlock::const_iterator;

private:
  friend class SILModule;
  friend class SILBuilder;

  /// The SIL module that the global variable belongs to.
  SILModule &Module;
  
  /// The mangled name of the variable, which will be propagated to the
  /// binary.  A pointer into the module's lookup table.
  StringRef Name;

  /// The lowered type of the variable.
  SILType LoweredType;
  
  /// The SIL location of the variable, which provides a link back to the AST.
  /// The variable only gets a location after it's been emitted.
  const SILLocation Location;

  /// The linkage of the global variable.
  unsigned Linkage : NumSILLinkageBits;

  /// The global variable's serialized attribute.
  /// Serialized means that the variable can be "inlined" into another module.
  /// Currently this flag is set for all global variables in the stdlib.
  unsigned Serialized : 1;
  
  /// Whether this is a 'let' property, which can only be initialized
  /// once (either in its declaration, or once later), making it immutable.
  unsigned IsLet : 1;

  /// Whether or not this is a declaration.
  unsigned IsDeclaration : 1;

  /// Whether or not there is a valid SILLocation.
  unsigned HasLocation : 1;

  /// The VarDecl associated with this SILGlobalVariable. Must by nonnull for
  /// language-level global variables.
  VarDecl *VDecl;

  /// If this block is not empty, the global variable has a static initializer.
  ///
  /// The last instruction of this block is the top-level value of the static
  /// initializer.
  ///
  /// The block is just used as a container for the instructions. So the
  /// instructions still have a parent SILBasicBlock (but no parent function).
  /// It would be somehow cleaner to just store an instruction list here and
  /// make the SILGlobalVariable the parent pointer of the instructions.
  SILBasicBlock StaticInitializerBlock;

  SILGlobalVariable(SILModule &M, SILLinkage linkage,
                    IsSerialized_t IsSerialized,
                    StringRef mangledName, SILType loweredType,
                    Optional<SILLocation> loc, VarDecl *decl);
  
public:
  static void registerBridgedMetatype(SwiftMetatype metatype) {
    registeredMetatype = metatype;
  }

  static SILGlobalVariable *create(SILModule &Module, SILLinkage Linkage,
                                   IsSerialized_t IsSerialized,
                                   StringRef MangledName, SILType LoweredType,
                                   Optional<SILLocation> Loc = None,
                                   VarDecl *Decl = nullptr);

  ~SILGlobalVariable();

  SILModule &getModule() const { return Module; }

  SILType getLoweredType() const { return LoweredType; }
  CanSILFunctionType getLoweredFunctionType() const {
    return LoweredType.castTo<SILFunctionType>();
  }
  SILType getLoweredTypeInContext(TypeExpansionContext context) const;
  CanSILFunctionType
  getLoweredFunctionTypeInContext(TypeExpansionContext context) const {
    return getLoweredTypeInContext(context).castTo<SILFunctionType>();
  }

  void unsafeSetLoweredType(SILType newType) { LoweredType = newType; }
  void unsafeAppend(SILInstruction *i) { StaticInitializerBlock.push_back(i); }
  void unsafeRemove(SILInstruction *i, SILModule &mod) {
    StaticInitializerBlock.erase(i, mod);
  }

  StringRef getName() const { return Name; }
  
  void setDeclaration(bool isD) { IsDeclaration = isD; }

  /// True if this is a definition of the variable.
  bool isDefinition() const { return !IsDeclaration; }

  /// Get this function's linkage attribute.
  SILLinkage getLinkage() const { return SILLinkage(Linkage); }
  void setLinkage(SILLinkage linkage) { Linkage = unsigned(linkage); }

  /// Returns true if the linkage of the SILFunction indicates that the global
  /// might be referenced from outside the current compilation unit.
  bool isPossiblyUsedExternally() const;

  /// Get this global variable's serialized attribute.
  IsSerialized_t isSerialized() const;
  void setSerialized(IsSerialized_t isSerialized);
  
  /// Is this an immutable 'let' property?
  bool isLet() const { return IsLet; }
  void setLet(bool isLet) { IsLet = isLet; }

  VarDecl *getDecl() const { return VDecl; }

  /// Check if the function has a location.
  /// FIXME: All functions should have locations, so this method should not be
  /// necessary.
  bool hasLocation() const {
    return HasLocation;
  }

  /// Get the source location of the function.
  SILLocation getLocation() const {
    assert(HasLocation);
    return Location;
  }

  /// Returns the value of the static initializer or null if the global has no
  /// static initializer.
  SILInstruction *getStaticInitializerValue();

  /// Returns true if the global is a statically initialized heap object.
  bool isInitializedObject() {
    return dyn_cast_or_null<ObjectInst>(getStaticInitializerValue()) != nullptr;
  }

  const_iterator begin() const { return StaticInitializerBlock.begin(); }
  const_iterator end() const { return StaticInitializerBlock.end(); }

  void dropAllReferences() {
    StaticInitializerBlock.dropAllReferences();
  }

  void clear() {
    dropAllReferences();
    StaticInitializerBlock.eraseAllInstructions(Module);
  }

  /// Return whether this variable corresponds to a Clang node.
  bool hasClangNode() const;

  /// Return the Clang node associated with this variable if it has one.
  ClangNode getClangNode() const;

  const clang::Decl *getClangDecl() const;

  //===--------------------------------------------------------------------===//
  // Miscellaneous
  //===--------------------------------------------------------------------===//

  /// verify - Run the IR verifier to make sure that the variable follows
  /// invariants.
  void verify() const;
  
  /// Pretty-print the variable.
  void dump(bool Verbose) const;
  /// Pretty-print the variable.
  ///
  /// This is a separate entry point for ease of debugging.
  void dump() const LLVM_ATTRIBUTE_USED;

  /// Pretty-print the variable to the designated stream as a 'sil_global'
  /// definition.
  ///
  /// \param Verbose In verbose mode, print the SIL locations.
  void print(raw_ostream &OS, bool Verbose = false) const;
  
  /// Pretty-print the variable name using SIL syntax,
  /// '@var_mangled_name'.
  void printName(raw_ostream &OS) const;
  
  ASTContext &getASTContext() const;
};
  
inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                     const SILGlobalVariable &F) {
  F.print(OS);
  return OS;
}

} // end swift namespace

//===----------------------------------------------------------------------===//
// ilist_traits for SILGlobalVariable
//===----------------------------------------------------------------------===//

namespace llvm {

template <>
struct ilist_traits<::swift::SILGlobalVariable> :
public ilist_node_traits<::swift::SILGlobalVariable> {
  using SILGlobalVariable = ::swift::SILGlobalVariable;

public:
  static void deleteNode(SILGlobalVariable *V) { V->~SILGlobalVariable(); }
  
private:
  void createNode(const SILGlobalVariable &);
};

} // end llvm namespace

//===----------------------------------------------------------------------===//
// Utilities for verification and optimization.
//===----------------------------------------------------------------------===//

namespace swift {

/// Given an addressor, AddrF, return the global variable being addressed, or
/// return nullptr if the addressor isn't a recognized pattern.
SILGlobalVariable *getVariableOfGlobalInit(SILFunction *AddrF);

/// Return the callee of a once call.
SILFunction *getCalleeOfOnceCall(BuiltinInst *BI);

/// Helper for getVariableOfGlobalInit(), so GlobalOpts can deeply inspect and
/// rewrite the initialization pattern.
///
/// Given an addressor, AddrF, find the call to the global initializer if
/// present, otherwise return null. If an initializer is returned, then
/// `CallToOnce` is initialized to the corresponding builtin "once" call.
SILFunction *findInitializer(SILFunction *AddrF, BuiltinInst *&CallToOnce);

/// Helper for getVariableOfGlobalInit(), so GlobalOpts can deeply inspect and
/// rewrite the initialization pattern.
///
/// Given a global initializer, InitFunc, return the GlobalVariable that it
/// statically initializes or return nullptr if it isn't an obvious static
/// initializer.
SILGlobalVariable *getVariableOfStaticInitializer(SILFunction *InitFunc);

} // namespace swift

#endif
