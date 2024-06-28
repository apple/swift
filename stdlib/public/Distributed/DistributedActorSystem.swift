//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020-2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
import Swift
import _Concurrency

/// A distributed actor system underpins and implements all functionality of distributed actors.
///
/// A ``DistributedActor`` is always initialized in association with some concrete actor system.
/// That actor system instance is then used to manage the identity of the actor, as well as
/// handle all remote interactions of the distributed actor.
///
/// ## Using a DistributedActorSystem library
///
/// From a library user's perspective (e.g. someone using a `ClusterSystem` or `SampleWebSocketActorSystem`),
/// the basic use of a distributed actor system is fairly opaque.
///
/// Any distributed actor must declare what actor system it is able to operate with. This is done either by a
/// `typealias ActorSystem` in the body of such `distributed actor` declaration, or a module-wide global
/// `typealias DefaultDistributedActorSystem`. Refer to the ``DistributedActor`` documentation to learn more
/// about the tradeoffs of these approaches.
///
/// Once an actor has declared the system it is able to work with, an instance of the system must be provided
/// at initialization time, in order for the system to be able to take over the actor's identity management.
///
/// For example, a simple distributed actor may look like this:
///
/// ```swift
/// distributed actor Greeter {
///   init(name: String, actorSystem: ActorSystem) {
///     self.name = name
///     self.actorSystem = actorSystem // required (!) initialization of implicit actorSystem property
///   }
/// }
/// ```
/// Notice that every distributed actor initializer must initialize the synthesized ``DistributedActor/actorSystem``.
/// This property is later used for identity management and other remote interactions of the actor.
/// For more details refer to ``DistributedActor`` which explains more about declaring distributed actors.
///
/// For more details about how the specific actor system implementation deals with remote message transports
/// and serialization, please refer to the specific system's documentation.
///
/// > Note: For example, you may refer to the [Swift Distributed Actors cluster library](https://github.com/apple/swift-distributed-actors/) documentation,
/// > which is one example of such feature complete distributed actor system implementation.
///
/// ## Implementing a DistributedActorSystem
///
/// This section is dedicated to distributed actor system library authors, and generally can be skipped over
/// by library users, as it explains the interactions of synthesized code and specific distributed actor system methods
/// and how they must be implemented.
///
/// Methods discussed in this section are generally not intended to be called directly, but instead will have calls
/// generated to them from distributed actor declarations in appropriate places (such as initializers, `distributed func` calls, or `distributed` computed properties).
///
/// ### Assigning and Resigning Actor Identifiers
///
/// During a local distributed actor's initialization (i.e. any `init` of a `distributed actor`), the actor system will
/// be invoked in order to assign an ``ActorID`` for this actor.
///
/// A call to ``assignID(_:)`` is made during the initialization of the distributed actor.
/// The snippet below showcases this, though no guarantees are made at this point about the exact placement of this call.
///
/// ```swift
/// distributed actor ShowcaseIDInit {
///   // let actorSystem: ActorSystem // synthesized;
///
///   // typealias ID = ActorSystem.ActorID
///   // let id: ID // synthesized; implements `Identifiable.id` requirement
///
///   init(actorSystem: ActorSystem) {
///     self.actorSystem = actorSystem
///     // ...
///     // self.id = actorSystem.assignID(Self.self) // synthesized;
///     // ...
///   }
/// }
/// ```
///
/// The result of ``assignID(_:)`` is then directly stored in the synthesized `id` property of the actor.
///
/// The actor system should assign _globally unique_ identifiers to types, such that they may be properly resolved
/// from any process in the distributed actor system. The exact shape of the ``ActorID`` is left up to the library to decide.
/// It can be as small as an integer based identifier, or as large as a series of key-value pairs identifying the actor.
///
/// The actor system must retain a mapping from the ``ActorID`` to the specific actor _instance_ which it is given in
/// ``actorReady(_:)`` in order to implement the ``resolve(id:using:)`` method, which is how incoming and outgoing remote calls are made possible.
///
/// Users have no control over this assignment, nor are they allowed to set the `id` property explicitly.
/// The ``DistributedActor/id`` is used to implement the distributed actor's ``Hashable``, ``Equatable``,
/// and even ``Codable`` conformance (which is synthesized if and only if the ``ActorID`` is ``Codable`` itself).
///
/// > Tip: Take note that throwing or failable initializers complicate this somewhat. Thankfully, the compiler
/// > will always emit the right code such that every ``assignID(_:)`` is balanced with a ``resignID(_:)`` call,
/// > when the actor either failed to initialize or deinitialize properly.
/// >
/// > It is also possible that a throwing initializer throws before assigning the `actorSystem` and `id` properties.
/// > In such case, no `assignID` nor `resignID` calls are made. There is no risk of the compiler ever attempting
/// > to call a `resignID(_:)` without first having assigned given ID.
///
/// Manually invoking `assignID` and `resignID` is generally not recommended but isn't strictly a programmer error,
/// and it is up to the actor system to decide how to deal with such calls.
///
/// Once the ``distributed actor`` deinitializes, a call to ``resignID(_:)`` will be made. Generally this is made from
/// the distributed actor's `deinit`, however in the case of throwing initializers it may also happen during such failed
/// init, in order to release the ID that is no longer used.
///
/// ```swift
/// // Synthesized inside a distributed actor's deinit:
/// deinit {
///   // actorSystem.resignID(self.id)
/// }
/// ```
///
/// After an ID is resigned, it technically could be used to identify another instance.
/// For example, an advanced actor system implementation could use such approach to implement actors which
/// are created "ad-hoc" and always contain the appropriate ID, and if one isn't allocated yet for such ID,
/// it could _then_ create one on demand and make sure it is assigned the required ID.
///
/// ### Readying Distributed Actors
///
/// Once a `distributed actor` has been _fully initialized_ during its initializer, a call to ``actorReady(_:)``
/// is synthesized. This call is made after the actor's properties (including all user-declared properties) have been
/// initialized, but before other user-defined code in the initializer gets a chance to run.
///
/// > Note: Generally due to actor initializer isolation rules, users will need to make their initializers `async`
/// in order to write code that safely performs extra actions after it has fully initialized.
///
/// The ``actorReady(_)`` call on the actor system is a signal to the actor system that this actor _instance_ is now ready
/// and may be resolved and interacted with via the actor system. Generally, a distributed actor system implementation
/// will _weakly retain_ the actors it has readied, because retaining them strongly would mean that they will never be
/// deallocated (and thus never resign their ID's).
///
/// > Note: Generally actor systems should retain actors _weakly_ in order to allow them be deinitialized when no longer in use.
/// >
/// > Sometimes though, it can be quite useful to have the system retain certain "well known" actors, for example when it is expected
/// > that other nodes in the distributed system will need to interact with them, even if end-user code no longer holds
/// > strong references to them. An example of such "retain while actor system is active" distributed actors would be any kind
/// > of actor which implements discovery or health check mechanisms between clustered nodes, sometimes called "system actors",
/// > i.e. actors that serve the actor system directly.
///
/// Next, we will discuss the just mentioned `resolve` method, which is closely tied to readying actors.
///
/// ### Resolving (potentially remote) Distributed Actors
///
/// An important aspect of any distributed actor system is being able to turn a ``DistributedActor`` type and ``ActorID``
/// into a reference to an actor (instance), regardless where the actor is located. The ID should have enough information stored
/// to be able to make the decision of _where_ the actor is located, without having to contact remote nodes. Specifically,
/// the implementation of ``DistributedActorSystem/resolve(id:as:)`` is _not_ `async` and should _not_ perform long running
/// or blocking operations in order to return.
///
/// > Note: Currently only concrete distributed actors types can be resolved.
///
/// The actor system's ``DistributedActorSystem/resolve(id:as:)`` method is called by the compiler whenever end-users
/// call the ``DistributedActor``'s ``DistributedActor/resolve(id:using:)`` method. The return types of those methods differ,
/// as the actor system's return type is `Act?` (and it may throw if unable to resolve the `ActorID`).
///
/// The actor system's `resolve` returning `nil` means that the ``ActorID`` passed to it refers to a _remote_
/// distributed actor. The Swift runtime reacts to this by creating a remote actor reference (sometimes called a "proxy").
///
///
/// ### Handling remote calls
///
/// Finally, calls on a _remote_ distributed actor reference's distributed methods are turned into invocations of
/// `remoteCall(on:target:invocation:returning:throwing:)` (or `remoteCallVoid(on:target:invocation:throwing:)` for Void returning methods).
///
/// Implementing the remote calls correctly and efficiently is the important task for a distributed actor system library.
///
/// Implementations of remote calls generally will serialize `actor.id`, `target` and `invocation`
/// into some form of wire envelope, and send it over the network (or process boundary) using some
/// transport mechanism of their choice. As they do so, they need to suspend the `remoteCall` function,
/// and resume it once a reply to the call arrives. Unless the transport layer is also async/await aware,
/// this will often require making use of a ``CheckedContinuation``.
///
/// While implementing remote calls please keep in mind any potential failure scenarios that may occur,
/// such as message loss, connection failures and similar issues. Such situations should all be
/// surfaced by resuming the `remoteCall` by throwing an error conforming to ``DistributedActorSystemError``.
///
/// While it is not _required_ to conform error thrown out of these methods to ``DistributedActorSystemError``,
/// the general guideline about conforming errors to this protocol is that errors which are outside of the user's control,
/// but are thrown because transport or actor system issues, should conform to it. This is to simplify
/// separating "business logic errors" from transport errors.
///
///
/// ### Further reading
///
/// For an even more in-depth explanation about the inner workings of a distributed actor system,
/// you can refer to the following Swift Evolution proposals:
///
/// - [SE-0336: Distributed Actor Isolation](https://github.com/apple/swift-evolution/blob/main/proposals/0336-distributed-actor-isolation.md)
/// - [SE-0344: Distributed Actor Runtime](https://github.com/apple/swift-evolution/blob/main/proposals/0344-distributed-actor-runtime.md)
@available(SwiftStdlib 5.7, *)
public protocol DistributedActorSystem<SerializationRequirement>: Sendable {
  /// The type ID that will be assigned to any distributed actor managed by this actor system.
  ///
  /// ### A note on Codable IDs
  /// If this type is ``Codable``, then any `distributed actor` using this `ActorID` as its ``DistributedActor/ID``
  /// will gain a synthesized ``Codable`` conformance which is implemented by encoding the `ID`.
  /// The decoding counter part of the ``Codable`` conformance is implemented by decoding the `ID` and passing it to
  /// the ``DistributedActor/resolve(id:using:)`` method.
  associatedtype ActorID: Sendable & Hashable

  /// Type of ``DistributedTargetInvocationEncoder`` that should be used when the Swift runtime needs to encode
  /// a distributed target call into an encoder, before passing it off to `remoteCall(...)`.
  associatedtype InvocationEncoder: DistributedTargetInvocationEncoder<SerializationRequirement>
  /// Type of ``DistributedTargetInvocationDecoder`` that should be used when decoding invocations during
  /// ``executeDistributedTarget(on:target:invocationDecoder:handler:)`` calls.
  associatedtype InvocationDecoder: DistributedTargetInvocationDecoder<SerializationRequirement>

  /// The type of the result handler which will be offered the results
  /// returned by a distributed function invocation called via
  /// ``executeDistributedTarget(on:target:invocationDecoder:handler:)``.
  associatedtype ResultHandler: DistributedTargetInvocationResultHandler<SerializationRequirement>

  /// The serialization requirement that will be applied to all distributed targets used with this system.
  associatedtype SerializationRequirement // TODO: constrain SerializationRequirement in type-system to only be ok with protocol or class here

  // ==== ---------------------------------------------------------------------
  // - MARK: Resolving actors by identity

  /// Resolves a local or remote ``ActorID`` to a reference to given actor, or throws if unable to.
  ///
  /// The returned value is either a local actor or proxy to a remote actor.
  ///
  /// This function is not intended to be used directly, but instead is called by the Swift runtime
  /// whenever ``DistributedActor/resolve(id:using:)` or  a concrete distributed actor's `init(from:)` is invoked.
  ///
  /// This function should either return an existing actor reference, or `nil` to signal that a remote distributed actor
  /// "proxy" should be created for this ``ActorID``. If the resolve fails, meaning that it can neither locate a local
  /// actor managed by this actor system, nor identify that the identity is located on some remote actor system, then
  /// this function should throw.
  ///
  /// ```swift
  /// distributed actor Worker { /* ... */ }
  ///
  /// // the following internally calls actorSystem.resolve(id: id, as: Worker.self)
  /// let worker: Worker = try  Worker.resolve(id: id, using: actorSystem)
  /// ```
  ///
  /// If this function returns correctly, the returned actor reference is immediately
  /// usable. It may not necessarily imply the strict *existence* of a remote actor
  /// the identity was pointing towards, e.g. when a remote system allocates actors
  /// lazily as they are first time messaged to, however this should not be a concern
  /// of the sending side.
  ///
  /// Detecting liveness of such remote actors shall be offered / by transport libraries
  /// by other means, such as "watching an actor for termination" or similar.
  ///
  /// - Parameter id: The `ActorID` to resolve an actor reference for
  /// - Parameter actorType: The type of distributed actor the ID is expected to point at.
  ///
  /// - Throws: When unable to confirm if the `id` is correct, the resolved actor does not match the expected `actorType`,
  ///           or any other internal validation error within the actor system's resolve process occurs.
  func resolve<Act>(id: ActorID, as actorType: Act.Type) throws -> Act?
    where Act: DistributedActor,
    Act.ID == ActorID

  // ==== ---------------------------------------------------------------------
  // - MARK: Actor Lifecycle

  /// Assign an ``ActorID`` for the passed actor type.
  ///
  /// This function is invoked by a distributed actor during its initialization,
  /// and the returned address value is stored along with it for the time of its
  /// lifetime.
  ///
  /// The address MUST uniquely identify the actor, and allow resolving it.
  /// E.g. if an actor is created under address `addr1` then immediately invoking
  /// `system.resolve(id: addr1, as: Greeter.self)` MUST return a reference
  /// to the same actor.
  func assignID<Act>(_ actorType: Act.Type) -> ActorID
    where Act: DistributedActor,
          Act.ID == ActorID

  /// Invoked during a distributed actor's initialization, as soon as it becomes fully initialized.
  ///
  /// The system is expected to store the reference to this actor, and maintain an `ActorID: DistributedActor`
  /// mapping for the purpose of implementing the `resolve(id:as:)` method.
  ///
  /// The system usually should NOT retain the passed reference, and it will be informed via
  /// ``resignID(_:)`` when the actor has been deallocated so it can remove the stale reference from its
  /// internal `ActorID: DistributedActor` mapping.
  ///
  /// The ``DistributedActor/id`` of the passed actor must be an ``ActorID`` that this system previously has assigned.
  ///
  /// If `actorReady` gets called with some unknown ID, it should crash immediately as it signifies some
  /// very unexpected use of the system.
  ///
  /// - Parameter actor: reference to the (local) actor that was just fully initialized.
  func actorReady<Act>(_ actor: Act)
    where Act: DistributedActor,
          Act.ID == ActorID

  /// Called during when a distributed actor is deinitialized, or fails to initialize completely (e.g. by throwing
  /// out of an `init` that did not completely initialize all of the actors stored properties yet).
  ///
  /// This method is guaranteed to be called at-most-once for a given id (assuming IDs are unique,
  /// and not re-cycled by the system), i.e. if it is called during a failure to initialize completely,
  /// the call from the actor's deinitializer will not happen (as under these circumstances, `deinit` will be run).
  ///
  /// If `resignID` gets called with some unknown ID, it should crash immediately as it signifies some
  /// very unexpected use of the system.
  ///
  /// - Parameter id: the id of an actor managed by this system that has begun its `deinit`.
  func resignID(_ id: ActorID)

  // ==== ---------------------------------------------------------------------
  // - MARK: Remote Method Invocations
  /// Invoked by the Swift runtime when a distributed remote call is about to be made.
  ///
  /// The returned `DistributedTargetInvocation` will be populated with all
  /// arguments, generic substitutions, and specific error and return types
  /// that are associated with this specific invocation.
  func makeInvocationEncoder() -> InvocationEncoder

  /// Invoked by the Swift runtime when making a remote call.
  ///
  /// The `arguments` are the arguments container that was previously created
  /// by `makeInvocationEncoder` and has been populated with all arguments.
  ///
  /// This method should perform the actual remote function call, and await for its response.
  ///
  /// ## Serialization Requirement
  /// Implementations of this method must ensure that the `Argument` type parameter conforms
  /// to the types' `SerializationRequirement`.
  ///
  /// ## Errors
  /// This method is allowed to throw because of underlying transport or serialization errors,
  /// as well as by re-throwing the error received from the remote callee (if able to).
  @available(SwiftStdlib 6.0, *)
  func remoteCall<Act, Err, Res>(
      on actor: Act,
      target: RemoteCallTarget,
      invocation: inout InvocationEncoder,
      throwing: Err.Type,
      returning: Res.Type
  ) async throws -> Res
      where Act: DistributedActor,
            Act.ID == ActorID,
            Err: Error
//          Res: SerializationRequirement

  /// Invoked by the Swift runtime when making a remote call.
  ///
  /// The `arguments` are the arguments container that was previously created
  /// by `makeInvocationEncoder` and has been populated with all arguments.
  ///
  /// This method should perform the actual remote function call, and await for its response.
  ///
  /// ## Errors
  /// This method is allowed to throw because of underlying transport or serialization errors,
  /// as well as by re-throwing the error received from the remote callee (if able to).
  @available(SwiftStdlib 6.0, *)
  func remoteCallVoid<Act, Err>(
      on actor: Act,
      target: RemoteCallTarget,
      invocation: inout InvocationEncoder,
      throwing: Err.Type
  ) async throws
      where Act: DistributedActor,
            Act.ID == ActorID,
            Err: Error

  // Implementation notes:
  // The `metatype` must be the type of `Value`, and it must conform to
  // `SerializationRequirement`. If it does not, the method will crash at
  // runtime. This is because we cannot express
  // `Value: SerializationRequirement`, however the generic `Value` is still
  // useful since it allows us to avoid boxing the value into an existential,
  // before we'd right away unbox it as first thing in the implementation of
  // this function.
  /// Implementation synthesized by the compiler.
  /// Not intended to be invoked explicitly from user code!
  func invokeHandlerOnReturn(
    handler: ResultHandler,
    resultBuffer: UnsafeRawPointer,
    metatype: Any.Type
  ) async throws
}

// ==== ----------------------------------------------------------------------------------------------------------------
// MARK: Execute Distributed Methods

@available(SwiftStdlib 5.7, *)
extension DistributedActorSystem {

  /// Prepare and execute a call to the distributed function identified by the passed arguments,
  /// on the passed `actor`, and collect its results using the `ResultHandler`.
  ///
  /// This method encapsulates multiple steps that are invoked in executing a distributed function,
  /// into one very efficient implementation. The steps involved are:
  ///
  /// - looking up the distributed function based on its name
  /// - decoding, in an efficient manner, all arguments from the `Args` container into a well-typed representation
  /// - using that representation to perform the call on the target method
  ///
  /// The reason for this API using a `ResultHandler` rather than returning values directly,
  /// is that thanks to this approach it can avoid any existential boxing, and can serve the most
  /// latency sensitive-use-cases.
  ///
  /// - Parameters:
  ///   - actor: actor on which the remote call should invoke the target
  ///   - target: the target (method) identifier that should be invoked
  ///   - invocationDecoder: used to obtain all arguments to be used to perform
  ///                        the target invocation
  ///   - handler: used to provide a type-safe way for library code to handle
  ///              the values returned by the target invocation.
  /// - Throws: if the target location, invocation argument decoding, or
  ///           some other mismatch between them happens. In general, this
  ///           method is allowed to throw in any situation that might otherwise
  ///           result in an illegal or unexpected invocation being performed.
  ///
  ///           Throws ``ExecuteDistributedTargetMissingAccessorError`` if the `target`
  ///           does not resolve to a valid distributed function accessor, i.e. the
  ///           call identifier is incorrect, corrupted, or simply not present in this process.
  public func executeDistributedTarget<Act>(
    on actor: Act,
    target: RemoteCallTarget,
    invocationDecoder: inout InvocationDecoder,
    handler: Self.ResultHandler
  ) async throws where Act: DistributedActor {
    // NOTE: Implementation could be made more efficient because we still risk
    // demangling a RemoteCallTarget identity (if it is a mangled name) multiple
    // times. We would prefer to store if it is a mangled name, demangle, and
    // always refer to that demangled repr perhaps? We do cache the resulting
    // pretty formatted name of the call target, but perhaps we can do better.

    // Get the expected parameter count of the func
    let targetName = target.identifier
    let targetNameUTF8 = Array(targetName.utf8)

    // Gen the generic environment (if any) associated with the target.
    let genericEnv =
      targetNameUTF8.withUnsafeBufferPointer { targetNameUTF8 in
        _getGenericEnvironmentOfDistributedTarget(
          targetNameUTF8.baseAddress!,
          UInt(targetNameUTF8.endIndex))
      }

    var substitutionsBuffer: UnsafeMutablePointer<Any.Type>? = nil
    var witnessTablesBuffer: UnsafeRawPointer? = nil
    var numWitnessTables: Int = 0

    defer {
      substitutionsBuffer?.deallocate()
      witnessTablesBuffer?.deallocate()
    }

    if let genericEnv = genericEnv {
      let subs = try invocationDecoder.decodeGenericSubstitutions()
      if subs.isEmpty {
        throw ExecuteDistributedTargetError(
          message: "Cannot call generic method without generic argument substitutions",
          errorCode: .missingGenericSubstitutions)
      }

      substitutionsBuffer = .allocate(capacity: subs.count)

      for (offset, substitution) in subs.enumerated() {
        let element = substitutionsBuffer?.advanced(by: offset)
        element?.initialize(to: substitution)
      }

      (witnessTablesBuffer, numWitnessTables) = _getWitnessTablesFor(environment: genericEnv,
        genericArguments: substitutionsBuffer!)
      if numWitnessTables < 0 {
        throw ExecuteDistributedTargetError(
          message: "Generic substitutions \(subs) do not satisfy generic requirements of \(target) (\(targetName))",
          errorCode: .invalidGenericSubstitutions)
      }
    }

    let paramCount =
      targetNameUTF8.withUnsafeBufferPointer { targetNameUTF8 in
        __getParameterCount(
          targetNameUTF8.baseAddress!,
          UInt(targetNameUTF8.endIndex))
      }

    guard paramCount >= 0 else {
      throw ExecuteDistributedTargetError(
        message: """
                 Failed to decode distributed invocation target expected parameter count,
                 error code: \(paramCount)
                 mangled name: \(targetName)
                 """,
        errorCode: .invalidParameterCount)
    }

    // Prepare buffer for the parameter types to be decoded into:
    let argumentTypesBuffer = UnsafeMutableBufferPointer<Any.Type>.allocate(capacity: Int(paramCount))
    defer {
      argumentTypesBuffer.deallocate()
    }

    // Demangle and write all parameter types into the prepared buffer
    let decodedNum = targetNameUTF8.withUnsafeBufferPointer { targetNameUTF8 in
      __getParameterTypeInfo(
        targetNameUTF8.baseAddress!,
        UInt(targetNameUTF8.endIndex),
        genericEnv,
        substitutionsBuffer,
        argumentTypesBuffer.baseAddress!._rawValue, Int(paramCount))
    }

    // Fail if the decoded parameter types count seems off and fishy
    guard decodedNum == paramCount else {
      throw ExecuteDistributedTargetError(
        message: """
                 Failed to decode the expected number of params of distributed invocation target, error code: \(decodedNum)
                 decoded: \(decodedNum), expected params: \(paramCount)
                 mangled name: \(targetName)
                 """,
        errorCode: .invalidParameterCount)
    }

    // Copy the types from the buffer into a Swift Array
    var argumentTypes: [Any.Type] = []
    do {
      argumentTypes.reserveCapacity(Int(decodedNum))
      for argumentType in argumentTypesBuffer {
        argumentTypes.append(argumentType)
      }
    }

    // Decode the return type
    func allocateReturnTypeBuffer<R>(_: R.Type) -> UnsafeRawPointer? {
      return UnsafeRawPointer(UnsafeMutablePointer<R>.allocate(capacity: 1))
    }

    let maybeReturnTypeFromTypeInfo =
      targetNameUTF8.withUnsafeBufferPointer { targetNameUTF8 in
        __getReturnTypeInfo(
          /*targetName:*/targetNameUTF8.baseAddress!,
          /*targetLength:*/UInt(targetNameUTF8.endIndex),
          /*genericEnv:*/genericEnv,
          /*genericArguments:*/substitutionsBuffer)
      }
    guard let returnTypeFromTypeInfo: Any.Type = maybeReturnTypeFromTypeInfo else {
      throw ExecuteDistributedTargetError(
        message: "Failed to decode distributed target return type",
        errorCode: .typeDeserializationFailure)
    }

    guard let resultBuffer = _openExistential(returnTypeFromTypeInfo, do: allocateReturnTypeBuffer) else {
      throw ExecuteDistributedTargetError(
        message: "Failed to allocate buffer for distributed target return type",
        errorCode: .typeDeserializationFailure)
    }

    func destroyReturnTypeBuffer<R>(_: R.Type) {
      resultBuffer.assumingMemoryBound(to: R.self).deallocate()
    }

    defer {
      _openExistential(returnTypeFromTypeInfo, do: destroyReturnTypeBuffer)
    }

    do {
      let returnType = try invocationDecoder.decodeReturnType() ?? returnTypeFromTypeInfo
      // let errorType = try invocationDecoder.decodeErrorType() // TODO(distributed): decide how to use when typed throws are done

      // Execute the target!
      try await _executeDistributedTarget(
        on: actor,
        /*targetNameData:*/targetName,
        /*targetNameLength:*/UInt(targetName.count),
        argumentDecoder: &invocationDecoder,
        argumentTypes: argumentTypesBuffer.baseAddress!._rawValue,
        resultBuffer: resultBuffer._rawValue,
        substitutions: UnsafeRawPointer(substitutionsBuffer),
        witnessTables: witnessTablesBuffer,
        numWitnessTables: UInt(numWitnessTables)
      )

      if returnType == Void.self {
        try await handler.onReturnVoid()
      } else {
        try await self.invokeHandlerOnReturn(
          handler: handler,
          resultBuffer: resultBuffer,
          metatype: returnType
        )
      }
    } catch {
      try await handler.onThrow(error: error)
    }
  }
}

/// Represents a 'target' of a distributed call, such as a `distributed func` or
/// `distributed` computed property. Identification schemes may vary between
/// systems, and are subject to evolution.
///
/// Actor systems generally should treat the `identifier` as an opaque string,
/// and pass it along to the remote system for in their `remoteCall`
/// implementation. Alternative approaches are possible, where the identifiers
/// are either compressed, cached, or represented in other ways, as long as the
/// recipient system is able to determine which target was intended to be
/// invoked.
///
/// The string representation will attempt to pretty print the target identifier,
/// however its exact format is not specified and may change in future versions.
@available(SwiftStdlib 5.7, *)
public struct RemoteCallTarget: CustomStringConvertible, Hashable {
  private let _identifier: String

  public init(_ identifier: String) {
    self._identifier = identifier
  }

  /// The underlying identifier of the target, returned as-is.
  public var identifier: String {
    return _identifier
  }

  /// Attempts to pretty format the underlying target identifier.
  /// If unable to, returns the raw underlying identifier.
  public var description: String {
    if let name = _getFunctionFullNameFromMangledName(mangledName: _identifier) {
      return name
    } else {
      return "\(_identifier)"
    }
  }
}

@available(SwiftStdlib 5.7, *)
@_silgen_name("swift_distributed_execute_target")
func _executeDistributedTarget<D: DistributedTargetInvocationDecoder>(
  on actor: AnyObject, // : DistributedActor
  _ targetName: UnsafePointer<UInt8>, _ targetNameLength: UInt,
  argumentDecoder: inout D,
  argumentTypes: Builtin.RawPointer,
  resultBuffer: Builtin.RawPointer,
  substitutions: UnsafeRawPointer?,
  witnessTables: UnsafeRawPointer?,
  numWitnessTables: UInt
) async throws

/// Used to encode an invocation of a distributed target (method or computed property).
///
/// ## Forming an invocation
///
/// On the sending-side an instance of an invocation is constructed by the runtime,
/// and calls to: `recordGenericSubstitution`, `recordArgument`, `recordReturnType`,
/// `recordErrorType`, and finally `doneRecording` are made (in this order).
///
/// If the return type of the target is `Void` the `recordReturnType` is not invoked.
///
/// If the error type thrown by the target is not defined the `recordErrorType` is not invoked.
///
/// An invocation implementation may decide to perform serialization right-away in the
/// `record...` invocations, or it may choose to delay doing so until the invocation is passed
/// to the `remoteCall`. This decision largely depends on if serialization is allowed to happen
/// on the caller's task, and if any smarter encoding can be used once all parameter calls have been
/// recorded (e.g. it may be possible to run-length encode values of certain types etc.)
///
/// Once encoded, the system should use some underlying transport mechanism to send the
/// bytes serialized by the invocation to the remote peer.
///
/// ## Decoding an invocation
/// Since every actor system is going to deal with a concrete invocation type, they may
/// implement decoding them whichever way is most optimal for the given system.
///
/// Once decided, the invocation must be passed to `executeDistributedTarget`
/// which will decode the substitutions, argument values, return and error types (in that order).
///
/// Note that the decoding will be provided the specific types that the sending side used to preform the call,
/// so decoding can rely on simply invoking e.g. `Codable` (if that is the `SerializationRequirement`) decoding
/// entry points on the provided types.
@available(SwiftStdlib 5.7, *)
public protocol DistributedTargetInvocationEncoder<SerializationRequirement> {
  /// The serialization requirement that the types passed to `recordArgument` and `recordReturnType` are required to conform to.
  associatedtype SerializationRequirement

  /// The arguments must be encoded order-preserving, and once `decodeGenericSubstitutions`
  /// is called, the substitutions must be returned in the same order in which they were recorded.
  ///
  /// - Parameter type: a generic substitution type to be recorded for this invocation.
  mutating func recordGenericSubstitution<T>(_ type: T.Type) throws

  /// Record an argument of `Argument` type.
  /// This will be invoked for every argument of the target, in declaration order.
  ///
  /// ### Serialization Requirement
  /// Implementations of this method must ensure that the `Value` type parameter conforms
  /// to the types' `SerializationRequirement`.
  @available(SwiftStdlib 6.0, *)
  mutating func recordArgument<Value/*: SerializationRequirement*/>(
    _ argument: RemoteCallArgument<Value>
  ) throws

  /// Record the error type of the distributed method.
  /// This method will not be invoked if the target is not throwing.
  ///
  /// - Parameter type: the type of error that was declared to be thrown by the invocation target. Currently this can only ever be `Error.self`.
  mutating func recordErrorType<E: Error>(_ type: E.Type) throws

  /// Record the return type of the distributed method.
  /// This method will not be invoked if the target is returning `Void`.
  ///
  /// ### Serialization Requirement
  /// Implementations of this method must ensure that the `R` type parameter conforms
  /// to the types' `SerializationRequirement`.
  @available(SwiftStdlib 6.0, *)
  mutating func recordReturnType<R/*: SerializationRequirement*/>(_ type: R.Type) throws

  /// Invoked to signal to the encoder that no further `record...` calls will be made on it.
  ///
  /// Useful if the encoder needs to perform some "final" task before the underlying message is considered complete,
  /// e.g. computing a checksum, or some additional message signing or finalization step.
  mutating func doneRecording() throws
}

/// Represents an argument passed to a distributed call target.
@available(SwiftStdlib 5.7, *)
public struct RemoteCallArgument<Value> {
  /// The "argument label" of the argument.
  /// The label is the name visible name used in external calls made to this
  /// target, e.g. for `func hello(label name: String)` it is `label`.
  ///
  /// If no label is specified (i.e. `func hi(name: String)`), the `label`,
  /// value is empty, however `effectiveLabel` is equal to the `name`.
  ///
  /// In most situations, using `effectiveLabel` is more useful to identify
  /// the user-visible name of this argument.
  public let label: String?

  /// The effective label of this argument. This reflects the semantics
  /// of call sites of function declarations without explicit label
  /// definitions in Swift.
  ///
  /// For example, for a method declared like `func hi(a: String)` the effective
  /// label is `a` while for a method like `func hi(a b: String)` or
  /// `func hi(_ b: String)` the label is the explicitly declared one,
  /// so `a` and `_` respectively.
  public var effectiveLabel: String {
    if let label {
      return label
    } else {
      return "_"
    }
  }

  /// The internal name of parameter this argument is accessible as in the
  /// function body. It is not part of the functions API and may change without
  /// breaking the target identifier.
  ///
  /// If the method did not declare an explicit `label`, it is used as the
  /// `effectiveLabel`.
  public let name: String

  /// The value of the argument being passed to the call.
  /// As `RemoteCallArgument` is always used in conjunction with
  /// `recordArgument` and populated by the compiler, this Value will generally
  /// conform to a distributed actor system's `SerializationRequirement`.
  public let value: Value

  public init(label: String?, name: String, value: Value) {
    self.label = label
    self.name = name
    self.value = value
  }
}

/// Decoder that must be provided to `executeDistributedTarget` and is used
/// by the Swift runtime to decode arguments of the invocation.
///
/// ### Decoding DistributedActor arguments using Codable
/// When using an actor system where ``ActorID`` is ``Codable``, every distributed actor using that system
/// is also implicitly ``Codable`` (see ``DistributedActorSystem``). Such distributed actors are encoded
/// as their ``ActorID`` stored in an ``Encoder/singleValueContainer``. When ``Codable`` is being used
/// by such a system, the ``decodeNextArgument`` method will be using ``Decoder`` to
/// decode the incoming values, which may themselves be distributed actors.
///
/// An actor system must be provided to the ``Decoder`` in order for a distributed actor's ``Decodable/init(from:)``
/// to be able to return the instance of the actor. Specifically, the decoded``ActorID`` is passed to the actor system's `resolve(id:as:)` method in order to 
/// return either a local instance identified by this ID, or creating a remote actor reference.
/// Thus, you must set the actor system the decoding is performed for, on the decoder's `userInfo`, as follows:
///
/// ```
/// mutating func decodeNextArgument<Argument: SerializationRequirement>() throws -> Argument {
///   let argumentData: Data = /// ...
///   // ...
///   decoder.userInfo[.actorSystemKey] = self.actorSystem
///   return try Argument.decode(
/// }
/// ```
@available(SwiftStdlib 5.7, *)
public protocol DistributedTargetInvocationDecoder<SerializationRequirement> {
  /// The serialization requirement that the types passed to `decodeNextArgument` are required to conform to.
  /// The type returned by `decodeReturnType` is also expected to conform to this associated type requirement.
  associatedtype SerializationRequirement

  /// Decode all generic substitutions that were recorded for this invocation.
  ///
  /// The values retrieved from here must be in the same order as they were recorded by
  /// ``DistributedTargetInvocationEncoder/recordGenericSubstitution(_:)``.
  ///
  /// - Returns: array of all generic substitutions necessary to execute this invocation target.
  /// - Throws: if decoding substitutions fails.
  mutating func decodeGenericSubstitutions() throws -> [Any.Type]

  /// Attempt to decode the next argument from the underlying buffers into pre-allocated storage
  /// pointed at by 'pointer'.
  ///
  /// This method should throw if it has no more arguments available, if decoding the argument failed,
  /// or, optionally, if the argument type we're trying to decode does not match the stored type.
  ///
  /// The result of the decoding operation must be stored into the provided 'pointer' rather than
  /// returning a value. This pattern allows the runtime to use a heavily optimized, pre-allocated
  /// buffer for all the arguments and their expected types. The 'pointer' passed here is a pointer
  /// to a "slot" in that pre-allocated buffer. That buffer will then be passed to a thunk that
  /// performs the actual distributed (local) instance method invocation.
  ///
  /// ### Serialization Requirement
  /// Implementations of this method must ensure that the `Argument` type parameter conforms
  /// to the types' `SerializationRequirement`.
  @available(SwiftStdlib 6.0, *)
  mutating func decodeNextArgument<Argument/*: SerializationRequirement*/>() throws -> Argument

  /// Decode the specific error type that the distributed invocation target has recorded.
  /// Currently this effectively can only ever be `Error.self`.
  ///
  /// If the target known to not be throwing, or no error type was recorded, the method should return `nil`.
  mutating func decodeErrorType() throws -> Any.Type?

  /// Attempt to decode the known return type of the distributed invocation.
  ///
  /// It is legal to implement this by returning `nil`, and then the system
  /// will take the concrete return type from the located function signature.
  mutating func decodeReturnType() throws -> Any.Type?
}

/// Protocol a distributed invocation execution's result handler.
///
/// An instance conforming to this type must be passed when invoking
/// ``executeDistributedTarget(on:target:invocationDecoder:handler:)`` while handling an incoming distributed call.
///
/// The handler will then be invoked with the return value (or error) that the invoked target returned (or threw).
@available(SwiftStdlib 5.7, *)
public protocol DistributedTargetInvocationResultHandler<SerializationRequirement> {
  /// The serialization requirement that the value passed to `onReturn` is required to conform to.
  associatedtype SerializationRequirement

  /// Invoked when the distributed target execution returns successfully.
  /// The `value` is the return value of the executed distributed invocation target.
  ///
  /// ### Serialization Requirement
  /// Implementations of this method must ensure that the `Success` type parameter conforms
  /// to the types' `SerializationRequirement`.
  @available(SwiftStdlib 6.0, *)
  func onReturn<Success/*: SerializationRequirement*/>(value: Success) async throws

  /// Invoked when the distributed target execution of a `Void` returning
  /// function has completed successfully.
  func onReturnVoid() async throws

  /// Invoked when the distributed target execution of a target has thrown an error.
  ///
  /// It is not guaranteed that the error conform to the ``SerializationRequirement``;
  /// This guarantee is only given to return values (and offered by `onReturn`).
  func onThrow<Err: Error>(error: Err) async throws
}

/******************************************************************************/
/******************************** Errors **************************************/
/******************************************************************************/

/// Error protocol to which errors thrown by any `DistributedActorSystem` should conform.
@available(SwiftStdlib 5.7, *)
public protocol DistributedActorSystemError: Error {}

/// Error thrown by ``DistributedActorSystem/executeDistributedTarget(on:target:invocationDecoder:handler:)``.
///
/// Inspect the ``errorCode`` for details about the underlying reason this error was thrown.
@available(SwiftStdlib 5.7, *)
public struct ExecuteDistributedTargetError: DistributedActorSystemError {
  public let errorCode: ErrorCode
  public let message: String

  public enum ErrorCode {
    /// Unable to resolve the target identifier to a function accessor.
    /// This can happen when the identifier is corrupt, illegal, or wrong in the
    /// sense that the caller and callee do not have the called function recorded
    /// using the same identifier.
    case targetAccessorNotFound

    /// Call target has different number of parameters than arguments
    /// provided by the invocation decoder.
    case invalidParameterCount

    /// Target expects generic environment information, but invocation decoder
    /// provided no generic substitutions.
    case missingGenericSubstitutions

    /// Generic substitutions provided by invocation decoder are incompatible
    /// with target of the call. E.g. the generic requirements on the actual
    /// target could not be fulfilled by the obtained generic substitutions.
    case invalidGenericSubstitutions

    // Failed to deserialize type or obtain type information for call.
    case typeDeserializationFailure

    /// A general issue during the execution of the distributed call target occurred.
    case other
  }

  public init(message: String) {
    self.message = message
    self.errorCode = .other
  }

  public init(message: String, errorCode: ErrorCode) {
    self.message = message
    self.errorCode = errorCode
  }
}

/// Error thrown by distributed actor systems while encountering encoding/decoding
/// issues.
///
/// Also thrown when an attempt to decode ``DistributedActor`` is made,
/// but no ``DistributedActorSystem`` is available in the `Decoder`'s
/// `userInfo[.actorSystemKey]`, as it is required to perform the resolve call.
@available(SwiftStdlib 5.7, *)
public struct DistributedActorCodingError: DistributedActorSystemError {
  public let message: String

  public init(message: String) {
    self.message = message
  }

  public static func missingActorSystemUserInfo<Act>(_ actorType: Act.Type) -> Self
      where Act: DistributedActor {
    .init(message: "Missing DistributedActorSystem userInfo while decoding")
  }
}

/// Invoked when a distributed "stub" method would be incorrectly invoked on a
/// local instance of the synthesized stub. Generally this should not be possible
/// as a stub always should be used as "remote" and therefore no local calls
/// are performed on them. Seeing this fatal error means a problem in the Swift
/// Distributed runtime, please report an issue.
@available(SwiftStdlib 6.0, *)
public // COMPILER_INTRINSIC
func _diagnoseDistributedStubMethodCalled(
  className: StaticString,
  funcName: StaticString = #function,
  file: StaticString = #file,
  line: UInt = #line,
  column: UInt = #column
) -> Never {
  #if !$Embedded
  fatalError("invoked distributed stub '\(className).\(funcName)'", file: file, line: line)
  #else
  Builtin.int_trap()
  #endif
}
