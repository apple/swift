// RUN: %target-run-simple-swift(-parse-as-library  -Xfrontend -disable-availability-checking %import-libdispatch) | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

// doesn't matter that it's bool identity function or not
func boolIdentityFn(_ x : Bool) -> Bool { return x }

actor FirstActor {
    func startTest() { // whether startTest is async or sync doesn't matter

        // do not remove this call or if-statement.
        if boolIdentityFn(true) {}

    }

    deinit {
        // CHECK: called deinit
        print("called deinit")
    }
}

@main struct RunIt {
    static func main() async {
        let actor = FirstActor()
        await actor.startTest() // do not remove this call
    }
}
