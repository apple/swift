// REQUIRES: concurrency
// RUN: %batch-code-completion -warn-concurrency

class MyNonSendable {}
struct MySendable {}

actor MyActor {
  var property: Int
  func syncFunc() -> Int { 1 }
  func syncNonSendable(arg: MyNonSendable) -> Int { 1 }
  func syncSendable(arg: MySendable) -> Int { 1 }
  func asyncFunc() async -> Int { 1 }
  subscript(idx: Int) -> Int { get { 1 } set { } }
}

extension MyActor {
    func testSyncFunc(other: MyActor) {
        let _ = #^IN_SYNCFUNC^#
// IN_SYNCFUNC-DAG: Decl[InstanceVar]/CurrNominal:      property[#Int#];
// IN_SYNCFUNC-DAG: Decl[InstanceMethod]/CurrNominal:   syncFunc()[#Int#];
// IN_SYNCFUNC-DAG: Decl[InstanceMethod]/CurrNominal:   syncNonSendable({#arg: MyNonSendable#})[#Int#];
// IN_SYNCFUNC-DAG: Decl[InstanceMethod]/CurrNominal:   syncSendable({#arg: MySendable#})[#Int#];
// IN_SYNCFUNC-DAG: Decl[InstanceMethod]/CurrNominal: asyncFunc()[' async'][#Int#];

        let _ = self.#^IN_SYNCFUNC_SELF_DOT^#
// IN_SYNCFUNC_SELF_DOT-DAG: Decl[InstanceVar]/CurrNominal:      property[#Int#];
// IN_SYNCFUNC_SELF_DOT-DAG: Decl[InstanceMethod]/CurrNominal:   syncFunc()[#Int#];
// IN_SYNCFUNC_SELF_DOT-DAG: Decl[InstanceMethod]/CurrNominal:   syncNonSendable({#arg: MyNonSendable#})[#Int#];
// IN_SYNCFUNC_SELF_DOT-DAG: Decl[InstanceMethod]/CurrNominal:   syncSendable({#arg: MySendable#})[#Int#];
// IN_SYNCFUNC_SELF_DOT-DAG: Decl[InstanceMethod]/CurrNominal: asyncFunc()[' async'][#Int#];

        let _ = self#^IN_SYNCFUNC_SELF_NODOT^#
// IN_SYNCFUNC_SELF_NODOT-DAG: Decl[InstanceVar]/CurrNominal:      .property[#Int#];
// IN_SYNCFUNC_SELF_NODOT-DAG: Decl[InstanceMethod]/CurrNominal:   .syncFunc()[#Int#];
// IN_SYNCFUNC_SELF_NODOT-DAG: Decl[InstanceMethod]/CurrNominal:   .syncNonSendable({#arg: MyNonSendable#})[#Int#];
// IN_SYNCFUNC_SELF_NODOT-DAG: Decl[InstanceMethod]/CurrNominal:   .syncSendable({#arg: MySendable#})[#Int#];
// IN_SYNCFUNC_SELF_NODOT-DAG: Decl[InstanceMethod]/CurrNominal: .asyncFunc()[' async'][#Int#];
// IN_SYNCFUNC_SELF_NODOT-DAG: Decl[Subscript]/CurrNominal:        [{#(idx): Int#}][#Int#];

        let _ = other.#^IN_SYNCFUNC_OTHER_DOT^#
// IN_SYNCFUNC_OTHER_DOT-DAG: Decl[InstanceVar]/CurrNominal: property[#Int#][' async'];
// IN_SYNCFUNC_OTHER_DOT-DAG: Decl[InstanceMethod]/CurrNominal: syncFunc()[' async'][#Int#];
// IN_SYNCFUNC_OTHER_DOT-DAG: Decl[InstanceMethod]/CurrNominal: syncNonSendable({#arg: MyNonSendable#})[' async'][#Int#];
// IN_SYNCFUNC_OTHER_DOT-DAG: Decl[InstanceMethod]/CurrNominal: syncSendable({#arg: MySendable#})[' async'][#Int#];
// IN_SYNCFUNC_OTHER_DOT-DAG: Decl[InstanceMethod]/CurrNominal: asyncFunc()[' async'][#Int#];

        let _ = other#^IN_SYNCFUNC_OTHER_NODOT^#
// IN_SYNCFUNC_OTHER_NODOT-DAG: Decl[InstanceVar]/CurrNominal: .property[#Int#][' async'];
// IN_SYNCFUNC_OTHER_NODOT-DAG: Decl[InstanceMethod]/CurrNominal: .syncFunc()[' async'][#Int#];
// IN_SYNCFUNC_OTHER_NODOT-DAG: Decl[InstanceMethod]/CurrNominal: .syncNonSendable({#arg: MyNonSendable#})[' async'][#Int#];
// IN_SYNCFUNC_OTHER_NODOT-DAG: Decl[InstanceMethod]/CurrNominal: .syncSendable({#arg: MySendable#})[' async'][#Int#];
// IN_SYNCFUNC_OTHER_NODOT-DAG: Decl[InstanceMethod]/CurrNominal: .asyncFunc()[' async'][#Int#];
// IN_SYNCFUNC_OTHER_NODOT-DAG: Decl[Subscript]/CurrNominal: [{#(idx): Int#}][' async'][#Int#];
    }
    func testAsyncFunc(other: MyActor) async {
        let _ = #^IN_ASYNCFUNC^#
// IN_ASYNCFUNC-DAG: Decl[InstanceVar]/CurrNominal:      property[#Int#];
// IN_ASYNCFUNC-DAG: Decl[InstanceMethod]/CurrNominal:   syncFunc()[#Int#];
// IN_ASYNCFUNC-DAG: Decl[InstanceMethod]/CurrNominal:   syncNonSendable({#arg: MyNonSendable#})[#Int#];
// IN_ASYNCFUNC-DAG: Decl[InstanceMethod]/CurrNominal:   syncSendable({#arg: MySendable#})[#Int#];
// IN_ASYNCFUNC-DAG: Decl[InstanceMethod]/CurrNominal:   asyncFunc()[' async'][#Int#];
// IN_ASYNCFUNC-DAG: Decl[InstanceMethod]/CurrNominal:   testSyncFunc({#other: MyActor#})[#Void#];
// IN_ASYNCFUNC-DAG: Decl[InstanceMethod]/CurrNominal:   testAsyncFunc({#other: MyActor#})[' async'][#Void#];
        let _ = self.#^IN_ASYNCFUNC_SELF_DOT^#
// IN_ASYNCFUNC_SELF_DOT-DAG: Decl[InstanceVar]/CurrNominal:      property[#Int#];
// IN_ASYNCFUNC_SELF_DOT-DAG: Decl[InstanceMethod]/CurrNominal:   syncFunc()[#Int#];
// IN_ASYNCFUNC_SELF_DOT-DAG: Decl[InstanceMethod]/CurrNominal:   syncNonSendable({#arg: MyNonSendable#})[#Int#];
// IN_ASYNCFUNC_SELF_DOT-DAG: Decl[InstanceMethod]/CurrNominal:   syncSendable({#arg: MySendable#})[#Int#];
// IN_ASYNCFUNC_SELF_DOT-DAG: Decl[InstanceMethod]/CurrNominal:   asyncFunc()[' async'][#Int#];

        let _ = self#^IN_ASYNCFUNC_SELF_NODOT^#
// IN_ASYNCFUNC_SELF_NODOT-DAG: Decl[InstanceVar]/CurrNominal:      .property[#Int#];
// IN_ASYNCFUNC_SELF_NODOT-DAG: Decl[InstanceMethod]/CurrNominal:   .syncFunc()[#Int#];
// IN_ASYNCFUNC_SELF_NODOT-DAG: Decl[InstanceMethod]/CurrNominal:   .syncNonSendable({#arg: MyNonSendable#})[#Int#];
// IN_ASYNCFUNC_SELF_NODOT-DAG: Decl[InstanceMethod]/CurrNominal:   .syncSendable({#arg: MySendable#})[#Int#];
// IN_ASYNCFUNC_SELF_NODOT-DAG: Decl[InstanceMethod]/CurrNominal:   .asyncFunc()[' async'][#Int#];
// IN_ASYNCFUNC_SELF_NODOT-DAG: Decl[Subscript]/CurrNominal:        [{#(idx): Int#}][#Int#];

        let _ = other.#^IN_ASYNCFUNC_OTHER_DOT^#
// IN_ASYNCFUNC_OTHER_DOT-DAG: Decl[InstanceVar]/CurrNominal:      property[#Int#][' async'];
// IN_ASYNCFUNC_OTHER_DOT-DAG: Decl[InstanceMethod]/CurrNominal:   syncFunc()[' async'][#Int#];
// IN_ASYNCFUNC_OTHER_DOT-DAG: Decl[InstanceMethod]/CurrNominal: syncNonSendable({#arg: MyNonSendable#})[' async'][#Int#];
// IN_ASYNCFUNC_OTHER_DOT-DAG: Decl[InstanceMethod]/CurrNominal:   syncSendable({#arg: MySendable#})[' async'][#Int#];
// IN_ASYNCFUNC_OTHER_DOT-DAG: Decl[InstanceMethod]/CurrNominal:   asyncFunc()[' async'][#Int#];

        let _ = other#^IN_ASYNCFUNC_OTHER_NODOT^#
// IN_ASYNCFUNC_OTHER_NODOT-DAG: Decl[InstanceVar]/CurrNominal:      .property[#Int#][' async'];
// IN_ASYNCFUNC_OTHER_NODOT-DAG: Decl[InstanceMethod]/CurrNominal:   .syncFunc()[' async'][#Int#];
// IN_ASYNCFUNC_OTHER_NODOT-DAG: Decl[InstanceMethod]/CurrNominal: .syncNonSendable({#arg: MyNonSendable#})[' async'][#Int#];
// IN_ASYNCFUNC_OTHER_NODOT-DAG: Decl[InstanceMethod]/CurrNominal:   .syncSendable({#arg: MySendable#})[' async'][#Int#];
// IN_ASYNCFUNC_OTHER_NODOT-DAG: Decl[InstanceMethod]/CurrNominal:   .asyncFunc()[' async'][#Int#];
// IN_ASYNCFUNC_OTHER_NODOT-DAG: Decl[Subscript]/CurrNominal:        [{#(idx): Int#}][' async'][#Int#];
    }

    func testActorKind() {
        let _ = #^GLOBAL^#
// GLOBAL: Decl[Actor]/CurrModule:             MyActor[#MyActor#]; name=MyActor
    }
}

