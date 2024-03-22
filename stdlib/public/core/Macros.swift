//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#if $Macros && hasAttribute(attached)

/// Specifies the module and type name for a macro's implementation.
///
/// This macro can only be used to define a macro;
/// using it in any other context is an error.
/// The specified type must conform to the protocols
/// that correspond to the roles of the macro being declared.
/// For example:
///
/// ```swift
/// macro stringify(_ value: T) -> (T, String) =
///     #externalMacro(module: "ExampleMacros", type: "StringifyMacro")
/// ```
///
/// - Parameters:
///     - module: The module name.
///     - type: The type that implements the macro.
///
/// - Returns: The macro's implementation.
@freestanding(expression)
public macro externalMacro<T>(module: String, type: String) -> T =
  Builtin.ExternalMacro

// File and path-related information

/// Produces a unique identifier for the source file in which the macro appears.
///
/// The unique identifier has the form *module*/*file*,
/// where *file* is the name of the file in which the expression appears
/// and *module* is the name of the module that this file is part of.
///
/// Because `#fileID` doesn't embed the full path to the source file,
/// unlike `#filePath`,
/// it gives you better privacy and reduces the size of the compiled binary.
///
/// Note: To parse a `#fileID` expression,
/// read the module name as the text before the first slash (`/`)
/// and the filename as the text after the last slash.
/// In future versions of Swift,
/// the string might contain multiple slashes,
/// such as `MyModule/some/disambiguation/MyFile.swift`.
///
/// This macro's value can be changed by `#sourceLocation`,
/// as described in [Line Control Statement][] in *[The Swift Programming Language][tspl]*.
///
/// [Line Control Statement]: https://docs.swift.org/swift-book/documentation/the-swift-programming-language/statements#Line-Control-Statement
/// [tspl]: https://docs.swift.org/swift-book/
@freestanding(expression)
public macro fileID<T: ExpressibleByStringLiteral>() -> T =
  Builtin.FileIDMacro

/// Produces the complete path to the file in which the macro appears.
///
/// Because `#fileID` doesn't embed the full path to the source file,
/// unlike `#filePath`,
/// it gives you better privacy and reduces the size of the compiled binary.
/// Avoid using `#filePath` outside of tests, build scripts,
/// or other code that doesn't become part of the shipping program.
///
/// This macro's value can be changed by `#sourceLocation`,
/// as described in [Line Control Statement][] in *[The Swift Programming Language][tspl]*.
///
/// [Line Control Statement]: https://docs.swift.org/swift-book/documentation/the-swift-programming-language/statements#Line-Control-Statement
/// [tspl]: https://docs.swift.org/swift-book/
@freestanding(expression)
public macro filePath<T: ExpressibleByStringLiteral>() -> T =
  Builtin.FilePathMacro

/// Produces the path to the file in which it appears.
///
/// The string value from `#file` depends on the language version,
/// to enable migration from the old `#filePath` behavior
/// to the new `#fileID` behavior.
/// Currently, `#file` has the same value as `#filePath`.
/// In a future version of Swift,
/// `#file` will have the same value as `#fileID` instead.
/// To adopt the future behavior,
/// replace `#file` with `#fileID` or `#filePath` as appropriate.
///
/// This macro's value can be changed by `#sourceLocation`,
/// as described in [Line Control Statement][] in *[The Swift Programming Language][tspl]*.
///
/// [Line Control Statement]: https://docs.swift.org/swift-book/documentation/the-swift-programming-language/statements#Line-Control-Statement
/// [tspl]: https://docs.swift.org/swift-book/
@freestanding(expression)
public macro file<T: ExpressibleByStringLiteral>() -> T =
  Builtin.FileMacro

/// Produces the name of the declaration in which it appears.
///
/// Inside a function,
/// the value `#function` produces is the name of that function,
/// inside a method it's the name of that method,
/// inside a property getter or setter it's the name of that property,
/// inside special members like `init` or `subscript`
/// it's the name of that keyword,
/// and at the top level of a file it's the name of the current module.
///
/// When used as the default value of a function or method parameter,
/// this macro's value is determined
/// when the default value expression is evaluated at the call site.
/// For example:
///
/// ```swift
/// func logFunctionName(string: String = #function) {
///     print(string)
/// }
/// func myFunction() {
///     logFunctionName() // Prints "myFunction()".
/// }
/// ```
@freestanding(expression)
public macro function<T: ExpressibleByStringLiteral>() -> T =
  Builtin.FunctionMacro

/// Produces the line number on which it appears.
///
/// This macro's value can be changed by `#sourceLocation`,
/// as described in [Line Control Statement][] in *[The Swift Programming Language][tspl]*.
///
/// [Line Control Statement]: https://docs.swift.org/swift-book/documentation/the-swift-programming-language/statements#Line-Control-Statement
/// [tspl]: https://docs.swift.org/swift-book/
@freestanding(expression)
public macro line<T: ExpressibleByIntegerLiteral>() -> T =
  Builtin.LineMacro

/// Produces the column number in which the macro begins.
@freestanding(expression)
public macro column<T: ExpressibleByIntegerLiteral>() -> T =
  Builtin.ColumnMacro

/// Produces the dynamic shared object (DSO) handle in use where the macro appears.
///
/// - Returns: The DSO handle.
@freestanding(expression)
public macro dsohandle() -> UnsafeRawPointer = Builtin.DSOHandleMacro

/// Produces the given warning message during compilation.
///
/// Compilation proceeds after emitting the message as a nonfatal warning.
///
/// - Parameter warning: The diagnostic message.
@freestanding(declaration)
public macro warning(_ message: String) = Builtin.WarningMacro

/// Emits the given message as a fatal error
/// and terminates the compilation process.
///
/// - Parameter message: The error message.
@freestanding(declaration)
public macro error(_ message: String) = Builtin.ErrorMacro

#endif // $Macros && hasAttribute(attached)
