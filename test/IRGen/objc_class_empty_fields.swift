// RUN: %target-swift-frontend %use_no_opaque_pointers -primary-file %s -enable-objc-interop -emit-ir | %FileCheck %s --check-prefix=CHECK-%target-ptrsize
// RUN: %target-swift-frontend -primary-file %s -enable-objc-interop -emit-ir

// https://github.com/apple/swift/issues/43667

// CHECK-64: @_DATA__TtC23objc_class_empty_fields14OneEnumWrapper = internal constant { {{.*}}* } { i32 {{[0-9]+}}, i32 16, i32 24
// CHECK-32: @_DATA__TtC23objc_class_empty_fields14OneEnumWrapper = internal constant { {{.*}}* } { i32 {{[0-9]+}}, i32 8, i32 12

enum OneCaseEnum {
    case X
}

class OneEnumWrapper {
    var myVar: OneCaseEnum
    var whyVar: OneCaseEnum
    var x: Int

    init(v: OneCaseEnum)
    {
        self.myVar = v
        self.whyVar = v
        self.x = 0
    }
}

let e = OneCaseEnum.X
print(e)
let x = OneEnumWrapper(v: e)
print(x)
