//===--- StringOptimization.cpp - Optimize string operations --------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This pass performs several optimizations on String operations.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "string-optimization"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Analysis/ValueTracking.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/ASTMangler.h"
#include "swift/Demangling/Demangle.h"
#include "llvm/Support/Debug.h"

using namespace swift;

namespace {

/// Optimizes String operations with constant operands.

/// Specifically:
///   * Replaces x.append(y) with x = y if x is empty.
///   * Removes x.append("")
///   * Replaces x.append(y) with x = x + y if x and y are constant strings.
///   * Replaces _typeName(T.self) with a constant string if T is statically
///     known.
///   * Replaces String(literal).utf8CString with the string literal itself.
///
/// This pass must run on high-level SIL, where semantic calls are still in
/// place.
///
/// The optimization is implemented in a simple way. Therefore it cannot handle
/// complicated patterns, e.g. the dataflow analysis for the String.append self
/// argument is only done within a single block.
/// But this is totally sufficient to be able to constant propagate strings in
/// string interpolations.
///
/// If we want to make this optimization more powerful it's best done by using
/// the ConstExprStepEvaluator (which is currently lacking a few features to be
/// used for this optimization).
class StringOptimization {

  struct StringInfo {
    /// The string
    StringRef str;
    
    /// Negative means: not constant
    int reservedCapacity = 0;
    
    StringInfo(StringRef str, int reservedCapacity = 0) :
      str(str), reservedCapacity(reservedCapacity) { }
    
    bool isConstant() const { return reservedCapacity >= 0; }
    bool isEmpty() const { return isConstant() && str.empty(); }
    
    static StringInfo unknown() { return StringInfo(StringRef(), -1); }
  };

  /// The stdlib's String type.
  SILType stringType;
  
  /// The String initializer which takes an UTF8 string literal as argument.
  SILFunction *makeUTF8Func = nullptr;
  
  /// Caches the analysis result for an alloc_stack or an inout function
  /// argument, whether it is an "identifiable" object.
  /// See mayWriteToIdentifyableObject().
  llvm::DenseMap<SILValue, bool> identifiableObjectsCache;

public:
  bool run(SILFunction *F);

private:

  bool optimizeBlock(SILBasicBlock &block);
  
  bool optimizeStringAppend(ApplyInst *appendCall,
                            llvm::DenseMap<SILValue, SILValue> &storedStrings);
  bool optimizeStringConcat(ApplyInst *concatCall);
  bool optimizeTypeName(ApplyInst *typeNameCall);
  bool optimizeGetCString(ApplyInst *getCStringCall);

  static ApplyInst *isSemanticCall(SILInstruction *inst, StringRef attr,
                                   unsigned numArgs);
  StoreInst *isStringStoreToIdentifyableObject(SILInstruction *inst);
  static void invalidateModifiedObjects(SILInstruction *inst,
                            llvm::DenseMap<SILValue, SILValue> &storedStrings);
  static StringInfo getStringInfo(SILValue value);
  static StringInfo getStringFromStaticLet(SILValue value);

  static Optional<int> getIntConstant(SILValue value);
  static void replaceAppendWith(ApplyInst *appendCall, SILValue newValue);
  static SILValue copyValue(SILValue value, SILInstruction *before);
  ApplyInst *createStringInit(StringRef str, SILInstruction *beforeInst);
};

/// The main entry point of the optimization.
bool StringOptimization::run(SILFunction *F) {
  NominalTypeDecl *stringDecl = F->getModule().getASTContext().getStringDecl();
  if (!stringDecl)
    return false;
  stringType = SILType::getPrimitiveObjectType(
                 stringDecl->getDeclaredInterfaceType()->getCanonicalType());

  bool changed = false;
  
  for (SILBasicBlock &block : *F) {
    changed |= optimizeBlock(block);
  }
  return changed;
}

/// Run the optimization on a basic block.
bool StringOptimization::optimizeBlock(SILBasicBlock &block) {
  bool changed = false;
  
  /// Maps identifiable objects (alloc_stack, inout parameters) to string values
  /// which are stored in those objects.
  llvm::DenseMap<SILValue, SILValue> storedStrings;
  
  for (auto iter = block.begin(); iter != block.end();) {
    SILInstruction *inst = &*iter++;

    if (StoreInst *store = isStringStoreToIdentifyableObject(inst)) {
      storedStrings[store->getDest()] = store->getSrc();
      continue;
    }
    if (ApplyInst *append = isSemanticCall(inst, semantics::STRING_APPEND, 2)) {
      if (optimizeStringAppend(append, storedStrings)) {
        changed = true;
        continue;
      }
    }
    if (ApplyInst *append = isSemanticCall(inst, semantics::STRING_CONCAT, 3)) {
      if (optimizeStringConcat(append)) {
        changed = true;
        continue;
      }
    }
    if (ApplyInst *typeName = isSemanticCall(inst, semantics::TYPENAME, 2)) {
      if (optimizeTypeName(typeName)) {
        changed = true;
        continue;
      }
    }
    if (ApplyInst *getCString = isSemanticCall(inst,
                                       semantics::STRING_GET_UTF8_CSTRING, 1)) {
      if (optimizeGetCString(getCString)) {
        changed = true;
        continue;
      }
    }
    // Remove items from storedStrings if inst overwrites (or potentially
    // overwrites) a stored String in an identifiable object.
    invalidateModifiedObjects(inst, storedStrings);
  }
  return changed;
}

/// Optimize String.append in case anything is known about the parameters.
bool StringOptimization::optimizeStringAppend(ApplyInst *appendCall,
                            llvm::DenseMap<SILValue, SILValue> &storedStrings) {
  SILValue rhs = appendCall->getArgument(0);
  StringInfo rhsString = getStringInfo(rhs);
  
  // Remove lhs.append(rhs) if rhs is empty.
  if (rhsString.isEmpty()) {
    appendCall->eraseFromParent();
    return true;
  }
  
  SILValue lhsAddr = appendCall->getArgument(1);
  StringInfo lhsString = getStringInfo(storedStrings[lhsAddr]);

  // The following two optimizations are a trade-off: Performance-wise it may be
  // beneficial to initialize an empty string with reserved capacity and then
  // append multiple other string components.
  // Removing the empty string (with the reserved capacity) might result in more
  // allocations.
  // So we just do this optimization up to a certain capacity limit (found by
  // experiment).
  if (lhsString.reservedCapacity > 50)
    return false;

  // Replace lhs.append(rhs) with 'lhs = rhs' if lhs is empty.
  if (lhsString.isEmpty()) {
    replaceAppendWith(appendCall, copyValue(rhs, appendCall));
    storedStrings[lhsAddr] = rhs;
    return true;
  }
  
  // Replace lhs.append(rhs) with "lhs = lhs + rhs" if both lhs and rhs are
  // constant.
  if (lhsString.isConstant() && rhsString.isConstant()) {
    std::string concat = lhsString.str.str();
    concat += rhsString.str;
    if (ApplyInst *stringInit = createStringInit(concat, appendCall)) {
      replaceAppendWith(appendCall, stringInit);
      storedStrings[lhsAddr] = stringInit;
      return true;
    }
  }
  
  return false;
}

/// Optimize String.+ in case anything is known about the parameters.
bool StringOptimization::optimizeStringConcat(ApplyInst *concatCall) {
  SILValue lhs = concatCall->getArgument(0);
  SILValue rhs = concatCall->getArgument(1);
  StringInfo rhsString = getStringInfo(rhs);
  
  // Replace lhs + "" with lhs
  if (rhsString.isEmpty()) {
    lhs = copyValue(lhs, concatCall);
    concatCall->replaceAllUsesWith(lhs);
    concatCall->eraseFromParent();
    return true;
  }
  
  // Replace "" + rhs with rhs
  StringInfo lhsString = getStringInfo(lhs);
  if (lhsString.isEmpty()) {
    rhs = copyValue(rhs, concatCall);
    concatCall->replaceAllUsesWith(rhs);
    concatCall->eraseFromParent();
    return true;
  }

  // Replace lhs + rhs with "lhs + rhs" if both lhs and rhs are constant.
  if (lhsString.isConstant() && rhsString.isConstant()) {
    std::string concat = lhsString.str.str();
    concat += rhsString.str;
    if (ApplyInst *stringInit = createStringInit(concat, concatCall)) {
      concatCall->replaceAllUsesWith(stringInit);
      concatCall->eraseFromParent();
      return true;
    }
  }
  
  return false;
}

/// Checks if the demangling tree contains any node which prevents constant
/// folding of the type name.
static bool containsProblematicNode(Demangle::Node *node, bool qualified) {
  switch (node->getKind()) {
    case Demangle::Node::Kind::LocalDeclName:
      // The printing of contexts for local types is completely different
      // in the runtime. Don't constant fold if we need to print the context.
      if (qualified)
        return true;
      break;
    case Demangle::Node::Kind::Class: {
      // ObjC class names are not derived from the mangling but from the
      // ObjC runtime. We cannot constant fold this.
      Demangle::Node *context = node->getChild(0);
      if (context->getKind() == Demangle::Node::Kind::Module &&
          context->getText() == "__C") {
        return true;
      }
      break;
    }
    default:
      break;
  }
  for (Demangle::Node *child : *node) {
    if (containsProblematicNode(child, qualified))
      return true;
  }
  return false;
}

/// Try to replace a _typeName() call with a constant string if the type is
/// statically known.
bool StringOptimization::optimizeTypeName(ApplyInst *typeNameCall) {
  // Check, if the type is statically known.
  auto *anyType =
    dyn_cast<InitExistentialMetatypeInst>(typeNameCall->getArgument(0));
  if (!anyType)
    return false;
  auto *metatypeInst = dyn_cast<MetatypeInst>(anyType->getOperand());
  if (!metatypeInst)
    return false;

  auto metatype = metatypeInst->getType().getAs<MetatypeType>();
  Type ty = metatype->getInstanceType();
  if (ty->hasArchetype() || ty->hasDynamicSelfType())
    return false;
  
  // Usually the "qualified" parameter of _typeName() is a constant boolean.
  Optional<int> isQualifiedOpt = getIntConstant(typeNameCall->getArgument(1));
  if (!isQualifiedOpt)
    return false;
  bool isQualified = isQualifiedOpt.value();

  // Create the constant type string by mangling + demangling.
  Mangle::ASTMangler mangler;
  std::string mangledTypeName = mangler.mangleTypeForTypeName(ty);

  Demangle::DemangleOptions options;
  options.PrintForTypeName = true;
  options.DisplayLocalNameContexts = false;
  options.QualifyEntities = isQualified;

  Demangle::Context ctx;
  Demangle::NodePointer root = ctx.demangleTypeAsNode(mangledTypeName);
  if (!root || containsProblematicNode(root, isQualified))
    return false;

  std::string typeStr = nodeToString(root, options);
  if (typeStr.empty())
    return false;

  ApplyInst *stringInit = createStringInit(typeStr, typeNameCall);
  if (!stringInit)
    return false;

  typeNameCall->replaceAllUsesWith(stringInit);
  typeNameCall->eraseFromParent();

  return true;
}

/// Replaces a String initializer followed by String.utf8CString with a
/// (UTF8 encoded) string literal.
///
/// Note that string literals are always generated with a trailing 0-byte.
bool StringOptimization::optimizeGetCString(ApplyInst *getCStringCall) {
  // Is this a String.utf8CString of a literal String?
  StringInfo stringInfo = getStringInfo(getCStringCall->getArgument(0));
  if (!stringInfo.isConstant())
    return false;

  StringLiteralInst *literal = nullptr;
  bool changed = false;
  SmallVector<SILInstruction *, 16> workList;
  workList.push_back(getCStringCall);

  /// String.utf8CString returns an array of Int8. Search for ref_tail_addr of
  /// the array buffer.
  while (!workList.empty()) {
    SILInstruction *inst = workList.pop_back_val();
    // Look through string_extract which extract the buffer from the array.
    if (isa<StructExtractInst>(inst) || inst == getCStringCall) {
      for (Operand *use : cast<SingleValueInstruction>(inst)->getUses()) {
        workList.push_back(use->getUser());
      }
      continue;
    }
    if (auto *rta = dyn_cast<RefTailAddrInst>(inst)) {
      // Replace the ref_tail_addr with a pointer_to_address of the string
      // literal.
      if (!literal) {
        // Build the literal if we don't have one, yet.
        SILBuilder builder(getCStringCall);
        literal = builder.createStringLiteral(getCStringCall->getLoc(),
                    stringInfo.str, StringLiteralInst::Encoding::UTF8);
      }
      SILBuilder builder(rta);
      auto *strAddr = builder.createPointerToAddress(rta->getLoc(), literal,
                        rta->getType(), /*isStrict*/ false);
      rta->replaceAllUsesWith(strAddr);
      changed = true;
    }
  }
  return changed;
}

/// Returns the apply instruction if \p inst is a call of a function which has
/// a semantic attribute \p attr and exactly \p numArgs arguments.
ApplyInst *StringOptimization::isSemanticCall(SILInstruction *inst,
                                              StringRef attr, unsigned numArgs) {
  auto *apply = dyn_cast<ApplyInst>(inst);
  if (!apply || apply->getNumArguments() != numArgs)
    return nullptr;
    
  SILFunction *callee = apply->getReferencedFunctionOrNull();
  if (callee && callee->hasSemanticsAttr(attr))
    return apply;

  return nullptr;
}

/// Returns true for all instructions which we can safely analyze as a potential
/// write to an identifiable objects.
///
/// If we see any other kind of object user, which may write to an object, or
/// let the object address escape in some unexpected way (like address
/// projections), we'll just ignore that object and will not treat it as
/// "identifiable" object.
static bool mayWriteToIdentifyableObject(SILInstruction *inst) {
  // For simplicity, only handle store and apply. This is sufficient for most
  // case, especially for string interpolation.
  return isa<StoreInst>(inst) || isa<ApplyInst>(inst);
}

/// Returns the store instruction if \p inst is a store of a String to an
/// identifiable object.
StoreInst *StringOptimization::
isStringStoreToIdentifyableObject(SILInstruction *inst) {
  auto *store = dyn_cast<StoreInst>(inst);
  if (!store)
    return nullptr;
  if (store->getSrc()->getType() != stringType)
    return nullptr;

  SILValue destAddr = store->getDest();
  // We only handle alloc_stack an indirect function arguments. For those we can
  // be sure that they are not aliased, just by checking all users.
  if (!isa<AllocStackInst>(destAddr) && !isExclusiveArgument(destAddr))
    return nullptr;

  if (identifiableObjectsCache.count(destAddr) != 0) {
    return identifiableObjectsCache[destAddr] ? store : nullptr;
  }

  // Check if it's an "identifiable" object. This is the case if it only has
  // users which we are able to track in a simple way: stores and applies.
  for (Operand *use : destAddr->getUses()) {
    SILInstruction *user = use->getUser();
    switch (user->getKind()) {
      // Those instructions do not write to destAddr nor let they destAddr
      // escape.
      case SILInstructionKind::DeallocStackInst:
      case SILInstructionKind::LoadInst:
        break;
      case SILInstructionKind::DebugValueInst:
        if (DebugValueInst::hasAddrVal(user))
          break;
        LLVM_FALLTHROUGH;
      default:
        if (!mayWriteToIdentifyableObject(user)) {
          // We don't handle user. It is some instruction which may write to
          // destAddr or let destAddr "escape" (like an address projection).
          identifiableObjectsCache[destAddr] = false;
          return nullptr;
        }
        break;
    }
  }
  identifiableObjectsCache[destAddr] = true;
  return store;
}

/// Removes all objects from \p storedStrings which \p inst (potentially)
/// modifies.
void StringOptimization::invalidateModifiedObjects(SILInstruction *inst,
                            llvm::DenseMap<SILValue, SILValue> &storedStrings) {
  // Ignore non-writing instructions, like "load", "dealloc_stack".
  // Note that identifiable objects (= keys in storedStrings) can only have
  // certain kind of instructions as users: all instruction which we handle in
  // isStringStoreToIdentifyableObject().
  if (!mayWriteToIdentifyableObject(inst))
    return;

  for (Operand &op : inst->getAllOperands()) {
    storedStrings.erase(op.get());
  }
}

/// If \p value is a struct_extract, return its operand and field.
static std::pair<SILValue, VarDecl *> skipStructExtract(SILValue value) {
  if (auto *sei = dyn_cast<StructExtractInst>(value))
    return {sei->getOperand(), sei->getField()};

  // Look through function calls, which do the struct_extract in the callee.
  // This specifically targets
  //    String(stringInterpolation: DefaultStringInterpolation)
  // which is not inlined in the high level pipeline (due to the specified
  // effects).
  auto *apply = dyn_cast<ApplyInst>(value);
  if (!apply)
    return {value, nullptr};
  
  SILFunction *callee = apply->getReferencedFunctionOrNull();
  if (!callee || !callee->isDefinition())
    return {value, nullptr};

  // `String(stringInterpolation: DefaultStringInterpolation)` has only a single
  // basic block. Avoid the effort of searching all blocks for a `return`.
  auto *ret = dyn_cast<ReturnInst>(callee->getEntryBlock()->getTerminator());
  if (!ret)
    return {value, nullptr};

  auto *sei = dyn_cast<StructExtractInst>(ret->getOperand());
  if (!sei)
    return {value, nullptr};
  
  auto *arg = dyn_cast<SILFunctionArgument>(sei->getOperand());
  if (!arg)
    return {value, nullptr};

  value = apply->getArgument(arg->getIndex());
  return {value, sei->getField()};
}

/// Returns information about value if it's a constant string.
StringOptimization::StringInfo StringOptimization::getStringInfo(SILValue value) {
  if (!value)
    return StringInfo::unknown();
  
  // Look through struct_extract(struct(value)).
  // This specifically targets calls to
  //    String(stringInterpolation: DefaultStringInterpolation)
  // which are not inlined in the high level pipeline.
  VarDecl *field = nullptr;
  std::tie(value, field) = skipStructExtract(value);
  if (field) {
    auto *si = dyn_cast<StructInst>(value);
    if (!si)
      return StringInfo::unknown();
    value = si->getFieldValue(field);
  }

  auto *apply = dyn_cast<ApplyInst>(value);
  if (!apply) {
    return getStringFromStaticLet(value);
  }

  SILFunction *callee = apply->getReferencedFunctionOrNull();
  if (!callee)
    return StringInfo::unknown();
    
  if (callee->hasSemanticsAttr(semantics::STRING_INIT_EMPTY)) {
    // An empty string initializer.
    return StringInfo("");
  }
    
  if (callee->hasSemanticsAttr(semantics::STRING_INIT_EMPTY_WITH_CAPACITY)) {
    // An empty string initializer with initial capacity.
    int reservedCapacity = std::numeric_limits<int>::max();
    if (apply->getNumArguments() > 0) {
      if (Optional<int> capacity = getIntConstant(apply->getArgument(0)))
        reservedCapacity = capacity.value();
    }
    return StringInfo("", reservedCapacity);
  }
    
  if (callee->hasSemanticsAttr(semantics::STRING_MAKE_UTF8)) {
    // A string literal initializer.
    SILValue stringVal = apply->getArgument(0);
    auto *stringLiteral = dyn_cast<StringLiteralInst>(stringVal);
    SILValue lengthVal = apply->getArgument(1);
    auto *intLiteral = dyn_cast<IntegerLiteralInst>(lengthVal);
    if (intLiteral && stringLiteral &&
        // For simplicity, we only support UTF8 string literals.
        stringLiteral->getEncoding() == StringLiteralInst::Encoding::UTF8 &&
        // This passed number of code units should always match the size of the
        // string in the string literal. Just to be on the safe side, check it.
        intLiteral->getValue() == stringLiteral->getValue().size()) {
      return StringInfo(stringLiteral->getValue());
    }
  }
  return StringInfo::unknown();
}

/// Return the string if \p value is a load from a global static let, which is
/// initialized with a String constant.
StringOptimization::StringInfo
StringOptimization::getStringFromStaticLet(SILValue value) {
  // Match the pattern
  //   %ptr_to_global = apply %addressor()
  //   %global_addr = pointer_to_address %ptr_to_global
  //   %value = load %global_addr
  auto *load = dyn_cast<LoadInst>(value);
  if (!load)
    return StringInfo::unknown();

  SILFunction *initializer = nullptr;
  auto *globalAddr = dyn_cast<GlobalAddrInst>(load->getOperand());
  if (globalAddr) {
    // The global accessor is inlined.

    // Usually the global_addr is immediately preceeded by a call to
    // `builtin "once"` which initializes the global.
    SILInstruction *prev = globalAddr->getPreviousInstruction();
    if (!prev)
      return StringInfo::unknown();
    auto *bi = dyn_cast<BuiltinInst>(prev);
    if (!bi || bi->getBuiltinInfo().ID != BuiltinValueKind::Once)
      return StringInfo::unknown();
    initializer = getCalleeOfOnceCall(bi);
  } else {
    // The global accessor is not inlined, yet.

    auto *pta = dyn_cast<PointerToAddressInst>(load->getOperand());
    if (!pta)
      return StringInfo::unknown();

    auto *addressorCall = dyn_cast<ApplyInst>(pta->getOperand());
    if (!addressorCall)
      return StringInfo::unknown();

    SILFunction *addressorFunc = addressorCall->getReferencedFunctionOrNull();
    if (!addressorFunc)
      return StringInfo::unknown();

    // The addressor function has a builtin.once call to the initializer.
    BuiltinInst *onceCall = nullptr;
    initializer = findInitializer(addressorFunc, onceCall);
  }
  if (!initializer || !initializer->isGlobalInitOnceFunction())
    return StringInfo::unknown();

  if (initializer->size() != 1)
    return StringInfo::unknown();

  // Match the pattern
  //   %addr = global_addr @staticStringLet
  //   ...
  //   %str = apply %stringInitializer(...)
  //   store %str to %addr
  GlobalAddrInst *gAddr = nullptr;
  for (SILInstruction &inst : initializer->front()) {
    if (auto *ga = dyn_cast<GlobalAddrInst>(&inst)) {
      if (gAddr)
        return StringInfo::unknown();
      gAddr = ga;
    }
  }
  if (!gAddr || !gAddr->getReferencedGlobal()->isLet())
    return StringInfo::unknown();

  if (globalAddr && globalAddr->getReferencedGlobal() != gAddr->getReferencedGlobal())
    return StringInfo::unknown();

  Operand *gUse = gAddr->getSingleUse();
  auto *store = dyn_cast<StoreInst>(gUse->getUser());
  if (!store || store->getDest() != gAddr)
    return StringInfo::unknown();
    
  SILValue initVal = store->getSrc();
  
  // This check is probably not needed, but let's be on the safe side:
  // it prevents an infinite recursion if the initializer of the global is
  // itself a load of another global, and so on.
  if (isa<LoadInst>(initVal))
    return StringInfo::unknown();

  return getStringInfo(initVal);
}

/// Returns the constant integer value if \a value is an Int or Bool struct with
/// an integer_literal as operand.
Optional<int> StringOptimization::getIntConstant(SILValue value) {
  auto *boolOrIntStruct = dyn_cast<StructInst>(value);
  if (!boolOrIntStruct || boolOrIntStruct->getNumOperands() != 1)
    return None;
    
  auto *literal = dyn_cast<IntegerLiteralInst>(boolOrIntStruct->getOperand(0));
  if (!literal || literal->getValue().getActiveBits() > 64)
    return None;

  return literal->getValue().getSExtValue();
}

/// Replace a String.append() with a store of \p newValue to the destination.
void StringOptimization::replaceAppendWith(ApplyInst *appendCall,
                                           SILValue newValue) {
  SILBuilder builder(appendCall);
  SILLocation loc = appendCall->getLoc();
  SILValue destAddr = appendCall->getArgument(1);
  if (appendCall->getFunction()->hasOwnership()) {
    builder.createStore(loc, newValue, destAddr,
                        StoreOwnershipQualifier::Assign);
  } else {
    builder.createDestroyAddr(loc, destAddr);
    builder.createStore(loc, newValue, destAddr,
                        StoreOwnershipQualifier::Unqualified);
  }
  appendCall->eraseFromParent();
}

/// Returns a copy of \p value. Depending if the function is in OSSA, insert
/// either a copy_value or retain_value.
SILValue StringOptimization::copyValue(SILValue value, SILInstruction *before) {
  SILBuilder builder(before);
  SILLocation loc = before->getLoc();
  if (before->getFunction()->hasOwnership())
    return builder.createCopyValue(loc, value);

  builder.createRetainValue(loc, value, builder.getDefaultAtomicity());
  return value;
}

/// Creates a call to a string initializer.
ApplyInst *StringOptimization::createStringInit(StringRef str,
                                                SILInstruction *beforeInst) {
  SILBuilderWithScope builder(beforeInst);
  SILLocation loc = beforeInst->getLoc();
  SILModule &module = beforeInst->getFunction()->getModule();
  ASTContext &ctxt = module.getASTContext();

  if (!makeUTF8Func) {
    // Find the String initializer which takes a string_literal as argument.
    ConstructorDecl *makeUTF8Decl = ctxt.getMakeUTF8StringDecl();
    if (!makeUTF8Decl)
      return nullptr;
    
    auto Mangled = SILDeclRef(makeUTF8Decl, SILDeclRef::Kind::Allocator).mangle();
    makeUTF8Func = module.loadFunction(Mangled, SILModule::LinkingMode::LinkAll);
    if (!makeUTF8Func)
      return nullptr;
  }

  auto *literal = builder.createStringLiteral(loc, str,
                    StringLiteralInst::Encoding::UTF8);

  auto *length = builder.createIntegerLiteral(loc,
                    SILType::getBuiltinWordType(ctxt),
                    literal->getCodeUnitCount());

  auto *isAscii = builder.createIntegerLiteral(loc,
                    SILType::getBuiltinIntegerType(1, ctxt),
                    intmax_t(ctxt.isASCIIString(str)));

  SILType stringMetaType = SILType::getPrimitiveObjectType(
    CanType(MetatypeType::get(stringType.getASTType(),
      MetatypeRepresentation::Thin)));

  auto *metaTypeInst = builder.createMetatype(loc, stringMetaType);

  auto *functionRef = builder.createFunctionRefFor(loc, makeUTF8Func);

  return builder.createApply(loc, functionRef, SubstitutionMap(),
                             { literal, length, isAscii, metaTypeInst });
}

/// The StringOptimization function pass.
class StringOptimizationPass : public SILFunctionTransform {
public:

  void run() override {
    SILFunction *F = getFunction();
    if (!F->shouldOptimize())
      return;

    LLVM_DEBUG(llvm::dbgs() << "*** StringOptimization on function: "
                            << F->getName() << " ***\n");

    StringOptimization stringOptimization;
    bool changed = stringOptimization.run(F);

    if (changed) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::CallsAndInstructions);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createStringOptimization() {
  return new StringOptimizationPass();
}
