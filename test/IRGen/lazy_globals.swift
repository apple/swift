// RUN: %target-swift-frontend %use_no_opaque_pointers -parse-as-library -emit-ir -primary-file %s | %FileCheck %s
// RUN: %target-swift-frontend -parse-as-library -emit-ir -primary-file %s

// REQUIRES: CPU=x86_64

// CHECK: @"[[T:.*]]Wz" = internal global i64 0, align 8
// CHECK: @"$s12lazy_globals1xSivp" = hidden global %TSi zeroinitializer, align 8
// CHECK: @"$s12lazy_globals1ySivp" = hidden global %TSi zeroinitializer, align 8
// CHECK: @"$s12lazy_globals1zSivp" = hidden global %TSi zeroinitializer, align 8

// CHECK: define internal void @"[[T]]WZ"(i8* %0) {{.*}} {
// CHECK: entry:
// CHECK:   store i64 1, i64* getelementptr inbounds (%TSi, %TSi* @"$s12lazy_globals1xSivp", i32 0, i32 0), align 8
// CHECK:   store i64 2, i64* getelementptr inbounds (%TSi, %TSi* @"$s12lazy_globals1ySivp", i32 0, i32 0), align 8
// CHECK:   store i64 3, i64* getelementptr inbounds (%TSi, %TSi* @"$s12lazy_globals1zSivp", i32 0, i32 0), align 8
// CHECK:   ret void
// CHECK: }

// CHECK: define hidden swiftcc i8* @"$s12lazy_globals1xSivau"() {{.*}} {
// CHECK: entry:
// CHECK:   call void @swift_once(i64* @"[[T]]Wz", i8* bitcast (void (i8*)* @"[[T]]WZ" to i8*), i8* undef)
// CHECK: }

// CHECK: define hidden swiftcc i8* @"$s12lazy_globals1ySivau"() {{.*}} {
// CHECK: entry:
// CHECK:   call void @swift_once(i64* @"[[T]]Wz", i8* bitcast (void (i8*)* @"[[T]]WZ" to i8*), i8* undef)
// CHECK: }

// CHECK: define hidden swiftcc i8* @"$s12lazy_globals1zSivau"() {{.*}} {
// CHECK: entry:
// CHECK:   call void @swift_once(i64* @"[[T]]Wz", i8* bitcast (void (i8*)* @"[[T]]WZ" to i8*), i8* undef)
// CHECK: }
var (x, y, z) = (1, 2, 3)

// CHECK: define hidden swiftcc i64 @"$s12lazy_globals4getXSiyF"() {{.*}} {
// CHECK: entry:
// CHECK:   %0 = call swiftcc i8* @"$s12lazy_globals1xSivau"()
// CHECK:   %1 = bitcast i8* %0 to %TSi*
// CHECK:   %._value = getelementptr inbounds %TSi, %TSi* %1, i32 0, i32 0
// CHECK:   [[V:%.*]] = load i64, i64* %._value, align 8
// CHECK:   ret i64 [[V]]
// CHECK: }
func getX() -> Int { return x }

