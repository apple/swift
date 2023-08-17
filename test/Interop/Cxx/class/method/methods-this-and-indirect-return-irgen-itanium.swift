// RUN: %target-swift-emit-irgen -I %S/Inputs -enable-experimental-cxx-interop %s -Xcc -fignore-exceptions | %FileCheck %s

// UNSUPPORTED: OS=windows-msvc

import Methods

public func use() -> CInt {
    var instance = HasMethods()
    let result = instance.nonConstPassThroughAsWrapper(42)
    return result.value
}

// CHECK: %[[instance:.*]] = alloca %TSo10HasMethodsV
// CHECK: %[[result:.*]] = alloca %TSo19NonTrivialInWrapperV
// CHECK: call void @_ZN10HasMethods28nonConstPassThroughAsWrapperEi(ptr sret(%struct.NonTrivialInWrapper) %[[result]], ptr %[[instance]], i32 42)

// CHECK: define {{.*}} void @_ZN10HasMethods28nonConstPassThroughAsWrapperEi(ptr noalias sret(%struct.NonTrivialInWrapper) {{.*}} %{{.*}}, ptr {{.*}} %{{.*}}, i32 noundef %{{.*}})
