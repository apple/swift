// RUN: %target-run-simple-swift | %FileCheck %s

// REQUIRES: executable_test

class Object {
    deinit { print("deinit object") }
}

struct Noncopyable: ~Copyable {
    deinit { print("deinit noncopyable") }
}

func testDeinitAfterConsume() {
    do {
        let object = Object()
        // CHECK: before consume
        print("before consume")
        // CHECK: deinit object
        _ = consume object
        // CHECK: after consume
        print("after consume")
    }
    
    print()
    
    do {
        let noncopyable = Noncopyable()
        // CHECK: before consume
        print("before consume")
        // CHECK: deinit noncopyable
        _ = consume noncopyable
        // CHECK: after consume
        print("after consume")
    }
}

testDeinitAfterConsume()
