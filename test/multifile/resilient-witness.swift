// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -module-name test -num-threads 1 -O -c -g %S/Inputs/resilient-witness-2.swift %s -o %t/resilient-witness-2.o -o %t/resilient-witness.o
// RUN: %target-swift-frontend -module-name test -num-threads 1 -O -emit-ir -g %S/Inputs/resilient-witness-2.swift %s -o %t/resilient-witness-2.ll -o %t/resilient-witness.ll
// RUN: %FileCheck %s < %t/resilient-witness.ll

// REQUIRES: swift_stable_abi

// We used to crash on this test case during code generation because the
// resilient witness table and the witness of the equatable conformance was
// emitted into different files. Because the witness is expressed as a relative
// pointer this was problematic.

class Context {
  enum SomeEnum: Int {
    case One = 1
    case MinusOne = -1

    init?(_ delimiter: Character) {
      switch delimiter {
      case "1":
        self = .One
      case "-":
        self = .MinusOne
      default:
        return nil
      }
    }
  }
  private struct SomeEnumStruct {
    var e: SomeEnum?
  }

  private var arr: [SomeEnumStruct?] = []
}

public func inlineThis(_ char: Character) {
  if Context.SomeEnum(char) == .MinusOne {
    print("foobar")
  }
}

// The witness table and witness definition need to be in the same file.

// CHECK: @"$s4test7ContextC8SomeEnumOSQAAMc" = {{.*}}sub {{.*}} @"$s4test7ContextC8SomeEnumOSQAASQ2eeoiySbx_xtFZTW" {{.*}} @"$s4test7ContextC8SomeEnumOSQAAMc"


// CHECK: define{{.*}}@"$s4test7ContextC8SomeEnumOSQAASQ2eeoiySbx_xtFZTW"({{.*}}){{.*}} {
// CHECK-NEXT: entry:
