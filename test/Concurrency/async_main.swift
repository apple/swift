// RUN: %target-swift-frontend -emit-sil -disable-availability-checking -parse-as-library %s | %FileCheck %s --check-prefix=CHECK-SIL
// RUN: %target-build-swift  -Xfrontend -disable-availability-checking -Xfrontend -parse-as-library %s -o %t_binary
// RUN: %target-codesign %t_binary
// RUN: %target-run %t_binary | %FileCheck %s --check-prefix=CHECK-EXEC

// REQUIRES: concurrency
// REQUIRES: executable_test
// REQUIRES: OS=macosx || OS=ios

// rdar://76038845
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

@MainActor
var foo: Int = 42

func asyncFunc() async {
  print("Hello World!")
}

@main struct MyProgram {
  static func main() async throws {
    print("\(foo)")
    foo += 1
    await asyncFunc()
    print("\(foo)")
  }
}

// CHECK-EXEC: 42
// CHECK-EXEC-NEXT: Hello World!
// CHECK-EXEC-NEXT: 43

// static MyProgram.main()
// CHECK-SIL-LABEL: sil hidden @$s10async_main9MyProgramV0B0yyYaKFZ : $@convention(method) @async (@thin MyProgram.Type) -> @error any Error


// static MyProgram.$main()
// CHECK-SIL-LABEL: sil hidden @$s10async_main9MyProgramV5$mainyyYaKFZ : $@convention(method) @async (@thin MyProgram.Type) -> @error any Error


// async_Main
// CHECK-SIL-LABEL: sil private @async_Main : $@convention(thin) @async () -> () {
// call main
// CHECK-SIL:  %0 = metatype $@thin MyProgram.Type             // user: %2
// CHECK-SIL-NEXT:  // function_ref static MyProgram.$main()
// CHECK-SIL-NEXT:  %1 = function_ref @$s10async_main9MyProgramV5$mainyyYaKFZ : $@convention(method) @async (@thin MyProgram.Type) -> @error any Error // user: %2
// CHECK-SIL-NEXT:  try_apply %1(%0) : $@convention(method) @async (@thin MyProgram.Type) -> @error any Error, normal bb1, error bb2 // id: %2

// unwrap error and exit or explode
// CHECK-SIL: bb1(%3 : $()):
// CHECK-SIL-NEXT:  %4 = integer_literal $Builtin.Int32, 0
// CHECK-SIL-NEXT:  %5 = struct $Int32 (%4 : $Builtin.Int32)
// CHECK-SIL-NEXT:  // function_ref exit
// CHECK-SIL-NEXT:  %6 = function_ref @exit : $@convention(c) (Int32) -> Never
// CHECK-SIL-NEXT:  %7 = apply %6(%5) : $@convention(c) (Int32) -> Never
// CHECK-SIL-NEXT:  unreachable

// CHECK-SIL: bb2(%9 : $any Error):
// CHECK-SIL-NEXT:  %10 = builtin "errorInMain"(%9 : $any Error) : $()
// CHECK-SIL-NEXT:  unreachable

// main
// CHECK-SIL-LABEL: sil @main : $@convention(c) (Int32, UnsafeMutablePointer<Optional<UnsafeMutablePointer<Int8>>>) -> Int32 {

// CHECK-SIL:       // function_ref async_Main
// CHECK-SIL-NEXT:  [[ASYNC_MAIN_FN:%.*]] = function_ref @async_Main : $@convention(thin) @async () -> ()
// CHECK-SIL-NEXT:  [[T0:%.*]] = integer_literal $Builtin.Int64, 2048
// CHECK-SIL-NEXT:  [[FLAGS:%.*]] = struct $Int ([[T0]] : $Builtin.Int64)
// CHECK-SIL-NEXT:  [[OPT_SERIAL_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-SIL-NEXT:  [[GROUP:%.*]] = enum $Optional<Builtin.RawPointer>, #Optional.none
// CHECK-SIL-NEXT:  [[TASK_EXECUTOR_UNOWNED:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-SIL-NEXT:  [[TASK_EXECUTOR_OWNED:%.*]] = enum $Optional<any TaskExecutor>, #Optional.none
// CHECK-SIL-NEXT:  // function_ref thunk for @escaping @convention(thin) @async () -> ()
// CHECK-SIL-NEXT:  [[THUNK_FN:%.*]] = function_ref @$sIetH_yts5Error_pIegHrzo_TR : $@convention(thin) @async (@convention(thin) @async () -> ()) -> (@out (), @error any Error)
// CHECK-SIL-NEXT:  [[THUNK:%.*]] = partial_apply [callee_guaranteed] [[THUNK_FN]]([[ASYNC_MAIN_FN]]) : $@convention(thin) @async (@convention(thin) @async () -> ()) -> (@out (), @error any Error)
// CHECK-SIL-NEXT:  [[CONVERTED_THUNK:%.*]] = convert_function [[THUNK]] : $@async @callee_guaranteed () -> (@out (), @error any Error) to $@async @callee_guaranteed @substituted <τ_0_0> () -> (@out τ_0_0, @error any Error) for <()>
// CHECK-SIL-NEXT:  [[TASK_RESULT:%.*]] = builtin "createAsyncTask"<()>([[FLAGS]] : $Int, [[OPT_SERIAL_EXECUTOR]] : $Optional<Builtin.Executor>, [[GROUP]] : $Optional<Builtin.RawPointer>, [[TASK_EXECUTOR_UNOWNED]] : $Optional<Builtin.Executor>, [[TASK_EXECUTOR_OWNED]] : $Optional<any TaskExecutor>, [[CONVERTED_THUNK]] : $@async @callee_guaranteed @substituted <τ_0_0> () -> (@out τ_0_0, @error any Error) for <()>) : $(Builtin.NativeObject, Builtin.RawPointer)
// CHECK-SIL-NEXT:  [[TASK:%.*]] = tuple_extract [[TASK_RESULT]] : $(Builtin.NativeObject, Builtin.RawPointer), 0
// CHECK-SIL-NEXT:  // function_ref swift_job_run
// CHECK-SIL-NEXT:  [[RUN_FN:%.*]] = function_ref @swift_job_run : $@convention(thin) (UnownedJob, UnownedSerialExecutor) -> ()
// CHECK-SIL-NEXT:  [[RAW_JOB:%.*]] = builtin "convertTaskToJob"([[TASK]] : $Builtin.NativeObject) : $Builtin.Job
// CHECK-SIL-NEXT:  [[JOB:%.*]] = struct $UnownedJob ([[RAW_JOB]] : $Builtin.Job)
// CHECK-SIL-NEXT:  [[RAW_MAIN_EXECUTOR:%.*]] = builtin "buildMainActorExecutorRef"() : $Builtin.Executor
// CHECK-SIL-NEXT:  [[MAIN_EXECUTOR:%.*]] = struct $UnownedSerialExecutor ([[RAW_MAIN_EXECUTOR]] : $Builtin.Executor)
// CHECK-SIL-NEXT:  apply [[RUN_FN]]([[JOB]], [[MAIN_EXECUTOR]]) : $@convention(thin) (UnownedJob, UnownedSerialExecutor) -> ()
// CHECK-SIL-NEXT:  // function_ref swift_task_asyncMainDrainQueue
// CHECK-SIL-NEXT:  [[DRAIN_FN:%.*]] = function_ref @swift_task_asyncMainDrainQueue : $@convention(thin) () -> Never
// CHECK-SIL-NEXT:  apply [[DRAIN_FN]]() : $@convention(thin) () -> Never
// CHECK-SIL-NEXT:  unreachable
