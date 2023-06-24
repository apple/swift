//===--- Tracing.cpp - Support code for runtime tracing ------------*- C++ -*-//
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

#include "Tracing.h"

#if SWIFT_STDLIB_TRACING

#define SWIFT_LOG_SUBSYSTEM "com.apple.swift"
#define SWIFT_LOG_SECTION_SCAN_CATEGORY "SectionScan"

namespace swift {
namespace runtime {
namespace trace {

os_log_t ScanLog;
swift::once_t LogsToken;

void setupLogs(void *unused) {
  ScanLog = os_log_create(SWIFT_LOG_SUBSYSTEM, SWIFT_LOG_SECTION_SCAN_CATEGORY);
}

} // namespace trace
} // namespace runtime
} // namespace swift

#endif
