// RUN: %target-swift-emit-silgen %s | %FileCheck %s

// CHECK-LABEL: sil hidden [ossa] @$s14metatype_casts6t_is_u{{[_0-9a-zA-Z]*}}F
// CHECK:         checked_cast_br {{.*}} $@thick T.Type to U.Type
func t_is_u<T, U>(_: T, _: U) -> Bool {
  return T.self is U.Type
}

// CHECK-LABEL: sil hidden [ossa] @$s14metatype_casts8int_is_t{{[_0-9a-zA-Z]*}}F
func int_is_t<T>() -> (Bool, T.Type?, T.Type) {
  // CHECK: checked_cast_br Int.Type in {{%.*}} : $@thick Int.Type to T.Type
  // CHECK: checked_cast_br Int.Type in {{%.*}} : $@thick Int.Type to T.Type
  // CHECK: unconditional_checked_cast {{%.*}} : $@thick Int.Type to T.Type
  return (Int.self is T.Type, Int.self as? T.Type, Int.self as! T.Type)
}

// CHECK-LABEL: sil hidden [ossa] @$s14metatype_casts8t_is_int{{[_0-9a-zA-Z]*}}F
func t_is_int<T>(_: T) -> (Bool, Int.Type?, Int.Type) {
  // CHECK: checked_cast_br T.Type in {{%.*}} : $@thick T.Type to Int.Type
  // CHECK: checked_cast_br T.Type in {{%.*}} : $@thick T.Type to Int.Type
  // CHECK: unconditional_checked_cast {{%.*}} : $@thick T.Type to Int.Type
  return (T.self is Int.Type, T.self as? Int.Type, T.self as! Int.Type)
}

// Mixed metatype casts take the slow path via *_cast_addr
protocol Emergency {}
class Ambulance : Emergency {}
class FashionPolice {}

// CHECK-LABEL: sil hidden [ossa] @$s14metatype_casts30anyObjectToExistentialMetatype{{[_0-9a-zA-Z]*}}F
func anyObjectToExistentialMetatype(o: AnyObject) -> Emergency.Type? {
  // CHECK: checked_cast_addr_br take_always AnyObject in {{%.*}} : $*AnyObject to any Emergency.Type in {{%.*}}
  return o as? Emergency.Type
}

// CHECK-LABEL: sil hidden [ossa] @$s14metatype_casts19anyObjectToMetatype{{[_0-9a-zA-Z]*}}F
func anyObjectToMetatype(o: AnyObject) -> FashionPolice.Type? {
  // CHECK: checked_cast_addr_br take_always AnyObject in {{%.*}} : $*AnyObject to FashionPolice.Type in {{%.*}}
  return o as? FashionPolice.Type
}
