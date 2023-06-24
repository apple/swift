// RUN: %target-swift-frontend  -O -Xllvm -sil-disable-pass=FunctionSignatureOpts -emit-sil -primary-file %s | %FileCheck %s

// We can't deserialize apply_inst with subst lists. When radar://14443304
// is fixed then we should convert this test to a SIL test.

struct YYY<T> {
  @inline(never)
  init(t : T) {m_t = t}
  @inline(never) mutating
  func AAA9(t t : T) -> Int { return AAA8(t: t) }
  @inline(never) mutating
  func AAA8(t t : T) -> Int { return AAA7(t: t) }
  @inline(never) mutating
  func AAA7(t t : T) -> Int { return AAA6(t: t) }
  @inline(never) mutating
  func AAA6(t t : T) -> Int { return AAA5(t: t) }
  @inline(never) mutating
  func AAA5(t t : T) -> Int { return AAA4(t: t) }
  @inline(never) mutating
  func AAA4(t t : T) -> Int { return AAA3(t: t) }
  @inline(never) mutating
  func AAA3(t t : T) -> Int { return AAA2(t: t) }
  @inline(never) mutating
  func AAA2(t t : T) -> Int { return AAA1(t: t) }
  @inline(never) mutating
  func AAA1(t t : T) -> Int { return AAA0(t: t) }
  @inline(never) mutating
  func AAA0(t t : T) -> Int { return foo(t: t) }
  @inline(never) mutating
  func foo(t t : T) -> Int { m_t = t; return 4 }
  var m_t : T
}

func exp1() {
  var II = YYY<Int>(t: 5)
  print(II.AAA9(t: 4), terminator: "")
}
//CHECK: sil shared [noinline] @$s16specialize_chain3YYYV4AAA9{{[_0-9a-zA-Z]*}}FSi_Tg5
//CHECK: sil shared [noinline] @$s16specialize_chain3YYYV4AAA8{{[_0-9a-zA-Z]*}}FSi_Tg5
//CHECK: sil shared [noinline] @$s16specialize_chain3YYYV4AAA7{{[_0-9a-zA-Z]*}}FSi_Tg5
//CHECK: sil shared [noinline] @$s16specialize_chain3YYYV4AAA6{{[_0-9a-zA-Z]*}}FSi_Tg5
//CHECK: sil shared [noinline] @$s16specialize_chain3YYYV4AAA5{{[_0-9a-zA-Z]*}}FSi_Tg5
//CHECK: sil shared [noinline] @$s16specialize_chain3YYYV4AAA4{{[_0-9a-zA-Z]*}}FSi_Tg5
//CHECK: sil shared [noinline] @$s16specialize_chain3YYYV4AAA3{{[_0-9a-zA-Z]*}}FSi_Tg5
//CHECK: sil shared [noinline] @$s16specialize_chain3YYYV4AAA2{{[_0-9a-zA-Z]*}}FSi_Tg5
//CHECK: sil shared [noinline] @$s16specialize_chain3YYYV4AAA1{{[_0-9a-zA-Z]*}}FSi_Tg5
//CHECK: exp1
//CHECK: function_ref @$s16specialize_chain3YYYV{{[_0-9a-zA-Z]*}}fCSi_Tgm5
//CHECK: function_ref @$s16specialize_chain3YYYV4AAA9{{[_0-9a-zA-Z]*}}FSi_Tg5
//CHECK: return
