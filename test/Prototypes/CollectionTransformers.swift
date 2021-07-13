//===----------------------------------------------------------------------===//
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
// RUN: %target-run-stdlib-swift
// REQUIRES: executable_test

// FIXME: This test runs very slowly on watchOS.
// UNSUPPORTED: OS=watchos

import SwiftPrivate

public enum ApproximateCount {
  case Unknown
  case Precise(Int64)
  case Underestimate(Int64)
  case Overestimate(Int64)
}

public protocol ApproximateCountableSequence : Sequence {
  /// Complexity: amortized O(1).
  var approximateCount: ApproximateCount { get }
}

/// A collection that provides an efficient way to split its index ranges.
public protocol SplittableCollection : Collection {
  // We need this protocol so that collections with only forward or bidirectional
  // traversals could customize their splitting behavior.
  //
  // FIXME: all collections with random access should conform to this protocol
  // automatically.

  /// Splits a given range of indices into a set of disjoint ranges covering
  /// the same elements.
  ///
  /// Complexity: amortized O(1).
  ///
  /// FIXME: should that be O(log n) to cover some strange collections?
  ///
  /// FIXME: index invalidation rules?
  ///
  /// FIXME: a better name.  Users will never want to call this method
  /// directly.
  ///
  /// FIXME: return an optional for the common case when split() cannot
  /// subdivide the range further.
  func split(_ range: Range<Index>) -> [Range<Index>]
}

internal func _splitRandomAccessIndexRange<
  C : RandomAccessCollection
>(
  _ elements: C,
  _ range: Range<C.Index>
) -> [Range<C.Index>] {
  let startIndex = range.lowerBound
  let endIndex = range.upperBound
  let length = elements.distance(from: startIndex, to: endIndex)
  if length < 2 {
    return [range]
  }
  let middle = elements.index(startIndex, offsetBy: length / 2)
  return [startIndex ..< middle, middle ..< endIndex]
}

/// A helper object to build a collection incrementally in an efficient way.
///
/// Using a builder can be more efficient than creating an empty collection
/// instance and adding elements one by one.
public protocol CollectionBuilder {
  associatedtype Destination : Collection
    
  associatedtype Element = Destination.Iterator.Element

  init()

  /// Gives a hint about the expected approximate number of elements in the
  /// collection that is being built.
  mutating func sizeHint(_ approximateSize: Int)

  /// Append `element` to `self`.
  ///
  /// If a collection being built supports a user-defined order, the element is
  /// added at the end.
  ///
  /// Complexity: amortized O(1).
  mutating func append(_ element: Destination.Iterator.Element)

  /// Append `elements` to `self`.
  ///
  /// If a collection being built supports a user-defined order, the element is
  /// added at the end.
  ///
  /// Complexity: amortized O(n), where `n` is equal to `count(elements)`.
  mutating func append<
    C : Collection
  >(contentsOf elements: C)    
  where C.Iterator.Element == Element


  /// Append elements from `otherBuilder` to `self`, emptying `otherBuilder`.
  ///
  /// Equivalent to::
  ///
  ///   self.append(contentsOf: otherBuilder.takeResult())
  ///
  /// but is more efficient.
  ///
  /// Complexity: O(1).
  mutating func moveContentsOf(_ otherBuilder: inout Self)

  /// Build the collection from the elements that were added to this builder.
  ///
  /// Once this function is called, the builder may not be reused and no other
  /// methods should be called.
  ///
  /// Complexity: O(n) or better (where `n` is the number of elements that were
  /// added to this builder); typically O(1).
  mutating func takeResult() -> Destination
}

public protocol BuildableCollectionProtocol : Collection {
  associatedtype Builder : CollectionBuilder
}

extension Array : SplittableCollection {
  public func split(_ range: Range<Int>) -> [Range<Int>] {
    return _splitRandomAccessIndexRange(self, range)
  }
}

public struct ArrayBuilder<T> : CollectionBuilder {
  // FIXME: the compiler didn't complain when I remove public on 'Collection'.
  // File a bug.
  public typealias Destination = Array<T>
  public typealias Element = T

  internal var _resultParts = [[T]]()
  internal var _resultTail = [T]()

  public init() {}

  public mutating func sizeHint(_ approximateSize: Int) {
    _resultTail.reserveCapacity(approximateSize)
  }

  public mutating func append(_ element: T) {
    _resultTail.append(element)
  }

  public mutating func append<
    C : Collection
  >(contentsOf elements: C)
  where C.Iterator.Element == T {
    _resultTail.append(contentsOf: elements)
  }

  public mutating func moveContentsOf(_ otherBuilder: inout ArrayBuilder<T>) {
    // FIXME: do something smart with the capacity set in this builder and the
    // other builder.
    _resultParts.append(_resultTail)
    _resultTail = []
    // FIXME: not O(1)!
    _resultParts.append(contentsOf: otherBuilder._resultParts)
    otherBuilder._resultParts = []
    swap(&_resultTail, &otherBuilder._resultTail)
  }

  public mutating func takeResult() -> Destination {
    _resultParts.append(_resultTail)
    _resultTail = []
    // FIXME: optimize.  parallelize.
    return Array(_resultParts.joined())
  }
}

extension Array : BuildableCollectionProtocol {
  public typealias Builder = ArrayBuilder<Element>
}

//===----------------------------------------------------------------------===//
// Fork-join
//===----------------------------------------------------------------------===//

// As sad as it is, I think for practical performance reasons we should rewrite
// the inner parts of the fork-join framework in C++.  In way too many cases
// than necessary Swift requires an extra allocation to pin objects in memory
// for safe multithreaded access.  -Dmitri

import SwiftShims
import SwiftPrivate
import Darwin
import Dispatch

// FIXME: port to Linux.
// XFAIL: linux, windows, openbsd, freebsd

// A wrapper for pthread_t with platform-independent interface.
public struct _stdlib_pthread_t : Equatable, Hashable {
  internal let _value: pthread_t

  public func hash(into hasher: inout Hasher) {
    hasher.combine(_value)
  }
}

public func == (lhs: _stdlib_pthread_t, rhs: _stdlib_pthread_t) -> Bool {
  return lhs._value == rhs._value
}

public func _stdlib_pthread_self() -> _stdlib_pthread_t {
  return _stdlib_pthread_t(_value: pthread_self())
}

struct _ForkJoinMutex {
  var _mutex: UnsafeMutablePointer<pthread_mutex_t>

  init() {
    _mutex = UnsafeMutablePointer.allocate(capacity: 1)
    if pthread_mutex_init(_mutex, nil) != 0 {
      fatalError("pthread_mutex_init")
    }
  }

  func `deinit`() {
    if pthread_mutex_destroy(_mutex) != 0 {
      fatalError("pthread_mutex_init")
    }
    _mutex.deinitialize(count: 1)
    _mutex.deallocate()
  }

  func withLock<Result>(_ body: () -> Result) -> Result {
    if pthread_mutex_lock(_mutex) != 0 {
      fatalError("pthread_mutex_lock")
    }
    let result = body()
    if pthread_mutex_unlock(_mutex) != 0 {
      fatalError("pthread_mutex_unlock")
    }
    return result
  }
}

struct _ForkJoinCond {
  var _cond: UnsafeMutablePointer<pthread_cond_t>

  init() {
    _cond = UnsafeMutablePointer.allocate(capacity: 1)
    if pthread_cond_init(_cond, nil) != 0 {
      fatalError("pthread_cond_init")
    }
  }

  func `deinit`() {
    if pthread_cond_destroy(_cond) != 0 {
      fatalError("pthread_cond_destroy")
    }
    _cond.deinitialize(count: 1)
    _cond.deallocate()
  }

  func signal() {
    pthread_cond_signal(_cond)
  }

  func wait(_ mutex: _ForkJoinMutex) {
    pthread_cond_wait(_cond, mutex._mutex)
  }
}

final class _ForkJoinOneShotEvent {
  var _mutex: _ForkJoinMutex = _ForkJoinMutex()
  var _cond: _ForkJoinCond = _ForkJoinCond()
  var _isSet: Bool = false

  init() {}

  deinit {
    _cond.`deinit`()
    _mutex.`deinit`()
  }

  func set() {
    _mutex.withLock {
      if !_isSet {
        _isSet = true
        _cond.signal()
      }
    }
  }

  /// Establishes a happens-before relation between calls to set() and wait().
  func wait() {
    _mutex.withLock {
      while !_isSet {
        _cond.wait(_mutex)
      }
    }
  }

  /// If the function returns true, it establishes a happens-before relation
  /// between calls to set() and isSet().
  func isSet() -> Bool {
    return _mutex.withLock {
      return _isSet
    }
  }
}

final class _ForkJoinWorkDeque<T> {
  // FIXME: this is just a proof-of-concept; very inefficient.

  // Implementation note: adding elements to the head of the deque is common in
  // fork-join, so _deque is stored reversed (appending to an array is cheap).
  // FIXME: ^ that is false for submission queues though.
  var _deque: ContiguousArray<T> = []
  var _dequeMutex: _ForkJoinMutex = _ForkJoinMutex()

  init() {}

  deinit {
    precondition(_deque.isEmpty)

    _dequeMutex.`deinit`()
  }

  var isEmpty: Bool {
    return _dequeMutex.withLock {
      return _deque.isEmpty
    }
  }

  func prepend(_ element: T) {
    _dequeMutex.withLock {
      _deque.append(element)
    }
  }

  func tryTakeFirst() -> T? {
    return _dequeMutex.withLock {
      let result = _deque.last
      if _deque.count > 0 {
        _deque.removeLast()
      }
      return result
    }
  }

  func tryTakeFirstTwo() -> (T?, T?) {
    return _dequeMutex.withLock {
      let result1 = _deque.last
      if _deque.count > 0 {
        _deque.removeLast()
      }
      let result2 = _deque.last
      if _deque.count > 0 {
        _deque.removeLast()
      }
      return (result1, result2)
    }
  }

  func append(_ element: T) {
    _dequeMutex.withLock {
      _deque.insert(element, at: 0)
    }
  }

  func tryTakeLast() -> T? {
    return _dequeMutex.withLock {
      let result = _deque.first
      if _deque.count > 0 {
        _deque.remove(at: 0)
      }
      return result
    }
  }

  func takeAll() -> ContiguousArray<T> {
    return _dequeMutex.withLock {
      let result = _deque
      _deque = []
      return result
    }
  }

  func tryReplace(
    _ value: T,
    makeReplacement: @escaping () -> T,
    isEquivalent: @escaping (T, T) -> Bool
  ) -> Bool {
    return _dequeMutex.withLock {
      for i in _deque.indices {
        if isEquivalent(_deque[i], value) {
          _deque[i] = makeReplacement()
          return true
        }
      }
      return false
    }
  }
}

final class _ForkJoinWorkerThread {
  internal var _tid: _stdlib_pthread_t?
  internal let _pool: ForkJoinPool
  internal let _submissionQueue: _ForkJoinWorkDeque<ForkJoinTaskBase>
  internal let _workDeque: _ForkJoinWorkDeque<ForkJoinTaskBase>

  internal init(
    _pool: ForkJoinPool,
    submissionQueue: _ForkJoinWorkDeque<ForkJoinTaskBase>,
    workDeque: _ForkJoinWorkDeque<ForkJoinTaskBase>
  ) {
    self._tid = nil
    self._pool = _pool
    self._submissionQueue = submissionQueue
    self._workDeque = workDeque
  }

  internal func startAsync() {
    var queue: DispatchQueue?
    if #available(OSX 10.10, iOS 8.0, *) {
      queue = DispatchQueue.global(qos: .background)
    } else {
      queue = DispatchQueue.global(priority: .background)
    }
    queue!.async {
      self._thread()
    }
  }

  internal func _thread() {
    print("_ForkJoinWorkerThread begin")
    _tid = _stdlib_pthread_self()
    outer: while !_workDeque.isEmpty || !_submissionQueue.isEmpty {
      _pool._addRunningThread(self)
      while true {
        if _pool._tryStopThread() {
          print("_ForkJoinWorkerThread detected too many threads")
          _pool._removeRunningThread(self)
          _pool._submitTasksToRandomWorkers(_workDeque.takeAll())
          _pool._submitTasksToRandomWorkers(_submissionQueue.takeAll())
          print("_ForkJoinWorkerThread end")
          return
        }

        // Process tasks in FIFO order: first the work queue, then the
        // submission queue.
        if let task = _workDeque.tryTakeFirst() {
          task._run()
          continue
        }
        if let task = _submissionQueue.tryTakeFirst() {
          task._run()
          continue
        }

        print("_ForkJoinWorkerThread stealing tasks")
        if let task = _pool._stealTask() {
          task._run()
          continue
        }

        // FIXME: steal from submission queues?

        break
      }
      _pool._removeRunningThread(self)
    }
    assert(_workDeque.isEmpty)
    assert(_submissionQueue.isEmpty)
    _ = _pool._totalThreads.fetchAndAdd(-1)
    print("_ForkJoinWorkerThread end")
  }

  internal func _forkTask(_ task: ForkJoinTaskBase) {
    // Try to inflate the pool.
    if !_pool._tryCreateThread({ task }) {
      _workDeque.prepend(task)
    }
  }

  internal func _waitForTask(_ task: ForkJoinTaskBase) {
    while true {
      if task._isComplete() {
        return
      }

      // If the task is in work queue of the current thread, run the task.
      if _workDeque.tryReplace(
        task,
        makeReplacement: { ForkJoinTask<()>() {} },
        isEquivalent: { $0 === $1 }) {

        // We found the task.  Run it in-place.
        task._run()
        return
      }

      // FIXME: also check the submission queue, maybe the task is there?

      // FIXME: try to find the task in other threads' queues.

      // FIXME: try to find tasks that were forked from this task in other
      // threads' queues.  Help thieves by stealing those tasks back.

      // At this point, we can't do any work to help with running this task.
      // We can't start new work either (if we do, we might end up creating
      // more in-flight work than we can chew, and crash with out-of-memory
      // errors).
      _pool._compensateForBlockedWorkerThread() {
        task._blockingWait()
        // FIXME: do a timed wait, and retry stealing.
      }
    }
  }
}

internal protocol _Future {
  associatedtype Result

  /// Establishes a happens-before relation between completing the future and
  /// the call to wait().
  func wait()

  func tryGetResult() -> Result?
  func tryTakeResult() -> Result?

  func waitAndGetResult() -> Result
  func waitAndTakeResult() -> Result
}

public class ForkJoinTaskBase {
  final internal var _pool: ForkJoinPool?

  // FIXME(performance): there is no need to create heavy-weight
  // synchronization primitives every time.  We could start with a lightweight
  // atomic int for the flag and inflate to a full event when needed.  Unless
  // we really need to block in wait(), we would avoid creating an event.
  final internal let _completedEvent: _ForkJoinOneShotEvent =
    _ForkJoinOneShotEvent()

  final internal func _isComplete() -> Bool {
    return _completedEvent.isSet()
  }

  final internal func _blockingWait() {
    _completedEvent.wait()
  }

  internal func _run() {
    fatalError("implement")
  }

  final public func fork() {
    precondition(_pool == nil)
    if let thread = ForkJoinPool._getCurrentThread() {
      thread._forkTask(self)
    } else {
      // FIXME: decide if we want to allow this.
      precondition(false)
      ForkJoinPool.commonPool.forkTask(self)
    }
  }

  final public func wait() {
    if let thread = ForkJoinPool._getCurrentThread() {
      thread._waitForTask(self)
    } else {
      _blockingWait()
    }
  }
}

final public class ForkJoinTask<Result> : ForkJoinTaskBase, _Future {
  internal let _task: () -> Result
  internal var _result: Result?

  public init(_task: @escaping () -> Result) {
    self._task = _task
  }

  override internal func _run() {
    _complete(_task())
  }

  /// It is not allowed to call _complete() in a racy way.  Only one thread
  /// should ever call _complete().
  internal func _complete(_ result: Result) {
    precondition(!_completedEvent.isSet())
    _result = result
    _completedEvent.set()
  }

  public func tryGetResult() -> Result? {
    if _completedEvent.isSet() {
      return _result
    }
    return nil
  }

  public func tryTakeResult() -> Result? {
    if _completedEvent.isSet() {
      let result = _result
      _result = nil
      return result
    }
    return nil
  }

  public func waitAndGetResult() -> Result {
    wait()
    return tryGetResult()!
  }

  public func waitAndTakeResult() -> Result {
    wait()
    return tryTakeResult()!
  }
}

final public class ForkJoinPool {
  internal static var _threadRegistry: [_stdlib_pthread_t : _ForkJoinWorkerThread] = [:]
  internal static var _threadRegistryMutex: _ForkJoinMutex = _ForkJoinMutex()

  internal static func _getCurrentThread() -> _ForkJoinWorkerThread? {
    return _threadRegistryMutex.withLock {
      return _threadRegistry[_stdlib_pthread_self()]
    }
  }

  internal let _maxThreads: Int
  /// Total number of threads: number of running threads plus the number of
  /// threads that are preparing to start).
  internal let _totalThreads = _stdlib_AtomicInt(0)

  internal var _runningThreads: [_ForkJoinWorkerThread] = []
  internal var _runningThreadsMutex: _ForkJoinMutex = _ForkJoinMutex()

  internal var _submissionQueues: [_ForkJoinWorkDeque<ForkJoinTaskBase>] = []
  internal var _submissionQueuesMutex: _ForkJoinMutex = _ForkJoinMutex()

  internal var _workDeques: [_ForkJoinWorkDeque<ForkJoinTaskBase>] = []
  internal var _workDequesMutex: _ForkJoinMutex = _ForkJoinMutex()

  internal init(_commonPool: ()) {
    self._maxThreads = _stdlib_getHardwareConcurrency()
  }

  deinit {
    _runningThreadsMutex.`deinit`()
    _submissionQueuesMutex.`deinit`()
    _workDequesMutex.`deinit`()
  }

  internal func _addRunningThread(_ thread: _ForkJoinWorkerThread) {
    ForkJoinPool._threadRegistryMutex.withLock {
      _runningThreadsMutex.withLock {
        _submissionQueuesMutex.withLock {
          _workDequesMutex.withLock {
            ForkJoinPool._threadRegistry[thread._tid!] = thread
            _runningThreads.append(thread)
            _submissionQueues.append(thread._submissionQueue)
            _workDeques.append(thread._workDeque)
          }
        }
      }
    }
  }

  internal func _removeRunningThread(_ thread: _ForkJoinWorkerThread) {
    ForkJoinPool._threadRegistryMutex.withLock {
      _runningThreadsMutex.withLock {
        _submissionQueuesMutex.withLock {
          _workDequesMutex.withLock {
            let i = _runningThreads.firstIndex { $0 === thread }!
            ForkJoinPool._threadRegistry[thread._tid!] = nil
            _runningThreads.remove(at: i)
            _submissionQueues.remove(at: i)
            _workDeques.remove(at: i)
          }
        }
      }
    }
  }

  internal func _compensateForBlockedWorkerThread(_ blockingBody: @escaping () -> ()) {
    // FIXME: limit the number of compensating threads.
    let submissionQueue = _ForkJoinWorkDeque<ForkJoinTaskBase>()
    let workDeque = _ForkJoinWorkDeque<ForkJoinTaskBase>()
    let thread = _ForkJoinWorkerThread(
      _pool: self, submissionQueue: submissionQueue, workDeque: workDeque)
    thread.startAsync()
    blockingBody()
    _ = _totalThreads.fetchAndAdd(1)
  }

  internal func _tryCreateThread(
    _ makeTask: () -> ForkJoinTaskBase?
  ) -> Bool {
    var success = false
    var oldNumThreads = _totalThreads.load()
    repeat {
      if oldNumThreads >= _maxThreads {
        return false
      }
      success = _totalThreads.compareExchange(
        expected: &oldNumThreads, desired: oldNumThreads + 1)
    } while !success
    if let task = makeTask() {
      let submissionQueue = _ForkJoinWorkDeque<ForkJoinTaskBase>()
      let workDeque = _ForkJoinWorkDeque<ForkJoinTaskBase>()
      workDeque.prepend(task)
      let thread = _ForkJoinWorkerThread(
        _pool: self, submissionQueue: submissionQueue, workDeque: workDeque)
      thread.startAsync()
    } else {
      _ = _totalThreads.fetchAndAdd(-1)
    }
    return true
  }

  internal func _stealTask() -> ForkJoinTaskBase? {
    return _workDequesMutex.withLock {
      let randomOffset = _workDeques.indices.randomElement()!
      let count = _workDeques.count
      for i in _workDeques.indices {
        let index = (i + randomOffset) % count
        if let task = _workDeques[index].tryTakeLast() {
          return task
        }
      }
      return nil
    }
  }

  /// Check if the pool has grown too large because of compensating
  /// threads.
  internal func _tryStopThread() -> Bool {
    var success = false
    var oldNumThreads = _totalThreads.load()
    repeat {
      // FIXME: magic number 2.
      if oldNumThreads <= _maxThreads + 2 {
        return false
      }
      success = _totalThreads.compareExchange(
        expected: &oldNumThreads, desired: oldNumThreads - 1)
    } while !success
    return true
  }

  internal func _submitTasksToRandomWorkers<
    C : Collection
  >(_ tasks: C)
  where C.Iterator.Element == ForkJoinTaskBase {
    if tasks.isEmpty {
      return
    }
    _submissionQueuesMutex.withLock {
      precondition(!_submissionQueues.isEmpty)
      for task in tasks {
        _submissionQueues.randomElement()!.append(task)
      }
    }
  }

  public func forkTask(_ task: ForkJoinTaskBase) {
    while true {
      // Try to inflate the pool first.
      if _tryCreateThread({ task }) {
        return
      }

      // Looks like we can't create more threads.  Submit the task to
      // a random thread.
      let done = _submissionQueuesMutex.withLock {
        () -> Bool in
        if !_submissionQueues.isEmpty {
          _submissionQueues.randomElement()!.append(task)
          return true
        }
        return false
      }
      if done {
        return
      }
    }
  }

  // FIXME: return a Future instead?
  public func forkTask<Result>(task: @escaping () -> Result) -> ForkJoinTask<Result> {
    let forkJoinTask = ForkJoinTask(_task: task)
    forkTask(forkJoinTask)
    return forkJoinTask
  }

  public static var commonPool = ForkJoinPool(_commonPool: ())

  public static func invokeAll(_ tasks: ForkJoinTaskBase...) {
    ForkJoinPool.invokeAll(tasks)
  }

  public static func invokeAll(_ tasks: [ForkJoinTaskBase]) {
    if tasks.isEmpty {
      return
    }
    if ForkJoinPool._getCurrentThread() != nil {
      // Run the first task in this thread, fork the rest.
      let first = tasks.first
      for t in tasks.dropFirst() {
        // FIXME: optimize forking in bulk.
        t.fork()
      }
      first!._run()
    } else {
      // FIXME: decide if we want to allow this.
      precondition(false)
    }
  }
}

//===----------------------------------------------------------------------===//
// Collection transformation DSL: implementation
//===----------------------------------------------------------------------===//

internal protocol _CollectionTransformerStepProtocol /*: class*/ {
  associatedtype PipelineInputElement
  associatedtype OutputElement

  func transform<
    InputCollection : Collection,
    Collector : _ElementCollector
  >(
    _ c: InputCollection,
    _ range: Range<InputCollection.Index>,
    _ collector: inout Collector
  )
  where
  InputCollection.Iterator.Element == PipelineInputElement,
  Collector.Element == OutputElement
}

internal class _CollectionTransformerStep<PipelineInputElement_, OutputElement_>
  : _CollectionTransformerStepProtocol {

  typealias PipelineInputElement = PipelineInputElement_
  typealias OutputElement = OutputElement_

  func map<U>(_ transform: @escaping (OutputElement) -> U)
    -> _CollectionTransformerStep<PipelineInputElement, U> {

    fatalError("abstract method")
  }

  func filter(_ isIncluded: @escaping (OutputElement) -> Bool)
    -> _CollectionTransformerStep<PipelineInputElement, OutputElement> {

    fatalError("abstract method")
  }

  func reduce<U>(_ initial: U, _ combine: @escaping (U, OutputElement) -> U)
    -> _CollectionTransformerFinalizer<PipelineInputElement, U> {

    fatalError("abstract method")
  }

  func collectTo<
    C : BuildableCollectionProtocol
  >(_: C.Type) -> _CollectionTransformerFinalizer<PipelineInputElement, C>
  where
  C.Builder.Destination == C,
  C.Builder.Element == C.Iterator.Element,
  C.Iterator.Element == OutputElement {

    fatalError("abstract method")
  }

  func transform<
    InputCollection : Collection,
    Collector : _ElementCollector
  >(
    _ c: InputCollection,
    _ range: Range<InputCollection.Index>,
    _ collector: inout Collector
  )
  where
  InputCollection.Iterator.Element == PipelineInputElement,
  Collector.Element == OutputElement {
    fatalError("abstract method")
  }
}

final internal class _CollectionTransformerStepCollectionSource<
  PipelineInputElement
> : _CollectionTransformerStep<PipelineInputElement, PipelineInputElement> {

  typealias InputElement = PipelineInputElement

  override func map<U>(_ transform: @escaping (InputElement) -> U)
    -> _CollectionTransformerStep<PipelineInputElement, U> {

    return _CollectionTransformerStepOneToMaybeOne(self) {
      transform($0)
    }
  }

  override func filter(_ isIncluded: @escaping (InputElement) -> Bool)
    -> _CollectionTransformerStep<PipelineInputElement, InputElement> {

    return _CollectionTransformerStepOneToMaybeOne(self) {
      isIncluded($0) ? $0 : nil
    }
  }

  override func reduce<U>(_ initial: U, _ combine: @escaping (U, InputElement) -> U)
    -> _CollectionTransformerFinalizer<PipelineInputElement, U> {

    return _CollectionTransformerFinalizerReduce(self, initial, combine)
  }

  override func collectTo<
    C : BuildableCollectionProtocol
  >(_ c: C.Type) -> _CollectionTransformerFinalizer<PipelineInputElement, C>
  where
  C.Builder.Destination == C,
  C.Builder.Element == C.Iterator.Element,
  C.Iterator.Element == OutputElement {

    return _CollectionTransformerFinalizerCollectTo(self, c)
  }

  override func transform<
    InputCollection : Collection,
    Collector : _ElementCollector
  >(
    _ c: InputCollection,
    _ range: Range<InputCollection.Index>,
    _ collector: inout Collector
  )
  where
  InputCollection.Iterator.Element == PipelineInputElement,
  Collector.Element == OutputElement {
    var i = range.lowerBound
    while i != range.upperBound {
      let e = c[i]
      collector.append(e)
      c.formIndex(after: &i)
    }
  }
}

final internal class _CollectionTransformerStepOneToMaybeOne<
  PipelineInputElement,
  OutputElement,
  InputStep : _CollectionTransformerStepProtocol
> : _CollectionTransformerStep<PipelineInputElement, OutputElement>
where InputStep.PipelineInputElement == PipelineInputElement {

  typealias _Self = _CollectionTransformerStepOneToMaybeOne
  typealias InputElement = InputStep.OutputElement

  let _input: InputStep
  let _transform: (InputElement) -> OutputElement?

  init(_ input: InputStep, _ transform: @escaping (InputElement) -> OutputElement?) {
    self._input = input
    self._transform = transform
    super.init()
  }

  override func map<U>(_ transform: @escaping (OutputElement) -> U)
    -> _CollectionTransformerStep<PipelineInputElement, U> {

    // Let the closure below capture only one variable, not the whole `self`.
    let localTransform = _transform
    return _CollectionTransformerStepOneToMaybeOne<PipelineInputElement, U, InputStep>(_input) {
      (input: InputElement) -> U? in
      if let e = localTransform(input) {
        return transform(e)
      }
      return nil
    }
  }

  override func filter(_ isIncluded: @escaping (OutputElement) -> Bool)
    -> _CollectionTransformerStep<PipelineInputElement, OutputElement> {

    // Let the closure below capture only one variable, not the whole `self`.
    let localTransform = _transform
    return _CollectionTransformerStepOneToMaybeOne<PipelineInputElement, OutputElement, InputStep>(_input) {
      (input: InputElement) -> OutputElement? in
      if let e = localTransform(input) {
        return isIncluded(e) ? e : nil
      }
      return nil
    }
  }

  override func reduce<U>(_ initial: U, _ combine: @escaping (U, OutputElement) -> U)
    -> _CollectionTransformerFinalizer<PipelineInputElement, U> {

    return _CollectionTransformerFinalizerReduce(self, initial, combine)
  }

  override func collectTo<
    C : BuildableCollectionProtocol
  >(_ c: C.Type) -> _CollectionTransformerFinalizer<PipelineInputElement, C>
  where
  C.Builder.Destination == C,
  C.Builder.Element == C.Iterator.Element,
  C.Iterator.Element == OutputElement {

    return _CollectionTransformerFinalizerCollectTo(self, c)
  }

  override func transform<
    InputCollection : Collection,
    Collector : _ElementCollector
  >(
    _ c: InputCollection,
    _ range: Range<InputCollection.Index>,
    _ collector: inout Collector
  )
  where
  InputCollection.Iterator.Element == PipelineInputElement,
  Collector.Element == OutputElement {
    var collectorWrapper =
      _ElementCollectorOneToMaybeOne(collector, _transform)
    _input.transform(c, range, &collectorWrapper)
    collector = collectorWrapper._baseCollector
  }
}

struct _ElementCollectorOneToMaybeOne<
  BaseCollector : _ElementCollector,
  Element_
> : _ElementCollector {
  typealias Element = Element_

  var _baseCollector: BaseCollector
  var _transform: (Element) -> BaseCollector.Element?

  init(
    _ baseCollector: BaseCollector,
    _ transform: @escaping (Element) -> BaseCollector.Element?
  ) {
    self._baseCollector = baseCollector
    self._transform = transform
  }

  mutating func sizeHint(_ approximateSize: Int) {}

  mutating func append(_ element: Element) {
    if let e = _transform(element) {
      _baseCollector.append(e)
    }
  }

  mutating func append<
    C : Collection
  >(contentsOf elements: C)
  where C.Iterator.Element == Element {
    for e in elements {
      append(e)
    }
  }
}

protocol _ElementCollector {
  associatedtype Element

  mutating func sizeHint(_ approximateSize: Int)

  mutating func append(_ element: Element)

  mutating func append<
    C : Collection
  >(contentsOf elements: C)
  where C.Iterator.Element == Element
}

class _CollectionTransformerFinalizer<PipelineInputElement, Result> {
  func transform<
    InputCollection : Collection
  >(_ c: InputCollection) -> Result
  where InputCollection.Iterator.Element == PipelineInputElement {
    fatalError("implement")
  }
}

final class _CollectionTransformerFinalizerReduce<
  PipelineInputElement,
  U,
  InputElementTy,
  InputStep : _CollectionTransformerStepProtocol
> : _CollectionTransformerFinalizer<PipelineInputElement, U>
where
InputStep.OutputElement == InputElementTy,
InputStep.PipelineInputElement == PipelineInputElement {

  var _input: InputStep
  var _initial: U
  var _combine: (U, InputElementTy) -> U

  init(_ input: InputStep, _ initial: U, _ combine: @escaping (U, InputElementTy) -> U) {
    self._input = input
    self._initial = initial
    self._combine = combine
  }

  override func transform<
    InputCollection : Collection
  >(_ c: InputCollection) -> U
  where InputCollection.Iterator.Element == PipelineInputElement {
    var collector = _ElementCollectorReduce(_initial, _combine)
    _input.transform(c, c.startIndex..<c.endIndex, &collector)
    return collector.takeResult()
  }
}

struct _ElementCollectorReduce<Element_, Result> : _ElementCollector {
  typealias Element = Element_

  var _current: Result
  var _combine: (Result, Element) -> Result

  init(_ initial: Result, _ combine: @escaping (Result, Element) -> Result) {
    self._current = initial
    self._combine = combine
  }

  mutating func sizeHint(_ approximateSize: Int) {}

  mutating func append(_ element: Element) {
    _current = _combine(_current, element)
  }

  mutating func append<
    C : Collection
  >(contentsOf elements: C)
  where C.Iterator.Element == Element {
    for e in elements {
      append(e)
    }
  }

  mutating func takeResult() -> Result {
    return _current
  }
}

final class _CollectionTransformerFinalizerCollectTo<
  PipelineInputElement,
  U : BuildableCollectionProtocol,
  InputElementTy,
  InputStep : _CollectionTransformerStepProtocol
> : _CollectionTransformerFinalizer<PipelineInputElement, U>
where
InputStep.OutputElement == InputElementTy,
InputStep.PipelineInputElement == PipelineInputElement,
U.Builder.Destination == U,
U.Builder.Element == U.Iterator.Element,
U.Iterator.Element == InputStep.OutputElement {

  var _input: InputStep

  init(_ input: InputStep, _: U.Type) {
    self._input = input
  }

  override func transform<
    InputCollection : Collection
  >(_ c: InputCollection) -> U
  where InputCollection.Iterator.Element == PipelineInputElement {
    var collector = _ElementCollectorCollectTo<U>()
    _input.transform(c, c.startIndex..<c.endIndex, &collector)
    return collector.takeResult()
  }
}

struct _ElementCollectorCollectTo<
  BuildableCollection : BuildableCollectionProtocol
> : _ElementCollector
where
BuildableCollection.Builder.Destination == BuildableCollection,
BuildableCollection.Builder.Element == BuildableCollection.Iterator.Element {

  typealias Element = BuildableCollection.Iterator.Element

  var _builder: BuildableCollection.Builder

  init() {
    self._builder = BuildableCollection.Builder()
  }

  mutating func sizeHint(_ approximateSize: Int) {
    _builder.sizeHint(approximateSize)
  }

  mutating func append(_ element: Element) {
    _builder.append(element)
  }

  mutating func append<
    C : Collection
  >(contentsOf elements: C)
  where C.Iterator.Element == Element {
    _builder.append(contentsOf: elements)
  }

  mutating func takeResult() -> BuildableCollection {
    return _builder.takeResult()
  }
}

internal func _optimizeCollectionTransformer<PipelineInputElement, Result>(
  _ transformer: _CollectionTransformerFinalizer<PipelineInputElement, Result>
) -> _CollectionTransformerFinalizer<PipelineInputElement, Result> {
  return transformer
}

internal func _runCollectionTransformer<
  InputCollection : Collection, Result
>(
  _ c: InputCollection,
  _ transformer: _CollectionTransformerFinalizer<InputCollection.Iterator.Element, Result>
) -> Result {
  dump(transformer)
  let optimized = _optimizeCollectionTransformer(transformer)
  dump(optimized)
  return transformer.transform(c)
}

//===----------------------------------------------------------------------===//
// Collection transformation DSL: public interface
//===----------------------------------------------------------------------===//

public struct CollectionTransformerPipeline<
  InputCollection : Collection, T
> {
  internal var _input: InputCollection
  internal var _step: _CollectionTransformerStep<InputCollection.Iterator.Element, T>

  public func map<U>(_ transform: @escaping (T) -> U)
    -> CollectionTransformerPipeline<InputCollection, U> {

    return CollectionTransformerPipeline<InputCollection, U>(
      _input: _input,
      _step: _step.map(transform)
    )
  }

  public func filter(_ isIncluded: @escaping (T) -> Bool)
    -> CollectionTransformerPipeline<InputCollection, T> {

    return CollectionTransformerPipeline<InputCollection, T>(
      _input: _input,
      _step: _step.filter(isIncluded)
    )
  }

  public func reduce<U>(
    _ initial: U, _ combine: @escaping (U, T) -> U
  ) -> U {
    return _runCollectionTransformer(_input, _step.reduce(initial, combine))
  }

  public func collectTo<
    C : BuildableCollectionProtocol
  >(_ c: C.Type) -> C
  where
  C.Builder.Destination == C,
  C.Iterator.Element == T,
  C.Builder.Element == T {
    return _runCollectionTransformer(_input, _step.collectTo(c))
  }

  public func toArray() -> [T] {
    return collectTo(Array<T>.self)
  }
}

public func transform<C : Collection>(_ c: C)
  -> CollectionTransformerPipeline<C, C.Iterator.Element> {

  return CollectionTransformerPipeline<C, C.Iterator.Element>(
    _input: c,
    _step: _CollectionTransformerStepCollectionSource<C.Iterator.Element>())
}

//===----------------------------------------------------------------------===//
// Collection transformation DSL: tests
//===----------------------------------------------------------------------===//

import StdlibUnittest


var t = TestSuite("t")

t.test("fusion/map+reduce") {
  let xs = [ 1, 2, 3 ]
  let result =
    transform(xs)
    .map { $0 * 2 }
    .reduce(0, { $0 + $1 })
  expectEqual(12, result)
}

t.test("fusion/map+filter+reduce") {
  let xs = [ 1, 2, 3 ]
  let result = transform(xs)
    .map { $0 * 2 }
    .filter { $0 != 0 }
    .reduce(0, { $0 + $1 })
  expectEqual(12, result)
}

t.test("fusion/map+collectTo") {
  let xs = [ 1, 2, 3 ]
  let result =
    transform(xs)
    .map { $0 * 2 }
    .collectTo(Array<Int>.self)
  expectEqual([ 2, 4, 6 ], result)
}

t.test("fusion/map+toArray") {
  let xs = [ 1, 2, 3 ]
  let result =
    transform(xs)
    .map { $0 * 2 }
    .toArray()
  expectEqual([ 2, 4, 6 ], result)
}

t.test("ForkJoinPool.forkTask") {
  var tasks: [ForkJoinTask<()>] = []
  for i in 0..<100 {
    tasks.append(ForkJoinPool.commonPool.forkTask {
      () -> () in
      var result = 1
      for i in 0..<10000 {
        result = result &* i
        _blackHole(result)
      }
      return ()
    })
  }
  for t in tasks {
    t.wait()
  }
}

func fib(_ n: Int) -> Int {
  if n == 1 || n == 2 {
    return 1
  }
  if n == 38 {
    print("\(pthread_self()) fib(\(n))")
  }
  if n < 39 {
    let r = fib(n - 1) + fib(n - 2)
    _blackHole(r)
    return r
  }
  print("fib(\(n))")
  let t1 = ForkJoinTask() { fib(n - 1) }
  let t2 = ForkJoinTask() { fib(n - 2) }
  ForkJoinPool.invokeAll(t1, t2)
  return t2.waitAndGetResult() + t1.waitAndGetResult()
}

t.test("ForkJoinPool.forkTask/Fibonacci") {
  let t = ForkJoinPool.commonPool.forkTask { fib(40) }
  expectEqual(102334155, t.waitAndGetResult())
}

func _parallelMap(_ input: [Int], transform: @escaping (Int) -> Int, range: Range<Int>)
  -> Array<Int>.Builder {

  var builder = Array<Int>.Builder()
  if range.count < 1_000 {
    builder.append(contentsOf: input[range].map(transform))
  } else {
    let tasks = input.split(range).map {
      (subRange) in
      ForkJoinTask<Array<Int>.Builder> {
        _parallelMap(input, transform: transform, range: subRange)
      }
    }
    ForkJoinPool.invokeAll(tasks)
    for t in tasks {
      var otherBuilder = t.waitAndGetResult()
      builder.moveContentsOf(&otherBuilder)
    }
  }
  return builder
}

func parallelMap(_ input: [Int], transform: @escaping (Int) -> Int) -> [Int] {
  let t = ForkJoinPool.commonPool.forkTask {
    _parallelMap(
      input,
      transform: transform,
      range: input.startIndex..<input.endIndex)
  }
  var builder = t.waitAndGetResult()
  return builder.takeResult()
}

t.test("ForkJoinPool.forkTask/MapArray") {
  expectEqual(
    Array(2..<1_001),
    parallelMap(Array(1..<1_000)) { $0 + 1 }
  )
}

/*
 * FIXME: reduce compiler crasher
t.test("ForkJoinPool.forkTask") {
  func fib(_ n: Int) -> Int {
    if n == 0 || n == 1 {
      return 1
    }
    let t1 = ForkJoinPool.commonPool.forkTask { fib(n - 1) }
    let t2 = ForkJoinPool.commonPool.forkTask { fib(n - 2) }
    return t2.waitAndGetResult() + t1.waitAndGetResult()
  }
  expectEqual(0, fib(10))
}
*/


/*

Useful links:

http://habrahabr.ru/post/255659/

*/

runAllTests()
