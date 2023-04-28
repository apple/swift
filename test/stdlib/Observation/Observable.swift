// REQUIRES: swift_swift_parser, executable_test

// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library -enable-experimental-feature Macros -Xfrontend -plugin-path -Xfrontend %swift-host-lib-dir/plugins)

// REQUIRES: observation
// REQUIRES: concurrency
// REQUIRES: objc_interop
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import StdlibUnittest
import _Observation
import _Concurrency

@available(SwiftStdlib 5.9, *)
@MainActor @Observable
final class StateMachine {
  enum State {
    case initializing
    case running
    case complete
  }

  var state: State = .initializing
}

@usableFromInline
@inline(never)
func _blackHole<T>(_ value: T) { }

final class UnsafeBox<Contents>: @unchecked Sendable {
  var contents: Contents

  init(_ contents: Contents) {
    self.contents = contents
  }
}

@available(SwiftStdlib 5.9, *)
final class TestWithoutMacro: Observable {
  let _registrar = ObservationRegistrar<TestWithoutMacro>()

  nonisolated func changes<Isolation>(
    for properties: TrackedProperties<TestWithoutMacro>, 
    isolatedTo isolation: Isolation
  ) -> ObservedChanges<TestWithoutMacro, Isolation> where Isolation: Actor {
    _registrar.changes(for: properties, isolatedTo: isolation)
  }

  nonisolated func values<Member>(
    for keyPath: KeyPath<TestWithoutMacro, Member>
  ) -> ObservedValues<TestWithoutMacro, Member> where Member : Sendable {
    _registrar.values(for: keyPath)
  }

  private struct _Storage {
    var field1 = "test"
    var field2 = "test"
    var field3 = 0
  }

  private var _storage = _Storage()

  var field1: String {
    get {
      _registrar.access(self, keyPath: \.field1)
      return _storage.field1
    }
    set {
      _registrar.withMutation(of: self, keyPath: \.field1) {
        _storage.field1 = newValue
      }
    }
  }

  var field2: String {
    get {
      _registrar.access(self, keyPath: \.field2)
      return _storage.field2
    }
    set {
      _registrar.withMutation(of: self, keyPath: \.field2) {
        _storage.field2 = newValue
      }
    }
  }

  var field3: Int {
    get {
      _registrar.access(self, keyPath: \.field3)
      return _storage.field3
    }
    set {
      _registrar.withMutation(of: self, keyPath: \.field3) {
        _storage.field3 = newValue
      }
    }
  }
}

@available(SwiftStdlib 5.9, *)
@Observable final class TestWithMacro {
  var field1 = "test"
  var field2 = "test"
  var field3 = 0
}

extension AsyncSequence {
  func triggerIteration(
    _ continuation: UnsafeContinuation<Void, Never>
  ) -> TriggerSequence<Self> {
    TriggerSequence(self, continuation: continuation)
  }
}

struct TriggerSequence<Base: AsyncSequence> {
  let base: Base
  let continuation: UnsafeContinuation<Void, Never>

  init(_ base: Base, continuation: UnsafeContinuation<Void, Never>) {
    self.base = base
    self.continuation = continuation
  }
}

extension TriggerSequence: AsyncSequence {
  typealias Element = Base.Element

  struct Iterator: AsyncIteratorProtocol {
    var continuation: UnsafeContinuation<Void, Never>?
    var base: Base.AsyncIterator

    init(
      _ base: Base.AsyncIterator, 
      continuation: UnsafeContinuation<Void, Never>
    ) {
      self.base = base
      self.continuation = continuation
    }

    mutating func next() async rethrows -> Base.Element? {
      if let continuation {
        self.continuation = nil
        continuation.resume()
      }
      return try await base.next()
    }
  }

  func makeAsyncIterator() -> Iterator {
    Iterator(base.makeAsyncIterator(), continuation: continuation)
  }
}

@main struct Main {
  @MainActor
  static func main() async {
    let suite = TestSuite("Observable")

    suite.test("unobserved value changes (macro)") {
      let subject = TestWithMacro()
      for i in 0..<100 {
        subject.field3 = i
      }
    }

    suite.test("unobserved value changes (nonmacro)") {
      let subject = TestWithoutMacro()
      for i in 0..<100 {
        subject.field3 = i
      }
    }

    suite.test("changes emit values (macro)") { @MainActor in
      let subject = TestWithMacro()
      var t: Task<String?, Never>?
      await withUnsafeContinuation { continuation in
        t = Task { @MainActor in
          // Note: this must be fully established 
          // so we must await the trigger to fire
          let values = subject.values(for: \.field1)
            .triggerIteration(continuation)
          for await value in values {
            return value
          }
          return nil
        }
      }
      subject.field1 = "a"
      let value = await t!.value
      expectEqual(value, "a")
    }

    suite.test("changes emit values (nonmacro)") { @MainActor in
      let subject = TestWithoutMacro()
      var t: Task<String?, Never>?
      await withUnsafeContinuation { continuation in
        t = Task { @MainActor in
          // Note: this must be fully established 
          // so we must await the trigger to fire
          let values = subject.values(for: \.field1)
            .triggerIteration(continuation)
          for await value in values {
            return value
          }
          return nil
        }
      }
      subject.field1 = "a"
      let value = await t!.value
      expectEqual(value, "a")
    }


    suite.test("changes cancellation terminates") { @MainActor in
      let subject = TestWithMacro()
      var finished = false
      let t = Task { @MainActor in
        for await _ in subject.changes(for: \.field1) {

        }
        finished = true
      }
      try? await Task.sleep(for: .seconds(0.1))
      expectEqual(finished, false)
      t.cancel()
      try? await Task.sleep(for: .seconds(0.1))
      expectEqual(finished, true)
    }

    suite.test("emit values (macro)") { @MainActor in
      let subject = TestWithMacro()
      var t: Task<String?, Never>?
      await withUnsafeContinuation { continuation in
        t = Task { @MainActor in
          // Note: this must be fully established 
          // so we must await the trigger to fire
          let values = subject.values(for: \.field1)
            .triggerIteration(continuation)
          for await value in values {
            return value
          }
          return nil
        }
      }
      subject.field1 = "a"
      let value = await t!.value
      expectEqual(value, "a")
    }

    suite.test("emit values (nonmacro)") { @MainActor in
      let subject = TestWithoutMacro()
      var t: Task<String?, Never>?
      await withUnsafeContinuation { continuation in
        t = Task { @MainActor in
          // Note: this must be fully established 
          // so we must await the trigger to fire
          let values = subject.values(for: \.field1)
            .triggerIteration(continuation)
          for await value in values {
            return value
          }
          return nil
        }
      }
      subject.field1 = "a"
      let value = await t!.value
      expectEqual(value, "a")
    }

    suite.test("tracking") { @MainActor in
      let subject = TestWithMacro()
      let changed = UnsafeBox(false)
      ObservationTracking.withTracking {
        _blackHole(subject.field1)
      } onChange: {
        changed.contents = true
      }
      expectEqual(changed.contents, false)
      subject.field2 = "asdf"
      expectEqual(changed.contents, false)
      subject.field1 = "asdf"
      expectEqual(changed.contents, true)
    }

    await runAllTestsAsync()
  }
}
