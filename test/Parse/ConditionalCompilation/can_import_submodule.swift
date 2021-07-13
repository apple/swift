// RUN: %target-typecheck-verify-swift -I %S/Inputs/can-import-submodule/ 

#if canImport(A)
import A
#else
#error("should can import A")
#endif

#if canImport(A.B)
import A.B
#else
#error("should can import A.B")
#endif

#if canImport(A.B.C)
import A.B.C
#else
#error("should can import A.B.C")
#endif


#if canImport(Z)
#error("should not can import Z")
#endif

#if canImport(A.Z)
#error("should not can import A.Z")
#endif

#if canImport(Z.B)
#error("should not can import Z.B")
#endif

#if canImport(Z.B.C)
#error("should not can import Z.B.C")
#endif

#if canImport(A.B.Z)
#error("should not can import A.B.Z")
#endif

#if canImport(A.Z.C)
#error("should not can import A.Z.C")
#endif

#if canImport(A.B.C.Z)
#error("should not can import A.B.C.Z")
#endif


#if canImport(A(_:).B) // expected-error {{unexpected platform condition argument: expected module name}}
#endif

#if canImport(A.B(c: "arg")) // expected-error {{unexpected platform condition argument: expected module name}}
#endif

#if canImport(A(b: 1, c: 2).B.C) // expected-error {{unexpected platform condition argument: expected module name}}
#endif

#if canImport(A.B("arg")(3).C) // expected-error {{unexpected platform condition argument: expected module name}}
#endif

#if canImport(A.B.C()) // expected-error {{unexpected platform condition argument: expected module name}}
#endif
