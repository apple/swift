// REQUIRES: concurrency 

// RUN: %batch-code-completion -warn-concurrency

func asyncFunc() async {}
func syncFunc() {}

struct MySendable {}
class MyNonSendable {}


actor MyActor {
    func receiveSendable(arg: MySendable) {}
    func receiveNonSendable(arg: MyNonSendable) {}
}

func testAsyncContext() {
    #^SYNC_CONTEXT^#
// SYNC_CONTEXT-DAG: Decl[FreeFunction]/CurrModule: asyncFunc()[' async'][#Void#]; name=asyncFunc(){{$}}
// SYNC_CONTEXT-DAG: Decl[FreeFunction]/CurrModule:      syncFunc()[#Void#]; name=syncFunc(){{$}}
}

func testActor(obj: MyActor) async {
    obj.#^ACTOR^#
// ACTOR-DAG: Decl[InstanceMethod]/CurrNominal:   receiveSendable({#arg: MySendable#})[' async'][#Void#]; name=receiveSendable(arg:){{$}}
// ACTOR-DAG: Decl[InstanceMethod]/CurrNominal: receiveNonSendable({#arg: MyNonSendable#})[' async'][#Void#]; name=receiveNonSendable(arg:){{$}}
}

func testClosure(obj: (Int) async -> Void) {
  obj(#^CLOSURE_CALL^#)
// CLOSURE_CALL-DAG: Pattern/Local/Flair[ArgLabels]: ['(']{#Int#}[')'][' async'][#Void#]; name={{$}}
}

func test() {
  struct Foo {
    var value: String? {
      get async { nil }
    }
  }

  var globalValue: String? {
    get async { nil }
  }

  let foo = Foo()
  foo.#^EXPLICITLY_ASYNC_PROPERTY^#
// EXPLICITLY_ASYNC_PROPERTY-DAG: Decl[InstanceVar]/CurrNominal: value[#String?#][' async']; name=value{{$}}

  #^EXPLICIT_GLOBAL_VAR^#
// EXPLICIT_GLOBAL_VAR-DAG: Decl[LocalVar]/Local: globalValue[#String?#][' async']; name=globalValue{{$}}

}
