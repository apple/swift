// RUN: %target-swift-frontend -disable-availability-checking -parse-as-library -emit-silgen -verify %s

// REQUIRES: concurrency

@globalActor final actor FirstActor {
    static let shared = FirstActor()
}

@globalActor final actor SecondActor {
    static let shared = SecondActor()
    
    // expected-warning@+1 {{async deinit contains no await statements; consider using isolated sync deinit instead}} {{5-17=isolated deinit}}
    deinit async {
        
    }
}

@FirstActor func isolatedFunc() {}
func asyncInt() async -> Int { 42 }
struct Boom: Error {}

@FirstActor
class A {
    // expected-warning@+1 {{async deinit contains no await statements; consider using isolated sync deinit instead}} {{5-17=isolated deinit}}
    deinit async {
        let _: Void = isolatedFunc()
        let bar: [Int] = []
        for x in bar {
            print(x)
        }
        let emptyStream = AsyncStream<Actor> { continuation in
            continuation.finish()
        }
        print(emptyStream)
        
        let c: @MainActor () async -> Void = {
            await isolatedFunc()
            await Task.yield()
        }
        print(c)
        
        class ClassDecl {
            func dummy() async {
                await Task.yield()
            }
        }
    }
}

class B {
    // expected-warning@+1 {{async deinit contains no await statements; consider using isolated sync deinit instead}} {{23-29=}}
    @FirstActor deinit async {
    }
}

@FirstActor
class C {
    // expected-warning@+1 {{async deinit contains no await statements; consider using isolated sync deinit instead}} {{20-26=}}
    isolated deinit async {
    }
}

@FirstActor
class D {
    // expected-warning@+1 {{async deinit contains no await statements; consider using isolated sync deinit instead}} {{23-29=}}
    nonisolated deinit async {
    }
}

class E {
    deinit async {
        await isolatedFunc()
    }
}

class F {
    deinit async {
        #if COND
            await isolatedFunc()
        #else
        #endif
    }
}

class G {
    deinit async {
        #if COND
        #else
            await isolatedFunc()
        #endif
    }
}

class H {
    deinit async {
        async let _: Void = isolatedFunc()
    }
}

class I {
    deinit async {
        let emptyStream = AsyncStream<Actor> { continuation in
            continuation.finish()
        }
        for await x in emptyStream {
            print(x)
        }
    }
}

class J {
    deinit async {
        for x: Int in [] {
            if false {
                do {
                    throw Boom()
                }
                catch {
                    print([
                        "abc",
                        "\(x * (await asyncInt()))",
                        ""
                    ])
                }
            }
        }
    }
}

class K {
    deinit async {
        switch 42 {
        case await asyncInt():
            print(true)
        default:
            print(false)
        }
    }
}
