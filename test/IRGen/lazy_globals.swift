// RUN: %target-swift-frontend -parse-as-library -emit-ir -primary-file %s | %FileCheck %s

// REQUIRES: CPU=x86_64

// CHECK: @"[[T:.*]]Wz" = internal global i64 0, align 8
// CHECK: @"$s12lazy_globals1xSivp" = hidden global %TSi zeroinitializer, align 8
// CHECK: @"$s12lazy_globals1ySivp" = hidden global %TSi zeroinitializer, align 8
// CHECK: @"$s12lazy_globals1zSivp" = hidden global %TSi zeroinitializer, align 8

// CHECK: define internal void @"[[T]]WZ"(ptr %0) {{.*}} {
// CHECK: entry:
// CHECK:   store i64 1, ptr @"$s12lazy_globals1xSivp", align 8
// CHECK:   store i64 2, ptr @"$s12lazy_globals1ySivp", align 8
// CHECK:   store i64 3, ptr @"$s12lazy_globals1zSivp", align 8
// CHECK:   ret void
// CHECK: }

// CHECK: define hidden swiftcc ptr @"$s12lazy_globals1xSivau"() {{.*}} {
// CHECK: entry:
// CHECK:   call void @swift_once(ptr @"[[T]]Wz", ptr @"[[T]]WZ", ptr undef)
// CHECK: }

// CHECK: define hidden swiftcc ptr @"$s12lazy_globals1ySivau"() {{.*}} {
// CHECK: entry:
// CHECK:   call void @swift_once(ptr @"[[T]]Wz", ptr @"[[T]]WZ", ptr undef)
// CHECK: }

// CHECK: define hidden swiftcc ptr @"$s12lazy_globals1zSivau"() {{.*}} {
// CHECK: entry:
// CHECK:   call void @swift_once(ptr @"[[T]]Wz", ptr @"[[T]]WZ", ptr undef)
// CHECK: }
var (x, y, z) = (1, 2, 3)

// CHECK: define hidden swiftcc i64 @"$s12lazy_globals4getXSiyF"() {{.*}} {
// CHECK: entry:
// CHECK:   %0 = call swiftcc ptr @"$s12lazy_globals1xSivau"()
// CHECK:   %._value = getelementptr inbounds %TSi, ptr %0, i32 0, i32 0
// CHECK:   [[V:%.*]] = load i64, ptr %._value, align 8
// CHECK:   ret i64 [[V]]
// CHECK: }
func getX() -> Int { return x }

