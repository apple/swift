// RUN: %target-swift-frontend -primary-file %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend -O -primary-file %s -emit-ir | %FileCheck %s --check-prefix=OPT

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

// CHECK-LABEL: define hidden swiftcc void @"$s16non_fixed_return1CVACyxGycfC"(ptr noalias nocapture sret(%swift.opaque) %0

// CHECK-LABEL: define hidden swiftcc void @"$s16non_fixed_return6createyAA1CVyxGxlF"(ptr noalias nocapture sret(%swift.opaque) %0, ptr noalias nocapture %1, ptr %T)
// CHECK:  call swiftcc void @"$s16non_fixed_return1CVACyxGycfC"(ptr noalias nocapture sret(%swift.opaque) %0
// CHECK:  ret void

// Make sure we don't loose the stores for the optional UInt32? in optimize mode.
// OPT-LABEL: define hidden swiftcc void @"$s16non_fixed_return1CVACyxGycfC"(ptr noalias nocapture sret(%swift.opaque) %0
// OPT:  store i32 0, ptr [[BASE:%[0-9]+]]
// OPT:  [[ADDR2:%.*]] = getelementptr inbounds %Ts6UInt32VSg, ptr [[BASE]], i64 0, i32 1
// OPT:  store i1 true, ptr [[ADDR2]]
// OPT:  [[ADDR4:%.*]] = getelementptr inbounds %T16non_fixed_return1BV, ptr [[BASE]], i64 0, i32 2
// OPT: call void @llvm.memset.p0.i64(ptr {{.*}}[[ADDR4]], i8 0, i64 16, i1 false)
// OPT: ret void
