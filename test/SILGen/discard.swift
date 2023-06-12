// RUN: %target-swift-emit-silgen -enable-experimental-feature MoveOnlyEnumDeinits -module-name test %s | %FileCheck %s --enable-var-scope
// RUN: %target-swift-emit-sil -enable-experimental-feature MoveOnlyEnumDeinits -module-name test -sil-verify-all %s | %FileCheck %s --check-prefix CHECK-SIL --enable-var-scope

// Swift sources are require to remove struct_extract so this check-not line passes:
// "CHECK-SIL-NOT: struct_extract"
// REQUIRES: swift_in_compiler

func invokedDeinit() {}

@_moveOnly enum MaybeFile {
  case some(File)
  case none

  deinit {}

  // NOTE: we can't pattern match on self since
  // that would consume it before we can discard self!
  var test: Int {
    __consuming get {
      discard self
      return 0
    }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s4test9MaybeFileOAASivg
  // CHECK:    [[SELF_BOX:%.*]] = alloc_box ${ let MaybeFile }, let, name "self", argno 1
  // CHECK:    [[SELF_REF:%.*]] = project_box [[SELF_BOX]] : ${ let MaybeFile }, 0
  // CHECK:    store {{.*}} to [init]
  // CHECK:    [[SELF_MMC:%.*]] = mark_must_check [no_consume_or_assign] [[SELF_REF]] : $*MaybeFile
  // CHECK:    [[SELF_VAL:%.*]] = load [copy] [[SELF_MMC]] : $*MaybeFile
  // CHECK:    [[DD:%.*]] = drop_deinit [[SELF_VAL]] : $MaybeFile
  // CHECK:    destroy_value [[DD]] : $MaybeFile

  // CHECK-SIL-LABEL: sil hidden @$s4test9MaybeFileOAASivg
  // CHECK-SIL:    [[SELF_STACK:%.*]] = alloc_stack $MaybeFile, let, name "self", argno 1
  // CHECK-SIL:    store {{.*}}
  // CHECK-SIL:    [[SELF_VAL:%.*]] = load [[SELF_STACK]] : $*MaybeFile
  // CHECK-SIL:    switch_enum [[SELF_VAL]] : $MaybeFile, case #MaybeFile.some!enumelt: bb1, case #MaybeFile.none!enumelt: bb2
  //
  // CHECK-SIL:  bb1([[FILE:%.*]] : $File):
  // CHECK-SIL:    release_value [[FILE]] : $File
}

@_moveOnly struct File {
  let fd: Int
  static var nextFD: Int = 0

  init() {
    fd = File.nextFD
    File.nextFD += 1
  }

  __consuming func takeDescriptor() -> Int {
    let id = fd
    discard self
    return id
  }

  // CHECK-LABEL: sil hidden [ossa] @$s4test4FileV14takeDescriptorSiyF
  // CHECK:  [[SELF_BOX:%.*]] = alloc_box ${ let File }, let, name "self", argno 1
  // CHECK:  [[SELF_REF:%.*]] = project_box [[SELF_BOX]] : ${ let File }, 0
  // CHECK:  store {{.*}} to [init]
  // CHECK:  load_borrow {{.*}} : $*File
  // CHECK:  [[SELF_MMC:%.*]] = mark_must_check [no_consume_or_assign] [[SELF_REF]] : $*File
  // CHECK:  [[SELF_VAL:%.*]] = load [copy] [[SELF_MMC]] : $*File
  // CHECK:  [[DD:%.*]] = drop_deinit [[SELF_VAL]] : $File
  // CHECK:  destroy_value [[DD]] : $File

  deinit {
    invokedDeinit()
  }
}

@_moveOnly struct PointerTree {
  let left: UnsafePointer<UInt>?
  let file: Int = 0
  lazy var popularity: Int = 0
  var right: Float = 0.0

  consuming func tryDestroy(doDiscard: Bool) throws {
    if doDiscard {
      discard self
    } else {
     _ = consume self
    }
    throw E.err
  }

// CHECK-LABEL: sil hidden [ossa] @$s4test11PointerTreeV10tryDestroy9doDiscardySb_tKF : $@convention(method) (Bool, @owned PointerTree) -> @error any Error {
// CHECK:   bb0{{.*}}:
// CHECK:     [[SELF_BOX:%.*]] = alloc_box ${ var PointerTree }, var, name "self"
// CHECK:     [[SELF_PTR:%.*]] = project_box [[SELF_BOX]] : ${ var PointerTree }, 0
//            .. skip to the conditional test ..
// CHECK:     [[SHOULD_FORGET:%.*]] = struct_extract {{.*}} : $Bool, #Bool._value
// CHECK:     cond_br [[SHOULD_FORGET]], bb1, bb2
//
// CHECK:   bb1:
// CHECK:     [[ACCESS:%.*]] = begin_access [read] [unknown] [[SELF_PTR]] : $*PointerTree
// CHECK:     [[MMC:%.*]] = mark_must_check [no_consume_or_assign] [[ACCESS]] : $*PointerTree
// CHECK:     [[COPIED_SELF:%.*]] = load [copy] [[MMC]] : $*PointerTree
// CHECK:     end_access [[ACCESS]] : $*PointerTree
// CHECK:     [[DD:%.*]] = drop_deinit [[COPIED_SELF]]
// CHECK:     destroy_value [[DD]]
// CHECK:     br bb3
//
// CHECK:   bb2:
// CHECK:     br bb3
//
// CHECK:   bb3:
// CHECK:     destroy_value [[SELF_BOX]] : ${ var PointerTree }
// CHECK:     throw
// CHECK: } // end sil function

// After the mandatory passes have run, check for correct deinitializations within the init.

// CHECK-SIL-LABEL: sil hidden @$s4test11PointerTreeV10tryDestroy9doDiscardySb_tKF
// CHECK-SIL:     [[SHOULD_FORGET:%.*]] = struct_extract {{.*}} : $Bool, #Bool._value
// CHECK-SIL:     cond_br [[SHOULD_FORGET]], bb1, bb2
//
// CHECK-SIL:  bb1:
// CHECK-SIL:     [[ACCESS:%.*]] = begin_access [modify] [static] {{.*}} : $*PointerTree
// CHECK-SIL:     [[SELF_VAL:%.*]] = load [[ACCESS]] : $*PointerTree
// CHECK-SIL:     end_access [[ACCESS]] : $*PointerTree
// CHECK-SIL-NOT: struct_extract
                  // no accesses to the fields are expected because the fields are no-op destroyed.
// CHECK-SIL:     br bb3
//
// CHECK-SIL:  bb2:
// CHECK-SIL:     destroy_addr %{{.*}} : $*PointerTree
// CHECK-SIL:     br bb3
//
// CHECK-SIL:  bb3:
// CHECK-SIL-NOT:  apply
// CHECK-SIL:      throw


  deinit {
    invokedDeinit()
  }
}

final class Wallet {
  var numCards = 0
}

@_moveOnly enum Ticket {
  case empty
  case within(Wallet)

  consuming func changeTicket(inWallet wallet: Wallet? = nil) {
    if let existingWallet = wallet {
      self = .within(existingWallet)
      _ = consume self
    } else {
      discard self
    }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s4test6TicketO06changeB08inWalletyAA0E0CSg_tF : $@convention(method) (@guaranteed Optional<Wallet>, @owned Ticket) -> () {
  // CHECK:    [[SELF_REF:%.*]] = project_box [[SELF_BOX:%.*]] : ${ var Ticket }, 0
  // CHECK:    switch_enum {{.*}} : $Optional<Wallet>, case #Optional.some!enumelt: {{.*}}, case #Optional.none!enumelt: [[NO_WALLET_BB:bb[0-9]+]]
  //
  // >> now we begin the destruction sequence, which involves pattern matching on self to destroy its innards
  // CHECK:  [[NO_WALLET_BB]]
  // CHECK:    [[SELF_ACCESS:%.*]] = begin_access [read] [unknown] {{%.*}} : $*Ticket
  // CHECK:    [[SELF_MMC:%.*]] = mark_must_check [no_consume_or_assign] [[SELF_ACCESS]]
  // CHECK:    [[SELF_COPY:%.*]] = load [copy] [[SELF_MMC]] : $*Ticket
  // CHECK:    end_access [[SELF_ACCESS:%.*]] : $*Ticket
  // CHECK:    [[DD:%.*]] = drop_deinit [[SELF_COPY]] : $Ticket
  // CHECK:    destroy_value [[DD]] : $Ticket

  // CHECK-SIL-LABEL: sil hidden @$s4test6TicketO06changeB08inWalletyAA0E0CSg_tF : $@convention(method) (@guaranteed Optional<Wallet>, @owned Ticket) -> () {
  // CHECK-SIL:    [[SELF_REF:%.*]] = alloc_stack [lexical] $Ticket, var, name "self", implicit 
  // CHECK-SIL:    switch_enum {{.*}} : $Optional<Wallet>, case #Optional.some!enumelt: {{.*}}, case #Optional.none!enumelt: [[NO_WALLET_BB:bb[0-9]+]]
  //
  // >> now we begin the destruction sequence, which involves pattern matching on self to destroy its innards
  // CHECK-SIL:  [[NO_WALLET_BB]]
  // CHECK-SIL:    [[SELF_ACCESS:%.*]] = begin_access [modify] [static] {{%.*}} : $*Ticket
  // CHECK-SIL:    [[SELF_COPY:%.*]] = load [[SELF_ACCESS]] : $*Ticket
  // CHECK-SIL:    end_access [[SELF_ACCESS:%.*]] : $*Ticket
  // CHECK-SIL:    switch_enum [[SELF_COPY]] : $Ticket, case #Ticket.empty!enumelt: [[TICKET_EMPTY:bb[0-9]+]], case #Ticket.within!enumelt: [[TICKET_WITHIN:bb[0-9]+]]
  // CHECK-SIL:  [[TICKET_EMPTY]]:
  // CHECK-SIL:    br [[JOIN_POINT:bb[0-9]+]]
  // CHECK-SIL:  [[TICKET_WITHIN]]([[PREV_SELF_WALLET:%.*]] : $Wallet):
  // CHECK-SIL:    strong_release [[PREV_SELF_WALLET]] : $Wallet
  // CHECK-SIL:    br [[JOIN_POINT]]
  // CHECK-SIL:  [[JOIN_POINT]]:

  deinit {
    print("destroying ticket")
  }
}

enum E: Error { case err }
class Ptr { var whatever: Int = 0 }
