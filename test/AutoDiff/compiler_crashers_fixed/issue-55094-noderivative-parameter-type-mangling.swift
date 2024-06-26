// RUN: %target-build-swift -g %s

// https://github.com/apple/swift/issues/55094
// IRGenDebugInfo type reconstruction crash because `@noDerivative` parameters
// are not mangled

// FIXME(https://github.com/apple/swift/issues/55466): Disabled due to flakiness on Linux, likely related to TF-1197.


import _Differentiation
func id(_ x: Float, _ y: Float) -> Float { x }
let transformed: @differentiable(reverse) (Float, @noDerivative Float) -> Float = id

// Incorrect reconstructed type for $sS3fIedgyyd_D
// Original type:
// (sil_function_type type=@differentiable(reverse) @callee_guaranteed (Float, @noDerivative Float) -> Float
//   (input=struct_type decl=Swift.(file).Float)
//   (input=struct_type decl=Swift.(file).Float)
//   (result=struct_type decl=Swift.(file).Float)
//   (substitution_map generic_signature=<nullptr>)
//   (substitution_map generic_signature=<nullptr>))
// Reconstructed type:
// (sil_function_type type=@differentiable(reverse) @callee_guaranteed (Float, Float) -> Float
//   (input=struct_type decl=Swift.(file).Float)
//   (input=struct_type decl=Swift.(file).Float)
//   (result=struct_type decl=Swift.(file).Float)
//   (substitution_map generic_signature=<nullptr>)
//   (substitution_map generic_signature=<nullptr>))
// Stack dump:
// ...
// 1.	Swift version 5.3-dev (LLVM 803d1b184d, Swift 477af9f90d)
// 2.	While evaluating request IRGenSourceFileRequest(IR Generation for file "noderiv.swift")
// 0  swift                    0x00000001104c7ae8 llvm::sys::PrintStackTrace(llvm::raw_ostream&) + 40
// 1  swift                    0x00000001104c6a68 llvm::sys::RunSignalHandlers() + 248
// 2  swift                    0x00000001104c80dd SignalHandler(int) + 285
// 3  libsystem_platform.dylib 0x00007fff718335fd _sigtramp + 29
// 4  libsystem_platform.dylib 000000000000000000 _sigtramp + 18446603338611739168
// 5  libsystem_c.dylib        0x00007fff71709808 abort + 120
// 6  swift                    0x0000000110604152 (anonymous namespace)::IRGenDebugInfoImpl::getOrCreateType(swift::irgen::DebugTypeInfo) (.cold.20) + 146
// 7  swift                    0x000000010c24ab1e (anonymous namespace)::IRGenDebugInfoImpl::getOrCreateType(swift::irgen::DebugTypeInfo) + 3614
// 8  swift                    0x000000010c245437 swift::irgen::IRGenDebugInfo::emitGlobalVariableDeclaration(llvm::GlobalVariable*, llvm::StringRef, llvm::StringRef, swift::irgen::DebugTypeInfo, bool, bool, llvm::Optional<swift::SILLocation>) + 167
