//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import Swift

/// A type that represents a globally-unique actor that can be used to isolate
/// various declarations anywhere in the program.
///
/// A type that conforms to the `GlobalActor` protocol and is marked with
/// the `@globalActor` attribute can be used as a custom attribute. Such types
/// are called global actor types, and can be applied to any declaration to
/// specify that such types are isolated to that global actor type. When using
/// such a declaration from another actor (or from nonisolated code),
/// synchronization is performed through the shared actor instance to ensure
/// mutually-exclusive access to the declaration.
///
/// ## Custom Actor Executors
/// A global actor uses a custom executor if it needs to customize its execution
/// semantics, for example, by making sure all of its invocations are run on a
/// specific thread or dispatch queue.
///
/// This is done the same way as with normal non-global actors, by declaring a
/// ``Actor/unownedExecutor`` nonisolated property in the ``ActorType``
/// underlying this global actor.
///
/// It is *not* necessary to override the ``sharedUnownedExecutor`` static
/// property of the global actor, as its default implementation already
/// delegates to the ``shared.unownedExecutor``, which is the most reasonable
/// and correct implementation of this protocol requirement.
///
/// You can find out more about custom executors, by referring to the
/// ``SerialExecutor`` protocol's documentation.
///
/// - SeeAlso: ``SerialExecutor``
@available(SwiftStdlib 5.1, *)
public protocol GlobalActor {
  /// The type of the shared actor instance that will be used to provide
  /// mutually-exclusive access to declarations annotated with the given global
  /// actor type.
  associatedtype ActorType: Actor

  /// The shared actor instance that will be used to provide mutually-exclusive
  /// access to declarations annotated with the given global actor type.
  ///
  /// The value of this property must always evaluate to the same actor
  /// instance.
  static var shared: ActorType { get }

  /// Shorthand for referring to the `shared.unownedExecutor` of this global actor.
  ///
  /// When declaring a global actor with a custom executor, prefer to implement
  /// the underlying actor's ``Actor/unownedExecutor`` property, and leave this
  /// `sharedUnownedExecutor` default implementation in-place as it will simply
  /// delegate to the `shared.unownedExecutor`.
  ///
  /// The value of this property must be equivalent to `shared.unownedExecutor`,
  /// as it may be used by the Swift concurrency runtime or explicit user code with
  /// that assumption in mind.
  ///
  /// Returning different executors for different invocations of this computed
  /// property is also illegal, as it could lead to inconsistent synchronization
  /// of the underlying actor.
  ///
  /// - SeeAlso: ``SerialExecutor``
  static var sharedUnownedExecutor: UnownedSerialExecutor { get }
}

@available(SwiftStdlib 5.1, *)
extension GlobalActor {
  public static var sharedUnownedExecutor: UnownedSerialExecutor {
    shared.unownedExecutor
  }
}

