//===--- SILBridgingUtils.h - utilities for swift bridging ----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILBRIDGINGUTILS_H
#define SWIFT_SIL_SILBRIDGINGUTILS_H

#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "llvm/ADT/StringRef.h"
extern "C" {
#include "SILBridging.h"
}
#include <string>

namespace swift {

inline BridgedStringRef getBridgedStringRef(llvm::StringRef str) {
  return { (const unsigned char *)str.data(), str.size() };
}

inline StringRef getStringRef(BridgedStringRef str) {
  return StringRef((const char *)str.data, str.length);
}

/// Copies the string in an malloc'ed memory and the caller is responsible for
/// freeing it.
inline BridgedStringRef getCopiedBridgedStringRef(std::string str,
                                           bool removeTrailingNewline = false) {
  // A couple of mallocs are needed for passing a std::string to libswift. But
  // it's currently only used or debug descriptions. So, its' maybe not so bad -
  // for now.
  // TODO: find a better way to pass std::strings to libswift.
  StringRef strRef(str);
  if (removeTrailingNewline)
    strRef.consume_back("\n");
  llvm::MallocAllocator allocator;
  StringRef copy = strRef.copy(allocator);
  return getBridgedStringRef(copy);
}

inline SILLocation getSILLocation(BridgedLocation loc) {
  return reinterpret_cast<SILDebugLocation *>(&loc)->getLocation();
}

inline RegularLocation getRegularLocation(BridgedLocation loc) {
  return RegularLocation(getSILLocation(loc));
}

inline const SILDebugScope *getSILDebugScope(BridgedLocation loc) {
  return reinterpret_cast<SILDebugLocation *>(&loc)->getScope();
}

inline SILType getSILType(BridgedType ty) {
  return SILType::getFromOpaqueValue(ty.typePtr);
}

inline SILNode *castToSILNode(BridgedNode node) {
  return static_cast<SILNode *>(node.obj);
}

inline SILValue castToSILValue(BridgedValue value) {
  return static_cast<ValueBase *>(value.obj);
}

template <class I = SILInstruction> I *castToInst(BridgedInstruction inst) {
  return cast<I>(static_cast<SILNode *>(inst.obj)->castToInstruction());
}

inline SILBasicBlock *castToBasicBlock(BridgedBasicBlock block) {
  return static_cast<SILBasicBlock *>(block.obj);
}

inline SILFunction *castToFunction(BridgedFunction function) {
  return static_cast<SILFunction *>(function.obj);
}

inline SILGlobalVariable *castToGlobal(BridgedGlobalVar global) {
  return static_cast<SILGlobalVariable *>(global.obj);
}

ArrayRef<SILValue> getSILValues(BridgedValueArray values,
                                SmallVectorImpl<SILValue> &storage);

} // namespace swift

#endif

