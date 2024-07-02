// RUN: %target-swift-emit-irgen %s -I %S/Inputs -cxx-interoperability-mode=default -Xcc -fignore-exceptions -disable-availability-checking | %FileCheck %s
// XFAIL: OS=windows-msvc

import ReferenceCounted


public func getLocalCount() -> NS.LocalCount {
    let result = NS.LocalCount.create()
    return result
}

// CHECK:      define {{.*}}swiftcc ptr @"$s4main13getLocalCountSo2NSO0cD0VyF"()
// CHECK-NEXT: entry:
// CHECK:        %0 = call ptr @{{_ZN2NS10LocalCount6createEv|"\?create\@LocalCount\@NS\@\@SAPEAU12\@XZ"}}()
// CHECK-NEXT:   call void @{{_Z8LCRetainPN2NS10LocalCountE|"\?LCRetain\@\@YAXPEAULocalCount\@NS\@\@\@Z"}}(ptr %0)
// CHECK:        ret ptr %0
// CHECK-NEXT: }


public func get42() -> Int32 {
    let result = NS.LocalCount.create()
    return result.returns42()
}

// CHECK:      define {{.*}}swiftcc i32 @"$s4main5get42s5Int32VyF"()
// CHECK-NEXT: entry:
// CHECK:        %0 = call ptr @{{_ZN2NS10LocalCount6createEv|"\?create\@LocalCount\@NS\@\@SAPEAU12\@XZ"}}()
// CHECK-NEXT:   call void @{{_Z8LCRetainPN2NS10LocalCountE|"\?LCRetain\@\@YAXPEAULocalCount\@NS\@\@\@Z"}}(ptr %0)
// CHECK:        %1 = call i32 @{{_ZN2NS10LocalCount9returns42Ev|"\?returns42\@LocalCount\@NS\@\@QEAAHXZ"}}
// CHECK:        ret i32 %1
// CHECK-NEXT: }


public func getNullable(wantNullptr: Bool) -> GlobalCountNullableInit? {
    let result = GlobalCountNullableInit.create(wantNullptr)
    return result
}

// CHECK:      define {{.*}}swiftcc i{{.*}} @"$s4main11getNullable11wantNullptrSo011GlobalCountC4InitVSgSb_tF"(i1 %0)
// CHECK-NEXT: entry:
// CHECK:        %1 = call ptr @{{_ZN23GlobalCountNullableInit6createEb|"\?create\@GlobalCountNullableInit\@\@SAPEAU1\@_N\@Z"}}
// CHECK-NEXT:   %2 = ptrtoint ptr %1 to i{{.*}}
// CHECK-NEXT:   %3 = inttoptr i{{.*}} %2 to ptr
// CHECK-NEXT:   %4 = icmp ne ptr %3, null
// CHECK-NEXT:   br i1 %4, label %lifetime.nonnull-value, label %lifetime.cont

// CHECK:      lifetime.nonnull-value:
// CHECK-NEXT:   call void @{{_Z20GCRetainNullableInitP23GlobalCountNullableInit|"\?GCRetainNullableInit\@\@YAXPEAUGlobalCountNullableInit\@\@\@Z"}}(ptr %3)
// CHECK-NEXT:   br label %lifetime.cont

// CHECK:      lifetime.cont:
// CHECK:          ret i{{.*}} %2
// CHECK-NEXT: }


public func getArrayOfLocalCount() -> [NS.LocalCount] {
    return [NS.LocalCount.create()]
}

// CHECK:      define {{.*}}swiftcc ptr @"$s4main20getArrayOfLocalCountSaySo2NSO0eF0VGyF"()
// CHECK-NEXT: entry:
// CHECK-NEXT:   %0 = call swiftcc %swift.metadata_response @"$sSo2NSO10LocalCountVMa"(i{{.*}} 0)
// CHECK-NEXT:   %1 = extractvalue %swift.metadata_response %0, 0
// CHECK-NEXT:   %2 = call swiftcc { ptr, ptr } @"$ss27_allocateUninitializedArrayySayxG_BptBwlF"(i{{.*}} 1, ptr %1)
// CHECK:        %5 = call ptr @{{_ZN2NS10LocalCount6createEv|"\?create\@LocalCount\@NS\@\@SAPEAU12\@XZ"}}()
// CHECK-NEXT:   call void @{{_Z8LCRetainPN2NS10LocalCountE|"\?LCRetain\@\@YAXPEAULocalCount\@NS\@\@\@Z"}}(ptr %5)
// CHECK:      }
