// RUN: %target-swift-frontend -module-name test -swift-version 5 -sil-verify-all -emit-sil %s | %FileCheck --enable-var-scope --implicit-check-not='hop_to_executor' %s

 enum ActingError<T> : Error {
   case forgotLine
   case smuggledValue(T)
 }

actor BoringActor {

    // CHECK-LABEL: sil hidden @$s4test11BoringActorCACyYacfc : $@convention(method) @async (@owned BoringActor) -> @owned BoringActor {
    // CHECK:   bb0([[SELF:%[0-9]+]] : $BoringActor):
    // CHECK:       initializeDefaultActor
    // CHECK-NEXT:  hop_to_executor [[SELF]]
    // CHECK: } // end sil function '$s4test11BoringActorCACyYacfc'
    init() async {}

    // CHECK-LABEL: sil hidden @$s4test11BoringActorC4sizeACSi_tYacfc : $@convention(method) @async (Int, @owned BoringActor) -> @owned BoringActor {
    // CHECK:   bb0({{%[0-9]+}} : $Int, [[SELF:%[0-9]+]] : $BoringActor):
    // CHECK:       initializeDefaultActor
    // CHECK-NEXT:  hop_to_executor [[SELF]]
    // CHECK: } // end sil function '$s4test11BoringActorC4sizeACSi_tYacfc'
    init(size: Int) async {
        var sz = size
        while sz > 0 {
            print(".")
            sz -= 1
        }
        print("done!")
    }

    @MainActor
    init(mainActor: Void) async {}

    // CHECK-LABEL: sil hidden @$s4test11BoringActorC6crashyACSgyt_tYacfc : $@convention(method) @async (@owned BoringActor) -> @owned Optional<BoringActor> {
    // CHECK:   bb0([[SELF:%[0-9]+]] : $BoringActor):
    // CHECK:       initializeDefaultActor
    // CHECK-NEXT:  hop_to_executor [[SELF]]
    // CHECK: } // end sil function '$s4test11BoringActorC6crashyACSgyt_tYacfc'
    init!(crashy: Void) async { return nil }

    // CHECK-LABEL: sil hidden @$s4test11BoringActorC5nillyACSgSi_tYacfc : $@convention(method) @async (Int, @owned BoringActor) -> @owned Optional<BoringActor> {
    // CHECK:   bb0({{%[0-9]+}} : $Int, [[SELF:%[0-9]+]] : $BoringActor):
    // CHECK:       initializeDefaultActor
    // CHECK-NEXT:  hop_to_executor [[SELF]]
    // CHECK: } // end sil function '$s4test11BoringActorC5nillyACSgSi_tYacfc'
    init?(nilly: Int) async {
        guard nilly > 0 else { return nil }
    }
}

 actor SingleVarActor {
    var myVar: Int

    // CHECK-LABEL: sil hidden @$s4test14SingleVarActorCACyYacfc : $@convention(method) @async (@owned SingleVarActor) -> @owned SingleVarActor {
    // CHECK:    bb0([[SELF:%[0-9]+]] : $SingleVarActor):
    // CHECK:       store {{%[0-9]+}} to {{%[0-9]+}}
    // CHECK-NEXT:  hop_to_executor [[SELF]] : $SingleVarActor
    // CHECK:       store {{%[0-9]+}} to {{%[0-9]+}}
    // CHECK: } // end sil function '$s4test14SingleVarActorCACyYacfc'
    init() async {
        myVar = 0
        myVar = 1
    }

    // CHECK-LABEL: sil hidden @$s4test14SingleVarActorC10iterationsACSi_tYacfc : $@convention(method) @async (Int, @owned SingleVarActor) -> @owned SingleVarActor {
    // CHECK:   bb0({{%[0-9]+}} : $Int, [[SELF:%[0-9]+]] : $SingleVarActor):
    // CHECK:       [[MYVAR_REF:%[0-9]+]] = ref_element_addr [[SELF]] : $SingleVarActor, #SingleVarActor.myVar
    // CHECK:       [[MYVAR:%[0-9]+]] = begin_access [modify] [dynamic] [[MYVAR_REF]] : $*Int
    // CHECK:       store {{%[0-9]+}} to [[MYVAR]] : $*Int
    // CHECK-NEXT:  hop_to_executor [[SELF]] : $SingleVarActor
    // CHECK: } // end sil function '$s4test14SingleVarActorC10iterationsACSi_tYacfc'
    init(iterations: Int) async {
        var iter = iterations
        repeat {
            myVar = 0
            iter -= 1
        } while iter > 0
    }

    // CHECK-LABEL: sil hidden @$s4test14SingleVarActorC2b12b2ACSb_SbtYacfc : $@convention(method) @async (Bool, Bool, @owned SingleVarActor) -> @owned SingleVarActor {
    // CHECK:   bb0({{%[0-9]+}} : $Bool, {{%[0-9]+}} : $Bool, [[SELF:%[0-9]+]] : $SingleVarActor):

    // CHECK:       store {{%[0-9]+}} to {{%[0-9]+}} : $*Int
    // CHECK-NEXT:  hop_to_executor [[SELF]] : $SingleVarActor

    // CHECK:       store {{%[0-9]+}} to {{%[0-9]+}} : $*Int
    // CHECK-NEXT:  hop_to_executor [[SELF]] : $SingleVarActor

    // CHECK:       store {{%[0-9]+}} to {{%[0-9]+}} : $*Int
    // CHECK-NEXT:  hop_to_executor [[SELF]] : $SingleVarActor

    // CHECK: } // end sil function '$s4test14SingleVarActorC2b12b2ACSb_SbtYacfc'
    init(b1: Bool, b2: Bool) async {
        if b1 {
            if b2 {
                myVar = 0
            }
            myVar = 1
        }
        myVar = 2
    }
 }

actor DefaultInit {
    var x: DefaultInit?
    var y: String = ""
    var z: ActingError<Int> = .smuggledValue(5)

    // CHECK-LABEL: sil hidden @$s4test11DefaultInitCACyYacfc : $@convention(method) @async (@owned DefaultInit) -> @owned DefaultInit {
    // CHECK:   bb0([[SELF:%[0-9]+]] : $DefaultInit):
    // CHECK:       store {{%[0-9]+}} to {{%[0-9]+}} : $*ActingError<Int>
    // CHECK-NEXT:  hop_to_executor [[SELF]] : $DefaultInit
    // CHECK: } // end sil function '$s4test11DefaultInitCACyYacfc'
    init() async {}

    // CHECK-LABEL: sil hidden @$s4test11DefaultInitC5nillyACSgSb_tYacfc : $@convention(method) @async (Bool, @owned DefaultInit) -> @owned Optional<DefaultInit> {
    // CHECK:   bb0({{%[0-9]+}} : $Bool, [[SELF:%[0-9]+]] : $DefaultInit):
    // CHECK:       store {{%[0-9]+}} to {{%[0-9]+}} : $*ActingError<Int>
    // CHECK-NEXT:  hop_to_executor [[SELF]] : $DefaultInit
    // CHECK: } // end sil function '$s4test11DefaultInitC5nillyACSgSb_tYacfc'
    init?(nilly: Bool) async {
        guard nilly else { return nil }
    }

    init(sync: Void) {}
    @MainActor init(mainActorSync: Void) {}
}

actor MultiVarActor {
    var firstVar: Int
    var secondVar: Float

    // CHECK-LABEL: sil hidden @$s4test13MultiVarActorC10doNotThrowACSb_tYaKcfc : $@convention(method) @async (Bool, @owned MultiVarActor) -> (@owned MultiVarActor, @error Error) {
    // CHECK:   bb0({{%[0-9]+}} : $Bool, [[SELF:%[0-9]+]] : $MultiVarActor):
    // CHECK:       [[REF:%[0-9]+]] = ref_element_addr [[SELF]] : $MultiVarActor, #MultiVarActor.firstVar
    // CHECK:       [[VAR:%[0-9]+]] = begin_access [modify] [dynamic] [[REF]] : $*Int
    // CHECK:       store {{%[0-9]+}} to [[VAR]] : $*Int
    // CHECK-NEXT:  hop_to_executor %1 : $MultiVarActor
    // CHECK: } // end sil function '$s4test13MultiVarActorC10doNotThrowACSb_tYaKcfc'
    init(doNotThrow: Bool) async throws {
        secondVar = 0
        guard doNotThrow else { throw ActingError<Any>.forgotLine }
        firstVar = 1
    }

    // CHECK-LABEL: sil hidden @$s4test13MultiVarActorC10noSuccCaseACSb_tYacfc : $@convention(method) @async (Bool, @owned MultiVarActor) -> @owned MultiVarActor {
    // CHECK:       store {{%[0-9]+}} to {{%[0-9]+}} : $*Int
    // CHECK-NEXT:  hop_to_executor {{%[0-9]+}} : $MultiVarActor

    // CHECK:       store {{%[0-9]+}} to {{%[0-9]+}} : $*Int
    // CHECK-NEXT:  hop_to_executor {{%[0-9]+}} : $MultiVarActor
    // CHECK: } // end sil function '$s4test13MultiVarActorC10noSuccCaseACSb_tYacfc'
    init(noSuccCase: Bool) async {
        secondVar = 0
        if noSuccCase {
            firstVar = 1
        }
        firstVar = 2
    }

    // CHECK-LABEL: sil hidden @$s4test13MultiVarActorC10noPredCaseACSb_tYacfc : $@convention(method) @async (Bool, @owned MultiVarActor) -> @owned MultiVarActor {
    // CHECK:       store {{%[0-9]+}} to {{%[0-9]+}} : $*Int
    // CHECK-NEXT:  hop_to_executor {{%[0-9]+}} : $MultiVarActor
    // CHECK: } // end sil function '$s4test13MultiVarActorC10noPredCaseACSb_tYacfc'
    init(noPredCase: Bool) async {
        secondVar = 0
        firstVar = 1
        if noPredCase {
            print("hello")
        }
        print("hi")
    }
}