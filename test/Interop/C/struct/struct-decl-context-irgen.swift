// RUN: %target-swift-emit-ir %use_no_opaque_pointers -I %S/Inputs %s | %FileCheck %s
// RUN: %target-swift-emit-ir -I %S/Inputs %s

// This test checks that structs that are imported from a C module are mangled
// in Swift names as if they are declared in the global namespace, even when
// they are lexically declared nested in other C structs.

import StructDeclContext

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo13StructRegularVF"({{.*}})
public func take(_ x: StructRegular) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo21StructNestedComplete1VF"({{.*}})
public func take(_: StructNestedComplete1) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo012StructNestedD9Complete1VF"({{.*}})
public func take(_: StructNestedNestedComplete1) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo012StructNestedD15CompletedLater1VF"({{.*}})
public func take(_: StructNestedNestedCompletedLater1) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo27StructNestedCompletedLater1VF"({{.*}})
public func take(_: StructNestedCompletedLater1) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo17StructTypedefTag2VF"({{.*}})
public func take(_: StructTypedefTag2) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo21StructNestedComplete2VF"({{.*}})
public func take(_: StructNestedComplete2) {}

// CHECK-LABEL: define {{.*}} void @"$s4main5take2yySo17StructTypedefTag2VF"({{.*}})
public func take2(_: StructTypedefName2) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo27StructNestedCompletedLater2VF"({{.*}})
public func take(_: StructNestedCompletedLater2) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo18StructTypedefName3aF"({{.*}})
public func take(_: StructTypedefName3) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo21StructNestedComplete3VF"({{.*}})
public func take(_: StructNestedComplete3) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo27StructNestedCompletedLater3VF"({{.*}})
public func take(_: StructNestedCompletedLater3) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo17StructTypedefTag4VF"({{.*}})
public func take(_: StructTypedefTag4) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo21StructNestedComplete4VF"({{.*}})
public func take(_: StructNestedComplete4) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySpySo17StructTypedefTag4VGF"(i8* %0)
public func take(_: StructTypedefName4) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo27StructNestedCompletedLater4VF"({{.*}})
public func take(_: StructNestedCompletedLater4) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo21StructNestedComplete5VF"({{.*}})
public func take(_: StructNestedComplete5) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyys13OpaquePointerVF"(i8* %0)
public func take(_: StructTypedefName5) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo27StructNestedCompletedLater5VF"({{.*}})
public func take(_: StructNestedCompletedLater5) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo18StructTypedefName6aF"({{.*}})
public func take(_: StructTypedefName6) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo21StructNestedComplete6VF"({{.*}})
public func take(_: StructNestedComplete6) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySpySo18StructTypedefName6aGF"(i8* %0)
public func take(_: StructTypedefName6Ptr) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo27StructNestedCompletedLater6VF"({{.*}})
public func take(_: StructNestedCompletedLater6) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo18StructTypedefName7aF"({{.*}})
public func take(_: StructTypedefName7) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo21StructNestedComplete7VF"({{.*}})
public func take(_: StructNestedComplete7) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySpySo18StructTypedefName7aGF"(i8* %0)
public func take(_: StructTypedefName7Ptr) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo27StructNestedCompletedLater7VF"({{.*}})
public func take(_: StructNestedCompletedLater7) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo18StructTypedefName8aF"({{.*}})
public func take(_: StructTypedefName8) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo21StructNestedComplete8VF"({{.*}})
public func take(_: StructNestedComplete8) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySpySo18StructTypedefName8aGF"(i8* %0)
public func take(_: StructTypedefName8Ptr) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySpySpySo18StructTypedefName8aGSgGF"(i8* %0)
public func take(_: StructTypedefName8PtrPtr) {}

// CHECK-LABEL: define {{.*}} void @"$s4main4takeyySo27StructNestedCompletedLater8VF"({{.*}})
public func take(_: StructNestedCompletedLater8) {}
