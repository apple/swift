"""
LLDB Formatters for LLVM data types for use in the swift project.

Load into LLDB with 'command script import /path/to/lldbDataFormatters.py'
"""

import sys
from enum import Enum


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand('type category define -e swift -l c++')

    tName = 'lldbSwiftDataFormatters.SmallBitVectorSummaryProvider'
    debugger.HandleCommand('type summary add -w llvm '
                           '-F %s -x "^llvm::SmallBitVector$"' % tName)
    debugger.HandleCommand('type summary add --expand --skip-references -w swift '
                           '-F lldbSwiftDataFormatters.DemangleNodeSummaryProvider '
                           '-x "^swift::Demangle::Node$"')
    debugger.HandleCommand('type synthetic add --skip-references -w swift '
                           '-l lldbSwiftDataFormatters.DemangleNodeSynthProvider '
                           '-x "^swift::Demangle::Node$"')
    debugger.HandleCommand('type summary add -w swift '
                           '-s "${var.Pointer%S}" '
                           'swift::Identifier')


def SmallBitVectorSummaryProvider(valobj, internal_dict):
    underlyingValue = valobj.GetChildMemberWithName('X').GetValueAsUnsigned()
    numBaseBits = 32
    is64Bit = sys.maxsize > 2**32
    if is64Bit:
        numBaseBits = 64
    smallNumRawBits = numBaseBits - 1
    smallNumSizeBits = None
    if numBaseBits == 32:
        smallNumSizeBits = 5
    elif numBaseBits == 64:
        smallNumSizeBits = 6
    else:
        smallNumSizeBits = smallNumRawBits
    smallNumDataBits = smallNumRawBits - smallNumSizeBits

    # If our underlying value is not small, print we can not dump large values.
    isSmallMask = 1
    if (underlyingValue & isSmallMask) == 0:
        return '<can not read large SmallBitVector>'

    smallRawBits = underlyingValue >> 1
    smallSize = smallRawBits >> smallNumDataBits
    bits = smallRawBits & ((1 << (smallSize + 1)) - 1)
    res = "["
    for i in reversed(range(0, smallSize)):
        if bool(bits & (1 << i)):
            res += '1'
        else:
            res += '0'
    res += "]"
    return res


class DemangleNodePayloadKind(Enum):
    NONE = 0
    TEXT = 1
    INDEX = 2
    ONE_CHILD = 3
    TWO_CHILDREN = 4
    MANY_CHILDREN = 5


class DemangleNodeSynthProvider:
    """ Provider for swift::Demangle::Node """

    def __init__(self, valobj, internal_dict):
        self.valobj = valobj
        self.update()  # initialize this provider

    def num_children(self):
        if self.payload_kind in [DemangleNodePayloadKind.TEXT,
                                 DemangleNodePayloadKind.INDEX,
                                 DemangleNodePayloadKind.ONE_CHILD]:
            return 2
        if self.payload_kind is DemangleNodePayloadKind.TWO_CHILDREN:
            return 3
        if self.payload_kind is DemangleNodePayloadKind.MANY_CHILDREN:
            return self.payload.GetChildMemberWithName('Children'). \
                GetChildMemberWithName('Number').GetValueAsUnsigned() + 1
        return 1

    def get_child_index(self, name):
        if name == 'NodeKind':
            return 0

        if self.payload_kind is DemangleNodePayloadKind.TEXT and name == 'Text':
            return 1
        if self.payload_kind is DemangleNodePayloadKind.INDEX and name == 'Index':
            return 1
        if self.payload_kind is DemangleNodePayloadKind.NONE:
            return None
        try:
            return int(name.lstrip('[').rstrip(']')) + 1
        except ValueError:
            return None

    def get_child_at_index(self, index):
        if index == 0:
            return self.valobj.GetChildMemberWithName('NodeKind')

        index -= 1

        if self.payload_kind is DemangleNodePayloadKind.TEXT and index == 0:
            return self.payload.GetChildMemberWithName('Text')
        if self.payload_kind is DemangleNodePayloadKind.INDEX and index == 0:
            return self.payload.GetChildMemberWithName('Index')
        if self.payload_kind is DemangleNodePayloadKind.ONE_CHILD \
                and index == 0:
            return self.payload \
                .GetChildMemberWithName('InlineChildren') \
                .GetChildAtIndex(0)
        if self.payload_kind is DemangleNodePayloadKind.TWO_CHILDREN \
                and 0 <= index <= 1:
            return self.payload \
                .GetChildMemberWithName('InlineChildren') \
                .GetChildAtIndex(index)
        if self.payload_kind is DemangleNodePayloadKind.MANY_CHILDREN \
                and index >= 0:
            node_vector = self.payload.GetChildMemberWithName('Children')
            length = node_vector.GetChildMemberWithName('Number')
            if index >= length.GetValueAsUnsigned():
                return None
            nodes_ptr = node_vector.GetChildMemberWithName('Nodes')
            offset = self.node_ptr_size * index
            return nodes_ptr.CreateChildAtOffset('[' + str(index) + ']',
                                                 offset, self.node_ptr_type)
        return None

    def update(self):
        self.payload_kind = DemangleNodePayloadKind(
            self.valobj.GetChildMemberWithName('NodePayloadKind').GetValueAsUnsigned()
        )
        valobj_type = self.valobj.GetType()
        if valobj_type.IsPointerType():
            self.node_ptr_type = valobj_type
        else:
            self.node_ptr_type = valobj_type.GetPointerType()
        self.node_ptr_size = self.node_ptr_type.GetByteSize()
        self.payload = self.valobj.GetChildAtIndex(0)


def DemangleNodeSummaryProvider(valobj, internal_dict):
    valobj = valobj.GetNonSyntheticValue()
    result = valobj.GetChildMemberWithName('NodeKind').GetValue()
    result += ', '
    payload_kind = DemangleNodePayloadKind(
        valobj.GetChildMemberWithName('NodePayloadKind').GetValueAsUnsigned()
    )
    if payload_kind is DemangleNodePayloadKind.NONE:
        result += '0 children'
    if payload_kind is DemangleNodePayloadKind.TEXT:
        result += 'Text='
        result += valobj \
            .GetChildAtIndex(0) \
            .GetChildMemberWithName('Text') \
            .GetSummary()
    if payload_kind is DemangleNodePayloadKind.INDEX:
        result += 'Index='
        index = valobj \
            .GetChildAtIndex(0) \
            .GetChildMemberWithName('Index') \
            .GetValueAsUnsigned()
        result += str(index)
    if payload_kind is DemangleNodePayloadKind.ONE_CHILD:
        result += '1 child'
    if payload_kind in [DemangleNodePayloadKind.TWO_CHILDREN,
                        DemangleNodePayloadKind.MANY_CHILDREN]:
        num_children = \
            DemangleNodeSynthProvider(valobj, internal_dict).num_children() - 1
        result += str(num_children)
        result += ' children'
    return result
