// RUN: %empty-directory(%t)
// RUN: split-file %s %t
// RUN: %target-swift-typecheck-module-from-interface(%t/bug.swiftinterface) -I %t

//--- bug.swiftinterface

// swift-interface-format-version: 1.0
// swift-compiler-version: Apple Swift version 6.0 effective-5.10 (swiftlang-6.0.0.4.52 clang-1600.0.21.1.3)
// swift-module-flags: -enable-objc-interop -enable-library-evolution -module-name bug
import Swift
import _Concurrency
import _StringProcessing
import _SwiftConcurrencyShims
#if compiler(>=5.3) && $NoncopyableGenerics
public enum Maybe<Wrapped> : ~Swift.Copyable where Wrapped : ~Copyable {
  case just(Wrapped)
  case none
}
#else
public enum Maybe<Wrapped> {
  case just(Wrapped)
  case none
}
#endif
#if compiler(>=5.3) && $NoncopyableGenerics
extension bug.Maybe : Swift.Copyable {
}
#else
extension bug.Maybe {
}
#endif

