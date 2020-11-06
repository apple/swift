// RUN: %target-sil-opt -enable-sil-verify-all %s -sil-combine | %FileCheck %s

sil_stage canonical

import Builtin
import Swift

class Foo {
}


sil @_TFC4main3Food : $@convention(method) (@owned Foo) -> @owned Builtin.NativeObject {
bb0(%0 : $Foo):
  debug_value %0 : $Foo, let, name "self" // id: %1
  %2 = unchecked_ref_cast %0 : $Foo to $Builtin.NativeObject // user: %3
  return %2 : $Builtin.NativeObject               // id: %3
}

sil @_TFC4main3FooD : $@convention(method) (@owned Foo) -> () {
bb0(%0 : $Foo):
  debug_value %0 : $Foo, let, name "self" // id: %1
  // function_ref main.Foo.deinit
  %2 = function_ref @_TFC4main3Food : $@convention(method) (@owned Foo) -> @owned Builtin.NativeObject // user: %3
  %3 = apply %2(%0) : $@convention(method) (@owned Foo) -> @owned Builtin.NativeObject // user: %4
  %4 = unchecked_ref_cast %3 : $Builtin.NativeObject to $Foo // user: %5
  dealloc_ref %4 : $Foo                           // id: %5
  %6 = tuple ()                                   // user: %7
  return %6 : $()                                 // id: %7
}

sil @_TFC4main3FoocfMS0_FT_S0_ : $@convention(method) (@owned Foo) -> @owned Foo {
bb0(%0 : $Foo):
  debug_value %0 : $Foo, let, name "self" // id: %1
  return %0 : $Foo                                // id: %2
}

sil @_TFC4main3FooCfMS0_FT_S0_ : $@convention(thin) (@thick Foo.Type) -> @owned Foo {
bb0(%0 : $@thick Foo.Type):
  %1 = alloc_ref $Foo                             // user: %3
  // function_ref main.Foo.init (main.Foo.Type)() -> main.Foo
  %2 = function_ref @_TFC4main3FoocfMS0_FT_S0_ : $@convention(method) (@owned Foo) -> @owned Foo // user: %3
  %3 = apply %2(%1) : $@convention(method) (@owned Foo) -> @owned Foo // user: %4
  return %3 : $Foo                                // id: %4
}

sil [readonly] @_TF4main9readonly_funcFT_CS_3Foo : $@convention(thin) () -> @owned Foo {
bb0:
  // function_ref main.Foo.__allocating_init (main.Foo.Type)() -> main.Foo
  %0 = function_ref @_TFC4main3FooCfMS0_FT_S0_ : $@convention(thin) (@thick Foo.Type) -> @owned Foo // user: %2
  %1 = metatype $@thick Foo.Type                  // user: %2
  %2 = apply %0(%1) : $@convention(thin) (@thick Foo.Type) -> @owned Foo // user: %3
  return %2 : $Foo                                // id: %3
}

//CHECK-LABEL: @_TF4main3bazFT_T_
//CHECK-NOT: function_ref
//CHECK-NOT: apply
//CHECK: tuple
//CHECK-NEXT: return
sil @_TF4main3bazFT_T_ : $@convention(thin) () -> () {
bb0:
  // function_ref main.readonly_func () -> main.Foo
  %0 = function_ref @_TF4main9readonly_funcFT_CS_3Foo : $@convention(thin) () -> @owned Foo // user: %1
  %1 = apply %0() : $@convention(thin) () -> @owned Foo       // users: %2, %3
  debug_value %1 : $Foo, let, name "unused" // id: %2
  strong_release %1 : $Foo                        // id: %3
  %4 = tuple ()                                   // user: %5
  return %4 : $()                                 // id: %5
}

sil_vtable Foo {
  #Foo.init!initializer: @_TFC4main3FoocfMS0_FT_S0_	// main.Foo.init (main.Foo.Type)() -> main.Foo
}



