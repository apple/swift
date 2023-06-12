
// >> First verify that when building the stdlib, we do have the copyable constraint. Note the module-name!!
// RUN: %target-swift-frontend -typecheck -verify -parse-stdlib -module-name Swift %s

// FIXME: Now demonstrate that plain -parse-stdlib, such as in some arbitrary test, doesn't get the Copyable constraint :(
// RUN: not %target-swift-frontend -typecheck -verify -parse-stdlib %s

@_marker public protocol _Copyable {}

func nextTime<T>(_ t: T) {}

@_moveOnly struct MO {}

nextTime(MO()) // expected-error {{noncopyable type 'MO' cannot be used with generics yet}}
