//===--- RaceTest.swift ---------------------------------------------------===//
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
///
/// This file implements support for race tests.
///
/// Race test harness executes the given operation in multiple threads over a
/// set of shared data, trying to ensure that executions overlap in time.
///
/// The name "race test" does not imply that the race actually happens in the
/// harness or in the operation being tested.  The harness contains all the
/// necessary synchronization for its own data, and for publishing test data to
/// threads.  But if the operation under test is, in fact, racy, it should be
/// easier to discover the bug in this environment.
///
/// Every execution of a race test is called a trial.  During a single trial
/// the operation under test is executed multiple times in each thread over
/// different data items (`RaceData` instances).  Different threads process
/// data in different order.  Choosing an appropriate balance between the
/// number of threads and data items, the harness uses the birthday paradox to
/// increase the probability of "collisions" between threads.
///
/// After performing the operation under test, the thread should observe the
/// data in a test-dependent way to detect if presence of other concurrent
/// actions disturbed the result.  The observation should be as short as
/// possible, and the results should be returned as `Observation`.  Evaluation
/// (cross-checking) of observations is deferred until the end of the trial.
///
//===----------------------------------------------------------------------===//

import SwiftPrivate
import SwiftPrivateLibcExtras
import SwiftPrivateThreadExtras
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#elseif os(WASI)
import WASILibc
#elseif os(Windows)
import CRT
import WinSDK
#endif

#if _runtime(_ObjC)
import ObjectiveC
#else
func autoreleasepool(invoking code: () -> Void) {
  // Native runtime does not have autorelease pools.  Execute the code
  // directly.
  code()
}
#endif

/// Race tests that need a fresh set of data for every trial should implement
/// this protocol.
///
/// All racing threads execute the same operation, `thread1`.
///
/// Types conforming to this protocol should be structs.  (The type
/// should be a struct to reduce unnecessary reference counting during
/// the test.)  The types should be stateless.
public protocol RaceTestWithPerTrialData {

  /// Input for threads.
  ///
  /// This type should be a class.  (The harness will not pass struct instances
  /// between threads correctly.)
  associatedtype RaceData : AnyObject

  /// Type of thread-local data.
  ///
  /// Thread-local data is newly created for every trial.
  associatedtype ThreadLocalData

  /// Results of the observation made after performing an operation.
  associatedtype Observation

  init()

  /// Creates a fresh instance of `RaceData`.
  func makeRaceData() -> RaceData

  /// Creates a fresh instance of `ThreadLocalData`.
  func makeThreadLocalData() -> ThreadLocalData

  /// Performs the operation under test and makes an observation.
  func thread1(
    _ raceData: RaceData, _ threadLocalData: inout ThreadLocalData) -> Observation

  /// Evaluates the observations made by all threads for a particular instance
  /// of `RaceData`.
  func evaluateObservations(
    _ observations: [Observation],
    _ sink: (RaceTestObservationEvaluation) -> Void)
}

/// The result of evaluating observations.
///
/// Case payloads can carry test-specific data.  Results will be grouped
/// according to it.
public enum RaceTestObservationEvaluation : Equatable, CustomStringConvertible {
  /// Normal 'pass'.
  case pass

  /// An unusual 'pass'.
  case passInteresting(String)

  /// A failure.
  case failure
  case failureInteresting(String)

  public var description: String {
    switch self {
    case .pass:
      return "Pass"

    case .passInteresting(let s):
      return "Pass(\(s))"

    case .failure:
      return "Failure"

    case .failureInteresting(let s):
      return "Failure(\(s))"
    }
  }
}

public func == (
  lhs: RaceTestObservationEvaluation, rhs: RaceTestObservationEvaluation
) -> Bool {
  switch (lhs, rhs) {
  case (.pass, .pass),
       (.failure, .failure):
    return true

  case (.passInteresting(let s1), .passInteresting(let s2)):
    return s1 == s2

  default:
    return false
  }
}

/// An observation result that consists of one `UInt`.
public struct Observation1UInt : Equatable, CustomStringConvertible {
  public var data1: UInt

  public init(_ data1: UInt) {
    self.data1 = data1
  }

  public var description: String {
    return "(\(data1))"
  }
}

public func == (lhs: Observation1UInt, rhs: Observation1UInt) -> Bool {
  return lhs.data1 == rhs.data1
}

/// An observation result that consists of four `UInt`s.
public struct Observation4UInt : Equatable, CustomStringConvertible {
  public var data1: UInt
  public var data2: UInt
  public var data3: UInt
  public var data4: UInt

  public init(_ data1: UInt, _ data2: UInt, _ data3: UInt, _ data4: UInt) {
    self.data1 = data1
    self.data2 = data2
    self.data3 = data3
    self.data4 = data4
  }

  public var description: String {
    return "(\(data1), \(data2), \(data3), \(data4))"
  }
}

public func == (lhs: Observation4UInt, rhs: Observation4UInt) -> Bool {
  return
    lhs.data1 == rhs.data1 &&
    lhs.data2 == rhs.data2 &&
    lhs.data3 == rhs.data3 &&
    lhs.data4 == rhs.data4
}

/// An observation result that consists of three `Int`s.
public struct Observation3Int : Equatable, CustomStringConvertible {
  public var data1: Int
  public var data2: Int
  public var data3: Int

  public init(_ data1: Int, _ data2: Int, _ data3: Int) {
    self.data1 = data1
    self.data2 = data2
    self.data3 = data3
  }

  public var description: String {
    return "(\(data1), \(data2), \(data3))"
  }
}

public func == (lhs: Observation3Int, rhs: Observation3Int) -> Bool {
  return
    lhs.data1 == rhs.data1 &&
    lhs.data2 == rhs.data2 &&
    lhs.data3 == rhs.data3
}

/// An observation result that consists of four `Int`s.
public struct Observation4Int : Equatable, CustomStringConvertible {
  public var data1: Int
  public var data2: Int
  public var data3: Int
  public var data4: Int

  public init(_ data1: Int, _ data2: Int, _ data3: Int, _ data4: Int) {
    self.data1 = data1
    self.data2 = data2
    self.data3 = data3
    self.data4 = data4
  }

  public var description: String {
    return "(\(data1), \(data2), \(data3), \(data4))"
  }
}

public func == (lhs: Observation4Int, rhs: Observation4Int) -> Bool {
  return
    lhs.data1 == rhs.data1 &&
    lhs.data2 == rhs.data2 &&
    lhs.data3 == rhs.data3 &&
    lhs.data4 == rhs.data4
}

/// An observation result that consists of five `Int`s.
public struct Observation5Int : Equatable, CustomStringConvertible {
  public var data1: Int
  public var data2: Int
  public var data3: Int
  public var data4: Int
  public var data5: Int

  public init(
    _ data1: Int, _ data2: Int, _ data3: Int, _ data4: Int, _ data5: Int
  ) {
    self.data1 = data1
    self.data2 = data2
    self.data3 = data3
    self.data4 = data4
    self.data5 = data5
  }

  public var description: String {
    return "(\(data1), \(data2), \(data3), \(data4), \(data5))"
  }
}

public func == (lhs: Observation5Int, rhs: Observation5Int) -> Bool {
  return
    lhs.data1 == rhs.data1 &&
    lhs.data2 == rhs.data2 &&
    lhs.data3 == rhs.data3 &&
    lhs.data4 == rhs.data4 &&
    lhs.data5 == rhs.data5
}

/// An observation result that consists of nine `Int`s.
public struct Observation9Int : Equatable, CustomStringConvertible {
  public var data1: Int
  public var data2: Int
  public var data3: Int
  public var data4: Int
  public var data5: Int
  public var data6: Int
  public var data7: Int
  public var data8: Int
  public var data9: Int

  public init(
    _ data1: Int, _ data2: Int, _ data3: Int, _ data4: Int,
    _ data5: Int, _ data6: Int, _ data7: Int, _ data8: Int,
    _ data9: Int
  ) {
    self.data1 = data1
    self.data2 = data2
    self.data3 = data3
    self.data4 = data4
    self.data5 = data5
    self.data6 = data6
    self.data7 = data7
    self.data8 = data8
    self.data9 = data9
  }

  public var description: String {
    return "(\(data1), \(data2), \(data3), \(data4), \(data5), \(data6), \(data7), \(data8), \(data9))"
  }
}

public func == (lhs: Observation9Int, rhs: Observation9Int) -> Bool {
  return
    lhs.data1 == rhs.data1 &&
    lhs.data2 == rhs.data2 &&
    lhs.data3 == rhs.data3 &&
    lhs.data4 == rhs.data4 &&
    lhs.data5 == rhs.data5 &&
    lhs.data6 == rhs.data6 &&
    lhs.data7 == rhs.data7 &&
    lhs.data8 == rhs.data8 &&
    lhs.data9 == rhs.data9
}

/// A helper that is useful to implement
/// `RaceTestWithPerTrialData.evaluateObservations()` in race tests.
public func evaluateObservationsAllEqual<T : Equatable>(_ observations: [T])
  -> RaceTestObservationEvaluation {
  let first = observations.first!
  for x in observations {
    if x != first {
      return .failure
    }
  }
  return .pass
}

// WebAssembly/WASI doesn't support multi-threading yet
#if os(WASI)
public func runRaceTest<RT : RaceTestWithPerTrialData>(
  _: RT.Type,
  trials: Int,
  timeoutInSeconds: Int? = nil,
  threads: Int? = nil
) {}
public func runRaceTest<RT : RaceTestWithPerTrialData>(
  _ test: RT.Type,
  operations: Int,
  timeoutInSeconds: Int? = nil,
  threads: Int? = nil
) {}
public func consumeCPU(units amountOfWork: Int) {}
public func runRaceTest(
  trials: Int,
  timeoutInSeconds: Int? = nil,
  threads: Int? = nil,
  invoking body: @escaping () -> Void
) {}

public func runRaceTest(
  operations: Int,
  timeoutInSeconds: Int? = nil,
  threads: Int? = nil,
  invoking body: @escaping () -> Void
) {}
#else

struct _RaceTestAggregatedEvaluations : CustomStringConvertible {
  var passCount: Int = 0
  var passInterestingCount = [String: Int]()
  var failureCount: Int = 0
  var failureInterestingCount = [String: Int]()

  init() {}

  mutating func addEvaluation(_ evaluation: RaceTestObservationEvaluation) {
    switch evaluation {
    case .pass:
      passCount += 1

    case .passInteresting(let s):
      if passInterestingCount[s] == nil {
        passInterestingCount[s] = 0
      }
      passInterestingCount[s] = passInterestingCount[s]! + 1

    case .failure:
      failureCount += 1

    case .failureInteresting(let s):
      if failureInterestingCount[s] == nil {
        failureInterestingCount[s] = 0
      }
      failureInterestingCount[s] = failureInterestingCount[s]! + 1
    }
  }

  var isFailed: Bool {
    return failureCount != 0 || !failureInterestingCount.isEmpty
  }

  var description: String {
    var result = ""
    result += "Pass: \(passCount) times\n"
    for desc in passInterestingCount.keys.sorted() {
      let count = passInterestingCount[desc]!
      result += "Pass \(desc): \(count) times\n"
    }
    result += "Failure: \(failureCount) times\n"
    for desc in failureInterestingCount.keys.sorted() {
      let count = failureInterestingCount[desc]!
      result += "Failure \(desc): \(count) times\n"
    }
    return result
  }
}

// FIXME: protect this class against false sharing.
class _RaceTestWorkerState<RT : RaceTestWithPerTrialData> {
  // FIXME: protect every element of 'raceData' against false sharing.
  var raceData: [RT.RaceData] = []
  var raceDataShuffle: [Int] = []
  var observations: [RT.Observation] = []
}

class _RaceTestSharedState<RT : RaceTestWithPerTrialData> {
  var racingThreadCount: Int
  var stopNow = _stdlib_AtomicInt(0)

  var trialBarrier: _stdlib_Barrier
  var trialSpinBarrier = _stdlib_AtomicInt()

  var raceData: [RT.RaceData] = []
  var workerStates: [_RaceTestWorkerState<RT>] = []
  var aggregatedEvaluations: _RaceTestAggregatedEvaluations =
    _RaceTestAggregatedEvaluations()

  init(racingThreadCount: Int) {
    self.racingThreadCount = racingThreadCount
    self.trialBarrier = _stdlib_Barrier(threadCount: racingThreadCount + 1)

    self.workerStates.reserveCapacity(racingThreadCount)
    for _ in 0..<racingThreadCount {
      self.workerStates.append(_RaceTestWorkerState<RT>())
    }
  }
}

func _masterThreadStopWorkers<RT>( _ sharedState: _RaceTestSharedState<RT>) {
  // Workers are proceeding to the first barrier in _workerThreadOneTrial.
  sharedState.stopNow.store(1)
  // Allow workers to proceed past that first barrier. They will then see
  // stopNow==true and stop.
  sharedState.trialBarrier.wait()
}

func _masterThreadOneTrial<RT>(_ sharedState: _RaceTestSharedState<RT>) {
  let racingThreadCount = sharedState.racingThreadCount
  let raceDataCount = racingThreadCount * racingThreadCount
  let rt = RT()

  sharedState.raceData.removeAll(keepingCapacity: true)

  sharedState.raceData.append(contentsOf: (0..<raceDataCount).lazy.map { _ in
    rt.makeRaceData()
  })

  let identityShuffle = Array(0..<sharedState.raceData.count)
  sharedState.workerStates.removeAll(keepingCapacity: true)
  
  sharedState.workerStates.append(contentsOf: (0..<racingThreadCount).lazy.map {
    _ in
    let workerState = _RaceTestWorkerState<RT>()

    // Shuffle the data so that threads process it in different order.
    let shuffle = identityShuffle.shuffled()
    workerState.raceData = scatter(sharedState.raceData, shuffle)
    workerState.raceDataShuffle = shuffle

    workerState.observations = []
    workerState.observations.reserveCapacity(sharedState.raceData.count)

    return workerState
  })

  sharedState.trialSpinBarrier.store(0)
  sharedState.trialBarrier.wait()
  // Race happens.
  sharedState.trialBarrier.wait()

  // Collect and compare results.
  for i in 0..<racingThreadCount {
    let shuffle = sharedState.workerStates[i].raceDataShuffle
    sharedState.workerStates[i].raceData =
      gather(sharedState.workerStates[i].raceData, shuffle)
    sharedState.workerStates[i].observations =
      gather(sharedState.workerStates[i].observations, shuffle)
  }
  if true {
    // FIXME: why doesn't the bracket syntax work?
    // <rdar://problem/18305718> Array sugar syntax does not work when used
    // with associated types
    var observations: [RT.Observation] = []
    observations.reserveCapacity(racingThreadCount)
    for i in 0..<raceDataCount {
      for j in 0..<racingThreadCount {
        observations.append(sharedState.workerStates[j].observations[i])
      }

      let sink = { sharedState.aggregatedEvaluations.addEvaluation($0) }
      rt.evaluateObservations(observations, sink)
      observations.removeAll(keepingCapacity: true)
    }
  }
}

func _workerThreadOneTrial<RT>(
  _ tid: Int, _ sharedState: _RaceTestSharedState<RT>
) -> Bool {
  sharedState.trialBarrier.wait()
  if sharedState.stopNow.load() == 1 {
    return true
  }
  let racingThreadCount = sharedState.racingThreadCount
  let workerState = sharedState.workerStates[tid]
  let rt = RT()
  var threadLocalData = rt.makeThreadLocalData()
  do {
    let trialSpinBarrier = sharedState.trialSpinBarrier
    _ = trialSpinBarrier.fetchAndAdd(1)
    while trialSpinBarrier.load() < racingThreadCount {}
  }
  // Perform racy operations.
  // Warning: do not add any synchronization in this loop, including
  // any implicit reference counting of shared data.
  for raceData in workerState.raceData {
    workerState.observations.append(rt.thread1(raceData, &threadLocalData))
  }
  sharedState.trialBarrier.wait()
  return false
}

/// One-shot sleep in one thread, allowing interrupt by another.
#if os(Windows)
class _InterruptibleSleep {
  let event: HANDLE?
  var completed: Bool = false

  init() {
    self.event = CreateEventW(nil, true, false, nil)
    precondition(self.event != nil)
  }

  deinit {
    CloseHandle(self.event)
  }

  func sleep(durationInSeconds duration: Int) {
    guard completed == false else { return }

    let result: DWORD = WaitForSingleObject(event, DWORD(duration * 1000))
    precondition(result == WAIT_OBJECT_0)

    completed = true
  }

  func wake() {
    guard completed == false else { return }
    let result: Bool = SetEvent(self.event)
    precondition(result == true)
  }
}
#else
class _InterruptibleSleep {
  let writeEnd: CInt
  let readEnd: CInt
  var completed = false

  init() {
    (readEnd: readEnd, writeEnd: writeEnd, _) = _stdlib_pipe()
  }

  deinit {
    close(readEnd)
    close(writeEnd)
  }

  /// Sleep for durationInSeconds or until another
  /// thread calls wake(), whichever comes first.
  func sleep(durationInSeconds duration: Int) {
    if completed {
      return
    }

    // WebAssembly/WASI on wasm32 is the only 32-bit platform with Int64 time_t,
    // needs an explicit conversion to `time_t` because of this.
    var timeout = timeval(tv_sec: time_t(duration), tv_usec: 0)

    var readFDs = _stdlib_fd_set()
    var writeFDs = _stdlib_fd_set()
    var errorFDs = _stdlib_fd_set()
    readFDs.set(readEnd)

    let ret = _stdlib_select(&readFDs, &writeFDs, &errorFDs, &timeout)
    precondition(ret >= 0)
    completed = true
  }

  /// Wake the thread in sleep().
  func wake() {
    if completed { return }

    let buffer: [UInt8] = [1]
    let ret = write(writeEnd, buffer, 1)
    precondition(ret >= 0)
  }
}
#endif

public func runRaceTest<RT : RaceTestWithPerTrialData>(
  _: RT.Type,
  trials: Int,
  timeoutInSeconds: Int? = nil,
  threads: Int? = nil
) {
  let racingThreadCount = threads ?? max(2, _stdlib_getHardwareConcurrency())
  let sharedState = _RaceTestSharedState<RT>(racingThreadCount: racingThreadCount)

  // Alarm thread sets timeoutReached.
  // Master thread sees timeoutReached and tells worker threads to stop.
  let timeoutReached = _stdlib_AtomicInt(0)
  let alarmTimer = _InterruptibleSleep()

  let masterThreadBody = {
    () -> Void in
    for t in 0..<trials {
      // Check for timeout.
      // _masterThreadStopWorkers must run BEFORE the last _masterThreadOneTrial
      // to make the thread coordination barriers work
      // but we do want to run at least one trial even if the timeout occurs.
      if timeoutReached.load() == 1 && t > 0 {
        _masterThreadStopWorkers(sharedState)
        break
      }

      autoreleasepool {
        _masterThreadOneTrial(sharedState)
      }
    }
  }

  let racingThreadBody = {
    (tid: Int) -> Void in
    for _ in 0..<trials {
      let stopNow = _workerThreadOneTrial(tid, sharedState)
      if stopNow { break }
    }
  }

  let alarmThreadBody = {
    () -> Void in
    guard let timeoutInSeconds = timeoutInSeconds
    else { return }

    alarmTimer.sleep(durationInSeconds: timeoutInSeconds)
    _ = timeoutReached.fetchAndAdd(1)
  }

  var testTids = [ThreadHandle]()
  var alarmTid: ThreadHandle

  // Create the master thread.
  do {
    let (ret, tid) = _stdlib_thread_create_block(masterThreadBody, ())
    expectEqual(0, ret)
    testTids.append(tid!)
  }

  // Create racing threads.
  for i in 0..<racingThreadCount {
    let (ret, tid) = _stdlib_thread_create_block(racingThreadBody, i)
    expectEqual(0, ret)
    testTids.append(tid!)
  }

  // Create the alarm thread that enforces the timeout.
  do {
    let (ret, tid) = _stdlib_thread_create_block(alarmThreadBody, ())
    expectEqual(0, ret)
    alarmTid = tid!
  }

  // Join all testing threads.
  for tid in testTids {
    let (ret, _) = _stdlib_thread_join(tid, Void.self)
    expectEqual(0, ret)
  }

  // Tell the alarm thread to stop if it hasn't already, then join it.
  do {
    alarmTimer.wake()
    let (ret, _) = _stdlib_thread_join(alarmTid, Void.self)
    expectEqual(0, ret)
  }

  let aggregatedEvaluations = sharedState.aggregatedEvaluations
  expectFalse(aggregatedEvaluations.isFailed)
  print(aggregatedEvaluations)
}

internal func _divideRoundUp(_ lhs: Int, _ rhs: Int) -> Int {
  return (lhs + rhs) / rhs
}

public func runRaceTest<RT : RaceTestWithPerTrialData>(
  _ test: RT.Type,
  operations: Int,
  timeoutInSeconds: Int? = nil,
  threads: Int? = nil
) {
  let racingThreadCount = threads ?? max(2, _stdlib_getHardwareConcurrency())

  // Each trial runs threads^2 operations.
  let operationsPerTrial = racingThreadCount * racingThreadCount
  let trials = _divideRoundUp(operations, operationsPerTrial)
  runRaceTest(test, trials: trials, timeoutInSeconds: timeoutInSeconds,
    threads: threads)
}

public func consumeCPU(units amountOfWork: Int) {
  for _ in 0..<amountOfWork {
    let scale = 16
    for _ in 0..<scale {
      _blackHole(42)
    }
  }
}

internal struct ClosureBasedRaceTest : RaceTestWithPerTrialData {
  static var thread: () -> Void = {}

  class RaceData {}
  typealias ThreadLocalData = Void
  typealias Observation = Void

  func makeRaceData() -> RaceData { return RaceData() }
  func makeThreadLocalData() -> Void { return Void() }

  func thread1(
    _ raceData: RaceData, _ threadLocalData: inout ThreadLocalData
  ) {
    ClosureBasedRaceTest.thread()
  }

  func evaluateObservations(
    _ observations: [Observation],
    _ sink: (RaceTestObservationEvaluation) -> Void
  ) {}
}

public func runRaceTest(
  trials: Int,
  timeoutInSeconds: Int? = nil,
  threads: Int? = nil,
  invoking body: @escaping () -> Void
) {
  ClosureBasedRaceTest.thread = body
  runRaceTest(ClosureBasedRaceTest.self, trials: trials,
    timeoutInSeconds: timeoutInSeconds, threads: threads)
}

public func runRaceTest(
  operations: Int,
  timeoutInSeconds: Int? = nil,
  threads: Int? = nil,
  invoking body: @escaping () -> Void
) {
  ClosureBasedRaceTest.thread = body
  runRaceTest(ClosureBasedRaceTest.self, operations: operations,
    timeoutInSeconds: timeoutInSeconds, threads: threads)
}

#endif // os(WASI)
