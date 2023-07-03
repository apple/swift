// RUN: %target-run-simple-swift(-Xfrontend -sil-verify-all -enable-experimental-feature MoveOnlyPartialConsumption) | %FileCheck %s
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all -enable-experimental-feature MoveOnlyPartialConsumption) | %FileCheck %s

// REQUIRES: executable_test

struct S : ~Copyable {
  let s: String
  init(_ s: String) { self.s = s }
  deinit {
    print("destroying \(s)")
  }
}
struct S2 : ~Copyable {
    var s1: S
    var s2: S
    init(_ s: String) {
      self.s1 = S("\(s).s1")
      self.s2 = S("\(s).s2")
    }
}
struct S3 : ~Copyable {
    var s1: S
    var s2: S
    var s3: S
    init(_ s: String) {
      self.s1 = S("\(s).s1")
      self.s2 = S("\(s).s2")
      self.s3 = S("\(s).s3")
    }
}

func consumeVal(_ s: consuming S) {}
func consumeVal(_ s: consuming S2) {}
func borrowVal(_ s: borrowing S) {}
func borrowVal(_ s: borrowing S2) {}

func marker(_ s: String) {
  print("\(#function): \(s)")
}

// Simple test that makes sure that we still after we consume have the lifetime
// of s be completely consumed by consumeVal.
// CHECK: destroying simpleTestVar().first.s1
// CHECK: destroying simpleTestVar().first.s2
// CHECK: destroying simpleTestVar().second.s1
// CHECK: destroying simpleTestVar().second.s2
// CHECK: marker(_:): simpleTestVar().1
@_silgen_name("simpleTestVar")
func simpleTestVar() {
    var s = S2("\(#function).first")
    s = S2("\(#function).second")
    consumeVal(s) // Lifetime of s should end here before end of scope.
    marker("\(#function).1")
}

// Simple test that proves that we can maximize lifetimes in a field sensitive
// manner. Since we only consume s.s1, s.s2's lifetime should still be maximized
// and be at end of scope.
// CHECK: destroying simpleTestVar2().first.s1
// CHECK: destroying simpleTestVar2().first.s2
// CHECK: destroying simpleTestVar2().second.s1
// CHECK: marker(_:): simpleTestVar2().1
// CHECK: destroying simpleTestVar2().second.s2
func simpleTestVar2() {
    var s = S2("\(#function).first")
    s = S2("\(#function).second")
    consumeVal(s.s1) // Lifetime of s1 should end here.
    marker("\(#function).1")
    // Lifetime of s2 should end at end of scope after marker.
}

// In this case, we consume all of s by consuming s.s1 and s.s2 separately, so
// all lifetimes should be done before marker.
// CHECK: destroying simpleTestVar3().first.s1
// CHECK: destroying simpleTestVar3().first.s2
// CHECK: destroying simpleTestVar3().second.s1
// CHECK: destroying simpleTestVar3().second.s2
// CHECK: marker(_:): simpleTestVar3().1
func simpleTestVar3() {
    var s = S2("\(#function).first")
    s = S2("\(#function).second")
    consumeVal(s.s1)
    consumeVal(s.s2)
    marker("\(#function).1") // Lifetimes should end before marker.
}

// In this case, we completely consume s and then reinitialize s implying we
// need to deal with two disjoint lifetimes. The second lifetime of s should end
// after marker.
// CHECK: destroying simpleTestVar3a().first.s1
// CHECK: destroying simpleTestVar3a().first.s2
// CHECK: destroying simpleTestVar3a().second.s1
// CHECK: destroying simpleTestVar3a().second.s2
// CHECK: marker(_:): simpleTestVar3a().1
// CHECK: marker(_:): simpleTestVar3a().2
// CHECK: destroying simpleTestVar3a().third.s1
// CHECK: destroying simpleTestVar3a().third.s2
func simpleTestVar3a() {
    var s = S2("\(#function).first")
    s = S2("\(#function).second")
    consumeVal(s.s1)
    consumeVal(s.s2)

    marker("\(#function).1")

    s = S2("\(#function).third")
    marker("\(#function).2")
}

// In this case, we have another borrowVal of s.s2. That should still let s.s2's
// lifetime end after marker.
// CHECK: destroying simpleTestVar3b().first.s1
// CHECK: destroying simpleTestVar3b().first.s2
// CHECK: destroying simpleTestVar3b().second.s1
// CHECK: marker(_:): simpleTestVar3b().1
// CHECK: destroying simpleTestVar3b().second.s2
func simpleTestVar3b() {
    var s = S2("\(#function).first")
    s = S2("\(#function).second")
    consumeVal(s.s1)
    borrowVal(s.s2)
    marker("\(#function).1") // s2 should end its lifetime after marker.
}

// In this case, we are testing reinitialization and making sure that we can
// handle two initializations properly. We also are testing conditional merge
// logic. Since in both cases below s is completely consumed in b, s's lifetime
// would end at marker.

// CHECK: destroying simpleTestVar4(_:_:)[false, false)].first.s1
// CHECK: destroying simpleTestVar4(_:_:)[false, false)].first.s2
// CHECK: marker(_:): simpleTestVar4(_:_:)[false, false)].1
// CHECK: destroying simpleTestVar4(_:_:)[false, false)].second.s1
// CHECK: destroying simpleTestVar4(_:_:)[false, false)].second.s2
// CHECK: destroying simpleTestVar4(_:_:)[false, false)].third.s1
// CHECK: destroying simpleTestVar4(_:_:)[false, false)].third.s2
// CHECK: marker(_:): simpleTestVar4(_:_:)[false, false)].2

// CHECK: destroying simpleTestVar4(_:_:)[false, true)].first.s1
// CHECK: destroying simpleTestVar4(_:_:)[false, true)].first.s2
// CHECK: marker(_:): simpleTestVar4(_:_:)[false, true)].1
// CHECK: destroying simpleTestVar4(_:_:)[false, true)].second.s1
// CHECK: destroying simpleTestVar4(_:_:)[false, true)].second.s2
// CHECK: destroying simpleTestVar4(_:_:)[false, true)].third.s1
// CHECK: destroying simpleTestVar4(_:_:)[false, true)].third.s2

// CHECK: destroying simpleTestVar4(_:_:)[true, false)].first.s1
// CHECK: destroying simpleTestVar4(_:_:)[true, false)].first.s2
// CHECK: destroying simpleTestVar4(_:_:)[true, false)].second.s1
// CHECK: destroying simpleTestVar4(_:_:)[true, false)].second.s2
// CHECK: destroying simpleTestVar4(_:_:)[true, false)].third.s1
// CHECK: destroying simpleTestVar4(_:_:)[true, false)].third.s2
// CHECK: marker(_:): simpleTestVar4(_:_:)[true, false)].2

// CHECK: destroying simpleTestVar4(_:_:)[true, true)].first.s1
// CHECK: destroying simpleTestVar4(_:_:)[true, true)].first.s2
// CHECK: destroying simpleTestVar4(_:_:)[true, true)].second.s1
// CHECK: destroying simpleTestVar4(_:_:)[true, true)].second.s2
// CHECK: destroying simpleTestVar4(_:_:)[true, true)].third.s1
// CHECK: destroying simpleTestVar4(_:_:)[true, true)].third.s2
func simpleTestVar4(_ b1: Bool, _ b2: Bool) {
    var s = S2("\(#function)[\(b1), \(b2))].first")
    s = S2("\(#function)[\(b1), \(b2))].second")

    if b1 {
        consumeVal(s)
    } else {
        marker("\(#function)[\(b1), \(b2))].1")
        // S's lifetime should end after marker in this block.
    }

    s = S2("\(#function)[\(b1), \(b2))].third")

    if b2 {
        consumeVal(s)
    } else {
        marker("\(#function)[\(b1), \(b2))].2")
        // S's 2nd lifetime should end after marker in this block.
    }
}

// This test is similar to the previous, except we are consuming different
// values along the if/else branch that completely covers the value. As a result
// of this, we need to end the lifetime of s in the branches.
// CHECK: destroying simpleTestVar6(_:)[false].first.s1
// CHECK: destroying simpleTestVar6(_:)[false].first.s2
// CHECK: destroying simpleTestVar6(_:)[false].second.s2
// CHECK: marker(_:): simpleTestVar6(_:)[false].2
// CHECK: destroying simpleTestVar6(_:)[false].second.s1
// CHECK: destroying simpleTestVar6(_:)[false].third.s1
// CHECK: destroying simpleTestVar6(_:)[false].third.s2

// CHECK: destroying simpleTestVar6(_:)[true].first.s1
// CHECK: destroying simpleTestVar6(_:)[true].first.s2
// CHECK: destroying simpleTestVar6(_:)[true].second.s1
// CHECK: marker(_:): simpleTestVar6(_:)[true].1
// CHECK: destroying simpleTestVar6(_:)[true].second.s2
// CHECK: destroying simpleTestVar6(_:)[true].third.s1
// CHECK: destroying simpleTestVar6(_:)[true].third.s2
func simpleTestVar6(_ b: Bool) {
    var s = S2("\(#function)[\(b)].first")
    s = S2("\(#function)[\(b)].second")

    if b {
        consumeVal(s.s1) // end of s.s1's lifetime.
        marker("\(#function)[\(b)].1")
        // s.s2 should end here.
    } else {
        consumeVal(s.s2) // end of s.s2's lifetime
        marker("\(#function)[\(b)].2")
        // end of s.s1's lifetime should end after marker.
    }

    s = S2("\(#function)[\(b)].third")
}

// In this case, we are using S3 implying we have three fields. So despite the
// fact that we are deleting these two values in the if-else branches, s3's
// lifetime needs to end at end of scope.
// CHECK: destroying simpleTestVar6a(_:)[false].first.s1
// CHECK: destroying simpleTestVar6a(_:)[false].first.s2
// CHECK: destroying simpleTestVar6a(_:)[false].first.s3
// CHECK: destroying simpleTestVar6a(_:)[false].second.s2
// CHECK: marker(_:): simpleTestVar6a(_:)[false].2
// CHECK: destroying simpleTestVar6a(_:)[false].second.s1
// CHECK: marker(_:): simpleTestVar6a(_:)[false].3
// CHECK: destroying simpleTestVar6a(_:)[false].second.s3

// CHECK: destroying simpleTestVar6a(_:)[true].first.s1
// CHECK: destroying simpleTestVar6a(_:)[true].first.s2
// CHECK: destroying simpleTestVar6a(_:)[true].first.s3
// CHECK: destroying simpleTestVar6a(_:)[true].second.s1
// CHECK: marker(_:): simpleTestVar6a(_:)[true].1
// CHECK: destroying simpleTestVar6a(_:)[true].second.s2
// CHECK: marker(_:): simpleTestVar6a(_:)[true].3
// CHECK: destroying simpleTestVar6a(_:)[true].second.s3
func simpleTestVar6a(_ b: Bool) {
    var s = S3("\(#function)[\(b)].first")
    s = S3("\(#function)[\(b)].second")

    if b {
        consumeVal(s.s1) // end of s.s1's lifetime.
        marker("\(#function)[\(b)].1")
        // s.s2 should end here.
    } else {
        consumeVal(s.s2) // end of s.s2's lifetime
        marker("\(#function)[\(b)].2")
        // end of s.s1's lifetime should end after marker.
    }

    marker("\(#function)[\(b)].3")
    // s.s3's lifetime should end here.
}

// In this case, we are using S3, but we are consuming two disjoint parts of S
// in the if statement so we cover again completely.
// CHECK: destroying simpleTestVar6b(_:)[false].first.s1
// CHECK: destroying simpleTestVar6b(_:)[false].first.s2
// CHECK: destroying simpleTestVar6b(_:)[false].first.s3
// CHECK: destroying simpleTestVar6b(_:)[false].second.s2
// CHECK: marker(_:): simpleTestVar6b(_:)[false].2
// CHECK: destroying simpleTestVar6b(_:)[false].second.s3
// CHECK: destroying simpleTestVar6b(_:)[false].second.s1
// CHECK: marker(_:): simpleTestVar6b(_:)[false].3

// CHECK: destroying simpleTestVar6b(_:)[true].first.s1
// CHECK: destroying simpleTestVar6b(_:)[true].first.s2
// CHECK: destroying simpleTestVar6b(_:)[true].first.s3
// CHECK: destroying simpleTestVar6b(_:)[true].second.s1
// CHECK: destroying simpleTestVar6b(_:)[true].second.s3
// CHECK: marker(_:): simpleTestVar6b(_:)[true].1
// CHECK: destroying simpleTestVar6b(_:)[true].second.s2
// CHECK: marker(_:): simpleTestVar6b(_:)[true].3
func simpleTestVar6b(_ b: Bool) {
    var s = S3("\(#function)[\(b)].first")
    s = S3("\(#function)[\(b)].second")

    if b {
        consumeVal(s.s1) // end of s.s1's lifetime.
        consumeVal(s.s3) // end of s.s3's lifetime
        marker("\(#function)[\(b)].1")
        // s.s2 should end here.
    } else {
        consumeVal(s.s2) // end of s.s2's lifetime
        marker("\(#function)[\(b)].2")
        // end of s.s1's lifetime should end after marker.
        // end of s.s3's lifetime should end after marker.
    }

    marker("\(#function)[\(b)].3")
}


simpleTestVar()
simpleTestVar2()
simpleTestVar3()
simpleTestVar3a()
simpleTestVar3b()
simpleTestVar4(false, false)
simpleTestVar4(false, true)
simpleTestVar4(true, false)
simpleTestVar4(true, true)
simpleTestVar6(false)
simpleTestVar6(true)
simpleTestVar6a(false)
simpleTestVar6a(true)
simpleTestVar6b(false)
simpleTestVar6b(true)

