// RUN: %target-swift-frontend %s -import-objc-header %S/Inputs/serialized-objc-header.h -emit-ir -g -o - | %FileCheck %s

// REQUIRES: objc_interop

protocol Named {
    var name : String { get }
}

// initializer.Person.__allocating_init (initializer.Person.Type)() -> initializer.Person
// CHECK: define hidden {{.*}}ptr @"$s11initializer6PersonCACycfC"(ptr{{.*}}) {{.*}} {
// CHECK:  call {{.*}}ptr @"$s11initializer6PersonCACycfc"(ptr {{.*}}), !dbg ![[ALLOCATING_INIT:.*]]

// initializer.Person.init (initializer.Person.Type)() -> initializer.Person
// CHECK: define hidden {{.*}}ptr @"$s11initializer6PersonCACycfc"(ptr{{.*}}) {{.*}} {

// CHECK-DAG: ![[ALLOCATING_INIT]]  = !DILocation(line: 0, scope
class Person : Named {
    var name : String { get { return "No Name" } }
    var age = 0
}

var person = Person()
