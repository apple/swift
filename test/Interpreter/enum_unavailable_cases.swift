// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -Xfrontend -unavailable-decl-optimization=complete -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out
// REQUIRES: executable_test

import StdlibUnittest

@available(*, unavailable)
class UnavailableClass {
  var x: UInt8 = 0
}

enum SingletonTrivial {
  @available(*, unavailable)
  case unavailable(UInt8)
}

enum SingletonClass {
  @available(*, unavailable)
  case unavailable(UnavailableClass)
}

enum NoPayload {
  case x
  @available(*, unavailable)
  case unavailable
  case y
}

enum SinglePayloadTrivial {
  case x
  @available(*, unavailable)
  case unavailable(UInt8)
  case y
}

enum MultiPayloadTrivial {
  case x(UInt8)
  @available(*, unavailable)
  case unavailable(UInt8, UInt8)
  case y
}

enum MultiPayloadGeneric<T, U> {
  case x(T)
  @available(*, unavailable)
  case unavailable(T, U)
  case y
}

expectEqual(MemoryLayout<SingletonTrivial>.size, 0)
expectEqual(MemoryLayout<SingletonClass>.size, 0)
expectEqual(MemoryLayout<NoPayload>.size, 1)
expectEqual(MemoryLayout<SinglePayloadTrivial>.size, 1)
expectEqual(MemoryLayout<MultiPayloadTrivial>.size, 2)
expectEqual(MemoryLayout<MultiPayloadGeneric<UInt8, UInt8>>.size, 2)
expectEqual(MemoryLayout<MultiPayloadGeneric<UInt32, UInt32>>.size, 5)
