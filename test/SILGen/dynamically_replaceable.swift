// RUN: %target-swift-emit-silgen -swift-version 5 %s | %FileCheck %s
// RUN: %target-swift-emit-silgen -swift-version 5 %s -enable-implicit-dynamic | %FileCheck %s --check-prefix=IMPLICIT
// RUN: %target-swift-emit-silgen -swift-version 5 %s -disable-previous-implementation-calls-in-dynamic-replacements | %FileCheck %s --check-prefix=NOPREVIOUS

// IMPLICIT-LABEL: sil private [ossa] @$s23dynamically_replaceable6$deferL_yyF
var x = 10
defer {
  let y = x
}

// CHECK-LABEL: sil hidden [ossa] @$s23dynamically_replaceable014maybe_dynamic_B0yyF : $@convention(thin) () -> () {
// IMPLICIT-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable014maybe_dynamic_B0yyF : $@convention(thin) () -> () {
func maybe_dynamic_replaceable() {
}

// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable08dynamic_B0yyF : $@convention(thin) () -> () {
dynamic func dynamic_replaceable() {
}

// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable6StructV1xACSi_tcfC : $@convention(method) (Int, @thin Struct.Type) -> Struct
// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable6StructV08dynamic_B0yyF : $@convention(method) (Struct) -> () {
// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable6StructV08dynamic_B4_varSivg
// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable6StructV08dynamic_B4_varSivs
// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable6StructVyS2icig : $@convention(method) (Int, Struct) -> Int
// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable6StructVyS2icis : $@convention(method) (Int, Int, @inout Struct) -> ()
// CHECK-LABEL: sil private [dynamically_replacable] [ossa] @$s23dynamically_replaceable6StructV22property_with_observerSivW
// CHECK-LABEL: sil private [dynamically_replacable] [ossa] @$s23dynamically_replaceable6StructV22property_with_observerSivw
struct Struct {
  dynamic init(x: Int) {
  }
  dynamic func dynamic_replaceable() {
  }

  dynamic var dynamic_replaceable_var : Int {
    get {
      return 10
    }
    set {
    }
  }

  dynamic subscript(x : Int) -> Int {
    get {
      return 10
    }
    set {
    }
  }

  dynamic var property_with_observer : Int {
    didSet {
    }
    willSet {
    }
  }
}

// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable5KlassC1xACSi_tcfc : $@convention(method) (Int, @owned Klass) -> @owned Klass
// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable5KlassC08dynamic_B0yyF : $@convention(method) (@guaranteed Klass) -> () {
// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable5KlassC08dynamic_B4_varSivg
// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable5KlassC08dynamic_B4_varSivs
// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable5KlassCyS2icig : $@convention(method) (Int, @guaranteed Klass) -> Int
// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable5KlassCyS2icis : $@convention(method) (Int, Int, @guaranteed Klass) -> ()
// CHECK-LABEL: sil private [dynamically_replacable] [ossa] @$s23dynamically_replaceable5KlassC22property_with_observerSivW
// CHECK-LABEL: sil private [dynamically_replacable] [ossa] @$s23dynamically_replaceable5KlassC22property_with_observerSivw
class Klass {
  dynamic init(x: Int) {
  }

  dynamic convenience init(c: Int) {
    self.init(x: c)
  }

  dynamic convenience init(a: Int, b: Int) {
  }
  dynamic func dynamic_replaceable() {
  }
  dynamic func dynamic_replaceable2() {
  }
  dynamic var dynamic_replaceable_var : Int {
    get {
      return 10
    }
    set {
    }
  }
  dynamic subscript(x : Int) -> Int {
    get {
      return 10
    }
    set {
    }
  }

  dynamic var property_with_observer : Int {
    didSet {
    }
    willSet {
    }
  }
}

class SubKlass : Klass {
  // CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable8SubKlassC1xACSi_tcfc
  // CHECK: // dynamic_function_ref Klass.init(x:)
  // CHECK: [[FUN:%.*]] = dynamic_function_ref @$s23dynamically_replaceable5KlassC1xACSi_tcfc
  // CHECK: apply [[FUN]]
  dynamic override init(x: Int) {
    super.init(x: x)
  }
}
// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable6globalSivg : $@convention(thin) () -> Int {
dynamic var global : Int {
  return 1
}

// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable08dynamic_B0yyF"] [ossa] @$s23dynamically_replaceable11replacementyyF : $@convention(thin) () -> () {
@_dynamicReplacement(for: dynamic_replaceable())
func replacement() {
}

extension Klass {
  // Calls to the replaced function inside the replacing function should be
  // statically dispatched.

  // CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable5KlassC08dynamic_B0yyF"] [ossa] @$s23dynamically_replaceable5KlassC11replacementyyF : $@convention(method) (@guaranteed Klass) -> () {
  // CHECK: [[FN:%.*]] = prev_dynamic_function_ref @$s23dynamically_replaceable5KlassC11replacementyyF
  // CHECK: apply [[FN]](%0) : $@convention(method) (@guaranteed Klass) -> ()
  // CHECK: [[METHOD:%.*]] = class_method %0 : $Klass, #Klass.dynamic_replaceable2 :
  // CHECK: = apply [[METHOD]](%0) : $@convention(method) (@guaranteed Klass) -> ()
  // CHECK: return
  // NOPREVIOUS-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable5KlassC08dynamic_B0yyF"] [ossa] @$s23dynamically_replaceable5KlassC11replacementyyF : $@convention(method) (@guaranteed Klass) -> () {
  // NOPREVIOUS: [[FN:%.*]] = class_method %0 : $Klass, #Klass.dynamic_replaceable
  // NOPREVIOUS: apply [[FN]](%0) : $@convention(method) (@guaranteed Klass) -> ()
  // NOPREVIOUS: [[METHOD:%.*]] = class_method %0 : $Klass, #Klass.dynamic_replaceable2 :
  // NOPREVIOUS: = apply [[METHOD]](%0) : $@convention(method) (@guaranteed Klass) -> ()
  // NOPREVIOUS: return
  @_dynamicReplacement(for: dynamic_replaceable())
  func replacement() {
    dynamic_replaceable()
    dynamic_replaceable2()
  }

  // CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable5KlassC1cACSi_tcfC"] [ossa] @$s23dynamically_replaceable5KlassC2crACSi_tcfC : $@convention(method) (Int, @thick Klass.Type) -> @owned Klass {
  // CHECK:  [[FUN:%.*]] = prev_dynamic_function_ref @$s23dynamically_replaceable5KlassC2crACSi_tcfC
  // CHECK:  apply [[FUN]]({{.*}}, %1)
  // NOPREVIOUS-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable5KlassC1cACSi_tcfC"] [ossa] @$s23dynamically_replaceable5KlassC2crACSi_tcfC : $@convention(method) (Int, @thick Klass.Type) -> @owned Klass {
  // NOPREVIOUS:  [[FUN:%.*]] = dynamic_function_ref @$s23dynamically_replaceable5KlassC1cACSi_tcfC
  // NOPREVIOUS:  apply [[FUN]]({{.*}}, %1)
  @_dynamicReplacement(for: init(c:))
  convenience init(cr: Int) {
    self.init(c: cr + 1)
  }

  // CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable5KlassC1a1bACSi_SitcfC"] [ossa] @$s23dynamically_replaceable5KlassC2ar2brACSi_SitcfC
  // CHECK: // dynamic_function_ref Klass.__allocating_init(c:)
  // CHECK: [[FUN:%.*]] = dynamic_function_ref @$s23dynamically_replaceable5KlassC1cACSi_tcfC
  // CHECK: apply [[FUN]]({{.*}}, %2)
  // NOPREVIOUS-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable5KlassC1a1bACSi_SitcfC"] [ossa] @$s23dynamically_replaceable5KlassC2ar2brACSi_SitcfC
  // NOPREVIOUS: // dynamic_function_ref Klass.__allocating_init(c:)
  // NOPREVIOUS: [[FUN:%.*]] = dynamic_function_ref @$s23dynamically_replaceable5KlassC1cACSi_tcfC
  // NOPREVIOUS: apply [[FUN]]({{.*}}, %2)
	@_dynamicReplacement(for: init(a: b:))
  convenience init(ar: Int, br: Int) {
    self.init(c: ar + br)
  }

  @_dynamicReplacement(for: init(x:))
    init(xr: Int) {
  }

// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable5KlassC08dynamic_B4_varSivg"] [ossa] @$s23dynamically_replaceable5KlassC1rSivg : $@convention(method) (@guaranteed Klass) -> Int {
// CHECK: bb0([[ARG:%.*]] : @guaranteed $Klass):
// CHECK:   [[ORIG:%.*]] = prev_dynamic_function_ref  @$s23dynamically_replaceable5KlassC1rSivg
// CHECK:   apply [[ORIG]]([[ARG]]) : $@convention(method) (@guaranteed Klass) -> Int
// NOPREVIOUS-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable5KlassC08dynamic_B4_varSivg"] [ossa] @$s23dynamically_replaceable5KlassC1rSivg : $@convention(method) (@guaranteed Klass) -> Int {
// NOPREVIOUS: bb0([[ARG:%.*]] : @guaranteed $Klass):
// NOPREVIOUS:   [[ORIG:%.*]] = class_method [[ARG]] : $Klass, #Klass.dynamic_replaceable_var!getter
// NOPREVIOUS:   apply [[ORIG]]([[ARG]]) : $@convention(method) (@guaranteed Klass) -> Int

// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable5KlassC08dynamic_B4_varSivs"] [ossa] @$s23dynamically_replaceable5KlassC1rSivs : $@convention(method) (Int, @guaranteed Klass) -> () {
// CHECK: bb0({{.*}} : $Int, [[SELF:%.*]] : @guaranteed $Klass):
// CHECK:   [[ORIG:%.*]] = prev_dynamic_function_ref @$s23dynamically_replaceable5KlassC1rSivs
// CHECK:   apply [[ORIG]]({{.*}}, [[SELF]]) : $@convention(method)
// NOPREVIOUS-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable5KlassC08dynamic_B4_varSivs"] [ossa] @$s23dynamically_replaceable5KlassC1rSivs : $@convention(method) (Int, @guaranteed Klass) -> () {
// NOPREVIOUS: bb0({{.*}} : $Int, [[SELF:%.*]] : @guaranteed $Klass):
// NOPREVIOUS:   [[ORIG:%.*]] = class_method [[SELF]] : $Klass, #Klass.dynamic_replaceable_var!setter
// NOPREVIOUS:   apply [[ORIG]]({{.*}}, [[SELF]]) : $@convention(method)
  @_dynamicReplacement(for: dynamic_replaceable_var)
  var r : Int {
    get {
      return dynamic_replaceable_var + 1
    }
    set {
      dynamic_replaceable_var = newValue + 1
    }
  }

// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable5KlassCyS2icig"] [ossa] @$s23dynamically_replaceable5KlassC1xS2i_tcig
// CHECK: bb0({{.*}} : $Int, [[SELF:%.*]] : @guaranteed $Klass):
// CHECK:   [[ORIG:%.*]] = prev_dynamic_function_ref @$s23dynamically_replaceable5KlassC1xS2i_tcig
// CHECK:   apply [[ORIG]]({{.*}}, [[SELF]]) : $@convention(method) (Int, @guaranteed Klass) -> Int

// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable5KlassCyS2icis"] [ossa] @$s23dynamically_replaceable5KlassC1xS2i_tcis
// CHECK: bb0({{.*}} : $Int, {{.*}} : $Int, [[SELF:%.*]] : @guaranteed $Klass):
// CHECK:   [[ORIG:%.*]] = prev_dynamic_function_ref @$s23dynamically_replaceable5KlassC1xS2i_tcis
// CHECK:   apply [[ORIG]]({{.*}}, {{.*}}, [[SELF]]) : $@convention(method) (Int, Int, @guaranteed Klass) -> ()

  @_dynamicReplacement(for: subscript(_:))
  subscript(x y: Int) -> Int {
    get {
      return self[y]
    }
    set {
      self[y] = newValue
    }
  }
}

extension Struct {

  // CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable6StructV08dynamic_B0yyF"] [ossa] @$s23dynamically_replaceable6StructV11replacementyyF : $@convention(method) (Struct) -> () {
  // CHECK:   [[FUN:%.*]] = prev_dynamic_function_ref @$s23dynamically_replaceable6StructV11replacementyyF
  // CHECK:   apply [[FUN]](%0) : $@convention(method) (Struct) -> ()
  @_dynamicReplacement(for: dynamic_replaceable())
  func replacement() {
    dynamic_replaceable()
  }
  // CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable6StructV1xACSi_tcfC"] [ossa] @$s23dynamically_replaceable6StructV1yACSi_tcfC : $@convention(method) (Int, @thin Struct.Type) -> Struct {
  // CHECK: [[FUN:%.*]] = prev_dynamic_function_ref @$s23dynamically_replaceable6StructV1yACSi_tcfC
  // CHECK: apply [[FUN]]({{.*}}, %1)
  @_dynamicReplacement(for: init(x:))
  init(y: Int) {
    self.init(x: y + 1)
  }

// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable6StructV08dynamic_B4_varSivg"] [ossa] @$s23dynamically_replaceable6StructV1rSivg
// CHECK: bb0([[ARG:%.*]] : $Struct):
// CHECK:   [[ORIG:%.*]] = prev_dynamic_function_ref @$s23dynamically_replaceable6StructV1rSivg
// CHECK:   apply [[ORIG]]([[ARG]]) : $@convention(method) (Struct) -> Int

// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable6StructV08dynamic_B4_varSivs"] [ossa] @$s23dynamically_replaceable6StructV1rSivs
// CHECK: bb0({{.*}} : $Int, [[ARG:%.*]] : $*Struct):
// CHECK:   [[BA:%.*]] = begin_access [modify] [unknown] [[ARG]] : $*Struct
// CHECK:   [[ORIG:%.*]] = prev_dynamic_function_ref @$s23dynamically_replaceable6StructV1rSivs
// CHECK:   apply [[ORIG]]({{.*}}, [[BA]]) : $@convention(method) (Int, @inout Struct) -> ()
// CHECK:   end_access [[BA]] : $*Struct
  @_dynamicReplacement(for: dynamic_replaceable_var)
  var r : Int {
    get {
      return dynamic_replaceable_var + 1
    }
    set {
      dynamic_replaceable_var = newValue + 1
    }
  }

// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable6StructVyS2icig"] [ossa] @$s23dynamically_replaceable6StructV1xS2i_tcig
// CHECK: bb0({{.*}} : $Int, [[SELF:%.*]] : $Struct):
// CHECK:   [[ORIG:%.*]] = prev_dynamic_function_ref @$s23dynamically_replaceable6StructV1xS2i_tcig
// CHECK:   apply [[ORIG]]({{.*}}, [[SELF]]) : $@convention(method) (Int, Struct) -> Int

// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable6StructVyS2icis"] [ossa] @$s23dynamically_replaceable6StructV1xS2i_tcis
// CHECK: bb0({{.*}} : $Int, {{.*}} : $Int, [[SELF:%.*]] : $*Struct):
// CHECK:   [[BA:%.*]] = begin_access [modify] [unknown] [[SELF]] : $*Struct
// CHECK:   [[ORIG:%.*]] = prev_dynamic_function_ref @$s23dynamically_replaceable6StructV1xS2i_tcis
// CHECK:   apply [[ORIG]]({{.*}}, {{.*}}, [[BA]]) : $@convention(method) (Int, Int, @inout Struct) -> ()
// CHECK:   end_access [[BA]] : $*Struct

 @_dynamicReplacement(for: subscript(_:))
 subscript(x y: Int) -> Int {
    get {
      return self[y]
    }
    set {
      self[y] = newValue
    }
  }
}


struct GenericS<T> {
  dynamic init(x: Int) {
  }
  dynamic func dynamic_replaceable() {
  }

  dynamic var dynamic_replaceable_var : Int {
    get {
      return 10
    }
    set {
    }
  }

  dynamic subscript(x : Int) -> Int {
    get {
      return 10
    }
    set {
    }
  }

// CHECK-LABEL: sil private [dynamically_replacable] [ossa] @$s23dynamically_replaceable8GenericSV22property_with_observerSivW
// CHECK-LABEL: sil private [dynamically_replacable] [ossa] @$s23dynamically_replaceable8GenericSV22property_with_observerSivw
  dynamic var property_with_observer : Int {
    didSet {
    }
    willSet {
    }
  }

}

extension GenericS {

// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable8GenericSV08dynamic_B0yyF"] [ossa] @$s23dynamically_replaceable8GenericSV11replacementyyF
// CHECK: prev_dynamic_function_ref @$s23dynamically_replaceable8GenericSV11replacementyyF
  @_dynamicReplacement(for: dynamic_replaceable())
  func replacement() {
    dynamic_replaceable()
  }
// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable8GenericSV1xACyxGSi_tcfC"] [ossa] @$s23dynamically_replaceable8GenericSV1yACyxGSi_tcfC
// CHECK: prev_dynamic_function_ref @$s23dynamically_replaceable8GenericSV1yACyxGSi_tcfC
  @_dynamicReplacement(for: init(x:))
  init(y: Int) {
    self.init(x: y + 1)
  }

// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable8GenericSV08dynamic_B4_varSivg"] [ossa] @$s23dynamically_replaceable8GenericSV1rSivg
// CHECK: prev_dynamic_function_ref @$s23dynamically_replaceable8GenericSV1rSivg

// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable8GenericSV08dynamic_B4_varSivs"] [ossa] @$s23dynamically_replaceable8GenericSV1rSivs
// CHECK: prev_dynamic_function_ref @$s23dynamically_replaceable8GenericSV1rSivs
  @_dynamicReplacement(for: dynamic_replaceable_var)
  var r : Int {
    get {
      return dynamic_replaceable_var + 1
    }
    set {
      dynamic_replaceable_var = newValue + 1
    }
  }

// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable8GenericSVyS2icig"] [ossa] @$s23dynamically_replaceable8GenericSV1xS2i_tcig
// CHECK: prev_dynamic_function_ref @$s23dynamically_replaceable8GenericSV1xS2i_tcig

// CHECK-LABEL: sil hidden [dynamic_replacement_for "$s23dynamically_replaceable8GenericSVyS2icis"] [ossa] @$s23dynamically_replaceable8GenericSV1xS2i_tcis
// CHECK: prev_dynamic_function_ref @$s23dynamically_replaceable8GenericSV1xS2i_tcis
 @_dynamicReplacement(for: subscript(_:))
 subscript(x y: Int) -> Int {
    get {
      return self[y]
    }
    set {
      self[y] = newValue
    }
  }

// CHECK-LABEL: sil private [dynamic_replacement_for "$s23dynamically_replaceable8GenericSV22property_with_observerSivW"] [ossa] @$s23dynamically_replaceable8GenericSV34replacement_property_with_observerSivW
// CHECK-LABEL: sil private [dynamic_replacement_for "$s23dynamically_replaceable8GenericSV22property_with_observerSivw"] [ossa] @$s23dynamically_replaceable8GenericSV34replacement_property_with_observerSivw
  @_dynamicReplacement(for: property_with_observer)
  var replacement_property_with_observer : Int {
    didSet {
    }
    willSet {
    }
  }
}

dynamic var globalX = 0
// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable7globalXSivg : $@convention(thin) () -> Int
// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable7globalXSivs : $@convention(thin) (Int) -> ()
// CHECK-LABEL: sil hidden [ossa] @$s23dynamically_replaceable7getsetXyS2iF
// CHECK: dynamic_function_ref @$s23dynamically_replaceable7globalXSivs
// CHECK: dynamic_function_ref @$s23dynamically_replaceable7globalXSivg
func getsetX(_ x: Int) -> Int {
  globalX = x
  return globalX
}

// CHECK-LABEL: sil hidden [ossa] @$s23dynamically_replaceable18funcWithDefaultArgyySSFfA_
// CHECK-LABEL: sil hidden [dynamically_replacable] [ossa] @$s23dynamically_replaceable18funcWithDefaultArgyySSF
dynamic func funcWithDefaultArg(_ arg : String = String("hello")) {
  print("hello")
}

// IMPLICIT-LABEL: sil hidden [thunk] [ossa] @barfoo
@_cdecl("barfoo")
func foobar() {
}

// IMPLICIT-LABEL: sil [dynamically_replacable] [ossa] @$s23dynamically_replaceable16testWithLocalFunyyF
// IMPLICIT-LABEL: sil private [ossa] @$s23dynamically_replaceable16testWithLocalFunyyF05localF0L_yyF
// IMPLICIT-LABEL: sil private [ossa] @$s23dynamically_replaceable16testWithLocalFunyyF05localF0L_yyF0geF0L_yyF
// IMPLICIT-LABEL: sil private [ossa] @$s23dynamically_replaceable16testWithLocalFunyyFyycfU_
public func testWithLocalFun() {
  func localFun() {
    func localLocalFun() { print("bar") }
    print("foo")
    localLocalFun()
  }
  localFun()
  let unamedClosure = { print("foo") }
  unamedClosure()
}

@propertyWrapper
struct WrapperWithInitialValue<T> {
  var wrappedValue: T

  init(wrappedValue initialValue: T) {
    self.wrappedValue = initialValue
  }
}

// CHECK-NOT: sil hidden [ossa] @$s23dynamically_replaceable10SomeStructV1tSbvpfP
public struct SomeStruct {
  @WrapperWithInitialValue var t = false
}

// Make sure that declaring the replacement before the original does not assert.
@_dynamicReplacement(for: orig)
func opaqueReplacement() -> Int {
  return 2
}

dynamic func orig() -> Int {
  return 1
}
