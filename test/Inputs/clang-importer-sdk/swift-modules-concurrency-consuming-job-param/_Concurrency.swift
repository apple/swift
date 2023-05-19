
// This simulates a pre-Swift5.9 concurrency library with an consuming declaration rather than __owned.
// This allows us to confirm the compiler and sdk can be mismatched but we'll build correctly.

@_moveOnly
public struct ExecutorJob {}

public protocol SerialExecutor {
  // pretend old SDK with `__owned` param rather than ``
  func enqueue(_ job: consuming ExecutorJob)
}