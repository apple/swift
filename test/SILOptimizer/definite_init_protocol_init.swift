// RUN: %target-swift-frontend -emit-sil %s | %FileCheck %s

// Ensure that convenience initializers on concrete types can
// delegate to factory initializers defined in protocol
// extensions.

protocol TriviallyConstructible {
  init(lower: Int)
}

extension TriviallyConstructible {
  init(middle: Int) {
    self.init(lower: middle)
  }

  init?(failingMiddle: Int) {
    self.init(lower: failingMiddle)
  }

  init(throwingMiddle: Int) throws {
    try self.init(lower: throwingMiddle)
  }
}

class TrivialClass : TriviallyConstructible {
  required init(lower: Int) {}

  // CHECK-LABEL: sil hidden @_TFC27definite_init_protocol_init12TrivialClasscfT5upperSi_S0_
  // CHECK:     bb0(%0 : $Int, %1 : $TrivialClass):
  // CHECK-NEXT:  [[SELF_BOX:%.*]] = alloc_stack $TrivialClass
  // CHECK:       store %1 to [[SELF_BOX]]
  // CHECK-NEXT:  [[METATYPE:%.*]] = value_metatype $@thick TrivialClass.Type, %1
  // CHECK:       [[FN:%.*]] = function_ref @_TFE27definite_init_protocol_initPS_22TriviallyConstructibleCfT6middleSi_x
  // CHECK-NEXT:  [[RESULT:%.*]] = alloc_stack $TrivialClass
  // CHECK-NEXT:  apply [[FN]]<TrivialClass>([[RESULT]], %0, [[METATYPE]])
  // CHECK-NEXT:  [[NEW_SELF:%.*]] = load [[RESULT]]
  // CHECK-NEXT:  store [[NEW_SELF]] to [[SELF_BOX]]
  // CHECK-NEXT:  [[METATYPE:%.*]] = value_metatype $@thick TrivialClass.Type, %1
  // CHECK-NEXT:  dealloc_partial_ref %1 : $TrivialClass, [[METATYPE]] : $@thick TrivialClass.Type
  // CHECK-NEXT:  dealloc_stack [[RESULT]]
  // CHECK-NEXT:  dealloc_stack [[SELF_BOX]]
  // CHECK-NEXT:  return [[NEW_SELF]]
  convenience init(upper: Int) {
    self.init(middle: upper)
  }

  convenience init?(failingUpper: Int) {
    self.init(failingMiddle: failingUpper)
  }

  convenience init(throwingUpper: Int) throws {
    try self.init(throwingMiddle: throwingUpper)
  }
}

struct TrivialStruct : TriviallyConstructible {
  let x: Int

  init(lower: Int) { self.x = lower }

// CHECK-LABEL: sil hidden @_TFV27definite_init_protocol_init13TrivialStructCfT5upperSi_S0_
// CHECK:     bb0(%0 : $Int, %1 : $@thin TrivialStruct.Type):
// CHECK-NEXT: [[SELF:%.*]] = alloc_stack $TrivialStruct
// CHECK:      [[FN:%.*]] = function_ref @_TFE27definite_init_protocol_initPS_22TriviallyConstructibleCfT6middleSi_x
// CHECK-NEXT: [[METATYPE:%.*]] = metatype $@thick TrivialStruct.Type
// CHECK-NEXT: [[SELF_BOX:%.*]] = alloc_stack $TrivialStruct
// CHECK-NEXT: apply [[FN]]<TrivialStruct>([[SELF_BOX]], %0, [[METATYPE]])
// CHECK-NEXT: [[NEW_SELF:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT: store [[NEW_SELF]] to [[SELF]]
// CHECK-NEXT: dealloc_stack [[SELF_BOX]]
// CHECK-NEXT: dealloc_stack [[SELF]]
// CHECK-NEXT: return [[NEW_SELF]]
  init(upper: Int) {
    self.init(middle: upper)
  }

  init?(failingUpper: Int) {
    self.init(failingMiddle: failingUpper)
  }

  init(throwingUpper: Int) throws {
    try self.init(throwingMiddle: throwingUpper)
  }
}

struct AddressOnlyStruct : TriviallyConstructible {
  let x: Any

  init(lower: Int) { self.x = lower }

// CHECK-LABEL: sil hidden @_TFV27definite_init_protocol_init17AddressOnlyStructCfT5upperSi_S0_
// CHECK:     bb0(%0 : $*AddressOnlyStruct, %1 : $Int, %2 : $@thin AddressOnlyStruct.Type):
// CHECK-NEXT: [[SELF:%.*]] = alloc_stack $AddressOnlyStruct
// CHECK:      [[FN:%.*]] = function_ref @_TFE27definite_init_protocol_initPS_22TriviallyConstructibleCfT6middleSi_x
// CHECK-NEXT: [[METATYPE:%.*]] = metatype $@thick AddressOnlyStruct.Type
// CHECK-NEXT: [[SELF_BOX:%.*]] = alloc_stack $AddressOnlyStruct
// CHECK-NEXT: apply [[FN]]<AddressOnlyStruct>([[SELF_BOX]], %1, [[METATYPE]])
// CHECK-NEXT: copy_addr [take] [[SELF_BOX]] to [initialization] [[SELF]]
// CHECK-NEXT: dealloc_stack [[SELF_BOX]]
// CHECK-NEXT: copy_addr [take] [[SELF]] to [initialization] %0
// CHECK-NEXT: [[RESULT:%.*]] = tuple ()
// CHECK-NEXT: dealloc_stack [[SELF]]
// CHECK-NEXT: return [[RESULT]]
  init(upper: Int) {
    self.init(middle: upper)
  }

  init?(failingUpper: Int) {
    self.init(failingMiddle: failingUpper)
  }

  init(throwingUpper: Int) throws {
    try self.init(throwingMiddle: throwingUpper)
  }
}

enum TrivialEnum : TriviallyConstructible {
  case NotSoTrivial

  init(lower: Int) {
    self = .NotSoTrivial
  }

// CHECK-LABEL: sil hidden @_TFO27definite_init_protocol_init11TrivialEnumCfT5upperSi_S0_
// CHECK:     bb0(%0 : $Int, %1 : $@thin TrivialEnum.Type):
// CHECK-NEXT: [[SELF:%.*]] = alloc_stack $TrivialEnum
// CHECK:      [[FN:%.*]] = function_ref @_TFE27definite_init_protocol_initPS_22TriviallyConstructibleCfT6middleSi_x
// CHECK-NEXT: [[METATYPE:%.*]] = metatype $@thick TrivialEnum.Type
// CHECK-NEXT: [[SELF_BOX:%.*]] = alloc_stack $TrivialEnum
// CHECK-NEXT: apply [[FN]]<TrivialEnum>([[SELF_BOX]], %0, [[METATYPE]])
// CHECK-NEXT: [[NEW_SELF:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT: store [[NEW_SELF]] to [[SELF]]
// CHECK-NEXT: dealloc_stack [[SELF_BOX]]
// CHECK-NEXT: dealloc_stack [[SELF]]
// CHECK-NEXT: return [[NEW_SELF]]
  init(upper: Int) {
    self.init(middle: upper)
  }

  init?(failingUpper: Int) {
    self.init(failingMiddle: failingUpper)
  }

  init(throwingUpper: Int) throws {
    try self.init(throwingMiddle: throwingUpper)
  }
}
