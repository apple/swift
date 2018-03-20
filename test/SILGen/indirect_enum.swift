// REQUIRES: plus_one_runtime

// RUN: %target-swift-frontend -module-name indirect_enum -Xllvm -sil-print-debuginfo -emit-silgen %s | %FileCheck %s

indirect enum TreeA<T> {
  case Nil
  case Leaf(T)
  case Branch(left: TreeA<T>, right: TreeA<T>)
}

// CHECK-LABEL: sil hidden @$S13indirect_enum11TreeA_cases_1l1ryx_AA0C1AOyxGAGtlF : $@convention(thin) <T> (@in T, @owned TreeA<T>, @owned TreeA<T>) -> () {
func TreeA_cases<T>(_ t: T, l: TreeA<T>, r: TreeA<T>) {
// CHECK: bb0([[ARG1:%.*]] : $*T, [[ARG2:%.*]] : $TreeA<T>, [[ARG3:%.*]] : $TreeA<T>):
// CHECK:         [[METATYPE:%.*]] = metatype $@thin TreeA<T>.Type
// CHECK:         [[ENUM_CASE:%.*]] = function_ref @$S13indirect_enum5TreeAO3NilyACyxGAEmlF
// CHECK-NEXT:    [[NIL:%.*]] = apply [[ENUM_CASE]]<T>([[METATYPE]])
// CHECK:         destroy_value [[NIL]]
  let _ = TreeA<T>.Nil

// CHECK-NEXT:    [[METATYPE:%.*]] = metatype $@thin TreeA<T>.Type
// CHECK-NEXT:    [[STACK:%.*]] = alloc_stack $T
// CHECK-NEXT:    copy_addr [[ARG1]] to [initialization] [[STACK]] : $*T
// CHECK:         [[ENUM_CASE:%.*]] = function_ref @$S13indirect_enum5TreeAO4LeafyACyxGxcAEmlF
// CHECK-NEXT:    [[LEAF:%.*]] = apply [[ENUM_CASE]]<T>([[STACK]], [[METATYPE]])
// CHECK-NEXT:    dealloc_stack [[STACK]]
// CHECK-NEXT:    destroy_value [[LEAF]]
  let _ = TreeA<T>.Leaf(t)

// CHECK-NEXT:    [[METATYPE:%.*]] = metatype $@thin TreeA<T>.Type
// CHECK-NEXT:    [[BORROWED_ARG2:%.*]] = begin_borrow [[ARG2]]
// CHECK-NEXT:    [[ARG2_COPY:%.*]] = copy_value [[BORROWED_ARG2]]
// CHECK-NEXT:    [[BORROWED_ARG3:%.*]] = begin_borrow [[ARG3]]
// CHECK-NEXT:    [[ARG3_COPY:%.*]] = copy_value [[BORROWED_ARG3]]
// CHECK:         [[ENUM_CASE:%.*]] = function_ref @$S13indirect_enum5TreeAO6BranchyACyxGAE_AEtcAEmlF
// CHECK-NEXT:    [[BRANCH:%.*]] = apply [[ENUM_CASE]]<T>([[ARG2_COPY]], [[ARG3_COPY]], [[METATYPE]])
// CHECK-NEXT:    end_borrow [[BORROWED_ARG3]] from [[ARG3]]
// CHECK-NEXT:    end_borrow [[BORROWED_ARG2]] from [[ARG2]]
// CHECK-NEXT:    destroy_value [[BRANCH]]
// CHECK-NEXT:    destroy_value [[ARG3]]
// CHECK-NEXT:    destroy_value [[ARG2]]
// CHECK-NEXT:    destroy_addr [[ARG1]]
  let _ = TreeA<T>.Branch(left: l, right: r)

}
// CHECK: // end sil function '$S13indirect_enum11TreeA_cases_1l1ryx_AA0C1AOyxGAGtlF'


// CHECK-LABEL: sil hidden @$S13indirect_enum16TreeA_reabstractyyS2icF : $@convention(thin) (@owned @callee_guaranteed (Int) -> Int) -> () {
func TreeA_reabstract(_ f: @escaping (Int) -> Int) {
// CHECK: bb0([[ARG:%.*]] : $@callee_guaranteed (Int) -> Int):
// CHECK:         [[METATYPE:%.*]] = metatype $@thin TreeA<(Int) -> Int>.Type
// CHECK-NEXT:    [[STACK:%.*]] = alloc_stack $@callee_guaranteed (@in Int) -> @out Int
// CHECK-NEXT:    [[BORROWED_ARG:%.*]] = begin_borrow [[ARG]]
// CHECK-NEXT:    [[ARG_COPY:%.*]] = copy_value [[BORROWED_ARG]]
// CHECK:         [[THUNK:%.*]] = function_ref @$SS2iIegyd_S2iIegir_TR : $@convention(thin) (@in Int, @guaranteed @callee_guaranteed (Int) -> Int) -> @out Int
// CHECK-NEXT:    [[FN:%.*]] = partial_apply [callee_guaranteed] [[THUNK]]([[ARG_COPY]])
// CHECK-NEXT:    store [[FN]] to [init] [[STACK]]
// CHECK:         [[ENUM_CASE:%.*]] = function_ref @$S13indirect_enum5TreeAO4LeafyACyxGxcAEmlF
// CHECK-NEXT:    [[LEAF:%.*]] = apply [[ENUM_CASE]]<(Int) -> Int>([[STACK]], [[METATYPE]])
// CHECK-NEXT:    end_borrow [[BORROWED_ARG]] from [[ARG]]
// CHECK-NEXT:    dealloc_stack [[STACK]]
// CHECK-NEXT:    destroy_value [[LEAF]]
// CHECK-NEXT:    destroy_value [[ARG]]
// CHECK: return
  let _ = TreeA<(Int) -> Int>.Leaf(f)
}
// CHECK: } // end sil function '$S13indirect_enum16TreeA_reabstractyyS2icF'

enum TreeB<T> {
  case Nil
  case Leaf(T)
  indirect case Branch(left: TreeB<T>, right: TreeB<T>)
}

// CHECK-LABEL: sil hidden @$S13indirect_enum11TreeB_cases_1l1ryx_AA0C1BOyxGAGtlF
func TreeB_cases<T>(_ t: T, l: TreeB<T>, r: TreeB<T>) {
// CHECK: bb0([[ARG1:%.*]] : $*T, [[ARG2:%.*]] : $*TreeB<T>, [[ARG3:%.*]] : $*TreeB<T>):

// CHECK:         [[NIL:%.*]] = alloc_stack $TreeB<T>
// CHECK-NEXT:    [[METATYPE:%.*]] = metatype $@thin TreeB<T>.Type
// CHECK:         [[ENUM_CASE:%.*]] = function_ref @$S13indirect_enum5TreeBO3NilyACyxGAEmlF
// CHECK-NEXT:    [[NIL_CASE:%.*]] = apply [[ENUM_CASE]]<T>([[NIL]], [[METATYPE]])
// CHECK-NOT:     destroy_value [[NIL_CASE]]
// CHECK-NEXT:    destroy_addr [[NIL]]
// CHECK-NEXT:    dealloc_stack [[NIL]]
  let _ = TreeB<T>.Nil

// CHECK-NEXT:    [[LEAF:%.*]] = alloc_stack $TreeB<T>
// CHECK-NEXT:    [[METATYPE:%.*]] = metatype $@thin TreeB<T>.Type
// CHECK-NEXT:    [[PAYLOAD:%.*]] = alloc_stack $T
// CHECK-NEXT:    copy_addr [[ARG1]] to [initialization] [[PAYLOAD]]
// CHECK:         [[ENUM_CASE:%.*]] = function_ref @$S13indirect_enum5TreeBO4LeafyACyxGxcAEmlF
// CHECK-NEXT:    [[LEAF_CASE:%.*]] = apply [[ENUM_CASE]]<T>([[LEAF]], [[PAYLOAD]], [[METATYPE]])
// CHECK-NEXT:    dealloc_stack [[PAYLOAD]]
// CHECK-NEXT:    destroy_addr [[LEAF]]
// CHECK-NEXT:    dealloc_stack [[LEAF]]
  let _ = TreeB<T>.Leaf(t)

// CHECK-NEXT:    [[BRANCH:%.*]] = alloc_stack $TreeB<T>
// CHECK-NEXT:    [[METATYPE:%.*]] = metatype $@thin TreeB<T>.Type
// CHECK-NEXT:    [[LEFT_STACK:%.*]] = alloc_stack $TreeB<T>
// CHECK-NEXT:    copy_addr [[ARG2]] to [initialization] [[LEFT_STACK]]
// CHECK-NEXT:    [[RIGHT_STACK:%.*]] = alloc_stack $TreeB<T>
// CHECK-NEXT:    copy_addr [[ARG3]] to [initialization] [[RIGHT_STACK]]
// CHECK:         [[ENUM_CASE:%.*]] = function_ref @$S13indirect_enum5TreeBO6BranchyACyxGAE_AEtcAEmlF
// CHECK-NEXT:    [[BRANCH_CASE:%.*]] = apply [[ENUM_CASE]]<T>([[BRANCH]], [[LEFT_STACK]], [[RIGHT_STACK]], [[METATYPE]])
// CHECK-NEXT:    dealloc_stack [[RIGHT_STACK]]
// CHECK-NEXT:    dealloc_stack [[LEFT_STACK]]
// CHECK-NEXT:    destroy_addr [[BRANCH]]
// CHECK-NEXT:    dealloc_stack [[BRANCH]]
// CHECK-NEXT:    destroy_addr [[ARG3]]
// CHECK-NEXT:    destroy_addr [[ARG2]]
// CHECK-NEXT:    destroy_addr [[ARG1]]
  let _ = TreeB<T>.Branch(left: l, right: r)

// CHECK:         return

}

// CHECK-LABEL: sil hidden @$S13indirect_enum13TreeInt_cases_1l1rySi_AA0cD0OAFtF : $@convention(thin) (Int, @owned TreeInt, @owned TreeInt) -> ()
func TreeInt_cases(_ t: Int, l: TreeInt, r: TreeInt) {
// CHECK: bb0([[ARG1:%.*]] : $Int, [[ARG2:%.*]] : $TreeInt, [[ARG3:%.*]] : $TreeInt):

// CHECK:         [[METATYPE:%.*]] = metatype $@thin TreeInt.Type
// CHECK:         [[ENUM_CASE:%.*]] = function_ref @$S13indirect_enum7TreeIntO3NilyA2CmF
// CHECK-NEXT:    [[NIL_CASE:%.*]] = apply [[ENUM_CASE]]([[METATYPE]])
// CHECK-NEXT:    destroy_value [[NIL_CASE]]
  let _ = TreeInt.Nil

// CHECK-NEXT:    [[METATYPE:%.*]] = metatype $@thin TreeInt.Type
// CHECK:         [[ENUM_CASE:%.*]] = function_ref @$S13indirect_enum7TreeIntO4LeafyACSicACmF
// CHECK-NEXT:    [[LEAF_CASE:%.*]] = apply [[ENUM_CASE]]([[ARG1]], [[METATYPE]])
// CHECK-NEXT:    destroy_value [[LEAF_CASE]]
  let _ = TreeInt.Leaf(t)

// CHECK-NEXT:    [[METATYPE:%.*]] = metatype $@thin TreeInt.Type
// CHECK-NEXT:    [[BORROWED_ARG2:%.*]] = begin_borrow [[ARG2]]
// CHECK-NEXT:    [[ARG2_COPY:%.*]] = copy_value [[BORROWED_ARG2]]
// CHECK-NEXT:    [[BORROWED_ARG3:%.*]] = begin_borrow [[ARG3]]
// CHECK-NEXT:    [[ARG3_COPY:%.*]] = copy_value [[BORROWED_ARG3]]
// CHECK:         [[ENUM_CASE:%.*]] = function_ref @$S13indirect_enum7TreeIntO6BranchyA2C_ACtcACmF
// CHECK-NEXT:    [[BRANCH_CASE:%.*]] = apply [[ENUM_CASE]]([[ARG2_COPY]], [[ARG3_COPY]], [[METATYPE]])
// CHECK-NEXT:    end_borrow [[BORROWED_ARG3]] from [[ARG3]]
// CHECK-NEXT:    end_borrow [[BORROWED_ARG2]] from [[ARG2]]
// CHECK-NEXT:    destroy_value [[BRANCH_CASE]]
// CHECK-NEXT:    destroy_value [[ARG3]]
// CHECK-NEXT:    destroy_value [[ARG2]]
  let _ = TreeInt.Branch(left: l, right: r)
}
// CHECK: } // end sil function '$S13indirect_enum13TreeInt_cases_1l1rySi_AA0cD0OAFtF'

enum TreeInt {
  case Nil
  case Leaf(Int)
  indirect case Branch(left: TreeInt, right: TreeInt)
}


enum TrivialButIndirect {
  case Direct(Int)
  indirect case Indirect(Int)
}

func a() {}
func b<T>(_ x: T) {}
func c<T>(_ x: T, _ y: T) {}
func d() {}

// CHECK-LABEL: sil hidden @$S13indirect_enum11switchTreeAyyAA0D1AOyxGlF : $@convention(thin) <T> (@owned TreeA<T>) -> () {
func switchTreeA<T>(_ x: TreeA<T>) {
  // CHECK: bb0([[ARG:%.*]] : $TreeA<T>):
  // --           x +2
  // CHECK:       [[BORROWED_ARG:%.*]] = begin_borrow [[ARG]]
  // CHECK:       [[ARG_COPY:%.*]] = copy_value [[BORROWED_ARG]]
  // CHECK:       switch_enum [[ARG_COPY]] : $TreeA<T>,
  // CHECK:          case #TreeA.Nil!enumelt: [[NIL_CASE:bb1]],
  // CHECK:          case #TreeA.Leaf!enumelt.1: [[LEAF_CASE:bb2]],
  // CHECK:          case #TreeA.Branch!enumelt.1: [[BRANCH_CASE:bb3]],
  switch x {
  // CHECK:     [[NIL_CASE]]:
  // CHECK:       end_borrow [[BORROWED_ARG]] from [[ARG]]
  // CHECK:       function_ref @$S13indirect_enum1ayyF
  // CHECK:       br [[OUTER_CONT:bb[0-9]+]]
  case .Nil:
    a()
  // CHECK:     [[LEAF_CASE]]([[LEAF_BOX:%.*]] : $<τ_0_0> { var τ_0_0 } <T>):
  // CHECK:       [[VALUE:%.*]] = project_box [[LEAF_BOX]]
  // CHECK:       copy_addr [[VALUE]] to [initialization] [[X:%.*]] : $*T
  // CHECK:       function_ref @$S13indirect_enum1b{{[_0-9a-zA-Z]*}}F
  // CHECK:       destroy_addr [[X]]
  // CHECK:       dealloc_stack [[X]]
  // --           x +1
  // CHECK:       destroy_value [[LEAF_BOX]]
  // CHECK:       end_borrow [[BORROWED_ARG]] from [[ARG]]
  // CHECK:       br [[OUTER_CONT]]
  case .Leaf(let x):
    b(x)

  // CHECK:     [[BRANCH_CASE]]([[NODE_BOX:%.*]] : $<τ_0_0> { var (left: TreeA<τ_0_0>, right: TreeA<τ_0_0>) } <T>):
  // CHECK:       [[TUPLE_ADDR:%.*]] = project_box [[NODE_BOX]]
  // CHECK:       [[TUPLE:%.*]] = load_borrow [[TUPLE_ADDR]]
  // CHECK:       [[LEFT:%.*]] = tuple_extract [[TUPLE]] {{.*}}, 0
  // CHECK:       [[RIGHT:%.*]] = tuple_extract [[TUPLE]] {{.*}}, 1
  // CHECK:       switch_enum [[LEFT]] : $TreeA<T>,
  // CHECK:          case #TreeA.Leaf!enumelt.1: [[LEAF_CASE_LEFT:bb[0-9]+]],
  // CHECK:          default [[FAIL_LEFT:bb[0-9]+]]

  // CHECK:     [[LEAF_CASE_LEFT]]([[LEFT_LEAF_BOX:%.*]] : $<τ_0_0> { var τ_0_0 } <T>):
  // CHECK:       [[LEFT_LEAF_VALUE:%.*]] = project_box [[LEFT_LEAF_BOX]]
  // CHECK:       switch_enum [[RIGHT]] : $TreeA<T>,
  // CHECK:          case #TreeA.Leaf!enumelt.1: [[LEAF_CASE_RIGHT:bb[0-9]+]],
  // CHECK:          default [[FAIL_RIGHT:bb[0-9]+]]

  // CHECK:     [[LEAF_CASE_RIGHT]]([[RIGHT_LEAF_BOX:%.*]] : $<τ_0_0> { var τ_0_0 } <T>):
  // CHECK:       [[RIGHT_LEAF_VALUE:%.*]] = project_box [[RIGHT_LEAF_BOX]]
  // CHECK:       copy_addr [[LEFT_LEAF_VALUE]]
  // CHECK:       copy_addr [[RIGHT_LEAF_VALUE]]
  // --           x +1
  // CHECK:       destroy_value [[NODE_BOX]]
  // CHECK:       end_borrow [[BORROWED_ARG]] from [[ARG]]
  // CHECK:       br [[OUTER_CONT]]

  // CHECK:     [[FAIL_RIGHT]]:
  // CHECK:       br [[DEFAULT:bb[0-9]+]]

  // CHECK:     [[FAIL_LEFT]]:
  // CHECK:       br [[DEFAULT]]

  case .Branch(.Leaf(let x), .Leaf(let y)):
    c(x, y)

  // CHECK:     [[DEFAULT]]:
  // --           x +1
  // CHECK:       destroy_value [[ARG_COPY]]
  // CHECK:       end_borrow [[BORROWED_ARG]] from [[ARG]]
  default:
    d()
  }

  // CHECK:     [[OUTER_CONT:%.*]]:
  // --           x +0
  // CHECK:       destroy_value [[ARG]] : $TreeA<T>
}
// CHECK: } // end sil function '$S13indirect_enum11switchTreeAyyAA0D1AOyxGlF'

// CHECK-LABEL: sil hidden @$S13indirect_enum11switchTreeB{{[_0-9a-zA-Z]*}}F
func switchTreeB<T>(_ x: TreeB<T>) {
  // CHECK:       copy_addr %0 to [initialization] [[SCRATCH:%.*]] :
  // CHECK:       switch_enum_addr [[SCRATCH]]
  switch x {

  // CHECK:     bb{{.*}}:
  // CHECK:       destroy_addr [[SCRATCH]]
  // CHECK:       dealloc_stack [[SCRATCH]]
  // CHECK:       function_ref @$S13indirect_enum1ayyF
  // CHECK:       br [[OUTER_CONT:bb[0-9]+]]
  case .Nil:
    a()

  // CHECK:     bb{{.*}}:
  // CHECK:       copy_addr [[SCRATCH]] to [initialization] [[LEAF_COPY:%.*]] :
  // CHECK:       [[LEAF_ADDR:%.*]] = unchecked_take_enum_data_addr [[LEAF_COPY]]
  // CHECK:       copy_addr [take] [[LEAF_ADDR]] to [initialization] [[LEAF:%.*]] :
  // CHECK:       function_ref @$S13indirect_enum1b{{[_0-9a-zA-Z]*}}F
  // CHECK:       destroy_addr [[LEAF]]
  // CHECK:       dealloc_stack [[LEAF]]
  // CHECK-NOT:   destroy_addr [[LEAF_COPY]]
  // CHECK:       dealloc_stack [[LEAF_COPY]]
  // CHECK:       destroy_addr [[SCRATCH]]
  // CHECK:       dealloc_stack [[SCRATCH]]
  // CHECK:       br [[OUTER_CONT]]
  case .Leaf(let x):
    b(x)

  // CHECK:     bb{{.*}}:
  // CHECK:       copy_addr [[SCRATCH]] to [initialization] [[TREE_COPY:%.*]] :
  // CHECK:       [[TREE_ADDR:%.*]] = unchecked_take_enum_data_addr [[TREE_COPY]]
  // --           box +1 immutable
  // CHECK:       [[BOX:%.*]] = load [take] [[TREE_ADDR]]
  // CHECK:       [[TUPLE:%.*]] = project_box [[BOX]]
  // CHECK:       [[LEFT:%.*]] = tuple_element_addr [[TUPLE]]
  // CHECK:       [[RIGHT:%.*]] = tuple_element_addr [[TUPLE]]
  // CHECK:       switch_enum_addr [[LEFT]] {{.*}}, default [[LEFT_FAIL:bb[0-9]+]]

  // CHECK:     bb{{.*}}:
  // CHECK:       copy_addr [[LEFT]] to [initialization] [[LEFT_COPY:%.*]] :
  // CHECK:       [[LEFT_LEAF:%.*]] = unchecked_take_enum_data_addr [[LEFT_COPY]] : $*TreeB<T>, #TreeB.Leaf
  // CHECK:       switch_enum_addr [[RIGHT]] {{.*}}, default [[RIGHT_FAIL:bb[0-9]+]]

  // CHECK:     bb{{.*}}:
  // CHECK:       copy_addr [[RIGHT]] to [initialization] [[RIGHT_COPY:%.*]] :
  // CHECK:       [[RIGHT_LEAF:%.*]] = unchecked_take_enum_data_addr [[RIGHT_COPY]] : $*TreeB<T>, #TreeB.Leaf
  // CHECK:       copy_addr [take] [[LEFT_LEAF]] to [initialization] [[X:%.*]] :
  // CHECK:       copy_addr [take] [[RIGHT_LEAF]] to [initialization] [[Y:%.*]] :
  // CHECK:       function_ref @$S13indirect_enum1c{{[_0-9a-zA-Z]*}}F
  // CHECK:       destroy_addr [[Y]]
  // CHECK:       dealloc_stack [[Y]]
  // CHECK:       destroy_addr [[X]]
  // CHECK:       dealloc_stack [[X]]
  // CHECK-NOT:   destroy_addr [[RIGHT_COPY]]
  // CHECK:       dealloc_stack [[RIGHT_COPY]]
  // CHECK-NOT:   destroy_addr [[LEFT_COPY]]
  // CHECK:       dealloc_stack [[LEFT_COPY]]
  // --           box +0
  // CHECK:       destroy_value [[BOX]]
  // CHECK-NOT:   destroy_addr [[TREE_COPY]]
  // CHECK:       dealloc_stack [[TREE_COPY]]
  // CHECK:       destroy_addr [[SCRATCH]]
  // CHECK:       dealloc_stack [[SCRATCH]]
  case .Branch(.Leaf(let x), .Leaf(let y)):
    c(x, y)

  // CHECK:     [[RIGHT_FAIL]]:
  // CHECK:       destroy_addr [[LEFT_LEAF]]
  // CHECK-NOT:   destroy_addr [[LEFT_COPY]]
  // CHECK:       dealloc_stack [[LEFT_COPY]]
  // CHECK:       destroy_value [[BOX]]
  // CHECK-NOT:   destroy_addr [[TREE_COPY]]
  // CHECK:       dealloc_stack [[TREE_COPY]]
  // CHECK:       br [[INNER_CONT:bb[0-9]+]]

  // CHECK:     [[LEFT_FAIL]]:
  // CHECK:       destroy_value [[BOX]]
  // CHECK-NOT:   destroy_addr [[TREE_COPY]]
  // CHECK:       dealloc_stack [[TREE_COPY]]
  // CHECK:       br [[INNER_CONT:bb[0-9]+]]

  // CHECK:     [[INNER_CONT]]:
  // CHECK:       destroy_addr [[SCRATCH]]
  // CHECK:       dealloc_stack [[SCRATCH]]
  // CHECK:       function_ref @$S13indirect_enum1dyyF
  // CHECK:       br [[OUTER_CONT]]
  default:
    d()
  }
  // CHECK:     [[OUTER_CONT]]:
  // CHECK:       destroy_addr %0
}

// CHECK-LABEL: sil hidden @$S13indirect_enum10guardTreeA{{[_0-9a-zA-Z]*}}F
func guardTreeA<T>(_ tree: TreeA<T>) {
  // CHECK: bb0([[ARG:%.*]] : $TreeA<T>):
  do {
    // CHECK:   [[BORROWED_ARG:%.*]] = begin_borrow [[ARG]]
    // CHECK:   [[ARG_COPY:%.*]] = copy_value [[BORROWED_ARG]]
    // CHECK:   switch_enum [[ARG_COPY]] : $TreeA<T>, case #TreeA.Nil!enumelt: [[YES:bb[0-9]+]], default [[NO:bb[0-9]+]]
    // CHECK: [[NO]]([[ORIGINAL_VALUE:%.*]] : $TreeA<T>):
    // CHECK:   destroy_value [[ORIGINAL_VALUE]]
    // CHECK:   end_borrow [[BORROWED_ARG]] from [[ARG]]
    // CHECK: [[YES]]:
    // CHECK:   end_borrow [[BORROWED_ARG]] from [[ARG]]
    guard case .Nil = tree else { return }

    // CHECK:   [[X:%.*]] = alloc_stack $T
    // CHECK:   [[BORROWED_ARG_2:%.*]] = begin_borrow [[ARG]]
    // CHECK:   [[ARG_COPY:%.*]] = copy_value [[BORROWED_ARG_2]]
    // CHECK:   switch_enum [[ARG_COPY]] : $TreeA<T>, case #TreeA.Leaf!enumelt.1: [[YES:bb[0-9]+]], default [[NO:bb[0-9]+]]
    // CHECK: [[NO]]([[ORIGINAL_VALUE:%.*]] : $TreeA<T>):
    // CHECK:   destroy_value [[ORIGINAL_VALUE]]
    // CHECK:   end_borrow [[BORROWED_ARG_2]] from [[ARG]]
    // CHECK: [[YES]]([[BOX:%.*]] : $<τ_0_0> { var τ_0_0 } <T>):
    // CHECK:   [[VALUE_ADDR:%.*]] = project_box [[BOX]]
    // CHECK:   [[TMP:%.*]] = alloc_stack
    // CHECK:   copy_addr [[VALUE_ADDR]] to [initialization] [[TMP]]
    // CHECK:   copy_addr [take] [[TMP]] to [initialization] [[X]]
    // CHECK:   destroy_value [[BOX]]
    guard case .Leaf(let x) = tree else { return }

    // CHECK:   [[BORROWED_ARG_3:%.*]] = begin_borrow [[ARG]]
    // CHECK:   [[ARG_COPY:%.*]] = copy_value [[BORROWED_ARG_3]]
    // CHECK:   switch_enum [[ARG_COPY]] : $TreeA<T>, case #TreeA.Branch!enumelt.1: [[YES:bb[0-9]+]], default [[NO:bb[0-9]+]]
    // CHECK: [[NO]]([[ORIGINAL_VALUE:%.*]] : $TreeA<T>):
    // CHECK:   destroy_value [[ORIGINAL_VALUE]]
    // CHECK:   end_borrow [[BORROWED_ARG_3]] from [[ARG]]
    // CHECK: [[YES]]([[BOX:%.*]] : $<τ_0_0> { var (left: TreeA<τ_0_0>, right: TreeA<τ_0_0>) } <T>):
    // CHECK:   [[VALUE_ADDR:%.*]] = project_box [[BOX]]
    // CHECK:   [[TUPLE:%.*]] = load [take] [[VALUE_ADDR]]
    // CHECK:   [[TUPLE_COPY:%.*]] = copy_value [[TUPLE]]
    // CHECK:   [[BORROWED_TUPLE_COPY:%.*]] = begin_borrow [[TUPLE_COPY]]
    // CHECK:   [[L:%.*]] = tuple_extract [[BORROWED_TUPLE_COPY]]
    // CHECK:   [[COPY_L:%.*]] = copy_value [[L]]
    // CHECK:   [[R:%.*]] = tuple_extract [[BORROWED_TUPLE_COPY]]
    // CHECK:   [[COPY_R:%.*]] = copy_value [[R]]
    // CHECK:   end_borrow [[BORROWED_TUPLE_COPY]] from [[TUPLE_COPY]]
    // CHECK:   destroy_value [[TUPLE_COPY]]
    // CHECK:   destroy_value [[BOX]]
    // CHECK:   end_borrow [[BORROWED_ARG_3]] from [[ARG]]
    guard case .Branch(left: let l, right: let r) = tree else { return }

    // CHECK:   destroy_value [[COPY_R]]
    // CHECK:   destroy_value [[COPY_L]]
    // CHECK:   destroy_addr [[X]]
  }

  do {
    // CHECK:   [[BORROWED_ARG:%.*]] = begin_borrow [[ARG]]
    // CHECK:   [[ARG_COPY:%.*]] = copy_value [[BORROWED_ARG]]
    // CHECK:   switch_enum [[ARG_COPY]] : $TreeA<T>, case #TreeA.Nil!enumelt: [[YES:bb[0-9]+]], default [[NO:bb[0-9]+]]
    // CHECK: [[NO]]([[ORIGINAL_VALUE:%.*]] : $TreeA<T>):
    // CHECK:   destroy_value [[ORIGINAL_VALUE]]
    // CHECK:   end_borrow [[BORROWED_ARG]] from [[ARG]]
    // CHECK: [[YES]]:
    // CHECK:   end_borrow [[BORROWED_ARG]] from [[ARG]]
    if case .Nil = tree { }

    // CHECK:   [[X:%.*]] = alloc_stack $T
    // CHECK:   [[BORROWED_ARG:%.*]] = begin_borrow [[ARG]]
    // CHECK:   [[ARG_COPY:%.*]] = copy_value [[BORROWED_ARG]]
    // CHECK:   switch_enum [[ARG_COPY]] : $TreeA<T>, case #TreeA.Leaf!enumelt.1: [[YES:bb[0-9]+]], default [[NO:bb[0-9]+]]
    // CHECK: [[NO]]([[ORIGINAL_VALUE:%.*]] : $TreeA<T>):
    // CHECK:   destroy_value [[ORIGINAL_VALUE]]
    // CHECK:   end_borrow [[BORROWED_ARG]] from [[ARG]]
    // CHECK: [[YES]]([[BOX:%.*]] : $<τ_0_0> { var τ_0_0 } <T>):
    // CHECK:   [[VALUE_ADDR:%.*]] = project_box [[BOX]]
    // CHECK:   [[TMP:%.*]] = alloc_stack
    // CHECK:   copy_addr [[VALUE_ADDR]] to [initialization] [[TMP]]
    // CHECK:   copy_addr [take] [[TMP]] to [initialization] [[X]]
    // CHECK:   destroy_value [[BOX]]
    // CHECK:   end_borrow [[BORROWED_ARG]] from [[ARG]]
    // CHECK:   destroy_addr [[X]]
    if case .Leaf(let x) = tree { }


    // CHECK:   [[BORROWED_ARG:%.*]] = begin_borrow [[ARG]]
    // CHECK:   [[ARG_COPY:%.*]] = copy_value [[BORROWED_ARG]]
    // CHECK:   switch_enum [[ARG_COPY]] : $TreeA<T>, case #TreeA.Branch!enumelt.1: [[YES:bb[0-9]+]], default [[NO:bb[0-9]+]]
    // CHECK: [[NO]]([[ORIGINAL_VALUE:%.*]] : $TreeA<T>):
    // CHECK:   destroy_value [[ORIGINAL_VALUE]]
    // CHECK:   end_borrow [[BORROWED_ARG]] from [[ARG]]
    // CHECK: [[YES]]([[BOX:%.*]] : $<τ_0_0> { var (left: TreeA<τ_0_0>, right: TreeA<τ_0_0>) } <T>):
    // CHECK:   [[VALUE_ADDR:%.*]] = project_box [[BOX]]
    // CHECK:   [[TUPLE:%.*]] = load [take] [[VALUE_ADDR]]
    // CHECK:   [[TUPLE_COPY:%.*]] = copy_value [[TUPLE]]
    // CHECK:   [[BORROWED_TUPLE_COPY:%.*]] = begin_borrow [[TUPLE_COPY]]
    // CHECK:   [[L:%.*]] = tuple_extract [[BORROWED_TUPLE_COPY]]
    // CHECK:   [[COPY_L:%.*]] = copy_value [[L]]
    // CHECK:   [[R:%.*]] = tuple_extract [[BORROWED_TUPLE_COPY]]
    // CHECK:   [[COPY_R:%.*]] = copy_value [[R]]
    // CHECK:   end_borrow [[BORROWED_TUPLE_COPY]] from [[TUPLE_COPY]]
    // CHECK:   destroy_value [[TUPLE_COPY]]
    // CHECK:   destroy_value [[BOX]]
    // CHECK:   end_borrow [[BORROWED_ARG]] from [[ARG]]
    // CHECK:   destroy_value [[COPY_R]]
    // CHECK:   destroy_value [[COPY_L]]
    if case .Branch(left: let l, right: let r) = tree { }
  }
}

// CHECK-LABEL: sil hidden @$S13indirect_enum10guardTreeB{{[_0-9a-zA-Z]*}}F
func guardTreeB<T>(_ tree: TreeB<T>) {
  do {
    // CHECK:   copy_addr %0 to [initialization] [[TMP:%.*]] :
    // CHECK:   switch_enum_addr [[TMP]] : $*TreeB<T>, case #TreeB.Nil!enumelt: [[YES:bb[0-9]+]], default [[NO:bb[0-9]+]]
    // CHECK: [[NO]]:
    // CHECK:   destroy_addr [[TMP]]
    // CHECK: [[YES]]:
    // CHECK:   destroy_addr [[TMP]]
    guard case .Nil = tree else { return }

    // CHECK:   [[X:%.*]] = alloc_stack $T
    // CHECK:   copy_addr %0 to [initialization] [[TMP:%.*]] :
    // CHECK:   switch_enum_addr [[TMP]] : $*TreeB<T>, case #TreeB.Leaf!enumelt.1: [[YES:bb[0-9]+]], default [[NO:bb[0-9]+]]
    // CHECK: [[NO]]:
    // CHECK:   destroy_addr [[TMP]]
    // CHECK: [[YES]]:
    // CHECK:   [[VALUE:%.*]] = unchecked_take_enum_data_addr [[TMP]]
    // CHECK:   copy_addr [take] [[VALUE]] to [initialization] [[X]]
    // CHECK:   dealloc_stack [[TMP]]
    guard case .Leaf(let x) = tree else { return }

    // CHECK:   [[L:%.*]] = alloc_stack $TreeB
    // CHECK:   [[R:%.*]] = alloc_stack $TreeB
    // CHECK:   copy_addr %0 to [initialization] [[TMP:%.*]] :
    // CHECK:   switch_enum_addr [[TMP]] : $*TreeB<T>, case #TreeB.Branch!enumelt.1: [[YES:bb[0-9]+]], default [[NO:bb[0-9]+]]
    // CHECK: [[NO]]:
    // CHECK:   destroy_addr [[TMP]]
    // CHECK: [[YES]]:
    // CHECK:   [[BOX_ADDR:%.*]] = unchecked_take_enum_data_addr [[TMP]]
    // CHECK:   [[BOX:%.*]] = load [take] [[BOX_ADDR]]
    // CHECK:   [[TUPLE_ADDR:%.*]] = project_box [[BOX]]
    // CHECK:   copy_addr [[TUPLE_ADDR]] to [initialization] [[TUPLE_COPY:%.*]] :
    // CHECK:   [[L_COPY:%.*]] = tuple_element_addr [[TUPLE_COPY]]
    // CHECK:   copy_addr [take] [[L_COPY]] to [initialization] [[L]]
    // CHECK:   [[R_COPY:%.*]] = tuple_element_addr [[TUPLE_COPY]]
    // CHECK:   copy_addr [take] [[R_COPY]] to [initialization] [[R]]
    // CHECK:   destroy_value [[BOX]]
    guard case .Branch(left: let l, right: let r) = tree else { return }

    // CHECK:   destroy_addr [[R]]
    // CHECK:   destroy_addr [[L]]
    // CHECK:   destroy_addr [[X]]
  }

  do {
    // CHECK:   copy_addr %0 to [initialization] [[TMP:%.*]] :
    // CHECK:   switch_enum_addr [[TMP]] : $*TreeB<T>, case #TreeB.Nil!enumelt: [[YES:bb[0-9]+]], default [[NO:bb[0-9]+]]
    // CHECK: [[NO]]:
    // CHECK:   destroy_addr [[TMP]]
    // CHECK: [[YES]]:
    // CHECK:   destroy_addr [[TMP]]
    if case .Nil = tree { }

    // CHECK:   [[X:%.*]] = alloc_stack $T
    // CHECK:   copy_addr %0 to [initialization] [[TMP:%.*]] :
    // CHECK:   switch_enum_addr [[TMP]] : $*TreeB<T>, case #TreeB.Leaf!enumelt.1: [[YES:bb[0-9]+]], default [[NO:bb[0-9]+]]
    // CHECK: [[NO]]:
    // CHECK:   destroy_addr [[TMP]]
    // CHECK: [[YES]]:
    // CHECK:   [[VALUE:%.*]] = unchecked_take_enum_data_addr [[TMP]]
    // CHECK:   copy_addr [take] [[VALUE]] to [initialization] [[X]]
    // CHECK:   dealloc_stack [[TMP]]
    // CHECK:   destroy_addr [[X]]
    if case .Leaf(let x) = tree { }

    // CHECK:   [[L:%.*]] = alloc_stack $TreeB
    // CHECK:   [[R:%.*]] = alloc_stack $TreeB
    // CHECK:   copy_addr %0 to [initialization] [[TMP:%.*]] :
    // CHECK:   switch_enum_addr [[TMP]] : $*TreeB<T>, case #TreeB.Branch!enumelt.1: [[YES:bb[0-9]+]], default [[NO:bb[0-9]+]]
    // CHECK: [[NO]]:
    // CHECK:   destroy_addr [[TMP]]
    // CHECK: [[YES]]:
    // CHECK:   [[BOX_ADDR:%.*]] = unchecked_take_enum_data_addr [[TMP]]
    // CHECK:   [[BOX:%.*]] = load [take] [[BOX_ADDR]]
    // CHECK:   [[TUPLE_ADDR:%.*]] = project_box [[BOX]]
    // CHECK:   copy_addr [[TUPLE_ADDR]] to [initialization] [[TUPLE_COPY:%.*]] :
    // CHECK:   [[L_COPY:%.*]] = tuple_element_addr [[TUPLE_COPY]]
    // CHECK:   copy_addr [take] [[L_COPY]] to [initialization] [[L]]
    // CHECK:   [[R_COPY:%.*]] = tuple_element_addr [[TUPLE_COPY]]
    // CHECK:   copy_addr [take] [[R_COPY]] to [initialization] [[R]]
    // CHECK:   destroy_value [[BOX]]
    // CHECK:   destroy_addr [[R]]
    // CHECK:   destroy_addr [[L]]
    if case .Branch(left: let l, right: let r) = tree { }
  }
}

// SEMANTIC ARC TODO: This test needs to be made far more comprehensive.
// CHECK-LABEL: sil hidden @$S13indirect_enum35dontDisableCleanupOfIndirectPayloadyyAA010TrivialButG0OF : $@convention(thin) (@owned TrivialButIndirect) -> () {
func dontDisableCleanupOfIndirectPayload(_ x: TrivialButIndirect) {
  // CHECK: bb0([[ARG:%.*]] : $TrivialButIndirect):
  // CHECK:   [[BORROWED_ARG:%.*]] = begin_borrow [[ARG]]
  // CHECK:   [[ARG_COPY:%.*]] = copy_value [[BORROWED_ARG]]
  // CHECK:   switch_enum [[ARG_COPY]] : $TrivialButIndirect, case #TrivialButIndirect.Direct!enumelt.1:  [[YES:bb[0-9]+]], case #TrivialButIndirect.Indirect!enumelt.1: [[NO:bb[0-9]+]]
  //
  // CHECK: [[NO]]([[PAYLOAD:%.*]] : ${ var Int }):
  // CHECK:   destroy_value [[PAYLOAD]]
  // CHECK:   end_borrow [[BORROWED_ARG]] from [[ARG]]
  guard case .Direct(let foo) = x else { return }

  // CHECK: [[YES]]({{%.*}} : $Int):
  // CHECK:   end_borrow [[BORROWED_ARG]] from [[ARG]]
  // CHECK:   [[BORROWED_ARG:%.*]] = begin_borrow [[ARG]]
  // CHECK:   [[ARG_COPY:%.*]] = copy_value [[BORROWED_ARG]]
  // CHECK:   switch_enum [[ARG_COPY]] : $TrivialButIndirect, case #TrivialButIndirect.Indirect!enumelt.1:  [[YES:bb[0-9]+]], case #TrivialButIndirect.Direct!enumelt.1: [[NO:bb[0-9]+]]

  // CHECK: [[NO]]({{%.*}} : $Int):
  // CHECK-NOT: destroy_value
  // CHECK:   end_borrow [[BORROWED_ARG]] from [[ARG]]

  // CHECK: [[YES]]([[BOX:%.*]] : ${ var Int }):
  // CHECK:   destroy_value [[BOX]]

  guard case .Indirect(let bar) = x else { return }
}
// CHECK: } // end sil function '$S13indirect_enum35dontDisableCleanupOfIndirectPayloadyyAA010TrivialButG0OF'
