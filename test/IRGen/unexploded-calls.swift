// RUN: %swift -disable-legacy-type-info -target thumbv7-unknown-windows-msvc -parse-stdlib -parse-as-library -I %S/Inputs/usr/include -module-name Swift -S -emit-ir -o - %s | %FileCheck %s
// RUN: %swift -disable-legacy-type-info -target thumbv7-unknown-linux-gnueabihf -parse-stdlib -parse-as-library -I %S/Inputs/usr/include -module-name Swift -S -emit-ir -o - %s | %FileCheck %s
// RUN: %swift -disable-legacy-type-info -target thumbv7-unknown-linux-gnueabi -Xcc -mfloat-abi=hard -parse-stdlib -parse-as-library -I %S/Inputs/usr/include -module-name Swift -S -emit-ir -o - %s | %FileCheck %s

// REQUIRES: CODEGENERATOR=ARM

struct Float {
  let _value: Builtin.FPIEEE32
}

typealias CFloat = Float
typealias Void = ()

import SRoA

public func g(_ s : S) {
  return f(s)
}

// CHECK: define {{.*}}swiftcc void @"$ss1gyySo1SVF"(float %0, float %1) {{.*}}{
// CHECK: entry:
// CHECK:   alloca
// CHECK:   [[ALLOCA:%[-._0-9a-zA-Z]+]] = alloca %TSo1SV, align 4
// CHECK:   [[ALLOCA]].f = getelementptr inbounds %TSo1SV, ptr [[ALLOCA]], i32 0, i32 0
// CHECK:   [[ALLOCA]].f._value = getelementptr inbounds %TSf, ptr [[ALLOCA]].f, i32 0, i32 0
// CHECK:   store float %0, ptr [[ALLOCA]].f._value, align 4
// CHECK:   [[ALLOCA]].g = getelementptr inbounds %TSo1SV, ptr [[ALLOCA]], i32 0, i32 1
// CHECK:   [[ALLOCA]].g._value = getelementptr inbounds %TSf, ptr [[ALLOCA]].g, i32 0, i32 0
// CHECK:   store float %1, ptr [[ALLOCA]].g._value, align 4
// CHECK:   %[[LOAD:.*]] = load %struct.S, ptr [[ALLOCA]], align 4
// CHECK:   call void @f(%struct.S %[[LOAD]])
// CHECK: }

