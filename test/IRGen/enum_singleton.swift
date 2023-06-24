// RUN: %target-swift-frontend -emit-ir %s | %FileCheck %s --check-prefix=CHECK
// RUN: %target-swift-frontend -emit-ir -O %s | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-OPT

public enum Payload {
  case c1(Bool)
  case c2(Bool)
  case c3(Any)
}

public enum SingletonEnum {
  case c1(Payload)
}

// CHECK: define internal void @"$s14enum_singleton13SingletonEnumOwxx"
// CHECK-OPT:   tail call void @"{{.*}}OwxxTm"
// CHECK:   ret void
// CHECK: }

// CHECK: define internal ptr @"$s14enum_singleton13SingletonEnumOwcp"
// CHECK-OPT:   [[R0:%.*]] = tail call ptr @"{{.*}}OwcpTm"
// CHECK-OPT:   ret ptr [[R0]]
// CHECK: }

// CHECK: define internal ptr @"$s14enum_singleton13SingletonEnumOwca"
// CHECK-OPT:   [[R1:%.*]] = tail call ptr @"{{.*}}OwcaTm"
// CHECK-OPT:   ret ptr [[R1]]
// CHECK: }

// CHECK: define internal ptr @"$s14enum_singleton13SingletonEnumOwta"
// CHECK-OPT:   [[R2:%.*]] = tail call ptr @"{{.*}}OwtaTm"
// CHECK-OPT:   ret ptr [[R2]]
// CHECK: }

// CHECK: define internal i32 @"$s14enum_singleton13SingletonEnumOwet"
// CHECK-OPT:   [[R2:%.*]] = tail call i32 @"{{.*}}OwetTm"
// CHECK-OPT:   ret i32 [[R2]]
// CHECK: }

// CHECK: define internal void @"$s14enum_singleton13SingletonEnumOwst"
// CHECK-OPT:   tail call void @"{{.*}}OwstTm"
// CHECK-OPT:   ret void
// CHECK: }

// CHECK: define internal i32 @"$s14enum_singleton13SingletonEnumOwug"
// CHECK:   ret i32 0
// CHECK: }

// CHECK: define internal void @"$s14enum_singleton13SingletonEnumOwup"
// CHECK:   ret void
// CHECK: }

// CHECK: define internal void @"$s14enum_singleton13SingletonEnumOwui"
// CHECK:   ret void
// CHECK: }
