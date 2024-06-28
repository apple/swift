// RUN: %empty-directory(%t)
// RUN: %llvm-nm -g --defined-only -f just-symbols %stdlib_dir/x86_64/libswiftDistributed.dylib > %t/symbols
// RUN: %abi-symbol-checker %s %t/symbols
// RUN: diff -u %S/../../Inputs/macOS/x86_64/distributed/baseline %t/symbols

// REQUIRES: swift_stdlib_no_asserts
// REQUIRES: STDLIB_VARIANT=macosx-x86_64

// *** DO NOT DISABLE OR XFAIL THIS TEST. *** (See comment below.)

// Welcome, Build Wrangler!
//
// This file lists APIs that have recently changed in a way that potentially
// indicates an ABI- or source-breaking problem.
//
// A failure in this test indicates that there is a potential breaking change in
// the Standard Library. If you observe a failure outside of a PR test, please
// reach out to the Standard Library team directly to make sure this gets
// resolved quickly! If your own PR fails in this test, you probably have an
// ABI- or source-breaking change in your commits. Please go and fix it.
//
// Please DO NOT DISABLE THIS TEST. In addition to ignoring the current set of
// ABI breaks, XFAILing this test also silences any future ABI breaks that may
// land on this branch, which simply generates extra work for the next person
// that picks up the mess.
//
// Instead of disabling this test, you'll need to extend the list of expected
// changes at the bottom. (You'll also need to do this if your own PR triggers
// false positives, or if you have special permission to break things.) You can
// find a diff of what needs to be added in the output of the failed test run.
// The order of lines doesn't matter, and you can also include comments to refer
// to any bugs you filed.
//
// Thank you for your help ensuring the stdlib remains compatible with its past!
//                                            -- Your friendly stdlib engineers

// Distributed Symbols

// protocol descriptor for Distributed._DistributedActorStub
Added: _$s11Distributed01_A9ActorStubMp

// base conformance descriptor for Distributed._DistributedActorStub: Distributed.DistributedActor
Added: _$s11Distributed01_A9ActorStubPAA0aB0Tb

// protocol requirements base descriptor for Distributed._DistributedActorStub
Added: _$s11Distributed01_A9ActorStubTL

// Distributed._diagnoseDistributedStubMethodCalled(className: Swift.StaticString, funcName: Swift.StaticString, file: Swift.StaticString, line: Swift.UInt, column: Swift.UInt) -> Swift.Never
Added: _$s11Distributed09_diagnoseA16StubMethodCalled9className04funcG04file4line6columns5NeverOs12StaticStringV_A2KS2utF

// dispatch thunk of Distributed.DistributedActorSystem.remoteCall<A, B, C where A1: Distributed.DistributedActor, B1: Swift.Error, A.ActorID == A1.ID>(on: A1, target: Distributed.RemoteCallTarget, invocation: inout A.InvocationEncoder, throwing: B1.Type, returning: C1.Type) async throws -> C1
Added: _$s11Distributed0A11ActorSystemP10remoteCall2on6target10invocation8throwing9returningqd_1_qd___AA06RemoteE6TargetV17InvocationEncoderQzzqd_0_mqd_1_mtYaKAA0aB0Rd__s5ErrorRd_0_2IDQyd__0bP0Rtzr1_lFTj

// async function pointer to dispatch thunk of Distributed.DistributedActorSystem.remoteCall<A, B, C where A1: Distributed.DistributedActor, B1: Swift.Error, A.ActorID == A1.ID>(on: A1, target: Distributed.RemoteCallTarget, invocation: inout A.InvocationEncoder, throwing: B1.Type, returning: C1.Type) async throws -> C1
Added: _$s11Distributed0A11ActorSystemP10remoteCall2on6target10invocation8throwing9returningqd_1_qd___AA06RemoteE6TargetV17InvocationEncoderQzzqd_0_mqd_1_mtYaKAA0aB0Rd__s5ErrorRd_0_2IDQyd__0bP0Rtzr1_lFTjTu

// method descriptor for Distributed.DistributedActorSystem.remoteCall<A, B, C where A1: Distributed.DistributedActor, B1: Swift.Error, A.ActorID == A1.ID>(on: A1, target: Distributed.RemoteCallTarget, invocation: inout A.InvocationEncoder, throwing: B1.Type, returning: C1.Type) async throws -> C1
Added: _$s11Distributed0A11ActorSystemP10remoteCall2on6target10invocation8throwing9returningqd_1_qd___AA06RemoteE6TargetV17InvocationEncoderQzzqd_0_mqd_1_mtYaKAA0aB0Rd__s5ErrorRd_0_2IDQyd__0bP0Rtzr1_lFTq

// dispatch thunk of Distributed.DistributedActorSystem.remoteCallVoid<A, B where A1: Distributed.DistributedActor, B1: Swift.Error, A.ActorID == A1.ID>(on: A1, target: Distributed.RemoteCallTarget, invocation: inout A.InvocationEncoder, throwing: B1.Type) async throws -> ()
Added: _$s11Distributed0A11ActorSystemP14remoteCallVoid2on6target10invocation8throwingyqd___AA06RemoteE6TargetV17InvocationEncoderQzzqd_0_mtYaKAA0aB0Rd__s5ErrorRd_0_2IDQyd__0bP0Rtzr0_lFTj

// async function pointer to dispatch thunk of Distributed.DistributedActorSystem.remoteCallVoid<A, B where A1: Distributed.DistributedActor, B1: Swift.Error, A.ActorID == A1.ID>(on: A1, target: Distributed.RemoteCallTarget, invocation: inout A.InvocationEncoder, throwing: B1.Type) async throws -> ()
Added: _$s11Distributed0A11ActorSystemP14remoteCallVoid2on6target10invocation8throwingyqd___AA06RemoteE6TargetV17InvocationEncoderQzzqd_0_mtYaKAA0aB0Rd__s5ErrorRd_0_2IDQyd__0bP0Rtzr0_lFTjTu

// method descriptor for Distributed.DistributedActorSystem.remoteCallVoid<A, B where A1: Distributed.DistributedActor, B1: Swift.Error, A.ActorID == A1.ID>(on: A1, target: Distributed.RemoteCallTarget, invocation: inout A.InvocationEncoder, throwing: B1.Type) async throws -> ()
Added: _$s11Distributed0A11ActorSystemP14remoteCallVoid2on6target10invocation8throwingyqd___AA06RemoteE6TargetV17InvocationEncoderQzzqd_0_mtYaKAA0aB0Rd__s5ErrorRd_0_2IDQyd__0bP0Rtzr0_lFTq

// dispatch thunk of Distributed.DistributedTargetInvocationDecoder.decodeNextArgument<A>() throws -> A1
Added: _$s11Distributed0A23TargetInvocationDecoderP18decodeNextArgumentqd__yKlFTj

// method descriptor for Distributed.DistributedTargetInvocationDecoder.decodeNextArgument<A>() throws -> A1
Added: _$s11Distributed0A23TargetInvocationDecoderP18decodeNextArgumentqd__yKlFTq

// dispatch thunk of Distributed.DistributedTargetInvocationEncoder.recordArgument<A>(Distributed.RemoteCallArgument<A1>) throws -> ()
Added: _$s11Distributed0A23TargetInvocationEncoderP14recordArgumentyyAA010RemoteCallF0Vyqd__GKlFTj

// method descriptor for Distributed.DistributedTargetInvocationEncoder.recordArgument<A>(Distributed.RemoteCallArgument<A1>) throws -> ()
Added: _$s11Distributed0A23TargetInvocationEncoderP14recordArgumentyyAA010RemoteCallF0Vyqd__GKlFTq

// dispatch thunk of Distributed.DistributedTargetInvocationEncoder.recordReturnType<A>(A1.Type) throws -> ()
Added: _$s11Distributed0A23TargetInvocationEncoderP16recordReturnTypeyyqd__mKlFTj

// method descriptor for Distributed.DistributedTargetInvocationEncoder.recordReturnType<A>(A1.Type) throws -> ()
Added: _$s11Distributed0A23TargetInvocationEncoderP16recordReturnTypeyyqd__mKlFTq

// dispatch thunk of Distributed.DistributedTargetInvocationResultHandler.onReturn<A>(value: A1) async throws -> ()
Added: _$s11Distributed0A29TargetInvocationResultHandlerP8onReturn5valueyqd___tYaKlFTj

// async function pointer to dispatch thunk of Distributed.DistributedTargetInvocationResultHandler.onReturn<A>(value: A1) async throws -> ()
Added: _$s11Distributed0A29TargetInvocationResultHandlerP8onReturn5valueyqd___tYaKlFTjTu

// method descriptor for Distributed.DistributedTargetInvocationResultHandler.onReturn<A>(value: A1) async throws -> ()
Added: _$s11Distributed0A29TargetInvocationResultHandlerP8onReturn5valueyqd___tYaKlFTq

// (extension in Distributed):Distributed.DistributedActor.asLocalActor.getter : Swift.Actor
Added: _$s11Distributed0A5ActorPAAE07asLocalB0ScA_pvg

// property descriptor for (extension in Distributed):Distributed.DistributedActor.asLocalActor : Swift.Actor
Added: _$s11Distributed0A5ActorPAAE07asLocalB0ScA_pvpMV

// property descriptor for (extension in Distributed):Distributed.DistributedActor.__actorUnownedExecutor : Swift.UnownedSerialExecutor
Added: _$s11Distributed0A5ActorPAAE22__actorUnownedExecutorScevpMV

// Distributed._distributedStubFatalError(function: Swift.String) -> Swift.Never
Added: _$s11Distributed26_distributedStubFatalError8functions5NeverOSS_tF

// Bin compat for typed throws overload of whenLocal
// (extension in Distributed):Distributed.DistributedActor.whenLocal<A, B where A1: Swift.Sendable, B1: Swift.Error>(@Sendable (isolated A) async throws(B1) -> A1) async throws(B1) -> A1?
Added: _$s11Distributed0A5ActorPAAE9whenLocalyqd__Sgqd__xYiYaYbqd_0_YKXEYaqd_0_YKs8SendableRd__s5ErrorRd_0_r0_lF
// async function pointer to (extension in Distributed):Distributed.DistributedActor.whenLocal<A, B where A1: Swift.Sendable, B1: Swift.Error>(@Sendable (isolated A) async throws(B1) -> A1) async throws(B1) -> A1?
Added: _$s11Distributed0A5ActorPAAE9whenLocalyqd__Sgqd__xYiYaYbqd_0_YKXEYaqd_0_YKs8SendableRd__s5ErrorRd_0_r0_lFTu