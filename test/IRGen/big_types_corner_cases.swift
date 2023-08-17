// XFAIL: CPU=powerpc64le
// XFAIL: CPU=s390x
// RUN: %target-swift-frontend -disable-type-layout %s -emit-ir | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize
// REQUIRES: optimized_stdlib
// UNSUPPORTED: CPU=powerpc64le

public struct BigStruct {
  var i0 : Int32 = 0
  var i1 : Int32 = 1
  var i2 : Int32 = 2
  var i3 : Int32 = 3
  var i4 : Int32 = 4
  var i5 : Int32 = 5
  var i6 : Int32 = 6
  var i7 : Int32 = 7
  var i8 : Int32 = 8
}

func takeClosure(execute block: () -> Void) {
}

class OptionalInoutFuncType {
  private var lp :  BigStruct?
  private var _handler : ((BigStruct?, Error?) -> ())?

  func execute(_ error: Error?) {
    var p : BigStruct?
    var handler: ((BigStruct?, Error?) -> ())?
    
    takeClosure {
      p = self.lp
      handler = self._handler
      self._handler = nil
    }

    handler?(p, error)
  }
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} i32 @main(i32 %0, ptr %1)
// CHECK: call void @llvm.lifetime.start
// CHECK: call void @llvm.memcpy
// CHECK: call void @llvm.lifetime.end
// CHECK: ret i32 0
let bigStructGlobalArray : [BigStruct] = [
  BigStruct()
]

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} internal swiftcc void @"$s22big_types_corner_cases21OptionalInoutFuncTypeC7executeyys5Error_pSgFyyXEfU_"(ptr nocapture dereferenceable({{.*}}) %0, ptr %1, ptr nocapture dereferenceable({{.*}})
// CHECK: call void @"$s22big_types_corner_cases9BigStructVSgs5Error_pSgIegng_SgWOe
// CHECK: call void @"$s22big_types_corner_cases9BigStructVSgs5Error_pSgIegng_SgWOy
// CHECK: ret void

public func f1_returns_BigType(_ x: BigStruct) -> BigStruct {
  return x
}

public func f2_returns_f1() -> (_ x: BigStruct) -> BigStruct {
  return f1_returns_BigType
}

public func f3_uses_f2() {
  let x = BigStruct()
  let useOfF2 = f2_returns_f1()
  let _ = useOfF2(x)
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s22big_types_corner_cases10f3_uses_f2yyF"()
// CHECK: call swiftcc void @"$s22big_types_corner_cases9BigStructVACycfC"(ptr noalias nocapture sret({{.*}}) 
// CHECK: call swiftcc { ptr, ptr } @"$s22big_types_corner_cases13f2_returns_f1AA9BigStructVADcyF"()
// CHECK: call swiftcc void {{.*}}(ptr noalias nocapture sret({{.*}}) {{.*}}, ptr noalias nocapture dereferenceable({{.*}}) {{.*}}, ptr swiftself {{.*}})
// CHECK: ret void

public func f4_tuple_use_of_f2() {
  let x = BigStruct()
  let tupleWithFunc = (f2_returns_f1(), x)
  let useOfF2 = tupleWithFunc.0
  let _ = useOfF2(x)
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s22big_types_corner_cases18f4_tuple_use_of_f2yyF"()
// CHECK: [[TUPLE:%.*]] = call swiftcc { ptr, ptr } @"$s22big_types_corner_cases13f2_returns_f1AA9BigStructVADcyF"()
// CHECK: [[TUPLE_EXTRACT:%.*]] = extractvalue { ptr, ptr } [[TUPLE]], 0
// CHECK:  call swiftcc void [[TUPLE_EXTRACT]](ptr noalias nocapture sret({{.*}}) {{.*}}, ptr noalias nocapture dereferenceable({{.*}}) {{.*}}, ptr swiftself %{{.*}})
// CHECK: ret void

public class BigClass {
  public init() {
  }

  public var optVar: ((BigStruct)-> Void)? = nil
    
  func useBigStruct(bigStruct: BigStruct) {
    optVar!(bigStruct)
  }
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} hidden swiftcc void @"$s22big_types_corner_cases8BigClassC03useE6Struct0aH0yAA0eH0V_tF"(ptr noalias nocapture dereferenceable({{.*}}) %0, ptr swiftself %1)
// CHECK: call swiftcc void {{.*}}(ptr noalias nocapture dereferenceable({{.*}}) %0, ptr swiftself 
// CHECK: ret void

public struct MyStruct {
  public let a: Int
  public let b: String?
 }

typealias UploadFunction = ((MyStruct, Int?) -> Void) -> Void
func takesUploader(_ u: UploadFunction) { }

class Foo {
  func blam() {
    takesUploader(self.myMethod) // crash compiling this
  }

  func myMethod(_ callback: (MyStruct, Int) -> Void) -> Void { }
}

// CHECK-LABEL: define internal swiftcc void @"$s22big_types_corner_cases3FooC4blamyyFyyAA8MyStructV_SitXEcACcfu_yyAF_SitXEcfu0_"(ptr %0, ptr %1, ptr %2)

public enum LargeEnum {
  public enum InnerEnum {
    case simple(Int64)
    case hard(Int64, String?, Int64)
  }
  case Empty1
  case Empty2
  case Full(InnerEnum)
}

public func enumCallee(_ x: LargeEnum) {
  switch x {
    case .Full(let inner): print(inner)
    case .Empty1: break
    case .Empty2: break
  }
}
// CHECK-64-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s22big_types_corner_cases10enumCalleeyyAA9LargeEnumOF"(ptr noalias nocapture dereferenceable({{.*}}) %0) #0 {
// CHECK-64: alloca %T22big_types_corner_cases9LargeEnumO05InnerF0O
// CHECK-64: alloca %T22big_types_corner_cases9LargeEnumO
// CHECK-64: $ss5print_9separator10terminatoryypd_S2StF
// CHECK-64: ret void

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} internal swiftcc void @"$s22big_types_corner_cases8SuperSubC1fyyFAA9BigStructVycfU_"(ptr noalias nocapture sret({{.*}}) %0, ptr %1)
// CHECK-64: [[ALLOC1:%.*]] = alloca %T22big_types_corner_cases9BigStructV
// CHECK-64: [[ALLOC2:%.*]] = alloca %T22big_types_corner_cases9BigStructV
// CHECK-64: [[ALLOC3:%.*]] = alloca %T22big_types_corner_cases9BigStructVSg
// CHECK-64: call swiftcc void @"$s22big_types_corner_cases9SuperBaseC4boomAA9BigStructVyF"(ptr noalias nocapture sret({{.*}}) [[ALLOC1]], ptr swiftself {{.*}})
// CHECK: ret void
class SuperBase {
  func boom() -> BigStruct {
    return BigStruct()
  }
}

class SuperSub : SuperBase {
  override func boom() -> BigStruct {
    return BigStruct()
  }
  func f() {
    let x = {
      nil ?? super.boom()
    }
    x()
  }
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s22big_types_corner_cases10MUseStructV16superclassMirrorAA03BigF0VSgvg"(ptr noalias nocapture sret({{.*}}) %0, ptr noalias nocapture swiftself dereferenceable({{.*}}) %1)
// CHECK: [[ALLOC:%.*]] = alloca %T22big_types_corner_cases9BigStructVSg
// CHECK: [[LOAD:%.*]] = load ptr, ptr %.callInternalLet.data
// CHECK: call swiftcc void %{{[0-9]+}}(ptr noalias nocapture sret({{.*}}) [[ALLOC]], ptr swiftself [[LOAD]])
// CHECK: ret void
public struct MUseStruct {
  var x = BigStruct()
  public var superclassMirror: BigStruct? {
    return callInternalLet()
  }

  internal let callInternalLet: () -> BigStruct?
}

// CHECK-LABEL-64: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s22big_types_corner_cases18stringAndSubstringSS_s0G0VtyF"(ptr noalias nocapture sret({{.*}}) %0) #0 {
// CHECK-LABEL-32: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s22big_types_corner_cases18stringAndSubstringSS_s0G0VtyF"(ptr noalias nocapture sret({{.*}}) %0) #0 {
// CHECK: alloca %TSs
// CHECK: alloca %TSs
// CHECK: ret void
public func stringAndSubstring() -> (String, Substring) {
  let s = "Hello, World"
  let a = Substring(s).dropFirst()
  return (s, a)
}

func bigStructGet() -> BigStruct {
  return BigStruct()
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s22big_types_corner_cases11testGetFuncyyF"()
// CHECK: ret void
public func testGetFunc() {
  let testGetPtr: @convention(thin) () -> BigStruct = bigStructGet
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} hidden swiftcc void @"$s22big_types_corner_cases7TestBigC4testyyF"(ptr swiftself %0)
// CHECK: [[CALL1:%.*]] = call {{.*}} @__swift_instantiateConcreteTypeFromMangledName({{.*}} @"$sSayy22big_types_corner_cases9BigStructVcSgGMD"
// CHECK: [[CALL2:%.*]] = call ptr @"$sSayy22big_types_corner_cases9BigStructVcSgGSayxGSlsWl
// CHECK:  call swiftcc void @"$sSlsE10firstIndex5where0B0QzSgSb7ElementQzKXE_tKF"(ptr noalias nocapture sret({{.*}}) %{{[0-9]+}}, ptr @"$s22big_types_corner_cases7TestBig{{.*}}", ptr null, ptr %{{[0-9]+}}, ptr [[CALL2]]
class TestBig {
    typealias Handler = (BigStruct) -> Void

    func test() {
        let arr = [Handler?]()
        let d = arr.firstIndex(where: { _ in true })
    }
    
    func test2() {
        let arr: [(ID: String, handler: Handler?)] = []
        for (_, handler) in arr {
        takeClosure {
          		handler?(BigStruct())
        	}
        }
    }
}

struct BigStructWithFunc {
    var incSize : BigStruct
    var foo: ((BigStruct) -> Void)?
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} hidden swiftcc void @"$s22big_types_corner_cases20UseBigStructWithFuncC5crashyyF"(ptr swiftself %0)
// CHECK: call swiftcc void @"$s22big_types_corner_cases20UseBigStructWithFuncC10callMethod
// CHECK: ret void
class UseBigStructWithFunc {
    var optBigStructWithFunc: BigStructWithFunc?

    func crash() {
        guard let bigStr = optBigStructWithFunc else { return }
        callMethod(ptr: bigStr.foo)
    }

    private func callMethod(ptr: ((BigStruct) -> Void)?) -> () {
    }
}

struct BiggerString {
  var str: String
  var double: Double
}

struct LoadableStructWithBiggerString {
    public var a1: BiggerString
    public var a2: [String]
    public var a3: [String]
}

class ClassWithLoadableStructWithBiggerString {
    public func f() -> LoadableStructWithBiggerString {
        return LoadableStructWithBiggerString(a1: BiggerString(str:"", double:0.0), a2: [], a3: [])
    }
}

//===----------------------------------------------------------------------===//
// https://github.com/apple/swift/issues/50609
//===----------------------------------------------------------------------===//

public typealias Filter = (BigStruct) -> Bool

public protocol Query {
    associatedtype Returned
}

public protocol ProtoQueryHandler {
    func forceHandle_1<Q: Query>(query: Q) -> Void
    func forceHandle_2<Q: Query>(query: Q) -> (Q.Returned, BigStruct?)
    func forceHandle_3<Q: Query>(query: Q) -> (Q.Returned, Filter?)
    func forceHandle_4<Q: Query>(query: Q) throws -> (Q.Returned, Filter?)
}

public protocol QueryHandler: ProtoQueryHandler {
    associatedtype Handled: Query
    func handle_1(query: Handled) -> Void
    func handle_2(query: Handled) -> (Handled.Returned, BigStruct?)
    func handle_3(query: Handled) -> (Handled.Returned, Filter?)
    func handle_4(query: Handled) throws -> (Handled.Returned, Filter?)
}


public extension QueryHandler {

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s22big_types_corner_cases12QueryHandlerPAAE13forceHandle_15queryyqd___tAA0E0Rd__lF"(ptr noalias nocapture %0, ptr{{.*}}, ptr{{.*}}, ptr {{.*}}.QueryHandler, ptr {{.*}}.Query, ptr noalias nocapture swiftself %1)
// CHECK: call swiftcc void {{.*}}(ptr noalias nocapture {{.*}}, ptr swiftself {{.*}})
// CHECK: ret void
    func forceHandle_1<Q: Query>(query: Q) -> Void {
        guard let body = handle_1 as? (Q) -> Void else {
            fatalError("handler \(self) is expected to handle query \(query)")
        }
        body(query)
    }

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s22big_types_corner_cases12QueryHandlerPAAE13forceHandle_25query8ReturnedQyd___AA9BigStructVSgtqd___tAA0E0Rd__lF"(ptr noalias nocapture sret({{.*}}) %0, ptr noalias nocapture %1, ptr noalias nocapture %2, ptr{{.*}}, ptr{{.*}}, ptr {{.*}}.QueryHandler, ptr {{.*}}.Query, ptr noalias nocapture swiftself %3)
// CHECK: [[ALLOC:%.*]] = alloca %T22big_types_corner_cases9BigStructVSg
// CHECK: call swiftcc void {{.*}}(ptr noalias nocapture sret({{.*}}) [[ALLOC]], ptr noalias nocapture {{.*}}, ptr noalias nocapture {{.*}}, ptr swiftself {{.*}})
// CHECK: ret void
    func forceHandle_2<Q: Query>(query: Q) -> (Q.Returned, BigStruct?) {
        guard let body = handle_2 as? (Q) -> (Q.Returned, BigStruct?) else {
            fatalError("handler \(self) is expected to handle query \(query)")
        }
        return body(query)
    }

// CHECK-LABEL-64: define{{( dllexport)?}}{{( protected)?}} swiftcc { i64, i64 } @"$s22big_types_corner_cases12QueryHandlerPAAE13forceHandle_35query8ReturnedQyd___SbAA9BigStructVcSgtqd___tAA0E0Rd__lF"(ptr noalias nocapture %0, ptr noalias nocapture %1, ptr{{.*}}, ptr{{.*}}, ptr {{.*}}.QueryHandler, ptr {{.*}}.Query, ptr noalias nocapture swiftself %2)
// CHECK-64: {{.*}} = call swiftcc { i64, i64 } {{.*}}(ptr noalias nocapture {{.*}}, ptr noalias nocapture {{.*}}, ptr swiftself {{.*}})
// CHECK-64: ret { i64, i64 }

// CHECK-LABEL-32: define{{( dllexport)?}}{{( protected)?}} swiftcc { i32, i32} @"$s22big_types_corner_cases12QueryHandlerPAAE13forceHandle_35query8ReturnedQyd___SbAA9BigStructVcSgtqd___tAA0E0Rd__lF"(ptr noalias nocapture %0, ptr noalias nocapture %1, ptr{{.*}}, ptr{{.*}}, ptr {{.*}}.QueryHandler, ptr {{.*}}.Query, ptr noalias nocapture swiftself %2)
// CHECK-32: {{.*}} = call swiftcc { i32, i32 } {{.*}}(ptr noalias nocapture {{.*}}, ptr noalias nocapture {{.*}}, ptr swiftself {{.*}})
// CHECK-32: ret { i32, i32 }
    func forceHandle_3<Q: Query>(query: Q) -> (Q.Returned, Filter?) {
        guard let body = handle_3 as? (Q) -> (Q.Returned, Filter?) else {
            fatalError("handler \(self) is expected to handle query \(query)")
        }
        return body(query)
    }

// CHECK-LABEL-64: define{{( dllexport)?}}{{( protected)?}} swiftcc { i64, i64 } @"$s22big_types_corner_cases12QueryHandlerPAAE13forceHandle_45query8ReturnedQyd___SbAA9BigStructVcSgtqd___tKAA0E0Rd__lF"(ptr noalias nocapture %0, ptr noalias nocapture %1, ptr{{.*}}, ptr{{.*}}, ptr {{.*}}.QueryHandler, ptr {{.*}}.Query, ptr noalias nocapture swiftself %2, ptr swifterror %3)
// CHECK-64: {{.*}} = call swiftcc { i64, i64 } {{.*}}(ptr noalias nocapture {{.*}}, ptr noalias nocapture {{.*}}, ptr swiftself {{.*}}, ptr noalias nocapture swifterror {{.*}})
// CHECK-64: ret { i64, i64 }

// CHECK-LABEL-32: define{{( dllexport)?}}{{( protected)?}} swiftcc { i32, i32} @"$s22big_types_corner_cases12QueryHandlerPAAE13forceHandle_45query8ReturnedQyd___SbAA9BigStructVcSgtqd___tKAA0E0Rd__lF"(ptr noalias nocapture %0, ptr noalias nocapture %1, ptr{{.*}}, ptr{{.*}}, ptr {{.*}}.QueryHandler, ptr {{.*}}.Query, ptr noalias nocapture swiftself %2, ptr swifterror %3)
// CHECK-32: {{.*}} = call swiftcc { i32, i32 } {{.*}}(ptr noalias nocapture {{.*}}, ptr noalias nocapture {{.*}}, ptr swiftself {{.*}}, ptr noalias nocapture {{.*}})
// CHECK-32: ret { i32, i32 }
    func forceHandle_4<Q: Query>(query: Q) throws -> (Q.Returned, Filter?) {
        guard let body = handle_4 as? (Q) throws -> (Q.Returned, Filter?) else {
            fatalError("handler \(self) is expected to handle query \(query)")
        }
        return try body(query)
    }
}

public func foo() -> Optional<(a: Int?, b: Bool, c: (Int?)->BigStruct?)> {
  return nil
}

public func dontAssert() {
  let _ = foo()
}
