// RUN: rm -rf %t
// RUN: split-file %s %t
// RUN: %target-build-swift %t/test.swift -I %t -o %t/out -Xfrontend -enable-experimental-cxx-interop -O
// RUN: %target-codesign %t/out
// RUN: %target-run %t/out

// Verify that a non-const ref value parameter can't implicitly receive
// aborrowed value.
// RUN: %target-swift-frontend -DBORROW_PASS_TO_VALUE_PARAM -emit-ir -o /dev/null -I %t %t/test.swift -enable-experimental-cxx-interop -verify

//--- Inputs/module.modulemap
module CxxTest {
    header "test.h"
    requires cplusplus
}

//--- Inputs/test.h

inline int &getCopyCounter() {
    static int value = 0;
    return value;
}

class BorrowMe {
public:
  BorrowMe(): x_(11) {}
  BorrowMe(const BorrowMe &other): x_(other.x_) {
    ++getCopyCounter();
  }

  const int &x() const { return x_; }
  int &x() { return x_; }
private:
  int x_;
};

inline int takeBorrowConstRef(const BorrowMe &value) {
    return value.x();
}

inline int takeBorrowByVal(BorrowMe value) {
    return value.x();
}

//--- test.swift

import CxxTest

extension BorrowMe {
    borrowing func getX() -> CInt {
        __xUnsafe().pointee
    }

    var x: CInt {
        borrowing get {
            getX()
        }
    }
}

func testBorrowingParam(_ value: borrowing BorrowMe) {
    let x = takeBorrowConstRef(value)
    assert(x == 11)
#if BORROW_PASS_TO_VALUE_PARAM
    takeBorrowByVal(value) // expected-error@-4 {{'value' is borrowed and cannot be consumed}} expected-note {{consumed here}}
    takeBorrowByVal(copy value) // ok
#endif
}

public func testBorrowingSafeReferenceUse() {
    let x: CInt
    do {
        let p = BorrowMe()
        x = p.x
        testBorrowingParam(p)
    }
    if x != 11 { fatalError("wrong value") }
    assert(getCopyCounter().pointee == 0)
}

testBorrowingSafeReferenceUse()
