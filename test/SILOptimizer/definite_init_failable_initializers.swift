// RUN: %target-swift-frontend -emit-sil -enable-copy-propagation=false %s | %FileCheck %s

// REQUIRES: swift_in_compiler

// Using -enable-copy-propagation=false to pattern match against older SIL
// output. At least until -enable-copy-propagation has been around
// long enough in the same form to be worth rewriting CHECK lines.

// High-level tests that DI handles early returns from failable and throwing
// initializers properly. The main complication is conditional release of self
// and stored properties.

// FIXME: not all of the test cases have CHECKs. Hopefully the interesting cases
// are fully covered, though.

////
// Structs with failable initializers
////

protocol Pachyderm {
  init()
}

class Canary : Pachyderm {
  required init() {}
}

// <rdar://problem/20941576> SILGen crash: Failable struct init cannot delegate to another failable initializer
struct TrivialFailableStruct {
  init?(blah: ()) { }
  init?(wibble: ()) {
    self.init(blah: wibble)
  }
}

struct FailableStruct {
  let x, y: Canary

  init(noFail: ()) {
    x = Canary()
    y = Canary()
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers14FailableStructV24failBeforeInitializationACSgyt_tcfC
// CHECK:       bb0(%0 : $@thin FailableStruct.Type):
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack $FailableStruct
// CHECK:         dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    [[SELF:%.*]] = enum $Optional<FailableStruct>, #Optional.none!enumelt
// CHECK-NEXT:    return [[SELF]]
  init?(failBeforeInitialization: ()) {
    return nil
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers14FailableStructV30failAfterPartialInitializationACSgyt_tcfC
// CHECK:       bb0(%0 : $@thin FailableStruct.Type):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $FailableStruct
// CHECK:         [[CANARY:%.*]] = apply
// CHECK-NEXT:    [[WRITE:%.*]] = begin_access [modify] [static] [[SELF_BOX]] : $*FailableStruct
// CHECK-NEXT:    [[X_ADDR:%.*]] = struct_element_addr [[WRITE]]
// CHECK-NEXT:    store [[CANARY]] to [[X_ADDR]]
// CHECK-NEXT:    end_access [[WRITE]] : $*FailableStruct
// CHECK-NEXT:    [[X_ADDR:%.*]] = struct_element_addr [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[X_ADDR]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    [[SELF:%.*]] = enum $Optional<FailableStruct>, #Optional.none!enumelt
// CHECK-NEXT:    return [[SELF]]
  init?(failAfterPartialInitialization: ()) {
    x = Canary()
    return nil
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers14FailableStructV27failAfterFullInitializationACSgyt_tcfC
// CHECK:       bb0
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack $FailableStruct
// CHECK:         [[CANARY1:%.*]] = apply
// CHECK-NEXT:    [[WRITE:%.*]] = begin_access [modify] [static] [[SELF_BOX]] : $*FailableStruct
// CHECK-NEXT:    [[X_ADDR:%.*]] = struct_element_addr [[WRITE]]
// CHECK-NEXT:    store [[CANARY1]] to [[X_ADDR]]
// CHECK-NEXT:    end_access [[WRITE]] : $*FailableStruct
// CHECK:         [[CANARY2:%.*]] = apply
// CHECK-NEXT:    [[WRITE:%.*]] = begin_access [modify] [static] [[SELF_BOX]] : $*FailableStruct
// CHECK-NEXT:    [[Y_ADDR:%.*]] = struct_element_addr [[WRITE]]
// CHECK-NEXT:    store [[CANARY2]] to [[Y_ADDR]]
// CHECK-NEXT:    end_access [[WRITE]] : $*FailableStruct
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    [[NEW_SELF:%.*]] = enum $Optional<FailableStruct>, #Optional.none!enumelt
// CHECK-NEXT:    return [[NEW_SELF]]
  init?(failAfterFullInitialization: ()) {
    x = Canary()
    y = Canary()
    return nil
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers14FailableStructV46failAfterWholeObjectInitializationByAssignmentACSgyt_tcfC :
// CHECK:       bb0
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack $FailableStruct
// CHECK:         [[CANARY:%.*]] = apply
// CHECK-NEXT:    [[WRITE:%.*]] = begin_access [modify] [static] [[SELF_BOX]] : $*FailableStruct
// CHECK-NEXT:    store [[CANARY]] to [[WRITE]]
// CHECK-NEXT:    end_access [[WRITE]] : $*FailableStruct
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    [[SELF_VALUE:%.*]] = enum $Optional<FailableStruct>, #Optional.none!enumelt
// CHECK-NEXT:    return [[SELF_VALUE]]
  init?(failAfterWholeObjectInitializationByAssignment: ()) {
    self = FailableStruct(noFail: ())
    return nil
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers14FailableStructV46failAfterWholeObjectInitializationByDelegationACSgyt_tcfC
// CHECK:       bb0
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack $FailableStruct
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers14FailableStructV6noFailACyt_tcfC
// CHECK-NEXT:    [[NEW_SELF:%.*]] = apply [[INIT_FN]](%0)
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    [[NEW_SELF:%.*]] = enum $Optional<FailableStruct>, #Optional.none!enumelt
// CHECK-NEXT:    return [[NEW_SELF]]
  init?(failAfterWholeObjectInitializationByDelegation: ()) {
    self.init(noFail: ())
    return nil
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers14FailableStructV20failDuringDelegationACSgyt_tcfC
// CHECK:       bb0
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack $FailableStruct
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers14FailableStructV24failBeforeInitializationACSgyt_tcfC
// CHECK-NEXT:    [[SELF_OPTIONAL:%.*]] = apply [[INIT_FN]](%0)
// CHECK:         [[COND:%.*]] = select_enum [[SELF_OPTIONAL]]
// CHECK-NEXT:    cond_br [[COND]], [[SUCC_BB:bb[0-9]+]], [[FAIL_BB:bb[0-9]+]]
//
// CHECK:       [[FAIL_BB]]:
// CHECK-NEXT:    release_value [[SELF_OPTIONAL]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    [[NEW_SELF:%.*]] = enum $Optional<FailableStruct>, #Optional.none!enumelt
// CHECK-NEXT:    br [[EPILOG_BB:bb[0-9]+]]([[NEW_SELF]] : $Optional<FailableStruct>)
//
// CHECK:       [[SUCC_BB]]:
// CHECK-NEXT:    [[SELF_VALUE:%.*]] = unchecked_enum_data [[SELF_OPTIONAL]]
// CHECK-NEXT:    retain_value [[SELF_VALUE]]
// CHECK-NEXT:    store [[SELF_VALUE]] to [[SELF_BOX]]
// CHECK-NEXT:    [[NEW_SELF:%.*]] = enum $Optional<FailableStruct>, #Optional.some!enumelt, [[SELF_VALUE]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    br [[EPILOG_BB:bb[0-9]+]]([[NEW_SELF]] : $Optional<FailableStruct>)
//
// CHECK:       [[EPILOG_BB]]([[NEW_SELF:%.*]] : $Optional<FailableStruct>)
// CHECK-NEXT:    return [[NEW_SELF]]
  // Optional to optional
  init?(failDuringDelegation: ()) {
    self.init(failBeforeInitialization: ())
  }

  // IUO to optional
  init!(failDuringDelegation2: ()) {
    self.init(failBeforeInitialization: ())! // unnecessary-but-correct '!'
  }

  // IUO to IUO
  init!(failDuringDelegation3: ()) {
    self.init(failDuringDelegation2: ())! // unnecessary-but-correct '!'
  }

  // non-optional to optional
  init(failDuringDelegation4: ()) {
    self.init(failBeforeInitialization: ())! // necessary '!'
  }

  // non-optional to IUO
  init(failDuringDelegation5: ()) {
    self.init(failDuringDelegation2: ())! // unnecessary-but-correct '!'
  }
}

extension FailableStruct {
  init?(failInExtension: ()) {
    self.init(failInExtension: failInExtension)
  }

  init?(assignInExtension: ()) {
    self = FailableStruct(noFail: ())
  }
}

struct FailableAddrOnlyStruct<T : Pachyderm> {
  var x, y: T

  init(noFail: ()) {
    x = T()
    y = T()
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers22FailableAddrOnlyStructV{{[_0-9a-zA-Z]*}}failBeforeInitialization{{.*}}tcfC
// CHECK:       bb0(%0 : $*Optional<FailableAddrOnlyStruct<T>>, %1 : $@thin FailableAddrOnlyStruct<T>.Type):
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack $FailableAddrOnlyStruct<T>
// CHECK:         dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    inject_enum_addr %0
// CHECK:         return
  init?(failBeforeInitialization: ()) {
    return nil
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers22FailableAddrOnlyStructV{{[_0-9a-zA-Z]*}}failAfterPartialInitialization{{.*}}tcfC
// CHECK:       bb0(%0 : $*Optional<FailableAddrOnlyStruct<T>>, %1 : $@thin FailableAddrOnlyStruct<T>.Type):
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack $FailableAddrOnlyStruct<T>
// CHECK:         [[X_BOX:%.*]] = alloc_stack $T
// CHECK-NEXT:    [[T_TYPE:%.*]] = metatype $@thick T.Type
// CHECK:         [[T_INIT_FN:%.*]] = witness_method $T, #Pachyderm.init!allocator
// CHECK-NEXT:    apply [[T_INIT_FN]]<T>([[X_BOX]], [[T_TYPE]])
// CHECK-NEXT:    [[WRITE:%.*]] = begin_access [modify] [static] [[SELF_BOX]] : $*FailableAddrOnlyStruct<T>
// CHECK-NEXT:    [[X_ADDR:%.*]] = struct_element_addr [[WRITE]]
// CHECK-NEXT:    copy_addr [take] [[X_BOX]] to [init] [[X_ADDR]]
// CHECK-NEXT:    end_access [[WRITE]] : $*FailableAddrOnlyStruct<T>
// CHECK-NEXT:    dealloc_stack [[X_BOX]]
// CHECK-NEXT:    [[X_ADDR:%.*]] = struct_element_addr [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[X_ADDR]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    inject_enum_addr %0
// CHECK:         return
  init?(failAfterPartialInitialization: ()) {
    x = T()
    return nil
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers22FailableAddrOnlyStructV{{[_0-9a-zA-Z]*}}failAfterFullInitialization{{.*}}tcfC
// CHECK:       bb0(%0 : $*Optional<FailableAddrOnlyStruct<T>>, %1 : $@thin FailableAddrOnlyStruct<T>.Type):
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack $FailableAddrOnlyStruct<T>
// CHECK:         [[X_BOX:%.*]] = alloc_stack $T
// CHECK-NEXT:    [[T_TYPE:%.*]] = metatype $@thick T.Type
// CHECK:         [[T_INIT_FN:%.*]] = witness_method $T, #Pachyderm.init!allocator
// CHECK-NEXT:    apply [[T_INIT_FN]]<T>([[X_BOX]], [[T_TYPE]])
// CHECK-NEXT:    [[WRITE:%.*]] = begin_access [modify] [static] [[SELF_BOX]] : $*FailableAddrOnlyStruct<T>
// CHECK-NEXT:    [[X_ADDR:%.*]] = struct_element_addr [[WRITE]]
// CHECK-NEXT:    copy_addr [take] [[X_BOX]] to [init] [[X_ADDR]]
// CHECK-NEXT:    end_access [[WRITE]] : $*FailableAddrOnlyStruct<T>
// CHECK-NEXT:    dealloc_stack [[X_BOX]]
// CHECK-NEXT:    [[Y_BOX:%.*]] = alloc_stack $T
// CHECK-NEXT:    [[T_TYPE:%.*]] = metatype $@thick T.Type
// CHECK-NEXT:    [[T_INIT_FN:%.*]] = witness_method $T, #Pachyderm.init!allocator
// CHECK-NEXT:    apply [[T_INIT_FN]]<T>([[Y_BOX]], [[T_TYPE]])
// CHECK-NEXT:    [[WRITE:%.*]] = begin_access [modify] [static] [[SELF_BOX]] : $*FailableAddrOnlyStruct<T>
// CHECK-NEXT:    [[Y_ADDR:%.*]] = struct_element_addr [[WRITE]]
// CHECK-NEXT:    copy_addr [take] [[Y_BOX]] to [init] [[Y_ADDR]]
// CHECK-NEXT:    end_access [[WRITE]] : $*FailableAddrOnlyStruct<T>
// CHECK-NEXT:    dealloc_stack [[Y_BOX]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    inject_enum_addr %0
// CHECK:         return
  init?(failAfterFullInitialization: ()) {
    x = T()
    y = T()
    return nil
  }

  init?(failAfterWholeObjectInitializationByAssignment: ()) {
    self = FailableAddrOnlyStruct(noFail: ())
    return nil
  }

  init?(failAfterWholeObjectInitializationByDelegation: ()) {
    self.init(noFail: ())
    return nil
  }

  // Optional to optional
  init?(failDuringDelegation: ()) {
    self.init(failBeforeInitialization: ())
  }

  // IUO to optional
  init!(failDuringDelegation2: ()) {
    self.init(failBeforeInitialization: ())! // unnecessary-but-correct '!'
  }

  // non-optional to optional
  init(failDuringDelegation3: ()) {
    self.init(failBeforeInitialization: ())! // necessary '!'
  }

  // non-optional to IUO
  init(failDuringDelegation4: ()) {
    self.init(failDuringDelegation2: ())! // unnecessary-but-correct '!'
  }
}

extension FailableAddrOnlyStruct {
  init?(failInExtension: ()) {
    self.init(failBeforeInitialization: failInExtension)
  }

  init?(assignInExtension: ()) {
    self = FailableAddrOnlyStruct(noFail: ())
  }
}

////
// Structs with throwing initializers
////

func unwrap(_ x: Int) throws -> Int { return x }

struct ThrowStruct {
  var x: Canary

  init(fail: ()) throws { x = Canary() }

  init(noFail: ()) { x = Canary() }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers11ThrowStructV20failBeforeDelegationACSi_tKcfC
// CHECK:       bb0(%0 : $Int, %1 : $@thin ThrowStruct.Type):
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack $ThrowStruct
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[RESULT:%.*]] : $Int):
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers11ThrowStructV6noFailACyt_tcfC
// CHECK-NEXT:    [[NEW_SELF:%.*]] = apply [[INIT_FN]](%1)
// CHECK-NEXT:    retain_value [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb2([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  init(failBeforeDelegation: Int) throws {
    try unwrap(failBeforeDelegation)
    self.init(noFail: ())
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers11ThrowStructV28failBeforeOrDuringDelegationACSi_tKcfC
// CHECK:       bb0(%0 : $Int, %1 : $@thin ThrowStruct.Type):
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack $ThrowStruct
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[RESULT:%.*]] : $Int):
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers11ThrowStructV4failACyt_tKcfC
// CHECK-NEXT:    try_apply [[INIT_FN]](%1)
// CHECK:       bb2([[NEW_SELF:%.*]] : $ThrowStruct):
// CHECK-NEXT:    retain_value [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb3([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb4([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb5([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  init(failBeforeOrDuringDelegation: Int) throws {
    try unwrap(failBeforeOrDuringDelegation)
    try self.init(fail: ())
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers11ThrowStructV29failBeforeOrDuringDelegation2ACSi_tKcfC
// CHECK:       bb0(%0 : $Int, %1 : $@thin ThrowStruct.Type):
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack $ThrowStruct
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[RESULT:%.*]] : $Int):
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers11ThrowStructV20failBeforeDelegationACSi_tKcfC
// CHECK-NEXT:    try_apply [[INIT_FN]]([[RESULT]], %1)
// CHECK:       bb2([[NEW_SELF:%.*]] : $ThrowStruct):
// CHECK-NEXT:    retain_value [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb3([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb4([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb5([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  init(failBeforeOrDuringDelegation2: Int) throws {
    try self.init(failBeforeDelegation: unwrap(failBeforeOrDuringDelegation2))
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers11ThrowStructV20failDuringDelegationACSi_tKcfC
// CHECK:       bb0(%0 : $Int, %1 : $@thin ThrowStruct.Type):
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack $ThrowStruct
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers11ThrowStructV4failACyt_tKcfC
// CHECK-NEXT:    try_apply [[INIT_FN]](%1)
// CHECK:       bb1([[NEW_SELF:%.*]] : $ThrowStruct):
// CHECK-NEXT:    retain_value [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb2([[ERROR:%.*]] : $any Error):
// CHECK:         dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  init(failDuringDelegation: Int) throws {
    try self.init(fail: ())
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers11ThrowStructV19failAfterDelegationACSi_tKcfC
// CHECK:       bb0(%0 : $Int, %1 : $@thin ThrowStruct.Type):
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack $ThrowStruct
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers11ThrowStructV6noFailACyt_tcfC
// CHECK-NEXT:    [[NEW_SELF:%.*]] = apply [[INIT_FN]](%1)
// CHECK-NEXT:    retain_value [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[RESULT:%.*]] : $Int):
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb2([[ERROR:%.*]] : $any Error):
// CHECK:         destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  init(failAfterDelegation: Int) throws {
    self.init(noFail: ())
    try unwrap(failAfterDelegation)
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers11ThrowStructV27failDuringOrAfterDelegationACSi_tKcfC
// CHECK:       bb0(%0 : $Int, %1 : $@thin ThrowStruct.Type):
// CHECK-NEXT:    [[BITMAP_BOX:%.*]] = alloc_stack $Builtin.Int1
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack [dynamic_lifetime] $ThrowStruct
// CHECK-NEXT:    [[ZERO:%.*]] = integer_literal $Builtin.Int1, 0
// CHECK-NEXT:    store [[ZERO]] to [[BITMAP_BOX]]
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers11ThrowStructV4failACyt_tKcfC
// CHECK-NEXT:    try_apply [[INIT_FN]](%1)
// CHECK:       bb1([[NEW_SELF:.*]] : $ThrowStruct):
// CHECK-NEXT:    [[BIT:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK-NEXT:    store [[BIT]] to [[BITMAP_BOX]]
// CHECK-NEXT:    retain_value [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb2([[RESULT:%.*]] : $Int):
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb3([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb4([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    release_value [[NEW_SELF]]
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb5([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    [[COND:%.*]] = load [[BITMAP_BOX]]
// CHECK-NEXT:    cond_br [[COND]], bb6, bb7
// CHECK:       bb6:
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    br bb8
// CHECK:       bb7:
// CHECK-NEXT:    br bb8
// CHECK:       bb8:
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  init(failDuringOrAfterDelegation: Int) throws {
    try self.init(fail: ())
    try unwrap(failDuringOrAfterDelegation)
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers11ThrowStructV27failBeforeOrAfterDelegationACSi_tKcfC
// CHECK:       bb0(%0 : $Int, %1 : $@thin ThrowStruct.Type):
// CHECK-NEXT:    [[BITMAP_BOX:%.*]] = alloc_stack $Builtin.Int1
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack [dynamic_lifetime] $ThrowStruct
// CHECK-NEXT:    [[ZERO:%.*]] = integer_literal $Builtin.Int1, 0
// CHECK-NEXT:    store [[ZERO]] to [[BITMAP_BOX]]
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[RESULT:%.*]] : $Int):
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers11ThrowStructV6noFailACyt_tcfC
// CHECK-NEXT:    [[NEW_SELF:%.*]] = apply [[INIT_FN]](%1)
// CHECK-NEXT:    [[BIT:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK-NEXT:    store [[BIT]] to [[BITMAP_BOX]]
// CHECK-NEXT:    retain_value [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb2([[RESULT:%.*]] : $Int):
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb3([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb4([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    release_value [[NEW_SELF]]
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb5([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    [[COND:%.*]] = load [[BITMAP_BOX]]
// CHECK-NEXT:    cond_br [[COND]], bb6, bb7
// CHECK:       bb6:
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    br bb8
// CHECK:       bb7:
// CHECK-NEXT:    br bb8
// CHECK:       bb8:
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  init(failBeforeOrAfterDelegation: Int) throws {
    try unwrap(failBeforeOrAfterDelegation)
    self.init(noFail: ())
    try unwrap(failBeforeOrAfterDelegation)
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers11ThrowStructV16throwsToOptionalACSgSi_tcfC
// CHECK:       bb0([[ARG1:%.*]] : $Int, [[ARG2:%.*]] : $@thin ThrowStruct.Type):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $ThrowStruct
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers11ThrowStructV20failDuringDelegationACSi_tKcfC
// CHECK-NEXT:    try_apply [[INIT_FN]]([[ARG1]], [[ARG2]]) : $@convention(method) (Int, @thin ThrowStruct.Type) -> (@owned ThrowStruct, @error any Error), normal [[TRY_APPLY_SUCC_BB:bb[0-9]+]], error [[TRY_APPLY_FAIL_BB:bb[0-9]+]]
//
// CHECK:       [[TRY_APPLY_SUCC_BB]]([[NEW_SELF:%.*]] : $ThrowStruct):
// CHECK-NEXT:    [[SELF_OPTIONAL:%.*]] = enum $Optional<ThrowStruct>, #Optional.some!enumelt, [[NEW_SELF]]
// CHECK-NEXT:    br [[TRY_APPLY_CONT:bb[0-9]+]]([[SELF_OPTIONAL]] : $Optional<ThrowStruct>)
//
// CHECK:       [[TRY_APPLY_CONT]]([[SELF_OPTIONAL:%.*]] : $Optional<ThrowStruct>):
// CHECK:         [[COND:%.*]] = select_enum [[SELF_OPTIONAL]]
// CHECK-NEXT:    cond_br [[COND]], [[SUCC_BB:bb[0-9]+]], [[FAIL_BB:bb[0-9]+]]
//
// CHECK:       [[FAIL_BB]]:
// CHECK:         release_value [[SELF_OPTIONAL]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    [[NEW_SELF:%.*]] = enum $Optional<ThrowStruct>, #Optional.none!enumelt
// CHECK-NEXT:    br [[EPILOG_BB:bb[0-9]+]]([[NEW_SELF]] : $Optional<ThrowStruct>)
//
// CHECK:       [[SUCC_BB]]:
// CHECK-NEXT:    [[SELF_VALUE:%.*]] = unchecked_enum_data [[SELF_OPTIONAL]]
// CHECK-NEXT:    retain_value [[SELF_VALUE]]
// CHECK-NEXT:    store [[SELF_VALUE]] to [[SELF_BOX]]
// CHECK-NEXT:    [[NEW_SELF:%.*]] = enum $Optional<ThrowStruct>, #Optional.some!enumelt, [[SELF_VALUE]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    br [[EPILOG_BB:bb[0-9]+]]([[NEW_SELF]] : $Optional<ThrowStruct>)
//
// CHECK:       [[EPILOG_BB]]([[NEW_SELF:%.*]] : $Optional<ThrowStruct>):
// CHECK-NEXT:    return [[NEW_SELF]] : $Optional<ThrowStruct>
//
// CHECK:       [[TRY_APPLY_FAIL_BB]]([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    strong_release [[ERROR]] : $any Error
// CHECK-NEXT:    [[NEW_SELF:%.*]] = enum $Optional<ThrowStruct>, #Optional.none!enumelt
// CHECK-NEXT:    br [[TRY_APPLY_CONT]]([[NEW_SELF]] : $Optional<ThrowStruct>)
  init?(throwsToOptional: Int) {
    try? self.init(failDuringDelegation: throwsToOptional)
  }

  init(throwsToIUO: Int) {
    try! self.init(failDuringDelegation: throwsToIUO)
  }

  init?(throwsToOptionalThrows: Int) throws {
    try? self.init(fail: ())
  }

  init(throwsOptionalToThrows: Int) throws {
    self.init(throwsToOptional: throwsOptionalToThrows)!
  }

  init?(throwsOptionalToOptional: Int) {
    try! self.init(throwsToOptionalThrows: throwsOptionalToOptional)
  }

  init(failBeforeSelfReplacement: Int) throws {
    try unwrap(failBeforeSelfReplacement)
    self = ThrowStruct(noFail: ())
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers11ThrowStructV25failDuringSelfReplacementACSi_tKcfC :
// CHECK:       bb0(%0 : $Int, %1 : $@thin ThrowStruct.Type):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $ThrowStruct
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers11ThrowStructV4failACyt_tKcfC
// CHECK-NEXT:    try_apply [[INIT_FN]](%1)
// CHECK:       bb1([[NEW_SELF:%.*]] : $ThrowStruct):
// CHECK-NEXT:    [[WRITE:%.*]] = begin_access [modify] [static] [[SELF_BOX]] : $*ThrowStruct
// CHECK-NEXT:    retain_value [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[WRITE]]
// CHECK-NEXT:    end_access [[WRITE]] : $*ThrowStruct
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb2([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  init(failDuringSelfReplacement: Int) throws {
    try self = ThrowStruct(fail: ())
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers11ThrowStructV24failAfterSelfReplacementACSi_tKcfC
// CHECK:       bb0(%0 : $Int, %1 : $@thin ThrowStruct.Type):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $ThrowStruct
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers11ThrowStructV6noFailACyt_tcfC
// CHECK-NEXT:    [[NEW_SELF:%.*]] = apply [[INIT_FN]](%1)
// CHECK-NEXT:    [[WRITE:%.*]] = begin_access [modify] [static] [[SELF_BOX]] : $*ThrowStruct
// CHECK-NEXT:    retain_value [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[WRITE]]
// CHECK-NEXT:    end_access [[WRITE]] : $*ThrowStruct
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[RESULT:%.*]] : $Int):
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb2([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    release_value [[NEW_SELF]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  init(failAfterSelfReplacement: Int) throws {
    self = ThrowStruct(noFail: ())
    try unwrap(failAfterSelfReplacement)
  }
}

extension ThrowStruct {
  init(failInExtension: ()) throws {
    try self.init(fail: failInExtension)
  }

  init(assignInExtension: ()) throws {
    try self = ThrowStruct(fail: ())
  }
}

struct ThrowAddrOnlyStruct<T : Pachyderm> {
  var x : T

  init(fail: ()) throws { x = T() }

  init(noFail: ()) { x = T() }

  init(failBeforeDelegation: Int) throws {
    try unwrap(failBeforeDelegation)
    self.init(noFail: ())
  }

  init(failBeforeOrDuringDelegation: Int) throws {
    try unwrap(failBeforeOrDuringDelegation)
    try self.init(fail: ())
  }

  init(failBeforeOrDuringDelegation2: Int) throws {
    try self.init(failBeforeDelegation: unwrap(failBeforeOrDuringDelegation2))
  }

  init(failDuringDelegation: Int) throws {
    try self.init(fail: ())
  }

  init(failAfterDelegation: Int) throws {
    self.init(noFail: ())
    try unwrap(failAfterDelegation)
  }

  init(failDuringOrAfterDelegation: Int) throws {
    try self.init(fail: ())
    try unwrap(failDuringOrAfterDelegation)
  }

  init(failBeforeOrAfterDelegation: Int) throws {
    try unwrap(failBeforeOrAfterDelegation)
    self.init(noFail: ())
    try unwrap(failBeforeOrAfterDelegation)
  }

  init?(throwsToOptional: Int) {
    try? self.init(failDuringDelegation: throwsToOptional)
  }

  init(throwsToIUO: Int) {
    try! self.init(failDuringDelegation: throwsToIUO)
  }

  init?(throwsToOptionalThrows: Int) throws {
    try? self.init(fail: ())
  }

  init(throwsOptionalToThrows: Int) throws {
    self.init(throwsToOptional: throwsOptionalToThrows)!
  }

  init?(throwsOptionalToOptional: Int) {
    try! self.init(throwsOptionalToThrows: throwsOptionalToOptional)
  }

  init(failBeforeSelfReplacement: Int) throws {
    try unwrap(failBeforeSelfReplacement)
    self = ThrowAddrOnlyStruct(noFail: ())
  }

  init(failAfterSelfReplacement: Int) throws {
    self = ThrowAddrOnlyStruct(noFail: ())
    try unwrap(failAfterSelfReplacement)
  }
}

extension ThrowAddrOnlyStruct {
  init(failInExtension: ()) throws {
    try self.init(fail: failInExtension)
  }

  init(assignInExtension: ()) throws {
    self = ThrowAddrOnlyStruct(noFail: ())
  }
}

////
// Classes with failable initializers
////

class FailableBaseClass {
  var member: Canary

  init(noFail: ()) {
    member = Canary()
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17FailableBaseClassC28failBeforeFullInitializationACSgyt_tcfc
// CHECK:       bb0(%0 : $FailableBaseClass):
// CHECK:         [[METATYPE:%.*]] = metatype $@thick FailableBaseClass.Type
// CHECK:         dealloc_partial_ref %0 : $FailableBaseClass, [[METATYPE]]
// CHECK:         [[RESULT:%.*]] = enum $Optional<FailableBaseClass>, #Optional.none!enumelt
// CHECK:         return [[RESULT]]
  init?(failBeforeFullInitialization: ()) {
    return nil
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17FailableBaseClassC27failAfterFullInitializationACSgyt_tcfc
// CHECK:       bb0(%0 : $FailableBaseClass):
// CHECK:         [[CANARY:%.*]] = apply
// CHECK-NEXT:    [[MEMBER_ADDR:%.*]] = ref_element_addr %0
// CHECK-NEXT:    [[WRITE:%.*]] = begin_access [init] [static] [[MEMBER_ADDR]] : $*Canary
// CHECK-NEXT:    store [[CANARY]] to [[WRITE]]
// CHECK-NEXT:    end_access [[WRITE]] : $*Canary
// CHECK-NEXT:    strong_release %0
// CHECK-NEXT:    [[NEW_SELF:%.*]] = enum $Optional<FailableBaseClass>, #Optional.none!enumelt
// CHECK-NEXT:    return [[NEW_SELF]]
  init?(failAfterFullInitialization: ()) {
    member = Canary()
    return nil
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17FailableBaseClassC20failBeforeDelegationACSgyt_tcfC
// CHECK:       bb0(%0 : $@thick FailableBaseClass.Type):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $FailableBaseClass
// CHECK:         dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    [[RESULT:%.*]] = enum $Optional<FailableBaseClass>, #Optional.none!enumelt
// CHECK-NEXT:    return [[RESULT]]
  convenience init?(failBeforeDelegation: ()) {
    return nil
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17FailableBaseClassC19failAfterDelegationACSgyt_tcfC
// CHECK:       bb0(%0 : $@thick FailableBaseClass.Type):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $FailableBaseClass
// CHECK:         [[INIT_FN:%.*]] = class_method %0
// CHECK-NEXT:    [[NEW_SELF:%.*]] = apply [[INIT_FN]](%0)
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    [[RESULT:%.*]] = enum $Optional<FailableBaseClass>, #Optional.none!enumelt
// CHECK-NEXT:    return [[RESULT]]
  convenience init?(failAfterDelegation: ()) {
    self.init(noFail: ())
    return nil
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17FailableBaseClassC20failDuringDelegationACSgyt_tcfC
// CHECK:       bb0(%0 : $@thick FailableBaseClass.Type):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $FailableBaseClass
// CHECK:         [[INIT_FN:%.*]] = class_method %0
// CHECK-NEXT:    [[SELF_OPTIONAL:%.*]] = apply [[INIT_FN]](%0)
// CHECK:         [[COND:%.*]] = select_enum [[SELF_OPTIONAL]]
// CHECK-NEXT:    cond_br [[COND]], [[SUCC_BB:bb[0-9]+]], [[FAIL_BB:bb[0-9]+]]
//
// CHECK:       [[FAIL_BB]]:
// CHECK:         release_value [[SELF_OPTIONAL]]
// CHECK:         dealloc_stack [[SELF_BOX]]
// CHECK:         [[NEW_SELF:%.*]] = enum $Optional<FailableBaseClass>, #Optional.none!enumelt
// CHECK-NEXT:    br [[EPILOG_BB:bb[0-9]+]]([[NEW_SELF]] : $Optional<FailableBaseClass>)
//
// CHECK:       [[SUCC_BB]]:
// CHECK-NEXT:    [[SELF_VALUE:%.*]] = unchecked_enum_data [[SELF_OPTIONAL]]
// CHECK-NEXT:    strong_retain [[SELF_VALUE]]
// CHECK-NEXT:    store [[SELF_VALUE]] to [[SELF_BOX]]
// CHECK-NEXT:    [[NEW_SELF:%.*]] = enum $Optional<FailableBaseClass>, #Optional.some!enumelt, [[SELF_VALUE]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    br [[EPILOG_BB]]([[NEW_SELF]] : $Optional<FailableBaseClass>)
//
// CHECK:       [[EPILOG_BB]]([[NEW_SELF:%.*]] : $Optional<FailableBaseClass>):
// CHECK-NEXT:    return [[NEW_SELF]]
  // Optional to optional
  convenience init?(failDuringDelegation: ()) {
    self.init(failBeforeFullInitialization: ())
  }

  // IUO to optional
  convenience init!(failDuringDelegation2: ()) {
    self.init(failBeforeFullInitialization: ())! // unnecessary-but-correct '!'
  }

  // IUO to IUO
  convenience init!(noFailDuringDelegation: ()) {
    self.init(failDuringDelegation2: ())! // unnecessary-but-correct '!'
  }

  // non-optional to optional
  convenience init(noFailDuringDelegation2: ()) {
    self.init(failBeforeFullInitialization: ())! // necessary '!'
  }
}

extension FailableBaseClass {
  convenience init?(failInExtension: ()) throws {
    self.init(failBeforeFullInitialization: failInExtension)
  }
}

// Chaining to failable initializers in a superclass
class FailableDerivedClass : FailableBaseClass {
  var otherMember: Canary

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers20FailableDerivedClassC27derivedFailBeforeDelegationACSgyt_tcfc
// CHECK:       bb0(%0 : $FailableDerivedClass):
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack $FailableDerivedClass
// CHECK:         store %0 to [[SELF_BOX]]
// CHECK-NEXT:    [[RELOAD_FROM_SELF_BOX:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT:    [[METATYPE:%.*]] = metatype $@thick FailableDerivedClass.Type
// CHECK-NEXT:    dealloc_partial_ref [[RELOAD_FROM_SELF_BOX]] : $FailableDerivedClass, [[METATYPE]] : $@thick FailableDerivedClass.Type
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    [[RESULT:%.*]] = enum $Optional<FailableDerivedClass>, #Optional.none!enumelt
// CHECK-NEXT:    return [[RESULT]]
// CHECK: } // end sil function '$s35definite_init_failable_initializers20FailableDerivedClassC27derivedFailBeforeDelegationACSgyt_tcfc'
  init?(derivedFailBeforeDelegation: ()) {
    return nil
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers20FailableDerivedClassC27derivedFailDuringDelegationACSgyt_tcfc
// CHECK:       bb0([[SELF:%.*]] : $FailableDerivedClass):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $FailableDerivedClass
// CHECK:         store [[SELF]] to [[SELF_BOX]]
// CHECK:         [[CANARY_FUN:%.*]] = function_ref @$s35definite_init_failable_initializers6CanaryCACycfC :
// CHECK:         [[CANARY:%.*]] = apply [[CANARY_FUN]](
// CHECK-NEXT:    [[MEMBER_ADDR:%.*]] = ref_element_addr [[SELF]]
// CHECK-NEXT:    [[WRITE:%.*]] = begin_access [init] [static] [[MEMBER_ADDR]] : $*Canary
// CHECK-NEXT:    store [[CANARY]] to [[WRITE]]
// CHECK-NEXT:    end_access [[WRITE]] : $*Canary
// CHECK-NEXT:    strong_release [[SELF]]
// CHECK-NEXT:    [[RELOAD_SELF:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT:    [[BASE_SELF:%.*]] = upcast [[RELOAD_SELF]]
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers17FailableBaseClassC28failBeforeFullInitializationACSgyt_tcfc
// CHECK-NEXT:    [[SELF_OPTIONAL:%.*]] = apply [[INIT_FN]]([[BASE_SELF]])
// CHECK:         [[COND:%.*]] = select_enum [[SELF_OPTIONAL]]
// CHECK-NEXT:    cond_br [[COND]], [[SUCC_BB:bb[0-9]+]], [[FAIL_BB:bb[0-9]+]]
//
// CHECK:       [[FAIL_BB]]:
// CHECK-NEXT:    release_value [[SELF_OPTIONAL]]
// CHECK:         dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    [[NEW_SELF:%.*]] = enum $Optional<FailableDerivedClass>, #Optional.none!enumelt
// CHECK-NEXT:    br [[EPILOG_BB]]([[NEW_SELF]] : $Optional<FailableDerivedClass>)
//
// CHECK:       [[SUCC_BB]]:
// CHECK-NEXT:    [[BASE_SELF_VALUE:%.*]] = unchecked_enum_data [[SELF_OPTIONAL]]
// CHECK-NEXT:    [[SELF_VALUE:%.*]] = unchecked_ref_cast [[BASE_SELF_VALUE]]
// CHECK-NEXT:    strong_retain [[SELF_VALUE]]
// CHECK-NEXT:    store [[SELF_VALUE]] to [[SELF_BOX]]
// CHECK-NEXT:    [[NEW_SELF:%.*]] = enum $Optional<FailableDerivedClass>, #Optional.some!enumelt, [[SELF_VALUE]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    br [[EPILOG_BB:bb[0-9]+]]([[NEW_SELF]] : $Optional<FailableDerivedClass>)
//
// CHECK:       [[EPILOG_BB]]([[NEW_SELF:%.*]] : $Optional<FailableDerivedClass>):
// CHECK-NEXT:    return [[NEW_SELF]] : $Optional<FailableDerivedClass>
// CHECK: } // end sil function '$s35definite_init_failable_initializers20FailableDerivedClassC27derivedFailDuringDelegationACSgyt_tcfc'
  init?(derivedFailDuringDelegation: ()) {
    self.otherMember = Canary()
    super.init(failBeforeFullInitialization: ())
  }

  init?(derivedFailAfterDelegation: ()) {
    self.otherMember = Canary()
    super.init(noFail: ())
    return nil
  }

  // non-optional to IUO
  init(derivedNoFailDuringDelegation: ()) {
    self.otherMember = Canary()
    super.init(failAfterFullInitialization: ())! // necessary '!'
  }

  // IUO to IUO
  init!(derivedFailDuringDelegation2: ()) {
    self.otherMember = Canary()
    super.init(failAfterFullInitialization: ())! // unnecessary-but-correct '!'
  }
}

extension FailableDerivedClass {
  convenience init?(derivedFailInExtension: ()) throws {
    self.init(derivedFailDuringDelegation: derivedFailInExtension)
  }
}

////
// Classes with throwing initializers
////

class ThrowBaseClass {
  required init() throws {}
  init(noFail: ()) {}
}

class ThrowDerivedClass : ThrowBaseClass {
// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17ThrowDerivedClassCACyKcfc
// CHECK:       bb0([[SELF:%.*]] : $ThrowDerivedClass):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $ThrowDerivedClass
// CHECK:         store [[SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    [[RELOAD_SELF:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT:    [[BASE_SELF:%.*]] = upcast [[RELOAD_SELF]]
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers14ThrowBaseClassCACyKcfc
// CHECK-NEXT:    try_apply [[INIT_FN]]([[BASE_SELF]])
// CHECK:       bb1([[NEW_SELF:%.*]] : $ThrowBaseClass):
// CHECK-NEXT:    [[DERIVED_SELF:%.*]] = unchecked_ref_cast [[NEW_SELF]]
// CHECK-NEXT:    strong_retain [[DERIVED_SELF]]
// CHECK-NEXT:    store [[DERIVED_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[DERIVED_SELF]]
// CHECK:       bb2([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  required init() throws {
    try super.init()
  }

  override init(noFail: ()) {
    try! super.init()
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17ThrowDerivedClassC28failBeforeFullInitializationACSi_tKcfc
// CHECK:       bb0(%0 : $Int, [[SELF:%.*]] : $ThrowDerivedClass):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $ThrowDerivedClass
// CHECK:         store [[SELF]] to [[SELF_BOX]]
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[RESULT:%.*]] : $Int):
// CHECK-NEXT:    [[RELOAD_SELF:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT:    [[BASE_SELF:%.*]] = upcast [[RELOAD_SELF]]
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers14ThrowBaseClassC6noFailACyt_tcfc
// CHECK-NEXT:    [[NEW_SELF:%.*]] = apply [[INIT_FN]]([[BASE_SELF]])
// CHECK-NEXT:    [[DERIVED_SELF:%.*]] = unchecked_ref_cast [[NEW_SELF]]
// CHECK-NEXT:    strong_retain [[DERIVED_SELF]]
// CHECK-NEXT:    store [[DERIVED_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[DERIVED_SELF]] : $ThrowDerivedClass
// CHECK:       bb2([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    [[RELOAD_SELF:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT:    [[METATYPE:%.*]] = metatype $@thick ThrowDerivedClass.Type
// CHECK-NEXT:    dealloc_partial_ref [[RELOAD_SELF]] : $ThrowDerivedClass, [[METATYPE]] : $@thick ThrowDerivedClass.Type
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  init(failBeforeFullInitialization: Int) throws {
    try unwrap(failBeforeFullInitialization)
    super.init(noFail: ())
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17ThrowDerivedClassC28failBeforeFullInitialization0h6DuringjK0ACSi_SitKcfc
// CHECK:       bb0(%0 : $Int, %1 : $Int, %2 : $ThrowDerivedClass):
// CHECK-NEXT:    [[BITMAP_BOX:%.*]] = alloc_stack $Builtin.Int1
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack [dynamic_lifetime] $ThrowDerivedClass
// CHECK-NEXT:    [[ZERO:%.*]] = integer_literal $Builtin.Int1, 0
// CHECK-NEXT:    store [[ZERO]] to [[BITMAP_BOX]]
// CHECK:         store %2 to [[SELF_BOX]] : $*ThrowDerivedClass
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[RESULT:%.*]] : $Int)
// CHECK-NEXT:    [[RELOAD_SELF:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT:    [[BASE_SELF:%.*]] = upcast [[RELOAD_SELF]]
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers14ThrowBaseClassCACyKcfc
// CHECK-NEXT:    [[BIT:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK-NEXT:    store [[BIT]] to [[BITMAP_BOX]]
// CHECK:         try_apply [[INIT_FN]]([[BASE_SELF]])
// CHECK:       bb2([[NEW_SELF:%.*]] : $ThrowBaseClass):
// CHECK-NEXT:    [[DERIVED_SELF:%.*]] = unchecked_ref_cast [[NEW_SELF]]
// CHECK-NEXT:    strong_retain [[DERIVED_SELF]]
// CHECK-NEXT:    store [[DERIVED_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    return [[DERIVED_SELF]]
// CHECK:       bb3([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb4([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb5([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    [[COND:%.*]] = load [[BITMAP_BOX]]
// CHECK-NEXT:    cond_br [[COND]], bb6, bb7
// CHECK:       bb6:
// CHECK-NEXT:    br bb8
// CHECK:       bb7:
// CHECK-NEXT:    [[RELOAD_SELF:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT:    [[METATYPE:%.*]] = metatype $@thick ThrowDerivedClass.Type
// CHECK-NEXT:    dealloc_partial_ref [[RELOAD_SELF]] : $ThrowDerivedClass, [[METATYPE]] : $@thick ThrowDerivedClass.Type
// CHECK-NEXT:    br bb8
// CHECK:       bb8:
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  init(failBeforeFullInitialization: Int, failDuringFullInitialization: Int) throws {
    try unwrap(failBeforeFullInitialization)
    try super.init()
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17ThrowDerivedClassC27failAfterFullInitializationACSi_tKcfc
// CHECK:       bb0(%0 : $Int, %1 : $ThrowDerivedClass):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $ThrowDerivedClass
// CHECK:         store %1 to [[SELF_BOX]]
// CHECK-NEXT:    [[RELOAD_SELF:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT:    [[BASE_SELF:%.*]] = upcast [[RELOAD_SELF]]
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers14ThrowBaseClassC6noFailACyt_tcfc
// CHECK-NEXT:    [[NEW_SELF:%.*]] = apply [[INIT_FN]]([[BASE_SELF]])
// CHECK-NEXT:    [[DERIVED_SELF:%.*]] = unchecked_ref_cast [[NEW_SELF]]
// CHECK-NEXT:    strong_retain [[DERIVED_SELF]]
// CHECK-NEXT:    store [[DERIVED_SELF]] to [[SELF_BOX]]
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[RESULT:%.*]] : $Int):
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[DERIVED_SELF]]
// CHECK:       bb2([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    strong_release [[DERIVED_SELF]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  init(failAfterFullInitialization: Int) throws {
    super.init(noFail: ())
    try unwrap(failAfterFullInitialization)
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17ThrowDerivedClassC27failAfterFullInitialization0h6DuringjK0ACSi_SitKcfc
// CHECK:       bb0(%0 : $Int, %1 : $Int, %2 : $ThrowDerivedClass):
// CHECK-NEXT:    [[BITMAP_BOX:%.*]] = alloc_stack $Builtin.Int2
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack [dynamic_lifetime] $ThrowDerivedClass
// CHECK:         [[ZERO:%.*]] = integer_literal $Builtin.Int2, 0
// CHECK-NEXT:    store [[ZERO]] to [[BITMAP_BOX]]
// CHECK:         store %2 to [[SELF_BOX]]
// CHECK-NEXT:    [[RELOAD_SELF:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT:    [[DERIVED_SELF:%.*]] = upcast [[RELOAD_SELF]]
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers14ThrowBaseClassCACyKcfc
// CHECK:    try_apply [[INIT_FN]]([[DERIVED_SELF]])
// CHECK:       bb1([[NEW_SELF:%.*]] : $ThrowBaseClass):
// CHECK-NEXT:    [[BIT:%.*]] = integer_literal $Builtin.Int2, -1
// CHECK-NEXT:    store [[BIT]] to [[BITMAP_BOX]]
// CHECK-NEXT:    [[DERIVED_SELF:%.*]] = unchecked_ref_cast [[NEW_SELF]]
// CHECK-NEXT:    strong_retain [[DERIVED_SELF]]
// CHECK-NEXT:    store [[DERIVED_SELF]] to [[SELF_BOX]]
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb2([[RESULT:%.*]] : $Int):
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    return [[DERIVED_SELF]]
// CHECK:       bb3([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb4([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    strong_release [[DERIVED_SELF]]
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb5([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    [[BITMAP:%.*]] = load [[BITMAP_BOX]]
// CHECK-NEXT:    [[ONE:%.*]] = integer_literal $Builtin.Int2, 1
// CHECK-NEXT:    [[BITMAP_MSB:%.*]] = builtin "lshr_Int2"([[BITMAP]] : $Builtin.Int2, [[ONE]] : $Builtin.Int2)
// CHECK-NEXT:    [[COND:%.*]] = builtin "trunc_Int2_Int1"([[BITMAP_MSB]] : $Builtin.Int2)
// CHECK-NEXT:    cond_br [[COND]], bb6, bb7
// CHECK:       bb6:
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    br bb8
// CHECK:       bb7:
// CHECK-NEXT:    br bb8
// CHECK:       bb8:
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  init(failAfterFullInitialization: Int, failDuringFullInitialization: Int) throws {
    try super.init()
    try unwrap(failAfterFullInitialization)
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17ThrowDerivedClassC28failBeforeFullInitialization0h5AfterjK0ACSi_SitKcfc
// CHECK:       bb0(%0 : $Int, %1 : $Int, %2 : $ThrowDerivedClass):
// CHECK-NEXT:    [[BITMAP_BOX:%.*]] = alloc_stack $Builtin.Int2
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack [dynamic_lifetime] $ThrowDerivedClass
// CHECK-NEXT:    [[ZERO:%.*]] = integer_literal $Builtin.Int2, 0
// CHECK-NEXT:    store [[ZERO]] to [[BITMAP_BOX]]
// CHECK:         store %2 to [[SELF_BOX]]
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[RESULT:%.*]] : $Int):
// CHECK-NEXT:    [[TWO:%.*]] = integer_literal $Builtin.Int2, -2
// CHECK-NEXT:    store [[TWO]] to [[BITMAP_BOX]]
// CHECK-NEXT:    [[RELOAD_SELF:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT:    [[BASE_SELF:%.*]] = upcast [[RELOAD_SELF]]
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers14ThrowBaseClassC6noFailACyt_tcfc
// CHECK-NEXT:    [[ONE:%.*]] = integer_literal $Builtin.Int2, -1
// CHECK-NEXT:    store [[ONE]] to [[BITMAP_BOX]]
// CHECK:         [[NEW_SELF:%.*]] = apply [[INIT_FN]]([[BASE_SELF]])
// CHECK-NEXT:    [[DERIVED_SELF:%.*]] = unchecked_ref_cast [[NEW_SELF]]
// CHECK-NEXT:    strong_retain [[DERIVED_SELF]]
// CHECK-NEXT:    store [[DERIVED_SELF]] to [[SELF_BOX]]
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%1)
// CHECK:       bb2([[RESULT:%.*]] : $Int):
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    return [[DERIVED_SELF]]
// CHECK:       bb3([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb4([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    strong_release [[DERIVED_SELF]]
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb5([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    [[BITMAP:%.*]] = load [[BITMAP_BOX]]
// CHECK-NEXT:    [[COND:%.*]] = builtin "trunc_Int2_Int1"([[BITMAP]] : $Builtin.Int2) : $Builtin.Int1
// CHECK-NEXT:    cond_br [[COND]], bb6, bb10
// CHECK:       bb6:
// CHECK-NEXT:    [[BITMAP:%.*]] = load [[BITMAP_BOX]]
// CHECK-NEXT:    [[ONE:%.*]] = integer_literal $Builtin.Int2, 1
// CHECK-NEXT:    [[SHIFTED:%.*]] = builtin "lshr_Int2"([[BITMAP]] : $Builtin.Int2, [[ONE]] : $Builtin.Int2) : $Builtin.Int2
// CHECK-NEXT:    [[COND:%.*]] = builtin "trunc_Int2_Int1"([[SHIFTED]] : $Builtin.Int2) : $Builtin.Int1
// CHECK-NEXT:    cond_br [[COND]], bb7, bb8
// CHECK:       bb7:
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    br bb9
// CHECK:       bb8:
// CHECK-NEXT:    br bb9
// CHECK:       bb9:
// CHECK-NEXT:    br bb11
// CHECK:       bb10:
// CHECK-NEXT:    [[BITMAP:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT:    [[METATYPE:%.*]] = metatype $@thick ThrowDerivedClass.Type
// CHECK-NEXT:    dealloc_partial_ref [[BITMAP]] : $ThrowDerivedClass, [[METATYPE]] : $@thick ThrowDerivedClass.Type
// CHECK-NEXT:    br bb11
// CHECK:       bb11:
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    throw [[ERROR]] : $any Error
  init(failBeforeFullInitialization: Int, failAfterFullInitialization: Int) throws {
    try unwrap(failBeforeFullInitialization)
    super.init(noFail: ())
    try unwrap(failAfterFullInitialization)
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17ThrowDerivedClassC28failBeforeFullInitialization0h6DuringjK00h5AfterjK0ACSi_S2itKcfc
// CHECK:       bb0(%0 : $Int, %1 : $Int, %2 : $Int, %3 : $ThrowDerivedClass):
// CHECK-NEXT:    [[BITMAP_BOX:%.*]] = alloc_stack $Builtin.Int2
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack [dynamic_lifetime] $ThrowDerivedClass
// CHECK-NEXT:    [[ZERO:%.*]] = integer_literal $Builtin.Int2, 0
// CHECK-NEXT:    store [[ZERO]] to [[BITMAP_BOX]]
// CHECK:         store %3 to [[SELF_BOX]]
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[RESULT:%.*]] : $Int):
// CHECK-NEXT:    [[RELOAD_SELF:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT:    [[BASE_SELF:%.*]] = upcast [[RELOAD_SELF]]
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers14ThrowBaseClassCACyKcfc
// CHECK-NEXT:    [[ONE:%.*]] = integer_literal $Builtin.Int2, 1
// CHECK-NEXT:    store [[ONE]] to [[BITMAP_BOX]]
// CHECK:         try_apply [[INIT_FN]]([[BASE_SELF]])
// CHECK:       bb2([[NEW_SELF:%.*]] : $ThrowBaseClass):
// CHECK-NEXT:    [[NEG_ONE:%.*]] = integer_literal $Builtin.Int2, -1
// CHECK-NEXT:    store [[NEG_ONE]] to [[BITMAP_BOX]]
// CHECK-NEXT:    [[DERIVED_SELF:%.*]] = unchecked_ref_cast [[NEW_SELF]]
// CHECK-NEXT:    strong_retain [[DERIVED_SELF]]
// CHECK-NEXT:    store [[DERIVED_SELF]] to [[SELF_BOX]]
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%2)
// CHECK:       bb3([[RESULT:%.*]] : $Int):
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    return [[DERIVED_SELF]]
// CHECK:       bb4([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    br bb7([[ERROR]] : $any Error)
// CHECK:       bb5([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    br bb7([[ERROR]] : $any Error)
// CHECK:       bb6([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    strong_release [[DERIVED_SELF]]
// CHECK-NEXT:    br bb7([[ERROR]] : $any Error)
// CHECK:       bb7([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    [[BITMAP:%.*]] = load [[BITMAP_BOX]]
// CHECK-NEXT:    [[COND:%.*]] = builtin "trunc_Int2_Int1"([[BITMAP]] : $Builtin.Int2)
// CHECK-NEXT:    cond_br [[COND]], bb8, bb12
// CHECK:       bb8:
// CHECK-NEXT:    [[BITMAP:%.*]] = load [[BITMAP_BOX]]
// CHECK-NEXT:    [[ONE:%.*]] = integer_literal $Builtin.Int2, 1
// CHECK-NEXT:    [[BITMAP_MSB:%.*]] = builtin "lshr_Int2"([[BITMAP]] : $Builtin.Int2, [[ONE]] : $Builtin.Int2)
// CHECK-NEXT:    [[COND:%.*]] = builtin "trunc_Int2_Int1"([[BITMAP_MSB]] : $Builtin.Int2)
// CHECK-NEXT:    cond_br [[COND]], bb9, bb10
// CHECK:       bb9:
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    br bb11
// CHECK:       bb10:
// CHECK-NEXT:    br bb11
// CHECK:       bb11:
// CHECK-NEXT:    br bb13
// CHECK:       bb12:
// CHECK-NEXT:    [[SELF:%.*]] = load [[SELF_BOX]]
// CHECK-NEXT:    [[METATYPE:%.*]] = metatype $@thick ThrowDerivedClass.Type
// CHECK-NEXT:    dealloc_partial_ref [[SELF]] : $ThrowDerivedClass, [[METATYPE]] : $@thick ThrowDerivedClass.Type
// CHECK-NEXT:    br bb13
// CHECK:       bb13:
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  init(failBeforeFullInitialization: Int, failDuringFullInitialization: Int, failAfterFullInitialization: Int) throws {
    try unwrap(failBeforeFullInitialization)
    try super.init()
    try unwrap(failAfterFullInitialization)
  }

  convenience init(noFail2: ()) {
    try! self.init()
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17ThrowDerivedClassC20failBeforeDelegationACSi_tKcfC
// CHECK:       bb0(%0 : $Int, %1 : $@thick ThrowDerivedClass.Type):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $ThrowDerivedClass
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[ARG:%.*]] : $Int):
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers17ThrowDerivedClassC6noFailACyt_tcfC
// CHECK-NEXT:    [[NEW_SELF:%.*]] = apply [[INIT_FN]](%1)
// CHECK-NEXT:    strong_retain [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb2([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  convenience init(failBeforeDelegation: Int) throws {
    try unwrap(failBeforeDelegation)
    self.init(noFail: ())
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17ThrowDerivedClassC20failDuringDelegationACSi_tKcfC
// CHECK:       bb0(%0 : $Int, %1 : $@thick ThrowDerivedClass.Type):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $ThrowDerivedClass
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers17ThrowDerivedClassCACyKcfC
// CHECK-NEXT:    try_apply [[INIT_FN]](%1)
// CHECK:       bb1([[NEW_SELF:%.*]] : $ThrowDerivedClass):
// CHECK-NEXT:    strong_retain [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb2([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR]] : $any Error
  convenience init(failDuringDelegation: Int) throws {
    try self.init()
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17ThrowDerivedClassC28failBeforeOrDuringDelegationACSi_tKcfC
// CHECK:       bb0(%0 : $Int, %1 : $@thick ThrowDerivedClass.Type):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $ThrowDerivedClass
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[ARG:%.*]] : $Int):
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers17ThrowDerivedClassCACyKcfC
// CHECK-NEXT:    try_apply [[INIT_FN]](%1)
// CHECK:       bb2([[NEW_SELF:%.*]] : $ThrowDerivedClass):
// CHECK-NEXT:    strong_retain [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb3([[ERROR1:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR1]] : $any Error)
// CHECK:       bb4([[ERROR2:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR2]] : $any Error)
// CHECK:       bb5([[ERROR3:%.*]] : $any Error):
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR3]]
  convenience init(failBeforeOrDuringDelegation: Int) throws {
    try unwrap(failBeforeOrDuringDelegation)
    try self.init()
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17ThrowDerivedClassC29failBeforeOrDuringDelegation2ACSi_tKcfC
// CHECK:         bb0(%0 : $Int, %1 : $@thick ThrowDerivedClass.Type):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $ThrowDerivedClass
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[ARG:%.*]] : $Int):
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers17ThrowDerivedClassC20failBeforeDelegationACSi_tKcfC
// CHECK-NEXT:    try_apply [[INIT_FN]]([[ARG]], %1)
// CHECK:       bb2([[NEW_SELF:%.*]] : $ThrowDerivedClass):
// CHECK-NEXT:    strong_retain [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb3([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR1]] : $any Error)
// CHECK:       bb4([[ERROR2:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR2]] : $any Error)
// CHECK:       bb5([[ERROR3:%.*]] : $any Error):
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR3]]
  convenience init(failBeforeOrDuringDelegation2: Int) throws {
    try self.init(failBeforeDelegation: unwrap(failBeforeOrDuringDelegation2))
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17ThrowDerivedClassC19failAfterDelegationACSi_tKcfC
// CHECK:       bb0(%0 : $Int, %1 : $@thick ThrowDerivedClass.Type):
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack $ThrowDerivedClass
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers17ThrowDerivedClassC6noFailACyt_tcfC
// CHECK-NEXT:    [[NEW_SELF:%.*]] = apply [[INIT_FN]](%1)
// CHECK-NEXT:    strong_retain [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[RESULT:%.*]] : $Int):
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb2([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    strong_release [[NEW_SELF]]
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  convenience init(failAfterDelegation: Int) throws {
    self.init(noFail: ())
    try unwrap(failAfterDelegation)
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17ThrowDerivedClassC27failDuringOrAfterDelegationACSi_tKcfC
// CHECK:       bb0(%0 : $Int, %1 : $@thick ThrowDerivedClass.Type):
// CHECK:         [[BITMAP_BOX:%.*]] = alloc_stack $Builtin.Int1
// CHECK:         [[SELF_BOX:%.*]] = alloc_stack [dynamic_lifetime] $ThrowDerivedClass
// CHECK:         [[ZERO:%.*]] = integer_literal $Builtin.Int1, 0
// CHECK-NEXT:    store [[ZERO]] to [[BITMAP_BOX]]
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers17ThrowDerivedClassCACyKcfC
// CHECK-NEXT:    try_apply [[INIT_FN]](%1)
// CHECK:       bb1([[NEW_SELF:%.*]] : $ThrowDerivedClass):
// CHECK-NEXT:    [[BIT:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK-NEXT:    store [[BIT]] to [[BITMAP_BOX]]
// CHECK-NEXT:    strong_retain [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb2([[RESULT:%.*]] : $Int):
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb3([[ERROR1:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR1]] : $any Error)
// CHECK:       bb4([[ERROR2:%.*]] : $any Error):
// CHECK-NEXT:    strong_release [[NEW_SELF]]
// CHECK-NEXT:    br bb5([[ERROR2]] : $any Error)
// CHECK:       bb5([[ERROR3:%.*]] : $any Error):
// CHECK-NEXT:    [[BITMAP:%.*]] = load [[BITMAP_BOX]]
// CHECK:         cond_br {{.*}}, bb6, bb7
// CHECK:       bb6:
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    br bb8
// CHECK:       bb7:
// CHECK-NEXT:    br bb8
// CHECK:       bb8:
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    throw [[ERROR3]]
  convenience init(failDuringOrAfterDelegation: Int) throws {
    try self.init()
    try unwrap(failDuringOrAfterDelegation)
  }

// CHECK-LABEL: sil hidden @$s35definite_init_failable_initializers17ThrowDerivedClassC27failBeforeOrAfterDelegationACSi_tKcfC
// CHECK:       bb0(%0 : $Int, %1 : $@thick ThrowDerivedClass.Type):
// CHECK-NEXT:    [[BITMAP_BOX:%.*]] = alloc_stack $Builtin.Int1
// CHECK-NEXT:    [[SELF_BOX:%.*]] = alloc_stack [dynamic_lifetime] $ThrowDerivedClass
// CHECK-NEXT:    [[ZERO:%.*]] = integer_literal $Builtin.Int1, 0
// CHECK-NEXT:    store [[ZERO]] to [[BITMAP_BOX]]
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb1([[RESULT:%.*]] : $Int):
// CHECK:         [[INIT_FN:%.*]] = function_ref @$s35definite_init_failable_initializers17ThrowDerivedClassC6noFailACyt_tcfC
// CHECK-NEXT:    [[NEW_SELF:%.*]] = apply [[INIT_FN]](%1)
// CHECK-NEXT:    [[BIT:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK-NEXT:    store [[BIT]] to [[BITMAP_BOX]]
// CHECK-NEXT:    strong_retain [[NEW_SELF]]
// CHECK-NEXT:    store [[NEW_SELF]] to [[SELF_BOX]]
// CHECK:         [[UNWRAP_FN:%.*]] = function_ref @$s35definite_init_failable_initializers6unwrapyS2iKF
// CHECK-NEXT:    try_apply [[UNWRAP_FN]](%0)
// CHECK:       bb2([[RESULT:%.*]] : $Int):
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    return [[NEW_SELF]]
// CHECK:       bb3([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb4([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    strong_release [[NEW_SELF]]
// CHECK-NEXT:    br bb5([[ERROR]] : $any Error)
// CHECK:       bb5([[ERROR:%.*]] : $any Error):
// CHECK-NEXT:    [[BITMAP:%.*]] = load [[BITMAP_BOX]]
// CHECK:         cond_br {{.*}}, bb6, bb7
// CHECK:       bb6:
// CHECK-NEXT:    destroy_addr [[SELF_BOX]]
// CHECK-NEXT:    br bb8
// CHECK:       bb7:
// CHECK-NEXT:    br bb8
// CHECK:       bb8:
// CHECK-NEXT:    dealloc_stack [[SELF_BOX]]
// CHECK-NEXT:    dealloc_stack [[BITMAP_BOX]]
// CHECK-NEXT:    throw [[ERROR]]
  convenience init(failBeforeOrAfterDelegation: Int) throws {
    try unwrap(failBeforeOrAfterDelegation)
    self.init(noFail: ())
    try unwrap(failBeforeOrAfterDelegation)
  }
}

////
// Enums with failable initializers
////

enum FailableEnum {
  case A

  init?(a: Int64) { self = .A }

  init!(b: Int64) {
    self.init(a: b)! // unnecessary-but-correct '!'
  }

  init(c: Int64) {
    self.init(a: c)! // necessary '!'
  }

  init(d: Int64) {
    self.init(b: d)! // unnecessary-but-correct '!'
  }
}

////
// Protocols and protocol extensions
////

// Delegating to failable initializers from a protocol extension to a
// protocol.
protocol P1 {
  init?(p1: Int64)
}

extension P1 {
  init!(p1a: Int64) {
    self.init(p1: p1a)! // unnecessary-but-correct '!'
  }

  init(p1b: Int64) {
    self.init(p1: p1b)! // necessary '!'
  }
}

protocol P2 : class {
  init?(p2: Int64)
}

extension P2 {
  init!(p2a: Int64) {
    self.init(p2: p2a)! // unnecessary-but-correct '!'
  }

  init(p2b: Int64) {
    self.init(p2: p2b)! // necessary '!'
  }
}

// Delegating to failable initializers from a protocol extension to a
// protocol extension.
extension P1 {
  init?(p1c: Int64) {
    self.init(p1: p1c)
  }

  init!(p1d: Int64) {
    self.init(p1c: p1d)! // unnecessary-but-correct '!'
  }

  init(p1e: Int64) {
    self.init(p1c: p1e)! // necessary '!'
  }
}

extension P2 {
  init?(p2c: Int64) {
    self.init(p2: p2c)
  }

  init!(p2d: Int64) {
    self.init(p2c: p2d)! // unnecessary-but-correct '!'
  }

  init(p2e: Int64) {
    self.init(p2c: p2e)! // necessary '!'
  }
}

////
// type(of: self) with uninitialized self
////

func use(_ a : Any) {}

class DynamicTypeBase {
  var x: Int

  init() {
    use(type(of: self))
    x = 0
  }

  convenience init(a : Int) {
    use(type(of: self))
    self.init()
  }
}

class DynamicTypeDerived : DynamicTypeBase {
  override init() {
    use(type(of: self))
    super.init()
  }

  convenience init(a : Int) {
    use(type(of: self))
    self.init()
  }
}

struct DynamicTypeStruct {
  var x: Int

  init() {
    use(type(of: self))
    x = 0
  }

  init(a : Int) {
    use(type(of: self))
    self.init()
  }
}
