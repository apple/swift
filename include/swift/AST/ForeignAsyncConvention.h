//===--- ForeignAsyncConvention.h - Async conventions -----------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the ForeignAsyncConvention structure, which
// describes the rules for how to detect that a foreign API is asynchronous.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_FOREIGN_ASYNC_CONVENTION_H
#define SWIFT_FOREIGN_ASYNC_CONVENTION_H

#include "swift/AST/Type.h"

namespace swift {

/// A small structure describing the async convention of a foreign declaration.
class ForeignAsyncConvention {
public:
  struct Info {
  private:
    /// The index of the completion handler parameters.
    unsigned CompletionHandlerParamIndex;

    /// When non-zero, subtracting one indicates which parameter to the completion handler is
    /// the Error? parameter that makes this async function also
    /// throwing.
    unsigned CompletionHandlerErrorParamIndexPlusOneOrZero;
    
    /// When non-zero, indicates that the presence of an error is determined by
    /// an integral argument to the completion handler being zero or nonzero.
    unsigned CompletionHandlerFlagParamIndexPlusOneWithPolarityOrZero;
    
  public:
    Info()
      : CompletionHandlerParamIndex(0),
        CompletionHandlerErrorParamIndexPlusOneOrZero(0) { }

    Info(unsigned completionHandlerParamIndex,
         llvm::Optional<unsigned> completionHandlerErrorParamIndex,
         llvm::Optional<unsigned> completionHandlerFlagParamIndex,
         bool completionHandlerFlagIsErrorOnZero)
        : CompletionHandlerParamIndex(completionHandlerParamIndex),
          CompletionHandlerErrorParamIndexPlusOneOrZero(
              completionHandlerErrorParamIndex
                  ? *completionHandlerErrorParamIndex + 1
                  : 0),
          CompletionHandlerFlagParamIndexPlusOneWithPolarityOrZero(
              completionHandlerFlagParamIndex
                  ? (*completionHandlerFlagParamIndex |
                     ((unsigned)completionHandlerFlagIsErrorOnZero << 31)) +
                        1
                  : 0) {}

    /// Retrieve the index of the completion handler argument in the method's
    /// parameter list.
    unsigned completionHandlerParamIndex() const {
      return CompletionHandlerParamIndex;
    }
    
    /// Retrieve the index of the \c Error? parameter in the completion handler's
    /// parameter list.
    ///
    /// Typically, when argument passed to this parameter is non-null, the
    /// provided error will be thrown by the async function. If a
    /// \c completionHandlerFlagParamIndex is also specified, the
    /// value of that flag instead indicates whether an error should be raised.
    llvm::Optional<unsigned> completionHandlerErrorParamIndex() const {
      if (CompletionHandlerErrorParamIndexPlusOneOrZero == 0)
        return llvm::None;

      return CompletionHandlerErrorParamIndexPlusOneOrZero - 1;
    }

    /// Retrieve the index of the error flag parameter in the completion handler's
    /// parameter list, if any.
    ///
    /// If present, the boolean value of this argument will indicate whether the
    /// operation completed with an error. The \c completionHandlerFlagIsErrorOnZero
    /// value indicates whether this argument being zero indicates an error, or
    /// whether being nonzero indicates an error.
    llvm::Optional<unsigned> completionHandlerFlagParamIndex() const {
      if (CompletionHandlerFlagParamIndexPlusOneWithPolarityOrZero == 0)
        return llvm::None;

      return (CompletionHandlerFlagParamIndexPlusOneWithPolarityOrZero - 1)
        & 0x7FFFFFFFu;
    }

    /// Indicates the polarity of the error flag parameter to the completion handler.
    ///
    /// It is only valid to call this if \c completionHandlerFlagParamIndex returns
    /// a non-\c None value; if there is no flag parameter to the completion handler, the value
    /// of this property is meaningless. Otherwise, if true is returned, then a zero flag value
    /// indicates an error, and nonzero indicates success. If false, then a zero flag value
    /// indicates success, and nonzero indicates an error.
    bool completionHandlerFlagIsErrorOnZero() const {
      return (CompletionHandlerFlagParamIndexPlusOneWithPolarityOrZero - 1)
        & 0x80000000u;
    }

    /// Whether the async function is throwing due to the completion handler
    /// having an \c Error? parameter.
    ///
    /// Equivalent to \c static_cast<bool>(completionHandlerErrorParamIndex()).
    bool isThrowing() const {
      return CompletionHandlerErrorParamIndexPlusOneOrZero != 0;
    }
  };

  /// The type of the completion handler parameter.
  CanType CompletionHandlerType;

  /// Information about the async convention that can be determined from an
  /// Objective-C declaration by itself.
  Info TheInfo;

public:
  ForeignAsyncConvention() : TheInfo() { }

  ForeignAsyncConvention(
      CanType completionHandlerType, unsigned completionHandlerParamIndex,
      llvm::Optional<unsigned> completionHandlerErrorParamIndex,
      llvm::Optional<unsigned> completionHandlerFlagParamIndex,
      bool completionHandlerFlagIsErrorOnZero)
      : CompletionHandlerType(completionHandlerType),
        TheInfo(completionHandlerParamIndex, completionHandlerErrorParamIndex,
                completionHandlerFlagParamIndex,
                completionHandlerFlagIsErrorOnZero) {}

  /// Retrieve the type of the completion handler parameter.
  CanType completionHandlerType() const { return CompletionHandlerType; }

  /// Retrieve the index of the completion handler parameter, which will be
  /// erased from the Swift signature of the imported async function.
  unsigned completionHandlerParamIndex() const {
    return TheInfo.completionHandlerParamIndex();
  }

  /// Retrieve the index of the \c Error? parameter in the completion handler's
  /// parameter list.
  ///
  /// Typically, when argument passed to this parameter is non-null, the
  /// provided error will be thrown by the async function. If a
  /// \c completionHandlerFlagParamIndex is also specified, the
  /// value of that flag instead indicates whether an error should be raised.
  llvm::Optional<unsigned> completionHandlerErrorParamIndex() const {
    return TheInfo.completionHandlerErrorParamIndex();
  }

  /// Retrieve the index of the error flag parameter in the completion handler's
  /// parameter list, if any.
  ///
  /// If present, the boolean value of this argument will indicate whether the
  /// operation completed with an error. The \c completionHandlerFlagIsErrorOnZero
  /// value indicates whether this argument being zero indicates an error, or
  /// whether being nonzero indicates an error.
  llvm::Optional<unsigned> completionHandlerFlagParamIndex() const {
    return TheInfo.completionHandlerFlagParamIndex();
  }

  /// Indicates the polarity of the error flag parameter to the completion handler.
  ///
  /// It is only valid to call this if \c completionHandlerFlagParamIndex returns
  /// a non-\c None value. If true is returned, then a zero flag value indicates an error,
  /// and nonzero indicates success. If false, then a zero flag value indicates success,
  /// and nonzero indicates an error.
  bool completionHandlerFlagIsErrorOnZero() const {
    return TheInfo.completionHandlerFlagIsErrorOnZero();
  }

  /// Whether the async function is throwing due to the completion handler
  /// having an \c Error? parameter.
  ///
  /// Equivalent to \c static_cast<bool>(completionHandlerErrorParamIndex()).
  bool isThrowing() const {
    return TheInfo.isThrowing();
  }

  bool operator==(ForeignAsyncConvention other) const {
    return CompletionHandlerType == other.CompletionHandlerType
        && TheInfo.completionHandlerParamIndex()
             == other.TheInfo.completionHandlerParamIndex()
        && TheInfo.completionHandlerErrorParamIndex()
             == other.TheInfo.completionHandlerErrorParamIndex();
  }

  bool operator!=(ForeignAsyncConvention other) const {
    return !(*this == other);
  }
};

}

#endif
