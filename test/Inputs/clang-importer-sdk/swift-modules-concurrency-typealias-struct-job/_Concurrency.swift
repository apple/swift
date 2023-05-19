
@_moveOnly
public struct ExecutorJob {}

public typealias Job = ExecutorJob

public protocol SerialExecutor {
  func enqueue(_ job: __owned ExecutorJob)
}