// RUN: %target-run-simple-swift( -plugin-path %swift-plugin-dir -Xfrontend -strict-concurrency=complete -Xfrontend -disable-availability-checking -parse-as-library %import-libdispatch) | %FileCheck %s --dump-input=always

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

enum TL {
  @TaskLocal
  static var one: Int = 1
  @TaskLocal
  static var two: Int = 2
  @TaskLocal
  static var three: Int = 3
  @TaskLocal
  static var four: Int = 4
}

// ==== ------------------------------------------------------------------------

func test() async {
  // no outer task locals
  await withTaskGroup(of: Void.self) { group in
    TL.$two.withValue(2222) {
      // should not have any effect on reads below
    }
    await TL.$two.withValue(22) {
      group.addTask { // will have to copy the `22`
        print("Survived, one: \(TL.one) @ \(#fileID):\(#line)") // CHECK: Survived, one: 1
        print("Survived, two: \(TL.two) @ \(#fileID):\(#line)") // CHECK: Survived, two: 22
      }
    }
  }

  await TL.$one.withValue(11) {
    await TL.$one.withValue(1111) {
      await withTaskGroup(of: Void.self) { group in

        TL.$two.withValue(2222) {
          group.addTask { // will have to copy the `2222`
            print("Survived, one: \(TL.one) @ \(#fileID):\(#line)") // CHECK: Survived, one: 1111
            print("Survived, two: \(TL.two) @ \(#fileID):\(#line)") // CHECK: Survived, two: 2222
          }
        }
        await group.next()
        print("--")

        TL.$two.withValue(2) {
          TL.$two.withValue(22) {
            TL.$two.withValue(2222) {
              group.addTask { // will have to copy the `2222`
                print("Survived, one: \(TL.one) @ \(#fileID):\(#line)") // CHECK: Survived, one: 1111
                print("Survived, two: \(TL.two) @ \(#fileID):\(#line)") // CHECK: Survived, two: 2222
              }
            }
          }
        }
        await group.next()
        print("--")

        TL.$two.withValue(2) {
          TL.$two.withValue(22) {
            TL.$three.withValue(33) {
              TL.$two.withValue(2222) {
                group.addTask { // will have to copy the `2222`
                  print("Survived, one: \(TL.one) @ \(#fileID):\(#line)") // CHECK: Survived, one: 1111
                  print("Survived, two: \(TL.two) @ \(#fileID):\(#line)") // CHECK: Survived, two: 2222

                  TL.$three.withValue(3333) {
                    print("Survived, one: \(TL.one) @ \(#fileID):\(#line)") // CHECK: Survived, one: 1111
                    print("Survived, two: \(TL.two) @ \(#fileID):\(#line)") // CHECK: Survived, two: 2222
                    print("Survived, three: \(TL.three) @ \(#fileID):\(#line)") // CHECK: Survived, three: 3333
                  }
                }
              }
            }
          }
        }
        await group.next()
        print("--")
      }
    }
  }

  await TL.$one.withValue(11) {
    await Task {
      async let x = await withTaskGroup(of: Void.self) { group in
        TL.$two.withValue(22) {
          group.addTask { // will have to copy the `2222`
            print("Survived, one: \(TL.one) @ \(#fileID):\(#line)") // CHECK: Survived, one: 11
            print("Survived, two: \(TL.two) @ \(#fileID):\(#line)") // CHECK: Survived, two: 22
          }
        }
      }
    }
      .value
    print("--")
  }

  // Multiple nested task groups *in the same task*
  await TL.$one.withValue(11) {
    await withTaskGroup(of: Void.self) { group in
      await TL.$three.withValue(33) {
        await withTaskGroup(of: Void.self) { group in
          TL.$two.withValue(2222) {
            group.addTask { // will have to copy the `2222`
              print("Survived, one: \(TL.one) @ \(#fileID):\(#line)") // CHECK: Survived, one: 11
              print("Survived, two: \(TL.two) @ \(#fileID):\(#line)") // CHECK: Survived, two: 2222
              print("Survived, three: \(TL.three) @ \(#fileID):\(#line)") // CHECK: Survived, three: 33
            }
          }
        }
      }
      print("--")
    }
  }

  // Multiple "nested" task groups, however in different child tasks
  await withTaskGroup(of: Void.self) { outer in
    // Child task inside  'outer':
    await TL.$one.withValue(11) { // DOES NOT have to be copied by the 'inner.addTask' (!)
      outer.addTask {
        await TL.$two.withValue(2222) {
          await withTaskGroup(of: Void.self) { inner in
            TL.$three.withValue(3333) { // MUST be copied by 'inner.addTask'
              inner.addTask {
                print("Survived, one: \(TL.one) @ \(#fileID):\(#line)") // CHECK: Survived, one: 11
                print("Survived, two: \(TL.two) @ \(#fileID):\(#line)") // CHECK: Survived, two: 2222
                print("Survived, three: \(TL.three) @ \(#fileID):\(#line)") // CHECK: Survived, three: 3333
              }
            }
          }
        }
        print("--")
      }
    }
  }

  // Multiple "safe" task locals around a withTaskGroup, check if relinking works ok then
  await TL.$one.withValue(11) {
    await TL.$two.withValue(22) {
      await TL.$three.withValue(33) {
        await withTaskGroup(of: Void.self) { group in
          await TL.$four.withValue(44) { // must copy
            group.addTask {
              print("Survived, one: \(TL.one) @ \(#fileID):\(#line)") // CHECK: Survived, one: 11
              print("Survived, two: \(TL.two) @ \(#fileID):\(#line)") // CHECK: Survived, two: 22
              print("Survived, three: \(TL.three) @ \(#fileID):\(#line)") // CHECK: Survived, three: 33
              print("Survived, four: \(TL.four) @ \(#fileID):\(#line)") // CHECK: Survived, four: 44
            }
          }
        }
      }
    }
    print("--")
  }


  print("Survived, done") // CHECK: Survived, done
}

@main struct Main {
  static func main() async {
    await test()
  }
}
