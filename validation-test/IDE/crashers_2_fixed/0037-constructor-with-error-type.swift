// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

public struct AnyError {

  public init(_ error: Swift.

extension AnyError {
  public static func error(from error: Error) -> AnyError {
    return AnyError(error)#^COMPLETE^#
  }
}

// COMPLETE: Begin completions, 1 items
// COMPLETE: Keyword[self]/CurrNominal:          .self[#AnyError#];
// COMPLETE: End completions

