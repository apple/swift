//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
//
//===----------------------------------------------------------------------===//


@available(SwiftStdlib 5.9, *)
@_marker public protocol Observable { }

#if $Macros && hasAttribute(attached)

@available(SwiftStdlib 5.9, *)
#if OBSERVATION_SUPPORTS_PEER_MACROS
@attached(member, names: named(_$observationRegistrar), named(access), named(withMutation))
#else
@attached(member, names: named(_$observationRegistrar), named(access), named(withMutation), arbitrary)
#endif
@attached(memberAttribute)
@attached(conformance)
public macro Observable() =
  #externalMacro(module: "ObservationMacros", type: "ObservableMacro")

@available(SwiftStdlib 5.9, *)
@attached(accessor, names: named(init), named(get), named(set))
#if OBSERVATION_SUPPORTS_PEER_MACROS
@attached(peer, names: prefixed(_))
#endif
public macro ObservationTracked() =
  #externalMacro(module: "ObservationMacros", type: "ObservationTrackedMacro")

@available(SwiftStdlib 5.9, *)
@attached(accessor, names: named(willSet))
public macro ObservationIgnored() =
  #externalMacro(module: "ObservationMacros", type: "ObservationIgnoredMacro")

#endif
