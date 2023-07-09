//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Swift

#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#elseif os(Windows)
import WinSDK
#endif

/// A `DistributedActorSystem` designed for local only testing.
///
/// It will crash on any attempt of remote communication, but can be useful
/// for learning about `distributed actor` isolation, as well as early
/// prototyping stages of development where a real system is not necessary yet.
@available(SwiftStdlib 5.7, *)
public final class LocalTestingDistributedActorSystem: DistributedActorSystem, @unchecked Sendable {
  public typealias ActorID = LocalTestingActorID
  public typealias ResultHandler = LocalTestingInvocationResultHandler
  public typealias InvocationEncoder = LocalTestingInvocationEncoder
  public typealias InvocationDecoder = LocalTestingInvocationDecoder
  public typealias SerializationRequirement = Codable

  private var activeActors: [ActorID: any DistributedActor] = [:]
  private let activeActorsLock = _Lock()

  private var idProvider: ActorIDProvider = ActorIDProvider()
  private var assignedIDs: Set<ActorID> = []
  private let assignedIDsLock = _Lock()

  public init() {}

  public func resolve<Act>(id: ActorID, as actorType: Act.Type)
    throws -> Act? where Act: DistributedActor {
    guard let anyActor = self.activeActorsLock.withLock({ self.activeActors[id] }) else {
      throw LocalTestingDistributedActorSystemError(message: "Unable to locate id '\(id)' locally")
    }
    guard let actor = anyActor as? Act else {
      throw LocalTestingDistributedActorSystemError(message: "Failed to resolve id '\(id)' as \(Act.Type.self)")
    }
    return actor
  }

  public func assignID<Act>(_ actorType: Act.Type) -> ActorID
    where Act: DistributedActor {
    let id = self.idProvider.next()
    self.assignedIDsLock.withLock {
      self.assignedIDs.insert(id)
    }
    return id
  }

  public func actorReady<Act>(_ actor: Act)
    where Act: DistributedActor, Act.ID == ActorID {
    guard self.assignedIDsLock.withLock({ self.assignedIDs.contains(actor.id) }) else {
      fatalError("Attempted to mark an unknown actor '\(actor.id)' ready")
    }
    self.activeActorsLock.withLock {
      self.activeActors[actor.id] = actor
    }
  }

  public func resignID(_ id: ActorID) {
    self.activeActorsLock.withLock {
      self.activeActors.removeValue(forKey: id)
    }
  }

  public func makeInvocationEncoder() -> InvocationEncoder {
    .init()
  }

  public func remoteCall<Act, Err, Res>(
    on actor: Act,
    target: RemoteCallTarget,
    invocation: inout InvocationEncoder,
    throwing errorType: Err.Type,
    returning returnType: Res.Type
  ) async throws -> Res
    where Act: DistributedActor,
          Act.ID == ActorID,
          Err: Error,
          Res: SerializationRequirement {
    fatalError("Attempted to make remote call to \(target) on actor \(actor) using a local-only actor system")
  }

  public func remoteCallVoid<Act, Err>(
    on actor: Act,
    target: RemoteCallTarget,
    invocation: inout InvocationEncoder,
    throwing errorType: Err.Type
  ) async throws
    where Act: DistributedActor,
          Act.ID == ActorID,
          Err: Error {
    fatalError("Attempted to make remote call to \(target) on actor \(actor) using  a local-only actor system")
  }

  private struct ActorIDProvider {
    private var counter: Int = 0
    private let counterLock = _Lock()

    init() {}

    mutating func next() -> LocalTestingActorID {
      let id: Int = self.counterLock.withLock {
        self.counter += 1
        return self.counter
      }
      return LocalTestingActorID(id: "\(id)")
    }
  }
}

@available(SwiftStdlib 5.7, *)
@available(*, deprecated, renamed: "LocalTestingActorID")
public typealias LocalTestingActorAddress = LocalTestingActorID

@available(SwiftStdlib 5.7, *)
public struct LocalTestingActorID: Hashable, Sendable, Codable {
  @available(*, deprecated, renamed: "id")
  public var address: String {
    self.id
  }
  public let id: String

  @available(*, deprecated, renamed: "init(id:)")
  public init(parse id: String) {
    self.id = id
  }

  public init(id: String) {
    self.id = id
  }

  public init(from decoder: Decoder) throws {
    let container = try decoder.singleValueContainer()
    self.id = try container.decode(String.self)
  }

  public func encode(to encoder: Encoder) throws {
    var container = encoder.singleValueContainer()
    try container.encode(self.id)
  }
}

@available(SwiftStdlib 5.7, *)
public struct LocalTestingInvocationEncoder: DistributedTargetInvocationEncoder {
  public typealias SerializationRequirement = Codable

  public mutating func recordGenericSubstitution<T>(_ type: T.Type) throws {
    fatalError("Attempted to call encoder method in a local-only actor system")
  }

  public mutating func recordArgument<Value: SerializationRequirement>(_ argument: RemoteCallArgument<Value>) throws {
    fatalError("Attempted to call encoder method in a local-only actor system")
  }

  public mutating func recordErrorType<E: Error>(_ type: E.Type) throws {
    fatalError("Attempted to call encoder method in a local-only actor system")
  }

  public mutating func recordReturnType<R: SerializationRequirement>(_ type: R.Type) throws {
    fatalError("Attempted to call encoder method in a local-only actor system")
  }

  public mutating func doneRecording() throws {
    fatalError("Attempted to call encoder method in a local-only actor system")
  }
}

@available(SwiftStdlib 5.7, *)
public final class LocalTestingInvocationDecoder: DistributedTargetInvocationDecoder {
  public typealias SerializationRequirement = Codable

  public func decodeGenericSubstitutions() throws -> [Any.Type] {
    fatalError("Attempted to call decoder method in a local-only actor system")
  }

  public func decodeNextArgument<Argument: SerializationRequirement>() throws -> Argument {
    fatalError("Attempted to call decoder method in a local-only actor system")
  }

  public func decodeErrorType() throws -> Any.Type? {
    fatalError("Attempted to call decoder method in a local-only actor system")
  }

  public func decodeReturnType() throws -> Any.Type? {
    fatalError("Attempted to call decoder method in a local-only actor system")
  }
}

@available(SwiftStdlib 5.7, *)
public struct LocalTestingInvocationResultHandler: DistributedTargetInvocationResultHandler {
  public typealias SerializationRequirement = Codable
  public func onReturn<Success: SerializationRequirement>(value: Success) async throws {
    fatalError("Attempted to call decoder method in a local-only actor system")
  }

  public func onReturnVoid() async throws {
    fatalError("Attempted to call decoder method in a local-only actor system")
  }

  public func onThrow<Err: Error>(error: Err) async throws {
    fatalError("Attempted to call decoder method in a local-only actor system")
  }
}

// === errors ----------------------------------------------------------------

@available(SwiftStdlib 5.7, *)
public struct LocalTestingDistributedActorSystemError: DistributedActorSystemError {
  public let message: String

  public init(message: String) {
    self.message = message
  }
}

// === lock ----------------------------------------------------------------

@available(SwiftStdlib 5.7, *)
fileprivate class _Lock {
  #if os(iOS) || os(macOS) || os(tvOS) || os(watchOS)
  private let underlying: UnsafeMutablePointer<os_unfair_lock>
  #elseif os(Windows)
  private let underlying: UnsafeMutablePointer<SRWLOCK>
  #elseif os(WASI)
  // pthread is currently not available on WASI
  #elseif os(Cygwin) || os(FreeBSD) || os(OpenBSD)
  private let underlying: UnsafeMutablePointer<pthread_mutex_t?>
  #else
  private let underlying: UnsafeMutablePointer<pthread_mutex_t>
  #endif

  init() {
    #if os(iOS) || os(macOS) || os(tvOS) || os(watchOS)
    self.underlying = UnsafeMutablePointer.allocate(capacity: 1)
    self.underlying.initialize(to: os_unfair_lock())
    #elseif os(Windows)
    self.underlying = UnsafeMutablePointer.allocate(capacity: 1)
    InitializeSRWLock(self.underlying)
    #elseif os(WASI)
    // WASI environment has only a single thread
    #else
    self.underlying = UnsafeMutablePointer.allocate(capacity: 1)
    guard pthread_mutex_init(self.underlying, nil) == 0 else {
      fatalError("pthread_mutex_init failed")
    }
    #endif
  }

  deinit {
    #if os(iOS) || os(macOS) || os(tvOS) || os(watchOS)
    // `os_unfair_lock`s do not need to be explicitly destroyed
    #elseif os(Windows)
    // `SRWLOCK`s do not need to be explicitly destroyed
    #elseif os(WASI)
    // WASI environment has only a single thread
    #else
    guard pthread_mutex_destroy(self.underlying) == 0 else {
      fatalError("pthread_mutex_destroy failed")
    }
    #endif

    #if !os(WASI)
    self.underlying.deinitialize(count: 1)
    self.underlying.deallocate()
    #endif
  }


  @discardableResult
  func withLock<T>(_ body: () -> T) -> T {
    #if os(iOS) || os(macOS) || os(tvOS) || os(watchOS)
    os_unfair_lock_lock(self.underlying)
    #elseif os(Windows)
    AcquireSRWLockExclusive(self.underlying)
    #elseif os(WASI)
    // WASI environment has only a single thread
    #else
    guard pthread_mutex_lock(self.underlying) == 0 else {
      fatalError("pthread_mutex_lock failed")
    }
    #endif

    defer {
      #if os(iOS) || os(macOS) || os(tvOS) || os(watchOS)
      os_unfair_lock_unlock(self.underlying)    
      #elseif os(Windows)
      ReleaseSRWLockExclusive(self.underlying)
      #elseif os(WASI)
      // WASI environment has only a single thread
      #else
      guard pthread_mutex_unlock(self.underlying) == 0 else {
        fatalError("pthread_mutex_unlock failed")
      }
      #endif
    }
    
    return body()
  }
}
