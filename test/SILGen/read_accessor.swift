// RUN: %target-swift-emit-silgen %s | %FileCheck %s

struct SimpleTest {
  var stored: String

  var readable: String {
// CHECK-LABEL: sil hidden [ossa] @$s13read_accessor10SimpleTestV8readableSSvr
// CHECK-SAME:    : $@yield_once @convention(method) (@guaranteed SimpleTest) -> @yields @guaranteed String {
// CHECK:         [[T0:%.*]] = struct_extract %0 : $SimpleTest, #SimpleTest.stored
// CHECK-NEXT:    yield [[T0]] : $String, resume bb1, unwind bb2
// CHECK:       bb1:
// CHECK-NEXT:    [[RET:%.*]] = tuple ()
// CHECK-NEXT:    return [[RET]] : $()
// CHECK:       bb2:
// CHECK-NEXT:    unwind
    _read {
      yield stored
    }
  }

// CHECK-LABEL: sil hidden [ossa] @$s13read_accessor10SimpleTestV3getSSyF
// CHECK:         [[T0:%.*]] = begin_access [read] [unknown] %0
// CHECK-NEXT:    [[SELF:%.*]] = load [copy] [[T0]] : $*SimpleTest
// CHECK-NEXT:    end_access [[T0]]
// CHECK-NEXT:    [[SELF_BORROW:%.*]] = begin_borrow [[SELF]]
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[READFN:%.*]] = function_ref @$s13read_accessor10SimpleTestV8readableSSvr : $@yield_once @convention(method) (@guaranteed SimpleTest) -> @yields @guaranteed String
// CHECK-NEXT:    ([[VALUE:%.*]], [[TOKEN:%.*]]) = begin_apply [[READFN]]([[SELF_BORROW]])
// CHECK-NEXT:    [[RET:%.*]] = copy_value [[VALUE]] : $String
// CHECK-NEXT:    end_apply [[TOKEN]]
// CHECK-NEXT:    end_borrow [[SELF_BORROW]]
// CHECK-NEXT:    destroy_value [[SELF]]
// CHECK-NEXT:    return [[RET]] : $String
  mutating func get() -> String {
    return readable
  }
}

class GetterSynthesis {
  var stored: String = "hello"
  var readable: String {
// CHECK-LABEL: sil hidden [transparent] [ossa] @$s13read_accessor15GetterSynthesisC8readableSSvg
// CHECK:         [[READFN:%.*]] = function_ref @$s13read_accessor15GetterSynthesisC8readableSSvr
// CHECK-NEXT:    ([[VALUE:%.*]], [[TOKEN:%.*]]) = begin_apply [[READFN]](%0)
// CHECK-NEXT:    [[RET:%.*]] = copy_value [[VALUE]] : $String
// CHECK-NEXT:    end_apply [[TOKEN]]
// CHECK-NEXT:    return [[RET]] : $String
    _read {
      yield stored
    }
  }
}

func void() {}

struct TupleReader {
  var stored: String

  subscript(i: Int) -> String {
    _read { yield stored }
  }

  func compute() -> String { return stored }
  func index() -> Int { return 0 }

  var readable: ((String, ()), String, ()) {
// CHECK-LABEL: sil hidden [ossa] @$s13read_accessor11TupleReaderV8readableSS_ytt_SSyttvr
// CHECK:         debug_value %0
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[INDEXFN:%.*]] = function_ref @$s13read_accessor11TupleReaderV5indexSiyF
// CHECK-NEXT:    [[INDEX:%.*]] = apply [[INDEXFN]](%0)
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[COMPUTEFN:%.*]] = function_ref @$s13read_accessor11TupleReaderV7computeSSyF
// CHECK-NEXT:    [[COMPUTE:%.*]] = apply [[COMPUTEFN]](%0)
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[VOIDFN:%.*]] = function_ref @$s13read_accessor4voidyyF
// CHECK-NEXT:    apply [[VOIDFN]]()
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[SUBREADFN:%.*]] = function_ref @$s13read_accessor11TupleReaderVySSSicir
// CHECK-NEXT:    ([[SUBREAD:%.*]], [[SUBTOKEN:%.*]]) = begin_apply [[SUBREADFN]]([[INDEX]], %0)
// CHECK-NEXT:    yield ([[SUBREAD]] : $String, [[COMPUTE]] : $String), resume bb1, unwind bb2
// CHECK:       bb1:
// CHECK-NEXT:    end_apply [[SUBTOKEN]]
// CHECK-NEXT:    destroy_value [[COMPUTE]] : $String
// CHECK-NEXT:    [[T0:%.*]] = tuple ()
// CHECK-NEXT:    return [[T0]] : $()
// CHECK:       bb2:
//   Should this be an abort_apply?
// CHECK-NEXT:    end_apply [[SUBTOKEN]]
// CHECK-NEXT:    destroy_value [[COMPUTE]] : $String
// CHECK-NEXT:    unwind
// CHECK-LABEL: } // end sil function '$s13read_accessor11TupleReaderV8readableSS_ytt_SSyttvr'
    _read {
      yield (((self[index()], ()), compute(), void()))
    }
  }

// CHECK-LABEL: sil hidden [ossa] @$s13read_accessor11TupleReaderV11useReadableyyF
// CHECK:         [[READFN:%.*]] = function_ref @$s13read_accessor11TupleReaderV8readableSS_ytt_SSyttvr
// CHECK-NEXT:    ([[FIRST:%.*]], [[SECOND:%.*]], [[TOKEN:%.*]]) = begin_apply [[READFN]](%0)
//   FIXME: this materialization is silly
// CHECK-NEXT:    [[TEMP:%.*]] = alloc_stack $((String, ()), String, ())
// CHECK-NEXT:    [[TEMP_0:%.*]] = tuple_element_addr [[TEMP]] : $*((String, ()), String, ()), 0
// CHECK-NEXT:    [[TEMP_1:%.*]] = tuple_element_addr [[TEMP]] : $*((String, ()), String, ()), 1
// CHECK-NEXT:    [[TEMP_2:%.*]] = tuple_element_addr [[TEMP]] : $*((String, ()), String, ()), 2
// CHECK-NEXT:    [[TEMP_0_0:%.*]] = tuple_element_addr [[TEMP_0]] : $*(String, ()), 0
// CHECK-NEXT:    [[TEMP_0_1:%.*]] = tuple_element_addr [[TEMP_0]] : $*(String, ()), 1
// CHECK-NEXT:    [[T0:%.*]] = copy_value [[FIRST]] : $String
// CHECK-NEXT:    store [[T0]] to [init] [[TEMP_0_0]]
// CHECK-NEXT:    [[T0:%.*]] = copy_value [[SECOND]] : $String
// CHECK-NEXT:    store [[T0]] to [init] [[TEMP_1]]
// CHECK-NEXT:    [[TUPLE:%.*]] = load [copy] [[TEMP]]
// CHECK-NEXT:    destructure_tuple
// CHECK-NEXT:    destructure_tuple
// CHECK-NEXT:    destroy_addr [[TEMP]]
// CHECK-NEXT:    end_apply
// CHECK-LABEL: } // end sil function '$s13read_accessor11TupleReaderV11useReadableyyF'
  func useReadable() {
    var v = readable
  }

  var computed: String {
    return compute()
  }

// CHECK-LABEL: sil hidden [ossa] @$s13read_accessor11TupleReaderV0A8ComputedSSvr
// CHECK:         [[GETTER:%.*]] = function_ref @$s13read_accessor11TupleReaderV8computedSSvg
// CHECK-NEXT:    [[VALUE:%.]] = apply [[GETTER]](%0)
// CHECK-NEXT:    [[BORROW:%.*]] = begin_borrow [[VALUE]] : $String
// CHECK-NEXT:    yield [[BORROW]] : $String, resume bb1
// CHECK:       bb1:
// CHECK-NEXT:    end_borrow [[BORROW]] : $String
// CHECK-NEXT:    destroy_value [[VALUE]] : $String
  var readComputed : String {
    _read {
      yield computed
    }
  }
}

struct TestKeyPath {
  var readable: String {
    _read {
      yield ""
    }
  }

  func useKeyPath() -> String {
    return self[keyPath: \.readable]
  }
}
//   Key-path getter for TestKeyPath.readable
// CHECK-LABEL: sil shared [thunk] [ossa] @$s13read_accessor11TestKeyPathV8readableSSvpACTK
// CHECK:       bb0(%0 : $*String, %1 : $*TestKeyPath):
// CHECK-NEXT:    [[SELF:%.*]] = load [trivial] %1
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[READ:%.*]] = function_ref @$s13read_accessor11TestKeyPathV8readableSSvr
// CHECK-NEXT:    ([[VALUE:%.*]], [[TOKEN:%.*]]) = begin_apply [[READ]]([[SELF]])
// CHECK-NEXT:    [[COPY:%.*]] = copy_value [[VALUE]]
// CHECK-NEXT:    end_apply [[TOKEN]]
// CHECK-NEXT:    store [[COPY]] to [init] %0 : $*String
// CHECK-NEXT:    [[RET:%.*]] = tuple ()
// CHECK-NEXT:    return [[RET]] : $()
// CHECK-LABEL: } // end sil function '$s13read_accessor11TestKeyPathV8readableSSvpACTK'

//   Check that we emit a read coroutine but not a getter for this.
//   This test assumes that we emit accessors in a particular order.
// CHECK-LABEL: sil [transparent] [ossa] @$s13read_accessor20TestBorrowedPropertyV14borrowedStringSSvpfi
// CHECK-NOT:   sil [transparent] [serialized] [ossa] @$s13read_accessor20TestBorrowedPropertyV14borrowedStringSSvg
// CHECK:       sil [transparent] [serialized] [ossa] @$s13read_accessor20TestBorrowedPropertyV14borrowedStringSSvr
// CHECK-NOT:   sil [transparent] [serialized] [ossa] @$s13read_accessor20TestBorrowedPropertyV14borrowedStringSSvg
// CHECK-LABEL: sil [transparent] [serialized] [ossa] @$s13read_accessor20TestBorrowedPropertyV14borrowedStringSSvs
public struct TestBorrowedProperty {
  @_borrowed
  public var borrowedString = ""
}

protocol ReadableTitle {
  @_borrowed
  var title: String { get }
}
class OverridableGetter : ReadableTitle {
  var title: String = ""
}
//   The read witness thunk does a direct call to the concrete read accessor.
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s13read_accessor17OverridableGetterCAA13ReadableTitleA2aDP5titleSSvrTW
// CHECK:       function_ref @$s13read_accessor17OverridableGetterC5titleSSvr
// CHECK-LABEL: // end sil function '$s13read_accessor17OverridableGetterCAA13ReadableTitleA2aDP5titleSSvrTW'
//   The concrete read accessor is generated on-demand and does a class dispatch to the getter.
// CHECK-LABEL: sil shared [ossa] @$s13read_accessor17OverridableGetterC5titleSSvr
// CHECK:       class_method %0 : $OverridableGetter, #OverridableGetter.title!getter
// CHECK-LABEL: // end sil function '$s13read_accessor17OverridableGetterC5titleSSvr'

protocol GettableTitle {
  var title: String { get }
}
class OverridableReader : GettableTitle {
  @_borrowed
  var title: String = ""
}
//   The getter witness thunk does a direct call to the concrete getter.
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s13read_accessor17OverridableReaderCAA13GettableTitleA2aDP5titleSSvgTW
// CHECK:       function_ref @$s13read_accessor17OverridableReaderC5titleSSvg
// CHECK-LABEL: // end sil function '$s13read_accessor17OverridableReaderCAA13GettableTitleA2aDP5titleSSvgTW'
//   The concrete getter is generated on-demand and does a class dispatch to the read accessor.
// CHECK-LABEL: sil shared [ossa] @$s13read_accessor17OverridableReaderC5titleSSvg
// CHECK:       class_method %0 : $OverridableReader, #OverridableReader.title!read
// CHECK-LABEL: // end sil function '$s13read_accessor17OverridableReaderC5titleSSvg'
