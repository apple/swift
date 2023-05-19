// RUN: %target-typecheck-verify-swift -D FOO -D BAZ -swift-version 4

#if FOO == BAZ // expected-error{{expected '&&' or '||' expression}}
var x = 0
#endif

#if ^FOO // expected-error {{expected unary '!' expression}}
var y = 0
#endif

#if foo(BAR) // expected-error {{unexpected platform condition (expected 'os', 'arch', or 'swift')}}
var z = 0
#endif

#if FOO || !FOO
func f() {}
#endif ; f() // expected-error {{extra tokens following conditional compilation directive}}

#if FOO || !FOO
func g() {}
#else g()  // expected-error {{extra tokens following conditional compilation directive}}
#endif


#if FOO || !FOO
func h() {}
#else /* aaa */
#endif /* bbb */

#if foo.bar() // expected-error {{unexpected platform condition (expected 'os', 'arch', or 'swift')}}
      .baz()

#endif


// <https://twitter.com/practicalswift/status/829066902869786625>
#if // expected-error {{incomplete condition in conditional compilation directive}}
#if 0 == // expected-error {{incomplete condition in conditional compilation directive}}
#if 0= // expected-error {{incomplete condition in conditional compilation directive}} expected-error {{'=' must have consistent whitespace on both sides}}
class Foo {
  #if // expected-error {{incomplete condition in conditional compilation directive}}
  #if 0 == // expected-error {{incomplete condition in conditional compilation directive}}
  #if 0= // expected-error {{incomplete condition in conditional compilation directive}} expected-error {{'=' must have consistent whitespace on both sides}}
}

struct S {
  #if FOO
  #else
  #else  // expected-error {{further conditions after #else are unreachable}}
  #endif
  
  #if FOO
  #elseif BAR
  #elseif BAZ
  #else
  #endif
}

#if FOO
#else
#else  // expected-error {{further conditions after #else are unreachable}}
#endif

#if FOO
#elseif BAR
#elseif BAZ
#else
#endif

#if os(ios) // expected-warning {{unknown operating system for build configuration 'os'}} expected-note{{did you mean 'iOS'?}} {{8-11=iOS}}
#endif

#if os(uOS) // expected-warning {{unknown operating system for build configuration 'os'}} expected-note{{did you mean 'iOS'?}} {{8-11=iOS}}
#endif

#if os(xxxxxxd) // expected-warning {{unknown operating system for build configuration 'os'}}
// expected-note@-1{{did you mean 'Linux'?}} {{8-15=Linux}}
// expected-note@-2{{did you mean 'FreeBSD'?}} {{8-15=FreeBSD}}
// expected-note@-3{{did you mean 'Android'?}} {{8-15=Android}}
// expected-note@-4{{did you mean 'OSX'?}} {{8-15=OSX}}
// expected-note@-5{{did you mean 'OpenBSD'?}} {{8-15=OpenBSD}}
#endif

#if arch(leg) // expected-warning {{unknown architecture for build configuration 'arch'}} expected-note{{did you mean 'arm'?}} {{10-13=arm}}
#endif

#if _endian(mid) // expected-warning {{unknown endianness for build configuration '_endian'}} expected-note{{did you mean 'big'?}} {{13-16=big}}
#endif

LABEL: #if true // expected-error {{expected statement}}
func fn_i() {}
#endif
fn_i() // OK

try #if false // expected-error {{expected expression}}
#else
func fn_j() {}
#endif
fn_j() // OK

#if foo || bar || nonExistent() // expected-error {{expected argument to platform condition}}
#endif

#if FOO = false
// expected-error @-1 {{invalid conditional compilation expression}}
undefinedFunc() // ignored.
#else
undefinedFunc() // expected-error {{cannot find 'undefinedFunc' in scope}}
#endif

#if false
#elseif FOO ? true : false
// expected-error @-1 {{invalid conditional compilation expression}}
undefinedFunc() // ignored.
#else
undefinedFunc() // expected-error {{cannot find 'undefinedFunc' in scope}}
#endif

/// Invalid platform condition arguments don't invalidate the whole condition.
#if !arch(tecture) && !os(ystem) && !_endian(ness)
// expected-warning@-1 {{unknown architecture for build configuration 'arch'}}
// expected-note@-2 {{did you mean 'arm'?}} {{11-18=arm}}
// expected-warning@-3 {{unknown operating system for build configuration 'os'}}
// expected-note@-4 {{did you mean 'OSX'?}} {{27-32=OSX}}
// expected-note@-5 {{did you mean 'PS4'?}} {{27-32=PS4}}
// expected-note@-6 {{did you mean 'none'?}} {{27-32=none}}
// expected-warning@-7 {{unknown endianness for build configuration '_endian'}}
// expected-note@-8 {{did you mean 'big'?}} {{46-50=big}}
func fn_k() {}
#endif
fn_k()

#if os(cillator) || arch(ive)
// expected-warning@-1 {{unknown operating system for build configuration 'os'}}
// expected-note@-2 {{did you mean 'macOS'?}} {{8-16=macOS}}
// expected-note@-3 {{did you mean 'iOS'?}} {{8-16=iOS}}
// expected-warning@-4 {{unknown architecture for build configuration 'arch'}}
// expected-note@-5 {{did you mean 'arm'?}} {{26-29=arm}}
// expected-note@-6 {{did you mean 'i386'?}} {{26-29=i386}}
func undefinedFunc() // ignored.
#endif
undefinedFunc() // expected-error {{cannot find 'undefinedFunc' in scope}}

#if os(simulator) // expected-warning {{unknown operating system for build configuration 'os'}} expected-note {{did you mean 'targetEnvironment'}} {{5-7=targetEnvironment}}
#endif

#if arch(iOS) // expected-warning {{unknown architecture for build configuration 'arch'}} expected-note {{did you mean 'os'}} {{5-9=os}}
#endif

#if _endian(arm64) // expected-warning {{unknown endianness for build configuration '_endian'}} expected-note {{did you mean 'arch'}} {{5-12=arch}}
#endif

#if targetEnvironment(_ObjC) // expected-warning {{unknown target environment for build configuration 'targetEnvironment'}} expected-note {{did you mean 'macabi'}} {{23-28=macabi}}
#endif

#if os(iOS) || os(simulator) // expected-warning {{unknown operating system for build configuration 'os'}} expected-note {{did you mean 'targetEnvironment'}} {{16-18=targetEnvironment}}
#endif

#if arch(ios) // expected-warning {{unknown architecture for build configuration 'arch'}} expected-note {{did you mean 'os'}} {{5-9=os}} expected-note {{did you mean 'iOS'}} {{10-13=iOS}}
#endif

#if FOO
#else if BAR
// expected-error@-1 {{unexpected 'if' keyword following '#else' conditional compilation directive; did you mean '#elseif'?}} {{1-9=#elseif}}
#else
#endif

#if FOO
#else
if true {}
#endif // OK

// rdar://83017601 Make sure we don't crash
#if canImport()
// expected-error@-1 {{expected argument to platform condition}}
#endif
