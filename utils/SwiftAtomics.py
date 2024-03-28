# ===----------------------------------------------------------------------===//
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2023-2024 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ===----------------------------------------------------------------------===//

atomicTypes = [
    # Name, Size, Alignment, Builtin, Swift Type
    ("_Atomic8BitStorage", "8", "1", "Builtin.Int8", "UInt8"),
    ("_Atomic16BitStorage", "16", "2", "Builtin.Int16", "UInt16"),
    ("_Atomic32BitStorage", "32", "4", "Builtin.Int32", "UInt32"),
    ("_Atomic64BitStorage", "64", "8", "Builtin.Int64", "UInt64"),

    # Note: WordPair only on 64 bit platforms.
    ("_Atomic128BitStorage", "128", "16", "Builtin.Int128", "WordPair"),
]

atomicBits = ["", "8", "16", "32", "64", "128"]

loadOrderings = [
    # Swift, API name, doc name, LLVM name
    ("relaxed", "Relaxed", "relaxed", "monotonic"),
    ("acquiring", "Acquiring", "acquiring", "acquire"),
    (
        "sequentiallyConsistent",
        "SequentiallyConsistent",
        "sequentially consistent",
        "seqcst"
    )
]

storeOrderings = [
    # Swift enum case, API name, doc name, LLVM name
    ("relaxed", "Relaxed", "relaxed", "monotonic"),
    ("releasing", "Releasing", "releasing", "release"),
    (
        "sequentiallyConsistent",
        "SequentiallyConsistent",
        "sequentially consistent",
        "seqcst"
    )
]

updateOrderings = [
    # Swift enum case, API name, doc name, LLVM name, failure name
    ("relaxed", "Relaxed", "relaxed", "monotonic", "monotonic"),
    ("acquiring", "Acquiring", "acquiring", "acquire", "acquire"),
    ("releasing", "Releasing", "releasing", "release", "monotonic"),
    (
        "acquiringAndReleasing",
        "AcquiringAndReleasing",
        "acquiring-and-releasing",
        "acqrel",
        "acquire"
    ),
    (
        "sequentiallyConsistent",
        "SequentiallyConsistent",
        "sequentially consistent",
        "seqcst",
        "seqcst"
    ),
]

integerOperations = [
    # Swift name, llvm name, operator, doc name
    ("WrappingAdd", "add", "&+", "wrapping add"),
    ("WrappingSubtract", "sub", "&-", "wrapping subtract"),
    ("BitwiseAnd", "and", "&", "bitwise AND"),
    ("BitwiseOr", "or", "|", "bitwise OR"),
    ("BitwiseXor", "xor", "^", "bitwise XOR"),

    # These two are handled specially in source.
    ("Min", "min", "", "minimum"),
    ("Max", "max", "", "maximum")
]

boolOperations = [
    # Swift name, llvm name, operator, doc
    ("LogicalAnd", "and", "&&", "logical AND"),
    ("LogicalOr", "or", "||", "logical OR"),
    ("LogicalXor", "xor", "!=", "logical XOR")
]


# LLVM doesn't support arbitrary ordering combinations yet, so for the
# two-ordering cmpxchg variants we need to upgrade the success
# ordering when necessary so that it is at least as "strong" as the
# failure case. This function implements that mapping.
#
# See llvm/Support/AtomicOrdering.h
def actualOrders(success, failure):
    def max(success, failure):
        if failure == "acquire":
            if success == "monotonic":
                return "acquire"
            if success == "release":
                return "acqrel"
        if failure == "seqcst":
            return "seqcst"
        return success
    actualSuccess = max(success, failure)
    return actualSuccess + "_" + failure


def llvmToCaseName(ordering):
    if ordering == "monotonic":
        return "relaxed"
    if ordering == "acquire":
        return "acquiring"
    if ordering == "release":
        return "releasing"
    if ordering == "acqrel":
        return "acquiringAndReleasing"
    if ordering == "seqcst":
        return "sequentiallyConsistent"


def atomicOperationName(intType, operation):
    if operation == "Min":
        return "umin" if intType.startswith("U") else "min"
    if operation == "Max":
        return "umax" if intType.startswith("U") else "max"
    return operation


def lowerFirst(str):
    return str[:1].lower() + str[1:] if str else ""


def argLabel(label):
    return label + ": " if label != "_" else ""
