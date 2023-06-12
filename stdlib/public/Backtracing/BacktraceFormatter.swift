//===--- BacktraceFormatter.swift -----------------------------*- swift -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  Provides functionality to format backtraces, with various additional
//  options.
//
//===----------------------------------------------------------------------===//

import Swift

@_implementationOnly import OS.Libc

/// A backtrace formatting theme.
@_spi(Formatting)
public protocol BacktraceFormattingTheme {
  func frameIndex(_ s: String) -> String
  func programCounter(_ s: String) -> String
  func frameAttribute(_ s: String) -> String
  func symbol(_ s: String) -> String
  func offset(_ s: String) -> String
  func sourceLocation(_ s: String) -> String
  func lineNumber(_ s: String) -> String
  func code(_ s: String) -> String
  func crashedLineNumber(_ s: String) -> String
  func crashedLine(_ s: String) -> String
  func crashLocation() -> String
  func imageName(_ s: String) -> String
  func imageAddressRange(_ s: String) -> String
  func imageBuildID(_ s: String) -> String
  func imagePath(_ s: String) -> String
}

extension BacktraceFormattingTheme {
  public func frameIndex(_ s: String) -> String { return s }
  public func programCounter(_ s: String) -> String { return s }
  public func frameAttribute(_ s: String) -> String { return "[\(s)]" }
  public func symbol(_ s: String) -> String { return s }
  public func offset(_ s: String) -> String { return s }
  public func sourceLocation(_ s: String) -> String { return s }
  public func lineNumber(_ s: String) -> String { return " \(s)|" }
  public func code(_ s: String) -> String { return s }
  public func crashedLineNumber(_ s: String) -> String { return "*\(s)|" }
  public func crashedLine(_ s: String) -> String { return s }
  public func crashLocation() -> String { return "^" }
  public func imageName(_ s: String) -> String { return s }
  public func imageAddressRange(_ s: String) -> String { return s }
  public func imageBuildID(_ s: String) -> String { return s }
  public func imagePath(_ s: String) -> String { return s}
}

/// Options for backtrace formatting.
///
/// This is used by chaining modifiers, e.g. .theme(.color).showSourceCode().
@_spi(Formatting)
public struct BacktraceFormattingOptions {
  var _theme: BacktraceFormattingTheme = BacktraceFormatter.Themes.plain
  var _showSourceCode: Bool = false
  var _sourceContextLines: Int = 2
  var _showAddresses: Bool = true
  var _showImages: ImagesToShow = .mentioned
  var _showImageNames: Bool = true
  var _showFrameAttributes: Bool = true
  var _skipRuntimeFailures: Bool = false
  var _skipThunkFunctions: Bool = true
  var _skipSystemFrames: Bool = true
  var _sanitizePaths: Bool = true
  var _demangle: Bool = true
  var _width: Int = 80

  public var selectedTheme: BacktraceFormattingTheme { return _theme }
  public var shouldShowSourceCode: Bool { return _showSourceCode }
  public var sourceContextLines: Int { return _sourceContextLines }
  public var shouldShowAddresses: Bool { return _showAddresses }
  public var imagesToShow: ImagesToShow { return _showImages }
  public var shouldShowImageNames: Bool { return _showImageNames }
  public var shouldShowFrameAttributes: Bool { return _showFrameAttributes }
  public var shouldSkipRuntimeFailures: Bool { return _skipRuntimeFailures }
  public var shouldSkipThunkFunctions: Bool { return _skipThunkFunctions }
  public var shouldSkipSystemFrames: Bool { return _skipSystemFrames }
  public var shouldSanitizePaths: Bool { return _sanitizePaths }
  public var shouldDemangle: Bool { return _demangle }
  public var formattingWidth: Int { return _width }

  public init() {}

  /// Theme to use for formatting.
  ///
  /// @param theme  A `BacktraceFormattingTheme` structure.
  ///
  /// @returns A new `BacktraceFormattingOptions` structure.
  public static func theme(_ theme: BacktraceFormattingTheme) -> BacktraceFormattingOptions {
    return BacktraceFormattingOptions().theme(theme)
  }
  public func theme(_ theme: BacktraceFormattingTheme) -> BacktraceFormattingOptions {
    var newOptions = self
    newOptions._theme = theme
    return newOptions
  }

  /// Enable or disable the display of source code in the backtrace.
  ///
  /// @param enabled       Whether or not to enable source code.
  ///
  /// @param contextLines  The number of lines of context either side of the
  ///                      line associated with the backtrace frame.
  ///
  /// @returns A new `BacktraceFormattingOptions` structure.
  public static func showSourceCode(_ enabled: Bool = true, contextLines: Int = 2) -> BacktraceFormattingOptions {
    return BacktraceFormattingOptions().showSourceCode(enabled,
                                                       contextLines: contextLines)
  }
  public func showSourceCode(_ enabled: Bool = true, contextLines: Int = 2) -> BacktraceFormattingOptions {
    var newOptions = self
    newOptions._showSourceCode = enabled
    newOptions._sourceContextLines = contextLines
    return newOptions
  }

  /// Enable or disable the display of raw addresses.
  ///
  /// @param enabled  If false, we will only display a raw address in the
  ///                 backtrace if we haven't been able to symbolicate.
  ///
  /// @returns A new `BacktraceFormattingOptions` structure.
  public static func showAddresses(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    return BacktraceFormattingOptions().showAddresses(enabled)
  }
  public func showAddresses(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    var newOptions = self
    newOptions._showAddresses = enabled
    return newOptions
  }

  /// Enable or disable the display of the image list.
  ///
  /// @param enabled  Says whether or not to output the image list.
  ///
  /// @returns A new `BacktraceFormattingOptions` structure.
  public enum ImagesToShow {
    case none
    case mentioned
    case all
  }
  public static func showImages(_ toShow: ImagesToShow = .all) -> BacktraceFormattingOptions {
    return BacktraceFormattingOptions().showImages(toShow)
  }
  public func showImages(_ toShow: ImagesToShow = .all) -> BacktraceFormattingOptions {
    var newOptions = self
    newOptions._showImages = toShow
    return newOptions
  }

  /// Enable or disable the display of image names in the frame list.
  ///
  /// @param enabled  If true, we will display the name of the image for
  ///                 each frame.
  ///
  /// @returns A new `BacktraceFormattingOptions` structure.
  public static func showImageNames(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    return BacktraceFormattingOptions().showImageNames(enabled)
  }
  public func showImageNames(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    var newOptions = self
    newOptions._showImageNames = enabled
    return newOptions
  }

  /// Enable or disable the display of frame attributes in the frame list.
  ///
  /// @param enabled  If true, we will display the frame attributes.
  ///
  /// @returns A new `BacktraceFormattingOptions` structure.
  public static func showFrameAttributes(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    return BacktraceFormattingOptions().showFrameAttributes(enabled)
  }
  public func showFrameAttributes(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    var newOptions = self
    newOptions._showFrameAttributes = enabled
    return newOptions
  }

  /// Set whether or not to show Swift runtime failure frames.
  ///
  /// @param enabled  If true, we will skip Swift runtime failure frames.
  ///
  /// @returns A new `BacktraceFormattingOptions` structure.
  public static func skipRuntimeFailures(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    return BacktraceFormattingOptions().skipRuntimeFailures(enabled)
  }
  public func skipRuntimeFailures(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    var newOptions = self
    newOptions._skipRuntimeFailures = enabled
    return newOptions
  }

  /// Set whether or not to show Swift thunk function frames.
  ///
  /// @param enabled  If true, we will skip Swift thunk function frames.
  ///
  /// @returns A new `BacktraceFormattingOptions` structure.
  public static func skipThunkFunctions(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    return BacktraceFormattingOptions().skipThunkFunctions(enabled)
  }
  public func skipThunkFunctions(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    var newOptions = self
    newOptions._skipThunkFunctions = enabled
    return newOptions
  }

  /// Set whether or not to show system frames.
  ///
  /// For instance, on macOS, this will cause us to skip the "start" frame
  /// at the very top of the stack.
  ///
  /// @param enabled  If true, we will skip system frames.
  ///
  /// @returns A new `BacktraceFormattingOptions` structure.
  public static func skipSystemFrames(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    return BacktraceFormattingOptions().skipSystemFrames(enabled)
  }
  public func skipSystemFrames(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    var newOptions = self
    newOptions._skipSystemFrames = enabled
    return newOptions
  }

  /// Enable or disable path sanitization.
  ///
  /// This is intended to avoid leaking PII into crash logs.
  ///
  /// @param enabled  If true, paths will be sanitized.
  ///
  /// @returns A new `BacktraceFormattingOptions` structure.
  public static func sanitizePaths(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    return BacktraceFormattingOptions().sanitizePaths(enabled)
  }
  public func sanitizePaths(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    var newOptions = self
    newOptions._sanitizePaths = enabled
    return newOptions
  }

  /// Set whether we show mangled or demangled names.
  ///
  /// @param enabled  If true, we show demangled names if we have them.
  ///
  /// @returns A new `BacktraceFormattingOptions` structure.
  public static func demangle(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    return BacktraceFormattingOptions().demangle(enabled)
  }
  public func demangle(_ enabled: Bool = true) -> BacktraceFormattingOptions {
    var newOptions = self
    newOptions._demangle = enabled
    return newOptions
  }

  /// Set the output width.
  ///
  /// @param width  The output width in characters.  This is only used to
  ///               highlight information, and defaults to 80.
  ///
  /// returns A new `BacktraceFormattingOptions` structure.
  public static func width(_ width: Int) -> BacktraceFormattingOptions {
    return BacktraceFormattingOptions().width(width)
  }
  public func width(_ width: Int) -> BacktraceFormattingOptions {
    var newOptions = self
    newOptions._width = width
    return newOptions
  }
}

/// Return the width of a given Unicode.Scalar.
///
/// It would be nice to have the Unicode width data, which would let us do
/// a better job of this.
private func measure(_ ch: Unicode.Scalar) -> Int {
  if ch.isASCII {
    return 1
  }

  if ch.properties.isEmoji {
    return 2
  }

  if ch.properties.isIdeographic
       && !(ch.value >= 0xff61 && ch.value <= 0xffdc)
       && !(ch.value >= 0xffe8 && ch.value <= 0xffee) {
    return 2
  }

  if ch.properties.canonicalCombiningClass.rawValue != 0 {
    return 0
  }

  switch ch.properties.generalCategory {
    case .control, .nonspacingMark:
      return 0
    default:
      return 1
  }
}

/// Compute the width of the given string, ignoring CSI formatting codes.
private enum MeasureState {
  // Normal state
  case normal

  // Start of an escape
  case escape

  // In a CSI escape
  case csi
}

private func measure<S: StringProtocol>(_ s: S) -> Int {
  var totalWidth = 0
  var state: MeasureState = .normal

  for ch in s.unicodeScalars {
    switch state {
      case .normal:
        if ch.value == 27 {
          // This is an escape sequence
          state = .escape
        } else {
          totalWidth += measure(ch)
        }
      case .escape:
        if ch.value == 0x5b {
          state = .csi
        } else {
          state = .normal
        }
      case .csi:
        if ch.value >= 0x40 && ch.value <= 0x7e {
          state = .normal
        }
    }
  }
  return totalWidth
}

/// Pad the given string to the given width using spaces.
private func pad(_ s: String, to width: Int,
                 aligned alignment: BacktraceFormatter.Alignment = .left)
  -> String {

  let currentWidth = measure(s)
  let padding = max(width - currentWidth, 0)

  switch alignment {
    case .left:
      let spaces = String(repeating: " ", count: padding)

      return s + spaces
    case .right:
      let spaces = String(repeating: " ", count: padding)

      return spaces + s
    case .center:
      let left = padding / 2
      let right = padding - left
      let leftSpaces = String(repeating: " ", count: left)
      let rightSpaces = String(repeating: " ", count: right)

      return "\(leftSpaces)\(s)\(rightSpaces)"
  }
}

/// Untabify the given string, assuming tabs of the specified size.
///
/// @param s         The string to untabify.
/// @param tabWidth  The tab width to assume (default 8).
///
/// @returns A string with all the tabs replaced with appropriate numbers
///          of spaces.
private func untabify(_ s: String, tabWidth: Int = 8) -> String {
  var result: String = ""
  var first = true
  for chunk in s.split(separator: "\t", omittingEmptySubsequences: false) {
    if first {
      first = false
    } else {
      let toTabStop = tabWidth - measure(result) % tabWidth
      result += String(repeating: " ", count: toTabStop)
    }
    result += chunk
  }
  return result
}

/// Sanitize a path to remove usernames, volume names and so on.
///
/// The point of this function is to try to remove anything that might
/// contain PII before it ends up in a log file somewhere.
///
/// @param path  The path to sanitize.
///
/// @returns A string containing the sanitized path.
private func sanitizePath(_ path: String) -> String {
  #if os(macOS)
  return CRCopySanitizedPath(path,
                             kCRSanitizePathGlobAllTypes
                               | kCRSanitizePathKeepFile)
  #else
  // For now, on non-macOS systems, do nothing
  return path
  #endif
}

/// Trim whitespace from the right hand end of a string.
///
/// @param s  The string to trim.
///
/// @returns A string with the whitespace trimmed.
private func rtrim<S: StringProtocol>(_ s: S) -> S.SubSequence {
  if let lastNonWhitespace = s.lastIndex(where: { !$0.isWhitespace }) {
    return s.prefix(through: lastNonWhitespace)
  }
  return s.dropLast(0)
}

/// Responsible for formatting backtraces.
@_spi(Formatting)
public struct BacktraceFormatter {

  /// The formatting options to apply when formatting data.
  public var options: BacktraceFormattingOptions

  public struct Themes {
    /// A plain formatting theme.
    public struct PlainTheme: BacktraceFormattingTheme {
    }

    public static let plain = PlainTheme()
  }

  public init(_ options: BacktraceFormattingOptions) {
    self.options = options
  }

  public enum TableRow {
    case columns([String])
    case raw(String)
  }

  public enum Alignment {
    case left
    case right
    case center
  }

  /// Output a table with each column nicely aligned.
  ///
  /// @param rows  An array of table rows, each of which holds an array
  ///              of table columns.
  ///
  /// @result A `String` containing the formatted table.
  public static func formatTable(_ rows: [TableRow],
                                  alignments: [Alignment] = []) -> String {
    // Work out how many columns we have
    let colCount = rows.map{
      if case let .columns(columns) = $0 {
        return columns.count
      } else {
        return 0
      }
    }.reduce(0, max)

    // Now compute their widths
    var widths = Array(repeating: 0, count: colCount)
    for row in rows {
      if case let .columns(columns) = row {
        for (n, width) in columns.lazy.map(measure).enumerated() {
          widths[n] = max(widths[n], width)
        }
      }
    }

    // Generate lines for the table
    var lines: [Substring] = []
    for row in rows {
      switch row {
        case let .columns(columns):
          let line = columns.enumerated().map{ n, column in
            let alignment = n < alignments.count ? alignments[n] : .left
            if n == colCount - 1 && alignment == .left {
              return column
            } else {
              return pad(column, to: widths[n], aligned: alignment)
            }
          }.joined(separator: " ")

          lines.append(rtrim(line))
        case let .raw(line):
          lines.append(rtrim(line))
      }
    }

    // Trim any empty lines from the end
    guard let lastNonEmpty = lines.lastIndex(where: { !$0.isEmpty }) else {
      return ""
    }

    return lines.prefix(through: lastNonEmpty).joined(separator: "\n")
  }

  /// Format an individual frame into a list of columns.
  ///
  /// @param frame         The frame to format.
  /// @param addressWidth  The width, in characters, of an address.
  /// @param index         The frame index, if required.
  ///
  /// @result An array of strings, one per column.
  public func formatColumns(frame: Backtrace.Frame,
                            addressWidth: Int,
                            index: Int? = nil) -> [String] {
    let pc: String
    var attrs: [String] = []

    switch frame {
      case let .programCounter(address):
        pc = "\(hex(address, width: addressWidth))"
      case let .returnAddress(address):
        pc = "\(hex(address, width: addressWidth))"
        attrs.append("ra")
      case let .asyncResumePoint(address):
        pc = "\(hex(address, width: addressWidth))"
        attrs.append("async")
      case .omittedFrames(_), .truncated:
        pc = "..."
    }

    var columns: [String] = []
    if let index = index {
      columns.append(options._theme.frameIndex("\(index)"))
    }
    columns.append(options._theme.programCounter(pc))
    if options._showFrameAttributes {
      columns.append(attrs.map(
                       options._theme.frameAttribute
                     ).joined(separator: " "))
    }

    return columns
  }

  /// Format a frame into a list of rows.
  ///
  /// @param frame         The frame to format.
  /// @param addressWidth  The width, in characters, of an address.
  /// @param index         The frame index, if required.
  ///
  /// @result An array of table rows.
  public func formatRows(frame: Backtrace.Frame,
                         addressWidth: Int,
                         index: Int? = nil) -> [TableRow] {
    return [.columns(formatColumns(frame: frame,
                                   addressWidth: addressWidth,
                                   index: index))]
  }

  /// Format just one frame.
  ///
  /// @param frame         The frame to format.
  /// @param addressWidth  The width, in characters, of an address.
  /// @param index         The frame index, if required.
  ///
  /// @result A `String` containing the formatted data.
  public func format(frame: Backtrace.Frame,
                     addressWidth: Int,
                     index: Int? = nil) -> String {
    let rows = formatRows(frame: frame,
                          addressWidth: addressWidth,
                          index: index)
    return BacktraceFormatter.formatTable(rows, alignments: [.right])
  }

  /// Format the frame list from a backtrace.
  ///
  /// @param frames        The frames to format.
  /// @param addressWidth  The width, in characters, of an address.
  ///
  /// @result A `String` containing the formatted data.
  public func format(frames: some Sequence<Backtrace.Frame>,
                     addressWidth: Int) -> String {
    var rows: [TableRow] = []

    var n = 0
    for frame in frames {
      rows += formatRows(frame: frame, addressWidth: addressWidth, index: n)

      if case let .omittedFrames(count) = frame {
        n += count
      } else {
        n += 1
      }
    }

    return BacktraceFormatter.formatTable(rows, alignments: [.right])
  }

  /// Format a `Backtrace`
  ///
  /// @param backtrace  The `Backtrace` object to format.
  ///
  /// @result A `String` containing the formatted data.
  public func format(backtrace: Backtrace) -> String {
    return format(frames: backtrace.frames,
                  addressWidth: (backtrace.addressWidth + 3) / 4)
  }

  /// Grab source lines for a symbolicated backtrace.
  ///
  /// Tries to open the file corresponding to the symbol; if successful,
  /// it will return a string containing the specified lines of context,
  /// with the point at which the program crashed highlighted.
  private func formattedSourceLines(from sourceLocation: SymbolicatedBacktrace.SourceLocation,
                                    indent theIndent: Int = 2) -> String? {
    guard let fp = fopen(sourceLocation.path, "rt") else {
      return nil
    }
    defer {
      fclose(fp)
    }

    let indent = String(repeating: " ", count: theIndent)
    var lines: [String] = []
    var line = 1
    let buffer = UnsafeMutableBufferPointer<CChar>.allocate(capacity: 4096)
    var currentLine = ""

    let maxLine = sourceLocation.line + options._sourceContextLines
    let maxLineWidth = max("\(maxLine)".count, 4)

    let doLine = { sourceLine in
      if line >= sourceLocation.line - options._sourceContextLines
           && line <= sourceLocation.line + options._sourceContextLines {
        let untabified = untabify(sourceLine)
        let code = options._theme.code(untabified)
        let theLine: String
        if line == sourceLocation.line {
          let lineNumber = options._theme.crashedLineNumber(pad("\(line)",
                                                                to: maxLineWidth,
                                                                aligned: .right))
          let highlightWidth = options._width - 2 * theIndent
          theLine = options._theme.crashedLine(pad("\(lineNumber) \(code)",
                                                   to: highlightWidth))
        } else {
          let lineNumber = options._theme.lineNumber(pad("\(line)",
                                                         to: maxLineWidth,
                                                         aligned: .right))
          theLine = "\(lineNumber) \(code)"
        }
        lines.append("\(indent)\(theLine)")

        if line == sourceLocation.line {
          // sourceLocation.column is an index in UTF-8 code units in
          // `untabified`.  We should point at the grapheme cluster that
          // contains that UTF-8 index.
          let adjustedColumn: Int
          if sourceLocation.column > 0 {
            adjustedColumn = sourceLocation.column
          } else {
            if let ndx = code.firstIndex(where: { $0 != " " }) {
              adjustedColumn = code.distance(from: code.startIndex, to: ndx) + 1
            } else {
              adjustedColumn = 1
            }
          }
          let utf8Ndx
            = untabified.utf8.index(untabified.utf8.startIndex,
                                    offsetBy: adjustedColumn,
                                    limitedBy: untabified.utf8.endIndex)
            ?? untabified.utf8.endIndex

          // Adjust it to point at a grapheme cluster start
          let strNdx = untabified.index(
            untabified.index(utf8Ndx, offsetBy: 1,
                             limitedBy: untabified.endIndex)
              ?? untabified.endIndex,
            offsetBy: -1,
            limitedBy: untabified.startIndex) ?? untabified.startIndex

          // Work out the terminal width up to that point
          let terminalWidth = measure(untabified.prefix(upTo: strNdx))

          let pad = String(repeating: " ",
                           count: max(terminalWidth - 1, 0))

          let marker = options._theme.crashLocation()
          let blankForNumber = options._theme.lineNumber(
            String(repeating: " ", count: maxLineWidth))

          lines.append("\(indent)\(blankForNumber) \(pad)\(marker)")
        }
      }
    }

    while feof(fp) == 0 && ferror(fp) == 0 {
      guard let result = fgets(buffer.baseAddress,
                               CInt(buffer.count), fp) else {
        break
      }

      let chunk = String(cString: result)
      currentLine += chunk
      if currentLine.hasSuffix("\n") {
        currentLine.removeLast()
        doLine(currentLine)
        currentLine = ""
        line += 1
      }
    }

    doLine(currentLine)

    return lines.joined(separator: "\n")
  }

  /// Format an individual frame into a list of columns.
  ///
  /// @params frame  The frame to format.
  ///
  /// @result An array of strings, one per column.
  public func formatColumns(frame: SymbolicatedBacktrace.Frame,
                            addressWidth: Int,
                            index: Int? = nil) -> [String] {
    let pc: String
    var attrs: [String] = []

    switch frame.captured {
      case let .programCounter(address):
        pc = "\(hex(address, width: addressWidth))"
      case let .returnAddress(address):
        pc = "\(hex(address, width: addressWidth))"
        attrs.append("ra")
      case let .asyncResumePoint(address):
        pc = "\(hex(address, width: addressWidth))"
        attrs.append("async")
      case .omittedFrames(_), .truncated:
        pc = ""
    }

    if frame.inlined {
      attrs.append("inlined")
    }

    if frame.isSwiftThunk {
      attrs.append("thunk")
    }

    if frame.isSystem {
      attrs.append("system")
    }

    var formattedSymbol: String? = nil
    var hasSourceLocation = false

    if let symbol = frame.symbol {
      let displayName = options._demangle ? symbol.name : symbol.rawName
      let themedName = options._theme.symbol(displayName)

      let offset: String
      if symbol.offset > 0 {
        offset = options._theme.offset(" + \(symbol.offset)")
      } else if symbol.offset < 0 {
        offset = options._theme.offset(" - \(-symbol.offset)")
      } else {
        offset = ""
      }

      let imageName: String
      if options._showImageNames {
        if symbol.imageIndex >= 0 {
          imageName = " in " + options._theme.imageName(symbol.imageName)
        } else {
          imageName = ""
        }
      } else {
        imageName = ""
      }

      let location: String
      if var sourceLocation = symbol.sourceLocation {
        if options._sanitizePaths {
          sourceLocation.path = sanitizePath(sourceLocation.path)
        }
        location = " at " + options._theme.sourceLocation("\(sourceLocation)")
        hasSourceLocation = true
      } else {
        location = ""
      }

      formattedSymbol = "\(themedName)\(offset)\(imageName)\(location)"
    }

    let location: String
    if !hasSourceLocation || options._showAddresses {
      let formattedPc = options._theme.programCounter(pc)
      if let formattedSymbol = formattedSymbol {
        location = "\(formattedPc) \(formattedSymbol)"
      } else {
        location = formattedPc
      }
    } else if let formattedSymbol = formattedSymbol {
      location = formattedSymbol
    } else {
      location = options._theme.programCounter(pc)
    }

    var columns: [String] = []

    if let index = index {
      let frameIndex: String
      switch frame.captured {
        case .omittedFrames(_), .truncated:
          frameIndex = options._theme.frameIndex("...")
        default:
          frameIndex = options._theme.frameIndex("\(index)")
      }
      columns.append(frameIndex)
    }

    if options._showFrameAttributes {
      columns.append(attrs.map(
                       options._theme.frameAttribute
                     ).joined(separator: " "))
    }

    columns.append(location)

    return columns
  }

  /// Format a frame into a list of rows.
  ///
  /// @param frame         The frame to format.
  /// @param addressWidth  The width, in characters, of an address.
  /// @param index         The frame index, if required.
  ///
  /// @result An array of table rows.
  public func formatRows(frame: SymbolicatedBacktrace.Frame,
                         addressWidth: Int,
                         index: Int? = nil,
                         showSource: Bool = true) -> [TableRow] {
    let columns = formatColumns(frame: frame,
                                addressWidth: addressWidth,
                                index: index)
    var rows: [TableRow] = [.columns(columns)]

    if showSource {
      if let symbol = frame.symbol,
         let sourceLocation = symbol.sourceLocation,
         let lines = formattedSourceLines(from: sourceLocation) {
        rows.append(.raw(""))
        rows.append(.raw(lines))
        rows.append(.raw(""))
      }
    }

    return rows
  }

  /// Format just one frame.
  ///
  /// @param frame         The frame to format.
  /// @param addressWidth  The width, in characters, of an address.
  /// @param index         The frame index, if required.
  ///
  /// @result A `String` containing the formatted data.
  public func format(frame: SymbolicatedBacktrace.Frame,
                     addressWidth: Int,
                     index: Int? = nil,
                     showSource: Bool = true) -> String {
    let rows = formatRows(frame: frame, addressWidth: addressWidth,
                          index: index, showSource: showSource)
    return BacktraceFormatter.formatTable(rows, alignments: [.right])
  }

  /// Return `true` if we should skip the specified frame
  public func shouldSkip(_ frame: SymbolicatedBacktrace.Frame) -> Bool {
    return (options._skipRuntimeFailures && frame.isSwiftRuntimeFailure)
      || (options._skipSystemFrames && frame.isSystem)
      || (options._skipThunkFunctions && frame.isSwiftThunk)
  }

  /// Format the frame list from a symbolicated backtrace.
  ///
  /// @param frames        The frames to format.
  /// @param addressWidth  The width, in characters, of an address.
  ///
  /// @result A `String` containing the formatted data.
  public func format(frames: some Sequence<SymbolicatedBacktrace.Frame>,
                     addressWidth: Int) -> String {
    var rows: [TableRow] = []
    var sourceLocationsShown = Set<SymbolicatedBacktrace.SourceLocation>()

    var n = 0
    for frame in frames {
      if shouldSkip(frame) {
        continue
      }

      var showSource = options._showSourceCode
      if let symbol = frame.symbol,
         let sourceLocation = symbol.sourceLocation {
        if sourceLocationsShown.contains(sourceLocation) {
          showSource = false
        } else {
          sourceLocationsShown.insert(sourceLocation)
        }
      }

      rows += formatRows(frame: frame, addressWidth: addressWidth,
                         index: n, showSource: showSource)

      if case let .omittedFrames(count) = frame.captured {
        n += count
      } else {
        n += 1
      }
    }

    return BacktraceFormatter.formatTable(rows, alignments: [.right])
  }

  /// Format a `SymbolicatedBacktrace`
  ///
  /// @param backtrace  The `SymbolicatedBacktrace` object to format.
  ///
  /// @result A `String` containing the formatted data.
  public func format(backtrace: SymbolicatedBacktrace) -> String {
    let addressChars = (backtrace.addressWidth + 3) / 4
    var result = format(frames: backtrace.frames, addressWidth: addressChars)

    switch options._showImages {
      case .none:
        break
      case .all:
        result += "\n\nImages:\n"
        result += format(images: backtrace.images,
                         addressWidth: addressChars)
      case .mentioned:
        var mentionedImages = Set<Int>()
        for frame in backtrace.frames {
          if shouldSkip(frame) {
            continue
          }
          if let symbol = frame.symbol, symbol.imageIndex >= 0 {
            mentionedImages.insert(symbol.imageIndex)
          }
        }

        let images = mentionedImages.sorted().map{ backtrace.images[$0] }
        let omitted = backtrace.images.count - images.count
        if omitted > 0 {
          result += "\n\nImages (\(omitted) omitted):\n"
        } else {
          result += "\n\nImages (only mentioned):\n"
        }
        result += format(images: images, addressWidth: addressChars)
    }

    return result
  }

  /// Format a `Backtrace.Image` into a list of columns.
  ///
  /// @param image         The `Image` object to format.
  /// @param addressWidth  The width of an address, in characters.
  ///
  /// @result An array of strings, one per column.
  public func formatColumns(image: Backtrace.Image,
                            addressWidth: Int) -> [String] {
    let addressRange = "\(hex(image.baseAddress, width: addressWidth))–\(hex(image.endOfText, width: addressWidth))"
    let buildID: String
    if let bytes = image.buildID {
      buildID = hex(bytes)
    } else {
      buildID = "<no build ID>"
    }
    let imagePath: String
    if options._sanitizePaths {
      imagePath = sanitizePath(image.path)
    } else {
      imagePath = image.path
    }
    return [
      options._theme.imageAddressRange(addressRange),
      options._theme.imageBuildID(buildID),
      options._theme.imageName(image.name),
      options._theme.imagePath(imagePath)
    ]
  }

  /// Format an array of `Backtrace.Image`s.
  ///
  /// @param images        The array of `Image` objects to format.
  /// @param addressWidth  The width of an address, in characters.
  ///
  /// @result A string containing the formatted data.
  public func format(images: some Sequence<Backtrace.Image>,
                     addressWidth: Int) -> String {
    let rows = images.map{
      TableRow.columns(
        formatColumns(image: $0,
                      addressWidth: addressWidth)
      )
    }

    return BacktraceFormatter.formatTable(rows)
  }
}
