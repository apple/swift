// RUN: %target-run-simple-swift(-Xfrontend -sil-verify-all) | %FileCheck %s
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all) | %FileCheck %s

// REQUIRES: executable_test

class Klass {
    var name: String

    init(_ x: String) {
        name = x
    }

    func doSomething() {
        print("Doing something \(name)")
    }
}

func simpleTest(_ x: consuming Klass) {
    // CHECK: Doing something MyName
    x.doSomething()
    let f: () -> () = {
        x.doSomething()
    }
    // CHECK: Doing something MyName
    f()
    x = Klass("MyOtherName")
    // CHECK: Doing something MyOtherName
    f()
    var f2: () -> () = {}
    f2 = {
        x.doSomething()
    }
    // CHECK: Doing something MyOtherName
    f2()
}

func main() {
    simpleTest(Klass("MyName"))
}

main()
