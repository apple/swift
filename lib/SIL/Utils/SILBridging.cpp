//===--- SILBridgingUtils.cpp - Utilities for swift bridging --------------===//
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

#include "swift/Basic/BridgingUtils.h"
#include "swift/AST/Attr.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/SIL/SILNode.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/SILBridging.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/MemAccessUtils.h"
#include <string>

using namespace swift;

namespace {

bool nodeMetatypesInitialized = false;

// Filled in by class registration in initializeSwiftModules().
SwiftMetatype nodeMetatypes[(unsigned)SILNodeKind::Last_SILNode + 1];

}

// Does return null if initializeSwiftModules() is never called.
SwiftMetatype SILNode::getSILNodeMetatype(SILNodeKind kind) {
  SwiftMetatype metatype = nodeMetatypes[(unsigned)kind];
  assert((!nodeMetatypesInitialized || metatype) &&
        "no metatype for bridged SIL node");
  return metatype;
}

//===----------------------------------------------------------------------===//
//                          Class registration
//===----------------------------------------------------------------------===//

static llvm::StringMap<SILNodeKind> valueNamesToKind;
static llvm::SmallPtrSet<SwiftMetatype, 4> unimplementedTypes;

// Utility to fill in a metatype of an "unimplemented" class for a whole range
// of class types.
static void setUnimplementedRange(SwiftMetatype metatype,
                                  SILNodeKind from, SILNodeKind to) {
  unimplementedTypes.insert(metatype);
  for (unsigned kind = (unsigned)from; kind <= (unsigned)to; ++kind) {
    assert((!nodeMetatypes[kind] || unimplementedTypes.count(metatype)) &&
           "unimplemented nodes must be registered first");
    nodeMetatypes[kind] = metatype;
  }
}

/// Registers the metatype of a swift SIL class.
/// Called by initializeSwiftModules().
void registerBridgedClass(StringRef className, SwiftMetatype metatype) {
  nodeMetatypesInitialized = true;

  // Handle the important non Node classes.
  if (className == "BasicBlock")
    return SILBasicBlock::registerBridgedMetatype(metatype);
  if (className == "GlobalVariable")
    return SILGlobalVariable::registerBridgedMetatype(metatype);
  if (className == "BlockArgument") {
    nodeMetatypes[(unsigned)SILNodeKind::SILPhiArgument] = metatype;
    return;
  }
  if (className == "FunctionArgument") {
    nodeMetatypes[(unsigned)SILNodeKind::SILFunctionArgument] = metatype;
    return;
  }

  // Pre-populate the "unimplemented" ranges of metatypes.
  // If a specific class is not implemented in Swift yet, it bridges to an
  // "unimplemented" class. This ensures that optimizations handle _all_ kind of
  // instructions gracefully, without the need to define the not-yet-used
  // classes in Swift.
#define VALUE_RANGE(ID) SILNodeKind::First_##ID, SILNodeKind::Last_##ID
  if (className == "UnimplementedRefCountingInst")
    return setUnimplementedRange(metatype, VALUE_RANGE(RefCountingInst));
  if (className == "UnimplementedSingleValueInst")
    return setUnimplementedRange(metatype, VALUE_RANGE(SingleValueInstruction));
  if (className == "UnimplementedInstruction")
    return setUnimplementedRange(metatype, VALUE_RANGE(SILInstruction));
#undef VALUE_RANGE

  if (valueNamesToKind.empty()) {
#define VALUE(ID, PARENT) \
    valueNamesToKind[#ID] = SILNodeKind::ID;
#define NON_VALUE_INST(ID, NAME, PARENT, MEMBEHAVIOR, MAYRELEASE) \
    VALUE(ID, NAME)
#define ARGUMENT(ID, PARENT) \
    VALUE(ID, NAME)
#define SINGLE_VALUE_INST(ID, NAME, PARENT, MEMBEHAVIOR, MAYRELEASE) \
    VALUE(ID, NAME)
#define MULTIPLE_VALUE_INST(ID, NAME, PARENT, MEMBEHAVIOR, MAYRELEASE) \
    VALUE(ID, NAME)
#include "swift/SIL/SILNodes.def"
  }

  std::string prefixedName;
  auto iter = valueNamesToKind.find(className);
  if (iter == valueNamesToKind.end()) {
    // Try again with a "SIL" prefix. For example Argument -> SILArgument.
    prefixedName = std::string("SIL") + std::string(className);
    iter = valueNamesToKind.find(prefixedName);
    if (iter == valueNamesToKind.end()) {
      llvm::errs() << "Unknown bridged node class " << className << '\n';
      abort();
    }
    className = prefixedName;
  }
  SILNodeKind kind = iter->second;
  SwiftMetatype existingTy = nodeMetatypes[(unsigned)kind];
  if (existingTy && !unimplementedTypes.count(existingTy)) {
    llvm::errs() << "Double registration of class " << className << '\n';
    abort();
  }
  nodeMetatypes[(unsigned)kind] = metatype;
}

//===----------------------------------------------------------------------===//
//                                SILFunction
//===----------------------------------------------------------------------===//

std::string BridgedFunction::getDebugDescription() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  getFunction()->print(os);
  str.pop_back(); // Remove trailing newline.
  return str;
}

//===----------------------------------------------------------------------===//
//                               SILBasicBlock
//===----------------------------------------------------------------------===//

std::string BridgedBasicBlock::getDebugDescription() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  getBlock()->print(os);
  str.pop_back(); // Remove trailing newline.
  return str;
}

//===----------------------------------------------------------------------===//
//                                SILValue
//===----------------------------------------------------------------------===//

std::string BridgedValue::getDebugDescription() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  getSILValue()->print(os);
  str.pop_back(); // Remove trailing newline.
  return str;
}

BridgedValue::Kind BridgedValue::getKind() const {
  SILValue v = getSILValue();
  if (isa<SingleValueInstruction>(v)) {
    return BridgedValue::Kind::SingleValueInstruction;
  } else if (isa<SILArgument>(v)) {
    return BridgedValue::Kind::Argument;
  } else if (isa<MultipleValueInstructionResult>(v)) {
    return BridgedValue::Kind::MultipleValueInstructionResult;
  } else if (isa<SILUndef>(v)) {
    return BridgedValue::Kind::Undef;
  }
  llvm_unreachable("unknown SILValue");
}

ArrayRef<SILValue> BridgedValueArray::getValues(SmallVectorImpl<SILValue> &storage) {
  for (unsigned idx = 0; idx < count; ++idx) {
    storage.push_back(base[idx].value.getSILValue());
  }
  return storage;
}

//===----------------------------------------------------------------------===//
//                            SILGlobalVariable
//===----------------------------------------------------------------------===//

std::string BridgedGlobalVar::getDebugDescription() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  getGlobal()->print(os);
  str.pop_back(); // Remove trailing newline.
  return str;
}

bool BridgedGlobalVar::canBeInitializedStatically() const {
  SILGlobalVariable *global = getGlobal();
  auto expansion = ResilienceExpansion::Maximal;
  if (hasPublicVisibility(global->getLinkage()))
    expansion = ResilienceExpansion::Minimal;

  auto &tl = global->getModule().Types.getTypeLowering(
      global->getLoweredType(),
      TypeExpansionContext::noOpaqueTypeArchetypesSubstitution(expansion));
  return tl.isLoadable();
}

bool BridgedGlobalVar::mustBeInitializedStatically() const {
  SILGlobalVariable *global = getGlobal();
  return global->mustBeInitializedStatically();
}

//===----------------------------------------------------------------------===//
//                            SILVTable
//===----------------------------------------------------------------------===//

std::string BridgedVTable::getDebugDescription() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  vTable->print(os);
  str.pop_back(); // Remove trailing newline.
  return str;
}

std::string BridgedVTableEntry::getDebugDescription() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  entry->print(os);
  str.pop_back(); // Remove trailing newline.
  return str;
}

//===----------------------------------------------------------------------===//
//                    SILVWitnessTable, SILDefaultWitnessTable
//===----------------------------------------------------------------------===//

std::string BridgedWitnessTableEntry::getDebugDescription() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  entry->print(os, /*verbose=*/ false, PrintOptions::printSIL());
  str.pop_back(); // Remove trailing newline.
  return str;
}

std::string BridgedWitnessTable::getDebugDescription() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  table->print(os);
  str.pop_back(); // Remove trailing newline.
  return str;
}

std::string BridgedDefaultWitnessTable::getDebugDescription() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  table->print(os);
  str.pop_back(); // Remove trailing newline.
  return str;
}

//===----------------------------------------------------------------------===//
//                               SILInstruction
//===----------------------------------------------------------------------===//

std::string BridgedInstruction::getDebugDescription() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  getInst()->print(os);
  str.pop_back(); // Remove trailing newline.
  return str;
}

bool BridgedInstruction::mayAccessPointer() const {
  return ::mayAccessPointer(getInst());
}

bool BridgedInstruction::mayLoadWeakOrUnowned() const {
  return ::mayLoadWeakOrUnowned(getInst());
}

bool BridgedInstruction::maySynchronizeNotConsideringSideEffects() const {
  return ::maySynchronizeNotConsideringSideEffects(getInst());
}

bool BridgedInstruction::mayBeDeinitBarrierNotConsideringSideEffects() const {
  return ::mayBeDeinitBarrierNotConsideringSideEffects(getInst());
}
