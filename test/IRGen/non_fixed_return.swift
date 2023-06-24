// RUN: %target-swift-frontend %use_no_opaque_pointers -primary-file %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend %use_no_opaque_pointers -O -primary-file %s -emit-ir | %FileCheck %s --check-prefix=OPT
// RUN: %target-swift-frontend -primary-file %s -emit-ir
// RUN: %target-swift-frontend -O -primary-file %s -emit-ir

// REQUIRES: CPU=x86_64

struct V : OptionSet {
  var rawValue: UInt64

  init() {
      self.init(rawValue: 0)
  }

  init(rawValue: UInt64) {
      self.rawValue = rawValue
  }

  subscript(index: Int) -> Bool {
      get {
          return (rawValue & UInt64(1) << UInt64(index)) != 0
      }
      set {
          let bit = UInt64(1) << UInt64(index)
          if newValue {
              rawValue = rawValue | bit
          } else {
              rawValue = rawValue & ~bit
          }
      }
  }

  var count: Int { return 64 }
  var indices: CountableRange<Int> { return 0 ..< 64 }
}

struct A {
    var a = 0.0
    var a2 = 0.0
}

struct B {
  var b: UInt32?
  var b2 = V()
  var b3 = 0.0
}

struct C<Data> {
   var c = A()
   var c2: Data?
   private var c3 = B()
}

func create<T>(_ t: T) -> C<T> {
    return C<T>()
}
// We use opaque storage types because LLVM performs type based analysis based
// on the sret storage type which goes wrong with non fixed types.

// CHECK-LABEL: define hidden swiftcc void @"$s16non_fixed_return1CVACyxGycfC"(%swift.opaque* noalias nocapture sret(%swift.opaque) %0

// CHECK-LABEL: define hidden swiftcc void @"$s16non_fixed_return6createyAA1CVyxGxlF"(%swift.opaque* noalias nocapture sret(%swift.opaque) %0, %swift.opaque* noalias nocapture %1, %swift.type* %T)
// CHECK:  [[CAST_PARAM:%.*]] = bitcast %swift.opaque* %0 to %T16non_fixed_return1CV*
// CHECK:  [[CAST_ARG:%.*]] = bitcast %T16non_fixed_return1CV* [[CAST_PARAM]] to %swift.opaque*
// CHECK:  call swiftcc void @"$s16non_fixed_return1CVACyxGycfC"(%swift.opaque* noalias nocapture sret(%swift.opaque) [[CAST_ARG]]
// CHECK:  ret void

// Make sure we don't loose the stores for the optional UInt32? in optimize mode.
// OPT-LABEL: define hidden swiftcc void @"$s16non_fixed_return1CVACyxGycfC"(%swift.opaque* noalias nocapture sret(%swift.opaque) %0
// OPT:  [[ADDR:%.*]] = bitcast i8* [[BASE:%.*]] to i32*
// OPT:  store i32 0, i32* [[ADDR]]
// OPT:  [[ADDR2:%.*]] = getelementptr inbounds i8, i8* [[BASE:%.*]], i64 4
// OPT:  [[ADDR3:%.*]] = bitcast i8* [[ADDR2]] to i1*
// OPT:  store i1 true, i1* [[ADDR3]]
// OPT:  [[ADDR4:%.*]] = getelementptr inbounds i8, i8* [[BASE]], i64 8
// OPT: call void @llvm.memset.p0i8.i64(i8* {{.*}}[[ADDR4]], i8 0, i64 16, i1 false)
// OPT: ret void
