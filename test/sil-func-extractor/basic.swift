// Passing demangled name

// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend %s -g -module-name basic -emit-sib -o %t/demangle-ext-foo.sib; %target-sil-func-extractor -module-name basic -func="basic.foo" %t/demangle-ext-foo.sib | %FileCheck %s -check-prefix=EXTRACT-FOO
// RUN: %target-swift-frontend %s -g -module-name basic -emit-sib -o %t/demangle-ext-test.sib; %target-sil-func-extractor -module-name basic -func="basic.X.test" %t/demangle-ext-test.sib | %FileCheck %s -check-prefix=EXTRACT-TEST
// RUN: %target-swift-frontend %s -g -module-name basic -emit-sib -o %t/demangle-ext-init.sib; %target-sil-func-extractor -module-name basic -func="basic.Vehicle.init" %t/demangle-ext-init.sib | %FileCheck %s -check-prefix=EXTRACT-INIT
// RUN: %target-swift-frontend %s -g -module-name basic -emit-sib -o %t/demangle-ext-now.sib; %target-sil-func-extractor -module-name basic -func="basic.Vehicle.now" %t/demangle-ext-now.sib | %FileCheck %s -check-prefix=EXTRACT-NOW

// Passing mangled name

// RUN: %target-swift-frontend %s -g -module-name basic -emit-sib -o %t/mangle-ext-foo.sib; %target-sil-func-extractor -module-name basic -func='$s5basic3fooSiyF' %t/mangle-ext-foo.sib | %FileCheck %s -check-prefix=EXTRACT-FOO
// RUN: %target-swift-frontend %s -g -module-name basic -emit-sib -o %t/mangle-ext-test.sib; %target-sil-func-extractor -module-name basic -func='$s5basic1XV4testyyF' %t/mangle-ext-test.sib | %FileCheck %s -check-prefix=EXTRACT-TEST
// RUN: %target-swift-frontend %s -g -module-name basic -emit-sib -o %t/mangle-ext-init.sib; %target-sil-func-extractor -module-name basic -func='$s5basic7VehicleC1nACSi_tcfc' %t/mangle-ext-init.sib | %FileCheck %s -check-prefix=EXTRACT-INIT
// RUN: %target-swift-frontend %s -g -module-name basic -emit-sib -o %t/mangle-ext-now.sib; %target-sil-func-extractor -module-name basic -func='$s5basic7VehicleC3nowSiyF' %t/mangle-ext-now.sib | %FileCheck %s -check-prefix=EXTRACT-NOW


// EXTRACT-FOO-NOT: sil hidden @$s5basic1XV4testyyF : $@convention(method) (X) -> () {
// EXTRACT-FOO-NOT: sil hidden @$s5basic7VehicleC1nACSi_tcfc : $@convention(method) (Int, @guaranteed Vehicle) -> @owned Vehicle {
// EXTRACT-FOO-NOT: sil hidden @$s5basic7VehicleC3nowSiyF : $@convention(method) (@guaranteed Vehicle) -> Int {

// EXTRACT-FOO-LABEL: sil @$s5basic3fooSiyF : $@convention(thin) () -> Int {
// EXTRACT-FOO:       bb0:
// EXTRACT-FOO-NEXT:    %0 = integer_literal
// EXTRACT-FOO:         %[[POS:.*]] = struct $Int
// EXTRACT-FOO-NEXT:    return %[[POS]] : $Int


// EXTRACT-TEST-NOT: sil hidden @$s5basic3fooSiyF : $@convention(thin) () -> Int {
// EXTRACT-TEST-NOT: sil hidden @$s5basic7VehicleC1nACSi_tcfc : $@convention(method) (Int, @guaranteed Vehicle) -> @owned Vehicle {
// EXTRACT-TEST-NOT: sil hidden @$s5basic7VehicleC3nowSiyF : $@convention(method) (@guaranteed Vehicle) -> Int {

// EXTRACT-TEST-LABEL:  sil @$s5basic1XV4testyyF : $@convention(method) (X) -> () {
// EXTRACT-TEST:        bb0(%0 : $X):
// EXTRACT-TEST-NEXT:     function_ref
// EXTRACT-TEST-NEXT:     function_ref @$s5basic3fooSiyF : $@convention(thin) () -> Int
// EXTRACT-TEST-NEXT:     apply
// EXTRACT-TEST-NEXT:     tuple
// EXTRACT-TEST-NEXT:     return


// EXTRACT-INIT-NOT: sil @$s5basic3fooSiyF : $@convention(thin) () -> Int {
// EXTRACT-INIT-NOT: sil @$s5basic1XV4testyyF : $@convention(method) (X) -> () {
// EXTRACT-INIT-NOT: sil @$s5basic7VehicleC3nowSiyF : $@convention(method) (@owned Vehicle) -> Int {

// EXTRACT-INIT-LABEL:   sil @$s5basic7VehicleC1nACSi_tcfc : $@convention(method) (Int, @owned Vehicle) -> @owned Vehicle {
// EXTRACT-INIT:         bb0
// EXTRACT-INIT-NEXT:      ref_element_addr
// EXTRACT-INIT-NEXT:      begin_access [init] [static]
// EXTRACT-INIT-NEXT:      store
// EXTRACT-INIT-NEXT:      end_access
// EXTRACT-INIT-NEXT:      return


// EXTRACT-NOW-NOT: sil @$s5basic3fooSiyF : $@convention(thin) () -> Int {
// EXTRACT-NOW-NOT: sil @$s5basic1XV4testyyF : $@convention(method) (X) -> () {
// EXTRACT-NOW-NOT: sil @$s5basic7VehicleC1nACSi_tcfc : $@convention(method) (Int, @guaranteed Vehicle) -> @owned Vehicle {

// EXTRACT-NOW-LABEL:   sil @$s5basic7VehicleC3nowSiyF : $@convention(method) (@guaranteed Vehicle) -> Int {
// EXTRACT-NOW:         bb0
// EXTRACT-NOW:           class_method
// EXTRACT-NOW-NEXT:      apply
// EXTRACT-NOW-NEXT:      return

public struct X {
  @usableFromInline
  @inlinable
  func test() {
    foo()
  }
}

@_fixed_layout
public class Vehicle {
    @usableFromInline
    var numOfWheels: Int

    @usableFromInline
    @inlinable
    init(n: Int) {
      numOfWheels = n
    }

    @usableFromInline
    @inlinable
    func now() -> Int {
        return numOfWheels
    }
}

@discardableResult
@usableFromInline
@inlinable
func foo() -> Int {
  return 7
}
