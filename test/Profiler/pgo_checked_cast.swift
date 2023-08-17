
// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -profile-generate -Xfrontend -disable-incremental-llvm-codegen -module-name pgo_checked_cast -o %t/main

// This unusual use of 'sh' allows the path of the profraw file to be
// substituted by %target-run.
// RUN: %target-codesign %t/main
// RUN: %target-run sh -c 'env LLVM_PROFILE_FILE=$1 $2' -- %t/default.profraw %t/main

// RUN: %llvm-profdata merge %t/default.profraw -o %t/default.profdata
// RUN: %target-swift-frontend %s -Xllvm -sil-full-demangle -profile-use=%t/default.profdata -emit-sorted-sil -emit-sil -module-name pgo_checked_cast -o - | %FileCheck %s --check-prefix=SIL
// need to lower checked_cast_addr_br(addr) into IR for this
// %target-swift-frontend %s -Xllvm -sil-full-demangle -profile-use=%t/default.profdata -emit-ir -module-name pgo_checked_cast -o - | %FileCheck %s --check-prefix=IR
// RUN: %target-swift-frontend %s -Xllvm -sil-full-demangle -profile-use=%t/default.profdata -O -emit-sorted-sil -emit-sil -module-name pgo_checked_cast -o - | %FileCheck %s --check-prefix=SIL-OPT
// need to lower checked_cast_addr_br(addr) into IR for this
// %target-swift-frontend %s -Xllvm -sil-full-demangle -profile-use=%t/default.profdata -O -emit-ir -module-name pgo_checked_cast -o - | %FileCheck %s --check-prefix=IR-OPT

// REQUIRES: profile_runtime
// REQUIRES: executable_test
// REQUIRES: OS=macosx

// SIL-LABEL: // pgo_checked_cast.check1<A>(Any, A) -> A
// SIL-LABEL: sil @$s16pgo_checked_cast6check1yxyp_xtlF : $@convention(thin) <T> (@in_guaranteed Any, @in_guaranteed T) -> @out T !function_entry_count(5001) {
// IR-LABEL: define swiftcc i32 @$s6pgo_checked_cast6guess1s5Int32VAD1x_tF
// IR-OPT-LABEL: define swiftcc i32 @$s6pgo_checked_cast6guess1s5Int32VAD1x_tF
public func check1<T>(_ a : Any, _ t : T) -> T {
  // SIL: checked_cast_addr_br take_always Any in {{.*}} : $*Any to T in {{.*}} : $*T, {{.*}}, {{.*}} !true_count(5000) !false_count(1)
  // SIL-OPT: checked_cast_addr_br take_always Any in {{.*}} : $*Any to T in {{.*}} : $*T, {{.*}}, {{.*}} !true_count(5000) !false_count(1)
  if let x = a as? T {
    return x
  } else {
    return t
  }
}

public class B {}
public class C : B {}
public class D : C {}

// SIL-LABEL: // pgo_checked_cast.check2(pgo_checked_cast.B) -> Swift.Int32
// SIL-LABEL: sil @$s16pgo_checked_cast6check2ys5Int32VAA1BCF : $@convention(thin) (@guaranteed B) -> Int32 !function_entry_count(5003) {
// IR-LABEL: define swiftcc i32 @$s6pgo_checked_cast6guess1s5Int32VAD1x_tF
// IR-OPT-LABEL: define swiftcc i32 @$s6pgo_checked_cast6guess1s5Int32VAD1x_tF
public func check2(_ a : B) -> Int32 {
  // SIL: checked_cast_br B in %0 : $B to D, {{.*}}, {{.*}} !true_count(5000)
  // SIL: checked_cast_br B in %0 : $B to C, {{.*}}, {{.*}} !true_count(2)
  // SIL-OPT: checked_cast_br B in %0 : $B to D, {{.*}}, {{.*}} !true_count(5000)
  // SIL-OPT: checked_cast_br B in %0 : $B to C, {{.*}}, {{.*}} !true_count(2)
  switch a {
    case is D:
      return 42
    case is C:
      return 23
    default:
      return 13
  }
}

func main() {
  let answer : Int32 = 42
  var sum : Int32 = 0

  sum += check1("The answer to the life, the universe, and everything", answer)
  
  sum += check2(B())
  sum += check2(C())
  sum += check2(C())

  for i : Int32 in 1...5000 {
    sum += check1(i, answer)
    sum += check2(D())
  }
}

main()

// IR: !{!"branch_weights", i32 5001, i32 2}
// IR-OPT: !{!"branch_weights", i32 5001, i32 2}
