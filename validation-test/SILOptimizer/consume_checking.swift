// RUN: %target-swift-frontend \
// RUN:   -emit-sil -verify    \
// RUN:   %s                   \
// RUN:   -sil-verify-all

////////////////////////////////////////////////////////////////////////////////
// https://github.com/apple/swift/issues/69252                {{
////////////////////////////////////////////////////////////////////////////////
public enum LineEnding: String {
  /// The default unix `\n` character
  case lineFeed = "\n"
  /// MacOS line ending `\r` character
  case carriageReturn = "\r"
  /// Windows line ending sequence `\r\n`
  case carriageReturnLineFeed = "\r\n"

  /// Initialize a line ending from a line string.
  /// - Parameter line: The line to use
  public init?(line: borrowing String) {
    guard let lastChar = line.last,
          let lineEnding = LineEnding(rawValue: String(lastChar)) else { return nil }
    self = lineEnding
  }


  static func makeLineEnding(_ line: borrowing String) -> LineEnding? {
     guard let lastChar = line.last,
          let lineEnding = LineEnding(rawValue: String(lastChar)) else { return nil }
      _ = lineEnding
     return nil
  }

  func makeLineEnding(_ line: borrowing String) -> LineEnding? {
    guard let lastChar = line.last,
          let lineEnding = LineEnding(rawValue: String(lastChar)) else { return nil }
    _ = lineEnding
    return nil
  }
}
////////////////////////////////////////////////////////////////////////////////
// https://github.com/apple/swift/issues/69252                }}
////////////////////////////////////////////////////////////////////////////////
