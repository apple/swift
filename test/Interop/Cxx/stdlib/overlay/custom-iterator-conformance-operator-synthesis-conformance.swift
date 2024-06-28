// RUN: %empty-directory(%t)
// RUN: %target-swiftxx-frontend -emit-module %s -module-name TestA -I %S/Inputs -o %t/test-part.swiftmodule
// RUN: %target-swiftxx-frontend -merge-modules -emit-module %t/test-part.swiftmodule -module-name TestA -I %S/Inputs -o %t/TestA.swiftmodule -sil-verify-none
// RUN: %target-typecheck-verify-swift -I %S/Inputs -cxx-interoperability-mode=default -I %t

#if USE

import TestA

public func test() {
    print(testEqualEqual())
    print(testPlusEqualMinus())
}

#else

import CustomSequence

@inlinable
public func testEqualEqual() -> Bool {
    let m = HasInheritedConstIterator()
    return m.__beginUnsafe() == m.__endUnsafe()
}

@inlinable
public func testPlusEqualOrMinus() -> Bool {
    var b = InheritedTypedConstRACIterator(0)
    b += 1
    return (b - b) == 0
}

#endif
