// RUN: %target-swift-frontend -emit-silgen %s -module-name test -swift-version 5  -disable-availability-checking | %FileCheck --enable-var-scope %s --implicit-check-not 'hop_to_executor {{%[0-9]+}}'
// REQUIRES: concurrency

@propertyWrapper
struct GiftWrapped<T> {
    private var _stored: T
    private var numReads : Int
    init(initially: T) {
        self._stored = initially
    }

    var wrappedValue: T {
        mutating get {
            numReads += 1
            return _stored
        }
        set { _stored = newValue }
    }
}

actor Birb {
    private var _storage : Int = 24

    var feathers : Int {
        _read {
            yield _storage
        }
        _modify {
            yield &_storage
        }
    }
}

actor Cat {
    @GlobalCat var leader : String {
        get { "Tiger" }
    }

    var storedBool : Bool = false

    private(set) var computedSweater : Sweater {
        get { return Sweater(self) }
        set {}
    }

    subscript(_ x : Int) -> Cat {
        get { self }
        set {}
    }

    var friend : Cat = Cat()

    var maybeFriend : Cat?

    @GiftWrapped<Birb>(initially: Birb())
    var bestFriend : Birb
}

struct Sweater : Sendable {
    let owner : Cat
    init (_ owner : Cat) {
        self.owner = owner
    }
}

class CatBox {
    var cat : Cat = Cat()
}

@globalActor
struct GlobalCat {
    static let shared : Cat = Cat()
}

@GlobalCat var globalBool : Bool = false

@GlobalCat var someBirb : Birb {
    get { Birb() }
    set {}
}

// CHECK-LABEL: sil hidden [ossa] @$s4test015accessSweaterOfC03catAA0C0VAA3CatC_tYaF : $@convention(thin) @async (@guaranteed Cat) -> @owned Sweater {
// CHECK:  bb0([[CAT:%[0-9]+]] : @guaranteed $Cat):
// CHECK:    [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK:    hop_to_executor [[GENERIC_EXEC]] :
// CHECK:    [[CAT_GETTER:%[0-9]+]] = class_method [[CAT]] : $Cat, #Cat.computedSweater!getter : (isolated Cat) -> () -> Sweater, $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Sweater
// CHECK:    hop_to_executor [[CAT]] : $Cat
// CHECK:    [[SWEATER1_REF:%[0-9]+]] = apply [[CAT_GETTER]]([[CAT]]) : $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Sweater
// CHECK:    hop_to_executor [[GENERIC_EXEC]]
// CHECK:    [[SWEATER1:%[0-9]+]] = begin_borrow [[SWEATER1_REF]] : $Sweater
// CHECK:    [[SWEATER1_OWNER:%[0-9]+]] = struct_extract [[SWEATER1]] : $Sweater, #Sweater.owner
// CHECK:    [[CAT2_REF:%[0-9]+]] = copy_value [[SWEATER1_OWNER]] : $Cat
// CHECK:    end_borrow [[SWEATER1]] : $Sweater
// CHECK:    destroy_value [[SWEATER1_REF]] : $Sweater

// CHECK:    [[CAT2_FOR_LOAD:%[0-9]+]] = begin_borrow [[CAT2_REF]] : $Cat
// CHECK:    [[CAT2_GETTER:%[0-9]+]] = class_method [[CAT2_FOR_LOAD]] : $Cat, #Cat.computedSweater!getter : (isolated Cat) -> () -> Sweater, $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Sweater
// CHECK:    hop_to_executor [[CAT2_FOR_LOAD]] : $Cat
// CHECK:    [[SWEATER2_OWNER:%[0-9]+]] = apply [[CAT2_GETTER]]([[CAT2_FOR_LOAD]]) : $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Sweater
// CHECK:    hop_to_executor [[GENERIC_EXEC]]
// CHECK:    end_borrow [[CAT2_FOR_LOAD]] : $Cat

// CHECK:    destroy_value [[CAT2_REF]] : $Cat
// CHECK:    return [[SWEATER2_OWNER]] : $Sweater
// CHECK: } // end sil function '$s4test015accessSweaterOfC03catAA0C0VAA3CatC_tYaF'
func accessSweaterOfSweater(cat : Cat) async -> Sweater {
    // note that Sweater is not an actor!
    return await cat.computedSweater.owner.computedSweater
}

// CHECK-LABEL: sil hidden [ossa] @$s4test26accessGlobalIsolatedMember3catSSAA3CatC_tYaF : $@convention(thin) @async (@guaranteed Cat) -> @owned String {
// CHECK:  bb0([[CAT:%[0-9]+]] : @guaranteed $Cat):
// CHECK:    [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK:    hop_to_executor [[GENERIC_EXEC]] :

// CHECK:    [[GETTER:%[0-9]+]] = class_method [[CAT]] : $Cat, #Cat.leader!getter : (Cat) -> () -> String, $@convention(method) (@guaranteed Cat) -> @owned String

// CHECK:    [[GLOBAL_CAT_SHARED:%[0-9]+]] = function_ref @$s4test9GlobalCatV6sharedAA0C0Cvau : $@convention(thin) () -> Builtin.RawPointer
// CHECK:    [[GLOBAL_CAT_RAWPTR:%[0-9]+]] = apply [[GLOBAL_CAT_SHARED]]() : $@convention(thin) () -> Builtin.RawPointer
// CHECK:    [[GLOBAL_CAT_ADDR:%[0-9]+]] = pointer_to_address [[GLOBAL_CAT_RAWPTR]] : $Builtin.RawPointer to [strict] $*Cat
// CHECK:    [[GLOBAL_CAT_REF:%[0-9]+]] = load [copy] [[GLOBAL_CAT_ADDR]] : $*Cat
// CHECK:    [[GLOBAL_CAT:%[0-9]+]] = begin_borrow [[GLOBAL_CAT_REF]] : $Cat

// CHECK:    hop_to_executor [[GLOBAL_CAT]] : $Cat
// CHECK:    [[THE_STRING:%[0-9]+]] = apply [[GETTER]]([[CAT]]) : $@convention(method) (@guaranteed Cat) -> @owned String
// CHECK:    end_borrow [[GLOBAL_CAT]] : $Cat
// CHECK:    destroy_value [[GLOBAL_CAT_REF]] : $Cat
// CHECK:    hop_to_executor [[GENERIC_EXEC]]
// CHECK:    return [[THE_STRING]] : $String
// CHECK: } // end sil function '$s4test26accessGlobalIsolatedMember3catSSAA3CatC_tYaF'
func accessGlobalIsolatedMember(cat : Cat) async -> String {
    return await cat.leader
}


actor Dog {

    // CHECK-LABEL: sil hidden [ossa] @$s4test3DogC15accessGlobalVarSbyYaF : $@convention(method) @async (@sil_isolated @guaranteed Dog) -> Bool {
    // CHECK:   [[GLOBAL_BOOL_ADDR:%[0-9]+]] = global_addr @$s4test10globalBoolSbvp : $*Bool
    // CHECK:   hop_to_executor [[SELF:%[0-9]+]] : $Dog
    // CHECK:   [[SHARED_REF_FN:%[0-9]+]] = function_ref @$s4test9GlobalCatV6sharedAA0C0Cvau : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:   [[SHARED_REF:%[0-9]+]] = apply [[SHARED_REF_FN]]() : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:   [[SHARED_CAT_ADDR:%[0-9]+]] = pointer_to_address [[SHARED_REF]] : $Builtin.RawPointer to [strict] $*Cat
    // CHECK:   [[CAT:%[0-9]+]] = load [copy] [[SHARED_CAT_ADDR]] : $*Cat
    // CHECK:   [[BORROWED_CAT:%[0-9]+]] = begin_borrow [[CAT]] : $Cat

    // CHECK:   hop_to_executor [[BORROWED_CAT]] : $Cat
    // CHECK:   [[GLOBAL_BOOL_ACCESS:%[0-9]+]] = begin_access [read] [dynamic] [[GLOBAL_BOOL_ADDR]] : $*Bool
    // CHECK:   [[THE_BOOL:%[0-9]+]] = load [trivial] [[GLOBAL_BOOL_ACCESS]] : $*Bool
    // CHECK:   end_access [[GLOBAL_BOOL_ACCESS]] : $*Bool

    // CHECK:   hop_to_executor [[SELF]] : $Dog
    // CHECK:   end_borrow [[BORROWED_CAT]] : $Cat
    // CHECK:   destroy_value [[CAT]] : $Cat
    // CHECK:   return [[THE_BOOL]] : $Bool
    // CHECK: } // end sil function '$s4test3DogC15accessGlobalVarSbyYaF'
    func accessGlobalVar() async -> Bool {
        return await globalBool
    }


    // CHECK-LABEL: sil hidden [ossa] @$s4test3DogC24accessGlobalComputedPropSiyYaF : $@convention(method) @async (@sil_isolated @guaranteed Dog) -> Int {
    // CHECK:  bb0([[SELF:%[0-9]+]] : @guaranteed $Dog):
    // CHECK:    hop_to_executor [[SELF]] : $Dog

    // CHECK:    [[SOMEBIRB_GETTER:%[0-9]+]] = function_ref @$s4test8someBirbAA0C0Cvg : $@convention(thin) () -> @owned Birb

    // CHECK:    [[SHARED_REF_FN:%[0-9]+]] = function_ref @$s4test9GlobalCatV6sharedAA0C0Cvau : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:    [[SHARED_REF:%[0-9]+]] = apply [[SHARED_REF_FN]]() : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:    [[SHARED_CAT_ADDR:%[0-9]+]] = pointer_to_address [[SHARED_REF]] : $Builtin.RawPointer to [strict] $*Cat
    // CHECK:    [[CAT:%[0-9]+]] = load [copy] [[SHARED_CAT_ADDR]] : $*Cat
    // CHECK:    [[BORROWED_CAT:%[0-9]+]] = begin_borrow [[CAT]] : $Cat

    // CHECK:    hop_to_executor [[BORROWED_CAT]] : $Cat
    // CHECK:    [[BIRB:%[0-9]+]] = apply [[SOMEBIRB_GETTER]]() : $@convention(thin) () -> @owned Birb

    // CHECK:    hop_to_executor [[SELF]] : $Dog
    // CHECK:    [[BORROWED_BIRB_FOR_LOAD:%[0-9]+]] = begin_borrow [[BIRB]] : $Birb
    // CHECK:    [[FEATHER_GETTER:%[0-9]+]] = class_method [[BORROWED_BIRB_FOR_LOAD]] : $Birb, #Birb.feathers!getter : (isolated Birb) -> () -> Int, $@convention(method) (@sil_isolated @guaranteed Birb) -> Int
    // CHECK:    hop_to_executor [[BORROWED_BIRB_FOR_LOAD]] : $Birb
    // CHECK:    [[THE_INT:%[0-9]+]] = apply [[FEATHER_GETTER]]([[BORROWED_BIRB_FOR_LOAD]]) : $@convention(method) (@sil_isolated @guaranteed Birb) -> Int

    // CHECK:    hop_to_executor [[SELF]] : $Dog
    // CHECK:    return [[THE_INT]] : $Int
    // CHECK: } // end sil function '$s4test3DogC24accessGlobalComputedPropSiyYaF'
    func accessGlobalComputedProp() async -> Int {
        return await someBirb.feathers
    }


    // CHECK-LABEL: sil hidden [ossa] @$s4test3DogC21accessWrappedProperty3catSiAA3CatC_tYaF : $@convention(method) @async (@guaranteed Cat, @sil_isolated @guaranteed Dog) -> Int {
    // CHECK:  bb0([[CAT:%[0-9]+]] : @guaranteed $Cat, [[SELF:%[0-9]+]] : @guaranteed $Dog):
    // CHECK:    hop_to_executor [[SELF]] : $Dog
    // CHECK:    [[CAT_GETTER:%[0-9]+]] = class_method [[CAT]] : $Cat, #Cat.bestFriend!getter : (isolated Cat) -> () -> Birb, $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Birb
    // CHECK:    hop_to_executor [[CAT]] : $Cat
    // CHECK:    [[BIRB_REF:%[0-9]+]] = apply [[CAT_GETTER]]([[CAT]]) : $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Birb
    // CHECK:    hop_to_executor [[SELF]] : $Dog
    // CHECK:    [[BIRB_FOR_LOAD:%[0-9]+]] = begin_borrow [[BIRB_REF]] : $Birb
    // CHECK:    [[BIRB_GETTER:%[0-9]+]] = class_method [[BIRB_FOR_LOAD]] : $Birb, #Birb.feathers!getter : (isolated Birb) -> () -> Int, $@convention(method) (@sil_isolated @guaranteed Birb) -> Int
    // CHECK:    hop_to_executor [[BIRB_FOR_LOAD]] : $Birb
    // CHECK:    [[THE_INT:%[0-9]+]] = apply [[BIRB_GETTER]]([[BIRB_FOR_LOAD]]) : $@convention(method) (@sil_isolated @guaranteed Birb) -> Int
    // CHECK:    hop_to_executor [[SELF]] : $Dog
    // CHECK:    return [[THE_INT]] : $Int
    // CHECK: } // end sil function '$s4test3DogC21accessWrappedProperty3catSiAA3CatC_tYaF'
    func accessWrappedProperty(cat : Cat) async -> Int {
        return await cat.bestFriend.feathers
    }


    // CHECK-LABEL: sil hidden [ossa] @$s4test3DogC16accessFromRValueSbyYaF : $@convention(method) @async (@sil_isolated @guaranteed Dog) -> Bool {
    // CHECK:   hop_to_executor [[SELF:%[0-9]+]] : $Dog
    // CHECK:   [[INIT:%[0-9]+]] = function_ref @$s4test3CatCACycfC : $@convention(method) (@thick Cat.Type) -> @owned Cat
    // CHECK:   [[CAT_REF:%[0-9]+]] = apply [[INIT]]({{%[0-9]+}}) : $@convention(method) (@thick Cat.Type) -> @owned Cat

    // CHECK:   [[CAT_BORROW_FOR_LOAD:%[0-9]+]] = begin_borrow [[CAT_REF]] : $Cat
    // CHECK:   [[GETTER:%[0-9]+]] = class_method [[CAT_BORROW_FOR_LOAD]] : $Cat, #Cat.storedBool!getter : (isolated Cat) -> () -> Bool, $@convention(method) (@sil_isolated @guaranteed Cat) -> Bool
    // CHECK:   hop_to_executor [[CAT_BORROW_FOR_LOAD]] : $Cat
    // CHECK:   [[THE_BOOL:%[0-9]+]] = apply [[GETTER]]([[CAT_BORROW_FOR_LOAD]]) : $@convention(method) (@sil_isolated @guaranteed Cat) -> Bool

    // CHECK:   hop_to_executor [[SELF]] : $Dog
    // CHECK:   return [[THE_BOOL]] : $Bool
    // CHECK: } // end sil function '$s4test3DogC16accessFromRValueSbyYaF'
    func accessFromRValue() async -> Bool {
        return await Cat().storedBool
    }


    // CHECK-LABEL: sil hidden [ossa] @$s4test3DogC23accessFromRValueChainedSbyYaF : $@convention(method) @async (@sil_isolated @guaranteed Dog) -> Bool {
    // CHECK:   hop_to_executor [[SELF:%[0-9]+]] : $Dog
    // CHECK:   [[INIT:%[0-9]+]] = function_ref @$s4test3CatCACycfC : $@convention(method) (@thick Cat.Type) -> @owned Cat
    // CHECK:   [[CAT_REF:%[0-9]+]] = apply [[INIT]]({{%[0-9]+}}) : $@convention(method) (@thick Cat.Type) -> @owned Cat

    // CHECK:   [[CAT_BORROW_FOR_LOAD:%[0-9]+]] = begin_borrow [[CAT_REF]] : $Cat
    // CHECK:   [[FRIEND_GETTER:%[0-9]+]] = class_method [[CAT_BORROW_FOR_LOAD]] : $Cat, #Cat.friend!getter : (isolated Cat) -> () -> Cat, $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Cat
    // CHECK:   hop_to_executor [[CAT_BORROW_FOR_LOAD]] : $Cat
    // CHECK:   [[FRIEND_REF:%[0-9]+]] = apply [[FRIEND_GETTER]]([[CAT_BORROW_FOR_LOAD]]) : $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Cat
    // CHECK:   hop_to_executor [[SELF]] : $Dog
    // CHECK:   end_borrow [[CAT_BORROW_FOR_LOAD]] : $Cat

    // CHECK:   destroy_value [[CAT_REF]] : $Cat

    // CHECK:   [[FRIEND_BORROW_FOR_LOAD:%[0-9]+]] = begin_borrow [[FRIEND_REF]] : $Cat
    // CHECK:   [[BOOL_GETTER:%[0-9]+]] = class_method [[FRIEND_BORROW_FOR_LOAD]] : $Cat, #Cat.storedBool!getter : (isolated Cat) -> () -> Bool, $@convention(method) (@sil_isolated @guaranteed Cat) -> Bool
    // CHECK:   hop_to_executor [[FRIEND_BORROW_FOR_LOAD]] : $Cat
    // CHECK:   [[THE_BOOL:%[0-9]+]] = apply [[BOOL_GETTER]]([[FRIEND_BORROW_FOR_LOAD]]) : $@convention(method) (@sil_isolated @guaranteed Cat) -> Bool

    // CHECK:   hop_to_executor [[SELF]] : $Dog
    // CHECK:   end_borrow [[FRIEND_BORROW_FOR_LOAD]] : $Cat
    // CHECK:   destroy_value [[FRIEND_REF]] : $Cat
    // CHECK:   return [[THE_BOOL]] : $Bool
    // CHECK: } // end sil function '$s4test3DogC23accessFromRValueChainedSbyYaF'
    func accessFromRValueChained() async -> Bool {
        return await Cat().friend.storedBool
    }


    // CHECK-LABEL: sil hidden [ossa] @$s4test3DogC15accessSubscript3catAA3CatCAG_tYaF : $@convention(method) @async (@guaranteed Cat, @sil_isolated @guaranteed Dog) -> @owned Cat {
    // CHECK: bb0([[CAT:%[0-9]+]] : @guaranteed $Cat, [[DOG:%[0-9]+]] : @guaranteed $Dog):
    // CHECK:   hop_to_executor [[DOG]] : $Dog
    // CHECK:   [[INTEGER1:%[0-9]+]] = apply {{%[0-9]+}}({{%[0-9]+}}, {{%[0-9]+}}) : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int

    // CHECK:   [[SUBSCRIPT_FN:%[0-9]+]] = class_method [[CAT]] : $Cat, #Cat.subscript!getter : (isolated Cat) -> (Int) -> Cat, $@convention(method) (Int, @sil_isolated @guaranteed Cat) -> @owned Cat
    // CHECK:   hop_to_executor [[CAT]] : $Cat
    // CHECK:   [[OTHER_CAT:%[0-9]+]] = apply [[SUBSCRIPT_FN]]([[INTEGER1]], [[CAT]]) : $@convention(method) (Int, @sil_isolated @guaranteed Cat) -> @owned Cat

    // CHECK:   hop_to_executor [[DOG]] : $Dog
    // CHECK:   return [[OTHER_CAT]] : $Cat
    // CHECK: } // end sil function '$s4test3DogC15accessSubscript3catAA3CatCAG_tYaF'
    func accessSubscript(cat : Cat) async -> Cat {
        return await cat[1]
    }

    // CHECK-LABEL: sil hidden [ossa] @$s4test3DogC27accessRValueNestedSubscriptAA3CatCyYaF : $@convention(method) @async (@sil_isolated @guaranteed Dog) -> @owned Cat {
    // CHECK:   hop_to_executor [[SELF:%[0-9]+]] : $Dog
    // CHECK:   [[RVALUE_CAT_REF:%[0-9]+]] = apply {{%[0-9]+}}({{%[0-9]+}}) : $@convention(method) (@thick Cat.Type) -> @owned Cat
    // CHECK:   [[LIT_ONE:%[0-9]+]] = integer_literal $Builtin.IntLiteral, 1
    // CHECK:   [[INT_ONE:%[0-9]+]] = apply {{%[0-9]+}}([[LIT_ONE]], {{%[0-9]+}}) : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int

    // CHECK:   [[RVALUE_CAT_FOR_LOAD:%[0-9]+]] = begin_borrow [[RVALUE_CAT_REF]] : $Cat
    // CHECK:   [[RVALUE_CAT_SUBSCRIPT:%[0-9]+]] = class_method [[RVALUE_CAT_FOR_LOAD]] : $Cat, #Cat.subscript!getter : (isolated Cat) -> (Int) -> Cat, $@convention(method) (Int, @sil_isolated @guaranteed Cat) -> @owned Cat
    // CHECK:   hop_to_executor [[RVALUE_CAT_FOR_LOAD]] : $Cat
    // CHECK:   [[FIRST_CAT_REF:%[0-9]+]] = apply [[RVALUE_CAT_SUBSCRIPT]]([[INT_ONE]], [[RVALUE_CAT_FOR_LOAD]]) : $@convention(method) (Int, @sil_isolated @guaranteed Cat) -> @owned Cat
    // CHECK:   hop_to_executor [[SELF]] : $Dog
    // CHECK:   end_borrow [[RVALUE_CAT_FOR_LOAD]] : $Cat

    // CHECK:   destroy_value [[RVALUE_CAT_REF]] : $Cat
    // CHECK:   [[LIT_TWO:%[0-9]+]] = integer_literal $Builtin.IntLiteral, 2
    // CHECK:   [[INT_TWO:%[0-9]+]] = apply {{%[0-9]+}}([[LIT_TWO]], {{%[0-9]+}}) : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int

    // CHECK:   [[FIRST_CAT_FOR_LOAD:%[0-9]+]] = begin_borrow [[FIRST_CAT_REF]] : $Cat
    // CHECK:   [[FIRST_CAT_SUBSCRIPT:%[0-9]+]] = class_method [[FIRST_CAT_FOR_LOAD]] : $Cat, #Cat.subscript!getter : (isolated Cat) -> (Int) -> Cat, $@convention(method) (Int, @sil_isolated @guaranteed Cat) -> @owned Cat
    // CHECK:   hop_to_executor [[FIRST_CAT_FOR_LOAD]] : $Cat
    // CHECK:   [[SECOND_CAT_REF:%[0-9]+]] = apply [[FIRST_CAT_SUBSCRIPT]]([[INT_TWO]], [[FIRST_CAT_FOR_LOAD]]) : $@convention(method) (Int, @sil_isolated @guaranteed Cat) -> @owned Cat
    // CHECK:   hop_to_executor [[SELF]] : $Dog
    // CHECK:   end_borrow [[FIRST_CAT_FOR_LOAD]] : $Cat

    // CHECK:   destroy_value [[FIRST_CAT_REF]] : $Cat
    // CHECK:   return [[SECOND_CAT_REF]] : $Cat
    // CHECK: } // end sil function '$s4test3DogC27accessRValueNestedSubscriptAA3CatCyYaF'
    func accessRValueNestedSubscript() async -> Cat {
        return await Cat()[1][2]
    }


    // CHECK-LABEL: sil hidden [ossa] @$s4test3DogC33accessStoredPropFromRefProjection3boxSbAA6CatBoxC_tYaF : $@convention(method) @async (@guaranteed CatBox, @sil_isolated @guaranteed Dog) -> Bool {
    // CHECK:   bb0([[BOX:%[0-9]+]] : @guaranteed $CatBox, [[SELF:%[0-9]+]] : @guaranteed $Dog):
    // CHECK:     hop_to_executor [[SELF]] : $Dog
    // CHECK:     [[BOX_GETTER:%[0-9]+]] = class_method [[BOX]] : $CatBox, #CatBox.cat!getter : (CatBox) -> () -> Cat, $@convention(method) (@guaranteed CatBox) -> @owned Cat
    // CHECK:     [[CAT_REF:%[0-9]+]] = apply [[BOX_GETTER]]([[BOX]]) : $@convention(method) (@guaranteed CatBox) -> @owned Cat

    // CHECK:     [[CAT_FOR_LOAD:%[0-9]+]] = begin_borrow [[CAT_REF:%[0-9]+]] : $Cat
    // CHECK:     [[GETTER:%[0-9]+]] = class_method [[CAT_FOR_LOAD]] : $Cat, #Cat.storedBool!getter : (isolated Cat) -> () -> Bool, $@convention(method) (@sil_isolated @guaranteed Cat) -> Bool
    // CHECK:     hop_to_executor [[CAT_FOR_LOAD]] : $Cat
    // CHECK:     [[THE_BOOL:%[0-9]+]] = apply [[GETTER]]([[CAT_FOR_LOAD]]) : $@convention(method) (@sil_isolated @guaranteed Cat) -> Bool
    // CHECK:     hop_to_executor [[SELF]] : $Dog
    // CHECK:     end_borrow [[CAT_FOR_LOAD]] : $Cat

    // CHECK:     destroy_value [[CAT_REF]] : $Cat
    // CHECK:    return [[THE_BOOL]] : $Bool
    // CHECK: } // end sil function '$s4test3DogC33accessStoredPropFromRefProjection3boxSbAA6CatBoxC_tYaF'
    func accessStoredPropFromRefProjection(box : CatBox) async -> Bool {
        return await box.cat.storedBool
    }

    // CHECK-LABEL: sil hidden [ossa] @$s4test3DogC015accessSweaterOfD03catAA0D0VAA3CatC_tYaF : $@convention(method) @async (@guaranteed Cat, @sil_isolated @guaranteed Dog) -> @owned Sweater {
    // CHECK:  bb0([[CAT:%[0-9]+]] : @guaranteed $Cat, [[SELF:%[0-9]+]] : @guaranteed $Dog):
    // CHECK:    hop_to_executor [[SELF]] : $Dog

    // CHECK:    [[CAT_GETTER:%[0-9]+]] = class_method [[CAT]] : $Cat, #Cat.computedSweater!getter : (isolated Cat) -> () -> Sweater, $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Sweater
    // CHECK:    hop_to_executor [[CAT]] : $Cat
    // CHECK:    [[SWEATER1_REF:%[0-9]+]] = apply [[CAT_GETTER]]([[CAT]]) : $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Sweater

    // CHECK:    hop_to_executor [[SELF]] : $Dog
    // CHECK:    [[SWEATER1:%[0-9]+]] = begin_borrow [[SWEATER1_REF]] : $Sweater
    // CHECK:    [[SWEATER1_OWNER:%[0-9]+]] = struct_extract [[SWEATER1]] : $Sweater, #Sweater.owner
    // CHECK:    [[CAT2_REF:%[0-9]+]] = copy_value [[SWEATER1_OWNER]] : $Cat
    // CHECK:    end_borrow [[SWEATER1]] : $Sweater
    // CHECK:    destroy_value [[SWEATER1_REF]] : $Sweater

    // CHECK:    [[CAT2_FOR_LOAD:%[0-9]+]] = begin_borrow [[CAT2_REF]] : $Cat
    // CHECK:    [[CAT2_GETTER:%[0-9]+]] = class_method [[CAT2_FOR_LOAD]] : $Cat, #Cat.computedSweater!getter : (isolated Cat) -> () -> Sweater, $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Sweater
    // CHECK:    hop_to_executor [[CAT2_FOR_LOAD]] : $Cat
    // CHECK:    [[SWEATER2_OWNER:%[0-9]+]] = apply [[CAT2_GETTER]]([[CAT2_FOR_LOAD]]) : $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Sweater

    // CHECK:    hop_to_executor [[SELF]] : $Dog
    // CHECK:    end_borrow [[CAT2_FOR_LOAD]] : $Cat
    // CHECK:    destroy_value [[CAT2_REF]] : $Cat
    // CHECK:    return [[SWEATER2_OWNER]] : $Sweater
    // CHECK: } // end sil function '$s4test3DogC015accessSweaterOfD03catAA0D0VAA3CatC_tYaF'
    func accessSweaterOfSweater(cat : Cat) async -> Sweater {
        // note that Sweater is not an actor!
        return await cat.computedSweater.owner.computedSweater
    }


    // CHECK-LABEL: sil hidden [ossa] @$s4test3DogC13accessCatList3catAA0D0CAG_tYaF : $@convention(method) @async (@guaranteed Cat, @sil_isolated @guaranteed Dog) -> @owned Cat {
    // CHECK:  bb0([[CAT:%[0-9]+]] : @guaranteed $Cat, [[SELF:%[0-9]+]] : @guaranteed $Dog):
    // CHECK:    hop_to_executor [[SELF]] : $Dog
    // CHECK:    [[CAT_GETTER:%[0-9]+]] = class_method [[CAT]] : $Cat, #Cat.friend!getter : (isolated Cat) -> () -> Cat, $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Cat
    // CHECK:    hop_to_executor [[CAT]] : $Cat
    // CHECK:    [[FRIEND1_REF:%[0-9]+]] = apply [[CAT_GETTER]]([[CAT]]) : $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Cat
    // CHECK:    hop_to_executor [[SELF]] : $Dog
    // CHECK:    [[FRIEND1_FOR_LOAD:%[0-9]+]] = begin_borrow [[FRIEND1_REF]] : $Cat
    // CHECK:    [[FRIEND1_GETTER:%[0-9]+]] = class_method [[FRIEND1_FOR_LOAD]] : $Cat, #Cat.friend!getter : (isolated Cat) -> () -> Cat, $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Cat
    // CHECK:    hop_to_executor [[FRIEND1_FOR_LOAD]] : $Cat
    // CHECK:    [[FRIEND2_REF:%[0-9]+]] = apply [[FRIEND1_GETTER]]([[FRIEND1_FOR_LOAD]]) : $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Cat
    // CHECK:    hop_to_executor [[SELF]] : $Dog
    // CHECK:    end_borrow [[FRIEND1_FOR_LOAD]] : $Cat
    // CHECK:    destroy_value [[FRIEND1_REF]] : $Cat
    // CHECK:    return [[FRIEND2_REF]] : $Cat
    // CHECK: } // end sil function '$s4test3DogC13accessCatList3catAA0D0CAG_tYaF'
    func accessCatList(cat : Cat) async -> Cat {
        return await cat.friend.friend
    }


    // CHECK-LABEL: sil hidden [ossa] @$s4test3DogC21accessOptionalCatList3catAA0E0CSgAG_tYaF : $@convention(method) @async (@guaranteed Cat, @sil_isolated @guaranteed Dog) -> @owned Optional<Cat> {
    // CHECK:  bb0([[CAT:%[0-9]+]] : @guaranteed $Cat, [[SELF:%[0-9]+]] : @guaranteed $Dog):
    // CHECK:    hop_to_executor [[SELF]] : $Dog
    // CHECK:    [[FRIEND1_STACK:%[0-9]+]] = alloc_stack $Optional<Cat>

    // CHECK:    [[MAYBE_GETTER:%[0-9]+]] = class_method [[CAT]] : $Cat, #Cat.maybeFriend!getter : (isolated Cat) -> () -> Cat?, $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Optional<Cat>
    // CHECK:    hop_to_executor [[CAT]] : $Cat
    // CHECK:    [[MAYBE_FRIEND:%[0-9]+]] = apply [[MAYBE_GETTER]]([[CAT]]) : $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Optional<Cat>
    // CHECK:    hop_to_executor [[SELF]] : $Dog
    // CHECK:    store [[MAYBE_FRIEND]] to [init] [[FRIEND1_STACK]] : $*Optional<Cat>

    // CHECK:    [[IS_SOME:%[0-9]+]] = select_enum_addr [[FRIEND1_STACK]] : $*Optional<Cat>
    // CHECK:    cond_br [[IS_SOME]], bb1, bb3

    // CHECK:  bb1:
    // CHECK:    [[FRIEND1_ADDR:%[0-9]+]] = unchecked_take_enum_data_addr [[FRIEND1_STACK]] : $*Optional<Cat>, #Optional.some!enumelt
    // CHECK:    [[FRIEND1_REF:%[0-9]+]] = load [copy] [[FRIEND1_ADDR]] : $*Cat
    // CHECK:    destroy_addr [[FRIEND1_STACK]] : $*Optional<Cat>
    // CHECK:    [[FRIEND1_FOR_LOAD:%[0-9]+]] = begin_borrow [[FRIEND1_REF]] : $Cat
    // CHECK:    [[FRIEND1_GETTER:%[0-9]+]] = class_method [[FRIEND1_FOR_LOAD]] : $Cat, #Cat.friend!getter : (isolated Cat) -> () -> Cat, $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Cat
    // CHECK:    hop_to_executor [[FRIEND1_FOR_LOAD]] : $Cat
    // CHECK:    [[FRIEND2_REF:%[0-9]+]] = apply [[FRIEND1_GETTER]]([[FRIEND1_FOR_LOAD]]) : $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Cat
    // CHECK:    hop_to_executor [[SELF]] : $Dog
    // CHECK:    end_borrow [[FRIEND1_FOR_LOAD]] : $Cat
    // CHECK:    destroy_value [[FRIEND1_REF]] : $Cat
    // CHECK:    [[FRIEND2_OPTIONAL:%[0-9]+]] = enum $Optional<Cat>, #Optional.some!enumelt, [[FRIEND2_REF]] : $Cat
    // CHECK:    dealloc_stack [[FRIEND1_STACK]] : $*Optional<Cat>
    // CHECK:    br bb2([[FRIEND2_OPTIONAL]] : $Optional<Cat>)

    // CHECK-NOT:  hop_to_executor
    // CHECK: } // end sil function '$s4test3DogC21accessOptionalCatList3catAA0E0CSgAG_tYaF'
    func accessOptionalCatList(cat : Cat) async -> Cat? {
        return await cat.maybeFriend?.friend
    }

    // CHECK-LABEL: sil hidden [ossa] @$s4test3DogC22accessOptionalViaIfLet3catAA3CatCSgAG_tYaF : $@convention(method) @async (@guaranteed Cat, @sil_isolated @guaranteed Dog) -> @owned Optional<Cat>
    // CHECK:  bb0([[CAT:%[0-9]+]] : @guaranteed $Cat, [[SELF:%[0-9]+]] : @guaranteed $Dog):
    func accessOptionalViaIfLet(cat: Cat) async -> Cat? {
        // CHECK: hop_to_executor [[SELF]] : $Dog
        // CHECK: [[METHOD:%.*]] = class_method [[CAT]] : $Cat, #Cat.maybeFriend!getter : (isolated Cat) -> () -> Cat?, $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Optional<Cat>
        // CHECK: hop_to_executor [[CAT]] : $Cat                       // id: %6
        // CHECK: [[RESULT:%.*]] = apply [[METHOD]]([[CAT]]) : $@convention(method) (@sil_isolated @guaranteed Cat) -> @owned Optional<Cat>
        // CHECK: hop_to_executor [[SELF]] : $Dog
        // CHECK: switch_enum [[RESULT]]
        if let friend = await cat.maybeFriend {
            return friend
        }
        return nil
    } // CHECK: } // end sil function '$s4test3DogC22accessOptionalViaIfLet3catAA3CatCSgAG_tYaF'
} // END OF DOG ACTOR

class Point {
    var pt: (Int, Int) = (0, 0)
}

@MainActor
var globalCircle: ((Int, Int)?, Float) = (nil, 1.1)

struct Container {
    @MainActor static var counter: Int = 10
    @MainActor static var this: Container?
    @MainActor static var staticCircle: ((Int, Int)?, Float) = (nil, 2.1)

    var noniso: Int = 20

    @GlobalCat var iso: Float = 1.0
    @GlobalCat var isoRef: CatBox = CatBox()

    // CHECK-LABEL: sil hidden [ossa] @$s4test9ContainerV12accessTuple1SfyYaF : $@convention(method) @async (@guaranteed Container) -> Float {
    // CHECK:     [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] :
    // CHECK:     hop_to_executor {{%[0-9]+}} : $MainActor
    // CHECK:     [[ACCESS:%[0-9]+]] = begin_access [read] [dynamic] {{%[0-9]+}} : $*(Optional<(Int, Int)>, Float)
    // CHECK:     [[ADDR:%[0-9]+]] = tuple_element_addr [[ACCESS]] : $*(Optional<(Int, Int)>, Float), 1
    // CHECK:     {{%[0-9]+}} = load [trivial] [[ADDR]] : $*Float
    // CHECK:     end_access [[ACCESS]] : $*(Optional<(Int, Int)>, Float)
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
    // CHECK: } // end sil function '$s4test9ContainerV12accessTuple1SfyYaF'
    func accessTuple1() async -> Float {
        return await globalCircle.1
    }

    // CHECK-LABEL: sil hidden [ossa] @$s4test9ContainerV12accessTuple2SiSgyYaFZ : $@convention(method) @async (@thin Container.Type) -> Optional<Int> {
    // CHECK:     [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] :
    // CHECK:     hop_to_executor {{%[0-9]+}} : $MainActor
    // CHECK:     [[ACCESS:%[0-9]+]] = begin_access [read] [dynamic] {{%[0-9]+}} : $*(Optional<(Int, Int)>, Float)
    // CHECK:     [[ADDR:%[0-9]+]] = tuple_element_addr [[ACCESS]] : $*(Optional<(Int, Int)>, Float), 0
    // CHECK:     switch_enum_addr [[SCRUTINEE:%[0-9]+]] : $*Optional<(Int, Int)>, case #Optional.some!enumelt: [[SOME_BB:bb[0-9]+]], case #Optional.none!enumelt: [[CRASH_BB:bb[0-9]+]]

    // CHECK: [[CRASH_BB]]:
    // CHECK-NOT:   hop_to_executor [[GENERIC_EXEC]]
    // CHECK:       unreachable

    // CHECK: [[SOME_BB]]:
    // CHECK:     [[TUPLE_ADDR:%[0-9]+]] = unchecked_take_enum_data_addr [[SCRUTINEE]] : $*Optional<(Int, Int)>, #Optional.some!enumelt
    // CHECK:     [[ELM_ADDR:%[0-9]+]] = tuple_element_addr [[TUPLE_ADDR]] : $*(Int, Int), 0
    // CHECK:     {{%[0-9]+}} = load [trivial] [[ELM_ADDR]] : $*Int
    // CHECK:     end_access [[ACCESS]] : $*(Optional<(Int, Int)>, Float)
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
    // CHECK: } // end sil function '$s4test9ContainerV12accessTuple2SiSgyYaFZ'
    static func accessTuple2() async -> Int? {
        return await globalCircle.0!.0
    }

    // CHECK-LABEL: sil hidden [ossa] @$s4test9ContainerV12accessTuple3SfyYaF : $@convention(method) @async (@guaranteed Container) -> Float {
    // CHECK:     [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] :
    // CHECK:     [[ADDRESS_ACCESSOR:%[0-9]+]] = function_ref @$s4test9ContainerV12staticCircleSi_SitSg_Sftvau : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:     hop_to_executor {{%[0-9]+}} : $MainActor
    // CHECK:     = apply [[ADDRESS_ACCESSOR]]() : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
    // CHECK:     hop_to_executor {{%[0-9]+}} : $MainActor
    // CHECK:     [[ACCESS:%[0-9]+]] = begin_access [read] [dynamic] {{%[0-9]+}} : $*(Optional<(Int, Int)>, Float)
    // CHECK:     [[ADDR:%[0-9]+]] = tuple_element_addr [[ACCESS]] : $*(Optional<(Int, Int)>, Float), 1
    // CHECK:     {{%[0-9]+}} = load [trivial] [[ADDR]] : $*Float
    // CHECK:     end_access [[ACCESS]] : $*(Optional<(Int, Int)>, Float)
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
    // CHECK: } // end sil function '$s4test9ContainerV12accessTuple3SfyYaF'
    func accessTuple3() async -> Float {
        return await Container.staticCircle.1
    }

    // CHECK-LABEL: sil hidden [ossa] @$s4test9ContainerV12accessTuple4SiSgyYaFZ : $@convention(method) @async (@thin Container.Type) -> Optional<Int> {
    // CHECK:     [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] :
    // CHECK:     [[ADDRESS_ACCESSOR:%[0-9]+]] = function_ref @$s4test9ContainerV12staticCircleSi_SitSg_Sftvau : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:     hop_to_executor {{%[0-9]+}} : $MainActor
    // CHECK:     = apply [[ADDRESS_ACCESSOR]]() : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
    // CHECK:     hop_to_executor {{%[0-9]+}} : $MainActor
    // CHECK:     [[ACCESS:%[0-9]+]] = begin_access [read] [dynamic] {{%[0-9]+}} : $*(Optional<(Int, Int)>, Float)
    // CHECK:     [[ADDR:%[0-9]+]] = tuple_element_addr [[ACCESS]] : $*(Optional<(Int, Int)>, Float), 0
    // CHECK:     switch_enum_addr [[SCRUTINEE:%[0-9]+]] : $*Optional<(Int, Int)>, case #Optional.some!enumelt: [[SOME_BB:bb[0-9]+]], case #Optional.none!enumelt: [[CRASH_BB:bb[0-9]+]]

    // CHECK: [[CRASH_BB]]:
    // CHECK-NOT:   hop_to_executor [[GENERIC_EXEC]]
    // CHECK:       unreachable

    // CHECK: [[SOME_BB]]:
    // CHECK:     [[TUPLE_ADDR:%[0-9]+]] = unchecked_take_enum_data_addr [[SCRUTINEE]] : $*Optional<(Int, Int)>, #Optional.some!enumelt
    // CHECK:     [[ELM_ADDR:%[0-9]+]] = tuple_element_addr [[TUPLE_ADDR]] : $*(Int, Int), 0
    // CHECK:     {{%[0-9]+}} = load [trivial] [[ELM_ADDR]] : $*Int
    // CHECK:     end_access [[ACCESS]] : $*(Optional<(Int, Int)>, Float)
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
    // CHECK: } // end sil function '$s4test9ContainerV12accessTuple4SiSgyYaFZ'
    static func accessTuple4() async -> Int? {
        return await Container.staticCircle.0!.0
    }


    // CHECK-LABEL: sil hidden [ossa] @$s4test9ContainerV8getCountSiyYaFZ : $@convention(method) @async (@thin Container.Type) -> Int {
    // CHECK:     [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] :
    // CHECK:     [[ADDRESS_ACCESSOR:%[0-9]+]] = function_ref @$s4test9ContainerV7counterSivau : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:     hop_to_executor {{%[0-9]+}} : $MainActor
    // CHECK:     = apply [[ADDRESS_ACCESSOR]]() : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
    // CHECK:   hop_to_executor {{%[0-9]+}} : $MainActor
    // CHECK:   {{%[0-9]+}} = begin_access [read] [dynamic] {{%[0-9]+}} : $*Int
    // CHECK:   {{%[0-9]+}} = load [trivial] {{%[0-9]+}} : $*Int
    // CHECK:   hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
    // CHECK: } // end sil function '$s4test9ContainerV8getCountSiyYaFZ'
    static func getCount() async -> Int {
        return await counter
    }


    // CHECK-LABEL: sil hidden [ossa] @$s4test9ContainerV8getValueSiSgyYaFZ : $@convention(method) @async (@thin Container.Type) -> Optional<Int> {
    // CHECK: bb0(%0 : $@thin Container.Type):
    // CHECK:     [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] :
    // CHECK:     [[ADDRESS_ACCESSOR:%[0-9]+]] = function_ref @$s4test9ContainerV4thisACSgvau : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:     hop_to_executor {{%[0-9]+}} : $MainActor
    // CHECK:     = apply [[ADDRESS_ACCESSOR]]() : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:     hop_to_executor {{%[0-9]+}} : $Optional<Builtin.Executor>
    // CHECK:    [[MAIN:%[0-9]+]] = begin_borrow {{%[0-9]+}} : $MainActor
    // CHECK:    hop_to_executor [[MAIN]] : $MainActor
    // CHECK:    [[ACCESS:%[0-9]+]] = begin_access [read] [dynamic] {{%[0-9]+}} : $*Optional<Container>
    // CHECK:    cond_br {{%[0-9]+}}, [[TRUE_BB:bb[0-9]+]], [[FALSE_BB:bb[0-9]+]]
    //
    // CHECK: [[TRUE_BB]]:
    // CHECK:    {{%[0-9]+}} = load [trivial] {{%[0-9]+}} : $*Int
    // CHECK:    end_access [[ACCESS]] : $*Optional<Container>
    // CHECK:    hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
    //
    // CHECK: [[FALSE_BB]]:
    // CHECK:    end_access [[ACCESS]] : $*Optional<Container>
    // CHECK:    hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
    //
    // CHECK: } // end sil function '$s4test9ContainerV8getValueSiSgyYaFZ'
    static func getValue() async -> Int? {
        return await this?.noniso
    }

    // CHECK-LABEL: sil hidden [ossa] @$s4test9ContainerV10getOrCrashSfyYaFZ : $@convention(method) @async (@thin Container.Type) -> Float {
    // CHECK: bb0({{%[0-9]+}} : $@thin Container.Type):
    // CHECK:     [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] :
    // CHECK:     [[ADDRESS_ACCESSOR:%[0-9]+]] = function_ref @$s4test9ContainerV4thisACSgvau : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:     hop_to_executor {{%[0-9]+}} : $MainActor
    // CHECK:     = apply [[ADDRESS_ACCESSOR]]() : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
    // CHECK:     hop_to_executor {{%.*}} : $MainActor
    // CHECK:     [[ACCESS:%[0-9]+]] = begin_access [read] [dynamic] {{%[0-9]+}} : $*Optional<Container>
    // CHECK:     switch_enum_addr [[ACCESS]] : $*Optional<Container>, case #Optional.some!enumelt: [[SOME_BB:bb[0-9]+]], case #Optional.none!enumelt: [[CRASH_BB:bb[0-9]+]]
    //
    // CHECK: [[CRASH_BB]]:
    // CHECK-NOT:   hop_to_executor [[GENERIC_EXEC]]
    // CHECK:       unreachable
    //
    // CHECK: [[SOME_BB]]:
    // CHECK:       [[DATA_ADDR:%[0-9]+]] = unchecked_take_enum_data_addr [[ACCESS]] : $*Optional<Container>, #Optional.some!enumelt
    // CHECK:       [[ELEM_ADDR:%[0-9]+]] = struct_element_addr [[DATA_ADDR]] : $*Container, #Container.iso
    // CHECK:       {{%[0-9]+}} = load [trivial] [[ELEM_ADDR]] : $*Float
    // CHECK:       hop_to_executor [[GENERIC_EXEC]] :
    // CHECK: } // end sil function '$s4test9ContainerV10getOrCrashSfyYaFZ'
    static func getOrCrash() async -> Float {
        return await this!.iso
    }

    // CHECK-LABEL: sil hidden [ossa] @$s4test9ContainerV13getRefOrCrashAA6CatBoxCyYaFZ : $@convention(method) @async (@thin Container.Type) -> @owned CatBox {
    // CHECK: bb0({{%[0-9]+}} : $@thin Container.Type):
    // CHECK:     [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] :
    // CHECK:     [[ADDRESS_ACCESSOR:%[0-9]+]] = function_ref @$s4test9ContainerV4thisACSgvau : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:     hop_to_executor {{%[0-9]+}} : $MainActor
    // CHECK:     = apply [[ADDRESS_ACCESSOR]]() : $@convention(thin) () -> Builtin.RawPointer
    // CHECK:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
    // CHECK:    hop_to_executor {{%.*}} : $MainActor
    // CHECK:    [[ACCESS:%[0-9]+]] = begin_access [read] [dynamic] {{%[0-9]+}} : $*Optional<Container>
    // CHECK:    switch_enum_addr [[ACCESS]] : $*Optional<Container>, case #Optional.some!enumelt: [[SOME_BB:bb[0-9]+]], case #Optional.none!enumelt: [[CRASH_BB:bb[0-9]+]]
    //
    // CHECK: [[CRASH_BB]]:
    // CHECK:       unreachable
    //
    // CHECK: [[SOME_BB]]:
    // CHECK:       [[DATA_ADDR:%[0-9]+]] = unchecked_take_enum_data_addr [[ACCESS]] : $*Optional<Container>, #Optional.some!enumelt
    // CHECK:       [[ELEM_ADDR:%[0-9]+]] = struct_element_addr [[DATA_ADDR]] : $*Container, #Container.iso
    // CHECK:       hop_to_executor {{%[0-9]+}} : $Cat
    // CHECK:       {{%[0-9]+}} = load [copy] [[ELEM_ADDR]] : $*CatBox
    // CHECK:       hop_to_executor [[GENERIC_EXEC]] :
    // CHECK:       hop_to_executor [[GENERIC_EXEC]] :
    // CHECK: } // end sil function '$s4test9ContainerV13getRefOrCrashAA6CatBoxCyYaFZ'
    static func getRefOrCrash() async -> CatBox {
        return await this!.isoRef
    }
}


@propertyWrapper
struct StateObject<ObjectType> {
    @preconcurrency @MainActor
    var wrappedValue: ObjectType {
        fatalError()
    }
    init(wrappedValue: ObjectType) {}
}

final private actor Coordinator {
    var someValue: Int?
}

struct Blah {
    @StateObject private var coordinator = Coordinator()

    // closure #1 in Blah.test()
    // CHECK-LABEL: sil private [ossa] @$s4test4BlahVAAyyFyyYacfU_ : $@convention(thin) @async @substituted <τ_0_0> (@guaranteed Optional<any Actor>, Blah) -> @out τ_0_0 for <()> {
    // CHECK:       [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
    // CHECK:       hop_to_executor [[GENERIC_EXEC]] :
    // CHECK:       hop_to_executor {{%[0-9]+}} : $MainActor
    // CHECK:       [[ACTOR_OBJ_RAW:%[0-9]+]] = apply {{%[0-9]+}}({{%[0-9]+}}) : $@convention(method) (Blah) -> @owned Coordinator
    // CHECK:       hop_to_executor {{%[0-9]+}} : $Optional<Builtin.Executor>
    // CHECK:       [[ACTOR_OBJ:%[0-9]+]] = begin_borrow [[ACTOR_OBJ_RAW]] : $Coordinator
    // CHECK:       [[VAL:%[0-9]+]] = ref_element_addr [[ACTOR_OBJ]] : $Coordinator, #Coordinator.someValue
    // CHECK:       hop_to_executor [[ACTOR_OBJ]]
    // CHECK:       [[VAL_ACCESS:%[0-9]+]] = begin_access [read] [dynamic] [[VAL]] : $*Optional<Int>
    // CHECK:       {{%[0-9]+}} = load [trivial] [[VAL_ACCESS]] : $*Optional<Int>
    // CHECK:       end_access [[VAL_ACCESS]] : $*Optional<Int>
    // CHECK:       hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
    // CHECK: } // end sil function '$s4test4BlahVAAyyFyyYacfU_'
    @available(SwiftStdlib 5.1, *)
    func test() {
        Task.detached {
            if await coordinator.someValue == nil {
                fatalError()
            }
        }
    }
}

typealias FancyActor = MainActor // helps cover rdar://96309577

@FancyActor
func getTemperature() -> Int { return 0 }

@MainActor
class Polar {
  static var temperature: Int = getTemperature()
}


// CHECK-LABEL: sil hidden{{.*}} @$s4test20accessStaticIsolatedSiyYaF : $@convention(thin) @async () -> Int {
// CHECK:        [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK:        hop_to_executor [[GENERIC_EXEC]] :
// CHECK:        [[ADDRESSOR:%[0-9]+]] = function_ref @$s4test5PolarC11temperatureSivau : $@convention(thin) () -> Builtin.RawPointer
// CHECK:        hop_to_executor {{%.*}} : $MainActor
// CHECK-NEXT:   [[RAW_ADDR:%[0-9]+]] = apply [[ADDRESSOR]]() : $@convention(thin) () -> Builtin.RawPointer
// CHECK-NEXT:   hop_to_executor {{%.*}} : $Optional<Builtin.Executor>
// CHECK:        [[ADDR:%[0-9]+]] = pointer_to_address [[RAW_ADDR]] : $Builtin.RawPointer to [strict] $*Int
// CHECK:        hop_to_executor {{%.*}} : $MainActor
// CHECK:        {{%.*}} = load [trivial] {{%.*}} : $*Int
// CHECK:        hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
func accessStaticIsolated() async -> Int {
  return await Polar.temperature
}
