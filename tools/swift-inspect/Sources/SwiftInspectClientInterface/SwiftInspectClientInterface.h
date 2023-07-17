//===----------------------------------------------------------------------===//
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

#if defined(_WIN32)

#include <stdint.h>

#define BUF_SIZE 512
#define SHARED_MEM_NAME_PREFIX "Local\\SwiftInspectFileMapping"
#define READ_EVENT_NAME_PREFIX "Local\\SwiftInspectReadEvent"
#define WRITE_EVENT_NAME_PREFIX "Local\\SwiftInspectWriteEvent"
#define WAIT_TIMEOUT_MS 30000

struct HeapEntry {
  uintptr_t Address;
  uintptr_t Size;
};

#endif
