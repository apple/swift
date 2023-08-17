// RUN: %target-swift-frontend -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -Xcc -fignore-exceptions -O | %FileCheck %s

import CopyMoveAssignment

@inline(never)
func takeValue<T>(_ x: T) {
    let _ = x
}

@inline(never)
public func copyAssign() {
    var instance = NonTrivialCopyAndCopyMoveAssign()
    let instance2 = NonTrivialCopyAndCopyMoveAssign()
    instance = instance2
    takeValue(instance2)
    takeValue(instance)
}

// CHECK-LABEL: define {{.*}}copyAssign
// CHECK: call {{void|ptr}} @{{_ZN31NonTrivialCopyAndCopyMoveAssignC1Ev|_ZN31NonTrivialCopyAndCopyMoveAssignC2Ev|"\?\?0NonTrivialCopyAndCopyMoveAssign@@QEAA@XZ"}}(ptr %[[COPY_INSTANCE:.*]])
// CHECK: call {{void|ptr}} @{{_ZN31NonTrivialCopyAndCopyMoveAssignC1Ev|_ZN31NonTrivialCopyAndCopyMoveAssignC2Ev|"\?\?0NonTrivialCopyAndCopyMoveAssign@@QEAA@XZ"}}(ptr %[[COPY_INSTANCE2:.*]])
// CHECK: call {{void|ptr}} @{{_ZN31NonTrivialCopyAndCopyMoveAssignD1Ev|_ZN31NonTrivialCopyAndCopyMoveAssignD2Ev|"\?\?1NonTrivialCopyAndCopyMoveAssign@@QEAA@XZ"}}(ptr %[[COPY_INSTANCE]])
// CHECK: call {{void|ptr}} @{{_ZN31NonTrivialCopyAndCopyMoveAssignC1EOS_|_ZN31NonTrivialCopyAndCopyMoveAssignC2EOS_|_ZN31NonTrivialCopyAndCopyMoveAssignC1ERKS_Tm|_ZN31NonTrivialCopyAndCopyMoveAssignC2ERKS_Tm|"\?\?0NonTrivialCopyAndCopyMoveAssign@@QEAA@AEBU0@@Z"}}(
// CHECK-SAME: %[[COPY_INSTANCE]],
// CHECK-SAME: ptr
// CHECK-SAME: %[[COPY_INSTANCE2]])
// CHECK: call {{.*}} @{{_ZN31NonTrivialCopyAndCopyMoveAssignD1Ev|_ZN31NonTrivialCopyAndCopyMoveAssignD2Ev|"\?\?1NonTrivialCopyAndCopyMoveAssign@@QEAA@XZ"}}(ptr %[[COPY_INSTANCE2]])
// CHECK: call {{.*}} @{{_ZN31NonTrivialCopyAndCopyMoveAssignD1Ev|_ZN31NonTrivialCopyAndCopyMoveAssignD2Ev|"\?\?1NonTrivialCopyAndCopyMoveAssign@@QEAA@XZ"}}(ptr %[[COPY_INSTANCE]])

// CHECK-LABEL: define {{.*}}takeAssign
// CHECK: call {{void|ptr}} @{{_ZN31NonTrivialCopyAndCopyMoveAssignC1Ev|_ZN31NonTrivialCopyAndCopyMoveAssignC2Ev|"\?\?0NonTrivialCopyAndCopyMoveAssign@@QEAA@XZ"}}(ptr %[[MOVE_INSTANCE:.*]])
// CHECK: call {{.*}} @{{_ZN31NonTrivialCopyAndCopyMoveAssignD1Ev|_ZN31NonTrivialCopyAndCopyMoveAssignD2Ev|"\?\?1NonTrivialCopyAndCopyMoveAssign@@QEAA@XZ"}}(ptr %[[MOVE_INSTANCE]])
// CHECK: call {{.*}} @{{_ZN31NonTrivialCopyAndCopyMoveAssignD1Ev|_ZN31NonTrivialCopyAndCopyMoveAssignD2Ev|"\?\?1NonTrivialCopyAndCopyMoveAssign@@QEAA@XZ"}}(ptr %[[MOVE_INSTANCE]])

// CHECK-LABEL: }

@inline(never)
public func takeAssign() {
    var instance = NonTrivialCopyAndCopyMoveAssign()
    instance = NonTrivialCopyAndCopyMoveAssign()
    takeValue(instance)
}

copyAssign()
takeAssign()
