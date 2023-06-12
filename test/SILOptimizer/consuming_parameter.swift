// RUN: %target-swift-frontend -c -disable-availability-checking -Xllvm --sil-print-final-ossa-module -O -module-name=main -o /dev/null %s 2>&1 | %FileCheck %s
 
// REQUIRES: concurrency

// CHECK-LABEL: sil [ossa] @async_dead_arg_call : {{.*}} {
// CHECK:       {{bb[0-9]+}}([[INSTANCE:%[^,]+]] : @noImplicitCopy @_eagerMove @owned
// CHECK:         destroy_value [[INSTANCE]]
// CHECK:         [[EXECUTOR:%[^,]+]] = enum $Optional<Builtin.Executor>, #Optional.none!enumelt 
// CHECK:         [[CALLEE:%[^,]+]] = function_ref @async_callee
// CHECK:         apply [[CALLEE]]()
// CHECK:         hop_to_executor [[EXECUTOR]]
// CHECK-LABEL: } // end sil function 'async_dead_arg_call'
@_silgen_name("async_dead_arg_call")
public func async_dead_arg_call(o: consuming AnyObject) async {
  // o should be destroyed here
  await bar()
}
 
// CHECK-LABEL: sil [ossa] @async_dead_arg_call_lexical : {{.*}} {
// CHECK:       {{bb[0-9]+}}([[INSTANCE:%[^,]+]] : @noImplicitCopy @_lexical @owned
// CHECK:         [[MOVE:%[^,]+]] = move_value [lexical] [[INSTANCE]]
// CHECK:         [[EXECUTOR:%[^,]+]] = enum $Optional<Builtin.Executor>, #Optional.none!enumelt 
// CHECK:         [[CALLEE:%[^,]+]] = function_ref @async_callee
// CHECK:         apply [[CALLEE]]()
// CHECK:         hop_to_executor [[EXECUTOR]]
// CHECK:         destroy_value [[MOVE]]
// CHECK-LABEL: } // end sil function 'async_dead_arg_call_lexical'
@_silgen_name("async_dead_arg_call_lexical")
public func async_dead_arg_call_lexical(@_noEagerMove o: consuming AnyObject) async {
  await bar()
  // o should be destroyed here
}

extension C {
  // CHECK-LABEL: sil [ossa] @async_dead_arg_call_lexical_method : {{.*}} {
  // CHECK:       {{bb[0-9]+}}([[INSTANCE:%[^,]+]] : @noImplicitCopy @_lexical @owned
  // CHECK-LABEL: } // end sil function 'async_dead_arg_call_lexical_method'
  @_silgen_name("async_dead_arg_call_lexical_method")
  @_noEagerMove
  consuming
  public func async_dead_arg_call_lexical_method() async {
    await bar()
    // self should be destroyed here
  }
}

public class C {
  // CHECK-LABEL: sil [ossa] @async_dead_arg_call_method : {{.*}} {
  // CHECK:       {{bb[0-9]+}}([[INSTANCE:%[^,]+]] : @noImplicitCopy @_eagerMove @owned
  // CHECK:         destroy_value [[INSTANCE]]
  // CHECK:         [[EXECUTOR:%[^,]+]] = enum $Optional<Builtin.Executor>, #Optional.none!enumelt 
  // CHECK:         [[CALLEE:%[^,]+]] = function_ref @async_callee : $@convention(thin) @async () -> () 
  // CHECK:         apply [[CALLEE]]() : $@convention(thin) @async () -> () 
  // CHECK:         hop_to_executor [[EXECUTOR]]
  // CHECK-LABEL: } // end sil function 'async_dead_arg_call_method'
  @_silgen_name("async_dead_arg_call_method")
  consuming
  public func async_dead_arg_call() async {
    // self should be destroyed here
    await bar()
  }
}

@inline(never)
@_silgen_name("async_callee")
func bar() async {}

// CHECK-LABEL: sil [ossa] @write_to_pointer : {{.*}} {
// CHECK:       {{bb[0-9]+}}([[CONSUMED_INSTANCE:%[^,]+]] : @noImplicitCopy @_eagerMove @owned $AnyObject, [[UMP:%[^,]+]] :
// CHECK:         [[PTR:%[^,]+]] = struct_extract [[UMP]]
// CHECK:         [[ADDR:%[^,]+]] = pointer_to_address [[PTR]]
// CHECK:         store [[CONSUMED_INSTANCE]] to [assign] [[ADDR]]
// CHECK:         [[PTR2:%[^,]+]] = struct_extract [[UMP]]
// CHECK:         [[ADDR2:%[^,]+]] = pointer_to_address [[PTR2]]
// CHECK:         [[OUT:%[^,]+]] = load [copy] [[ADDR2]]
// CHECK:         return [[OUT]]
// CHECK-LABEL: } // end sil function 'write_to_pointer'
@_silgen_name("write_to_pointer")
public func write_to_pointer(o: consuming AnyObject, p: UnsafeMutablePointer<AnyObject>) -> AnyObject {
  // o should be destroyed here
  p.pointee = o
  return p.pointee
}

extension C {
  // CHECK-LABEL: sil [ossa] @write_to_pointer_method : {{.*}} {
  // CHECK:       {{bb[0-9]+}}([[UMP:%[^,]+]] : $UnsafeMutablePointer<C>, [[INSTANCE:%[^,]+]] : @noImplicitCopy @_eagerMove @owned
  // CHECK:         [[PTR:%[^,]+]] = struct_extract [[UMP]]
  // CHECK:         [[ADDR:%[^,]+]] = pointer_to_address [[PTR]]
  // CHECK:         store [[INSTANCE]] to [assign] [[ADDR]]
  // CHECK:         [[ADDR2:%[^,]+]] = struct_extract [[UMP]]
  // CHECK:         [[PTR2:%[^,]+]] = pointer_to_address [[ADDR2]]
  // CHECK:         [[OUT:%[^,]+]] = load [copy] [[PTR2]]
  // CHECK:         return [[OUT]]
  // CHECK-LABEL: } // end sil function 'write_to_pointer_method'
  @_silgen_name("write_to_pointer_method")
  consuming
  public func write_to_pointer(p: UnsafeMutablePointer<C>) -> C {
    // o should be destroyed here
    p.pointee = self
    return p.pointee
  }
}
