//===--- Tracing.h - Support code for runtime tracing --------------*- C++ -*-//
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
//
// Support code for tracing events in the Swift runtime
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_TRACING_H
#define SWIFT_TRACING_H

#include "llvm/ADT/StringRef.h"
#include "swift/ABI/Metadata.h"
#include "swift/Demangling/Demangler.h"

#if SWIFT_STDLIB_TRACING
#include <os/signpost.h>

#include "swift/Runtime/HeapObject.h"

#define SWIFT_LOG_SECTION_SCAN "section_scan"

namespace swift {
namespace runtime {
namespace trace {

extern os_log_t ScanLog;
extern swift::once_t LogsToken;

void setupLogs(void *unused);

// Every function does ENSURE_LOGS() before making any os_signpost calls, so
// we can skip availability checking on all the individual calls.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

// Check a representative os_signpost function for NULL rather than doing a
// standard availability check, for better performance if the check doesn't get
// optimized out.
#define ENSURE_LOG(log)                                                        \
  do {                                                                         \
    if (!SWIFT_RUNTIME_WEAK_CHECK(os_signpost_enabled))                        \
      return {};                                                               \
    swift::once(LogsToken, setupLogs, nullptr);                                \
  } while (0)

/// A struct that captures the state of a scan tracing signpost. When the scan
/// is complete, call end() with the result of the scan. If the state struct
/// goes out of scope without calling end(), then it will automatically do the
/// equivalent of end(nullptr).
struct ScanTraceState {
  os_signpost_id_t signpostID;

  bool ended = false;

  template <typename T>
  T *end(T *result) {
    ended = true;
    os_signpost_interval_end(ScanLog, signpostID, SWIFT_LOG_SECTION_SCAN,
                             "result=%p", result);
    return result;
  }

  ~ScanTraceState() {
    if (!ended)
      end((void *)nullptr);
  }
};

static inline ScanTraceState
accessible_function_scan_begin(llvm::StringRef name) {
  ENSURE_LOG(ScanLog);

  auto id = os_signpost_id_generate(ScanLog);
  os_signpost_interval_begin(ScanLog, id, SWIFT_LOG_SECTION_SCAN,
                             "accessible function scan for '%.*s'",
                             (int)name.size(), name.data());
  return {id};
}

static inline ScanTraceState metadata_scan_begin(Demangle::NodePointer node) {
  ENSURE_LOG(ScanLog);

  auto id = os_signpost_id_generate(ScanLog);
  os_signpost_interval_begin(ScanLog, id, SWIFT_LOG_SECTION_SCAN,
                             "metadata scan for %s",
                             node ? nodeToString(node).c_str() : "<null>");
  return {id};
}

static inline ScanTraceState
protocol_conformance_scan_begin(Demangle::NodePointer node) {
  ENSURE_LOG(ScanLog);

  auto id = os_signpost_id_generate(ScanLog);
  os_signpost_interval_begin(ScanLog, id, SWIFT_LOG_SECTION_SCAN,
                             "protocol conformance scan for %s",
                             node ? nodeToString(node).c_str() : "<null>");
  return {id};
}

static inline ScanTraceState
protocol_conformance_scan_begin(const Metadata *type,
                                const ProtocolDescriptor *protocol) {
  ENSURE_LOG(ScanLog);

  auto id = os_signpost_id_generate(ScanLog);

  // Check for enablement separately to avoid the potentially expensive
  // swift_getTypeName call when tracing is not enabled.
  if (os_signpost_enabled(ScanLog)) {
    auto typeName = swift_getTypeName(type, /*qualified*/ true);
    auto protoName = protocol ? protocol->Name.get() : "<null>";
    os_signpost_interval_begin(ScanLog, id, SWIFT_LOG_SECTION_SCAN,
                               "protocol conformance scan for %.*s(%p): %s(%p)",
                               (int)typeName.length, typeName.data, type,
                               protoName, protocol);
  }
  return {id};
}

static inline ScanTraceState protocol_scan_begin(Demangle::NodePointer node) {
  ENSURE_LOG(ScanLog);

  auto id = os_signpost_id_generate(ScanLog);
  os_signpost_interval_begin(ScanLog, id, SWIFT_LOG_SECTION_SCAN,
                             "protocol scan for '%s'",
                             node ? nodeToString(node).c_str() : "<null>");
  return {id};
}

#pragma clang diagnostic pop

} // namespace trace
} // namespace runtime
} // namespace swift

#else

namespace swift {
namespace runtime {
namespace trace {

struct ScanTraceState {
  template <typename T>
  T *end(T *result) {
    return result;
  }
};

static inline ScanTraceState
accessible_function_scan_begin(llvm::StringRef name) {
  return {};
}

static inline ScanTraceState metadata_scan_begin(Demangle::NodePointer node) {
  return {};
}

static inline ScanTraceState
protocol_conformance_scan_begin(Demangle::NodePointer node) {
  return {};
}

static inline ScanTraceState
protocol_conformance_scan_begin(const Metadata *type,
                                const ProtocolDescriptor *protocol) {
  return {};
}

static inline ScanTraceState protocol_scan_begin(Demangle::NodePointer node) {
  return {};
}

} // namespace trace
} // namespace runtime
} // namespace swift

#endif

#endif // SWIFT_TRACING_H
