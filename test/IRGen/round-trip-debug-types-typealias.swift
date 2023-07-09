// RUN: %target-build-swift -g %s

// https://github.com/apple/swift/issues/66554
// IRGenDebugInfo type reconstruction crash because existential types
// inside typealias are not taken into account when comparing type
// equality

protocol Protocol<T> { associatedtype T }
typealias AnyProtocol<T> = any Protocol<T>
let crash: AnyProtocol<Any?>
