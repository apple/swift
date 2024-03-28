///===--- NonDispatchGlobalExecutor.inc ---------------------*- C++ -*--===///
///
/// This source file is part of the Swift.org open source project
///
/// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
/// Licensed under Apache License v2.0 with Runtime Library Exception
///
/// See https:///swift.org/LICENSE.txt for license information
/// See https:///swift.org/CONTRIBUTORS.txt for the list of Swift project authors
///
///===------------------------------------------------------------------===///
///
/// The implementation of the global executor when not using Dispatch but
/// also not using the cooperative global executor.  The general assumption
/// is that clients will be installing the appropriate hooks when all of
/// the functions here are called.
///
/// This file is included into GlobalExecutor.cpp only when both
/// Dispatch integration and the cooperative global executor are disabled.
/// It is expected to define the following functions:
///   swift_task_enqueueGlobalImpl
///   swift_task_enqueueGlobalWithDelayImpl
///   swift_task_enqueueMainExecutorImpl
///
///===------------------------------------------------------------------===///

SWIFT_CC(swift)
static void swift_task_enqueueGlobalImpl(Job *job) {
  assert(job && "no job provided");

  swift_reportError(0, "operation unsupported without libdispatch: "
                       "swift_task_enqueueGlobal");
}

SWIFT_CC(swift)
static void swift_task_enqueueGlobalWithDelayImpl(JobDelay delay,
                                                  Job *job) {
  assert(job && "no job provided");

  swift_reportError(0, "operation unsupported without libdispatch: "
                       "swift_task_enqueueGlobalWithDelay");
}

SWIFT_CC(swift)
static void swift_task_enqueueGlobalWithDeadlineImpl(long long sec,
                                                     long long nsec,
                                                     long long tsec,
                                                     long long tnsec,
                                                     int clock, Job *job) {
  assert(job && "no job provided");

  swift_reportError(0, "operation unsupported without libdispatch: "
                       "swift_task_enqueueGlobalWithDeadline");
}

/// Enqueues a task on the main executor.
SWIFT_CC(swift)
static void swift_task_enqueueMainExecutorImpl(Job *job) {
  assert(job && "no job provided");

  swift_reportError(0, "operation unsupported without libdispatch: "
                       "swift_task_enqueueMainExecutor");
}

SWIFT_CC(swift)
static void swift_task_checkIsolatedImpl(SerialExecutorRef executor) {
  _task_serialExecutor_checkIsolated(
      executor.getIdentity(), swift_getObjectType(executor.getIdentity()),
      executor.getSerialExecutorWitnessTable());
}
