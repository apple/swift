// RUN: %target-typecheck-verify-swift                       \
// RUN:     -enable-experimental-feature NonescapableTypes   \
// RUN:     -enable-experimental-feature NoncopyableGenerics \
// RUN:     -enable-experimental-feature BitwiseCopyable     \
// RUN:     -enable-builtin-module                           \
// RUN:     -debug-diagnostic-names

// This test file only exists in order to test without noncopyable_generics and can be deleted once that is always enabled.

@_nonescapable
struct S_Implicit_Nonescapable {}

struct S_Implicit_Noncopyable : ~Copyable {}

struct S_Explicit_With_Any_BitwiseCopyable : _BitwiseCopyable {
  var a: any _BitwiseCopyable // expected-error {{non_bitwise_copyable_type_member}}
}

struct S {}

indirect enum E_Explicit_Indirect : _BitwiseCopyable { // expected-error {{non_bitwise_copyable_type_indirect_enum}}
  case s(S)
}

enum E_Explicit_Indirect_Case : _BitwiseCopyable { // expected-error {{non_bitwise_copyable_type_indirect_enum_element}}
  indirect case s(S) // expected-note {{note_non_bitwise_copyable_type_indirect_enum_element}}
}

func take<T : _BitwiseCopyable>(_ t: T) {}

// public (effectively) => !conforms
@usableFromInline internal struct InternalUsableStruct {
  public var int: Int
}

func passInternalUsableStruct(_ s: InternalUsableStruct) { take(s) } // expected-error{{type_does_not_conform_decl_owner}}
                                                                     // expected-note@-8{{where_requirement_failure_one_subst}}

func passMemoryLayout<T>(_ m: MemoryLayout<T>) { take(m) } // expected-error{{global function 'take' requires that 'MemoryLayout<T>' conform to '_BitwiseCopyable'}}
                                                           // expected-note@-11{{where 'T' = 'MemoryLayout<T>'}}

func passCommandLine(_ m: CommandLine) { take(m) } // expected-error{{global function 'take' requires that 'CommandLine' conform to '_BitwiseCopyable'}}
                                                   // expected-note@-14{{where 'T' = 'CommandLine'}}

func passUnicode(_ m: Unicode) { take(m) } // expected-error{{global function 'take' requires that 'Unicode' conform to '_BitwiseCopyable'}}
                                           // expected-note@-17{{where 'T' = 'Unicode'}}

import Builtin

#if arch(arm)
func passBuiltinFloat16(_ f: Builtin.FPIEEE16) { take(f) }
@available(SwiftStdlib 5.3, *)
func passFloat16(_ f: Float16) { take(f) }
#endif

enum E_Raw_Int : Int {
  case one = 1
  case sixty_three = 63
}

func passE_Raw_Int(_ e: E_Raw_Int) { take(e) }

enum E_Raw_String : String {
  case one = "one"
  case sixty_three = "sixty three"
}

func passE_Raw_String(_ e: E_Raw_String) { take(e) }

struct S_Suppressed : ~_BitwiseCopyable {
  var i: Int
}

func passS_Suppressed(_ s: S_Suppressed) { take(s) } // expected-error{{global function 'take' requires that 'S_Suppressed' conform to '_BitwiseCopyable'}}
                                                     // expected-note@-46{{where 'T' = 'S_Suppressed'}}

struct S_Suppressed_Extension {}
extension S_Suppressed_Extension : ~_BitwiseCopyable {} // expected-error{{conformance to 'BitwiseCopyable' cannot be suppressed in an extension}}

struct S_Explicit_Suppressed : _BitwiseCopyable, ~_BitwiseCopyable {} // expected-error{{cannot both conform to and suppress conformance to 'BitwiseCopyable'}}

func passS_Explicit_Suppressed(_ s: S_Explicit_Suppressed) { take(s) }

struct S_Suppressed_Explicit : ~_BitwiseCopyable, _BitwiseCopyable {} // expected-error{{cannot both conform to and suppress conformance to 'BitwiseCopyable'}}

func passS_Explicit_Suppressed(_ s: S_Suppressed_Explicit) { take(s) }
