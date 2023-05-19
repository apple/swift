// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

struct FileDescriptor: ~#^COMPLETE^# {}
// FIXME: This should be emitting a `Copyable` declaration instead of a keyword, once there is a declaration for `Copyable`
// COMPLETE: Keyword/None:                       Copyable; name=Copyable
