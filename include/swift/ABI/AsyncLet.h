//===--- AsyncLet.h - ABI structures for async let -00-----------*- C++ -*-===//
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
// Swift ABI describing task groups.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ABI_TASK_ASYNC_LET_H
#define SWIFT_ABI_TASK_ASYNC_LET_H

#include "swift/ABI/Task.h"
#include "swift/ABI/HeapObject.h"
#include "swift/Runtime/Concurrency.h"
#include "swift/Runtime/Config.h"
#include "swift/Basic/RelativePointer.h"
#include "swift/Basic/STLExtras.h"

namespace swift {

/// Represents an in-flight `async let`, i.e. the Task that is computing the
/// result of the async let, along with the awaited status and other metadata.
class alignas(Alignment_AsyncLet) AsyncLet {
public:
  // These constructors do not initialize the AsyncLet instance, and the
  // destructor does not destroy the AsyncLet instance; you must call
  // swift_asyncLet_{start,end} yourself.
  constexpr AsyncLet()
    : PrivateData{} {}

  void *PrivateData[NumWords_AsyncLet];

  // TODO: we could offer a "was awaited on" check here

  /// Returns the child task that is associated with this async let.
  /// The tasks completion is used to fulfil the value represented by this async let.
  AsyncTask *getTask() const;
  
  // The compiler preallocates a large fixed space for the `async let`, with the
  // intent that most of it be used for the child task context. The next two
  // methods return the address and size of that space.
  
  /// Return a pointer to the unused space within the async let block.
  void *getPreallocatedSpace();
  
  /// Return the size of the unused space within the async let block.
  static size_t getSizeOfPreallocatedSpace();
};

} // end namespace swift

#endif // SWIFT_ABI_TASK_ASYNC_LET_H
