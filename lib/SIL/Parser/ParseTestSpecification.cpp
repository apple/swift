//== ParseTestSpecification.cpp - Parsing for test instructions -*- C++ -*- ==//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/Assertions.h"
#include "swift/SIL/ParseTestSpecification.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILSuccessor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

using namespace swift;
using namespace swift::test;

namespace {

// Helpers: Finding marker instructions

void findAndDeleteTraceValues(SILFunction *function,
                              SmallVectorImpl<SILValue> &values) {
  for (auto &block : *function) {
    for (SILInstruction &inst : block.deletableInstructions()) {
      if (auto *debugValue = dyn_cast<DebugValueInst>(&inst)) {
        if (!debugValue->hasTrace())
          continue;
        values.push_back(debugValue->getOperand());
        debugValue->eraseFromParent();
      }
    }
  }
}

bool isDeleteableTestInstruction(SILInstruction const *instruction) {
  if (auto *dvi = dyn_cast<DebugValueInst>(instruction))
    return dvi->hasTrace();
  if (isa<SpecifyTestInst>(instruction))
    return true;
  return false;
}

SILInstruction *findAnchorInstructionAfter(SpecifyTestInst *tsi) {
  for (auto *instruction = tsi->getNextInstruction(); instruction;
       instruction = instruction->getNextInstruction()) {
    if (!isDeleteableTestInstruction(instruction))
      return instruction;
  }
  // This can't happen because a SpecifyTestInst isn't a terminator itself
  // nor are any deleteable instructions.
  llvm_unreachable("found no anchor after SpecifyTestInst!?");
}

// Helpers: Looking up subobjects by index

SILInstruction *getInstruction(SILFunction *function, unsigned long index) {
  unsigned long i = 0;
  for (auto &block : *function) {
    for (auto &instruction : block) {
      if (i == index) {
        return &instruction;
      }
      ++i;
    }
  }
  llvm_unreachable("bad index!?");
}

SILInstruction *getInstruction(SILBasicBlock *block, unsigned long index) {
  unsigned long i = 0;
  for (auto &instruction : *block) {
    if (i == index) {
      return &instruction;
    }
    ++i;
  }
  llvm_unreachable("bad index!?");
}

SILBasicBlock *getNextBlock(SILBasicBlock *block) {
  auto next = std::next(block->getIterator());
  if (next == block->getFunction()->end())
    return nullptr;
  return &*next;
}

SILBasicBlock *getPreviousBlock(SILBasicBlock *block) {
  auto iterator = block->getIterator();
  if (iterator == block->getFunction()->begin())
    return nullptr;
  return &*std::prev(iterator);
}

SILInstruction *getNextInstructionIgnoringBlocks(SILInstruction *instruction) {
  if (auto *nextInstruction = instruction->getNextInstruction())
    return nextInstruction;
  if (auto *nextBlock = getNextBlock(instruction->getParent()))
    return &nextBlock->front();
  return nullptr;
}

SILInstruction *
getPreviousInstructionIgnoringBlocks(SILInstruction *instruction) {
  if (auto *previousInstruction = instruction->getPreviousInstruction())
    return previousInstruction;
  if (auto *previousBlock = getPreviousBlock(instruction->getParent()))
    return &previousBlock->back();
  return nullptr;
}

SILInstruction *getInstructionOffsetFrom(SILInstruction *base, long offset) {
  if (offset == 0) {
    return base;
  }
  auto *instruction = base;
  if (offset > 0) {
    for (auto index = 0; index < offset; ++index) {
      instruction = getNextInstructionIgnoringBlocks(instruction);
      assert(instruction && "too large an offset!?");
    }
    return instruction;
  }
  // offset < 0
  for (auto index = 0; index > offset; --index) {
    instruction = getPreviousInstructionIgnoringBlocks(instruction);
    assert(instruction && "too negative an offset!?");
  }
  return instruction;
}

SILBasicBlock *getBlockOffsetFrom(SILBasicBlock *base, long offset) {
  if (offset == 0)
    return base;
  if (offset > 0) {
    auto *block = base;
    for (auto counter = 0; counter < offset; ++counter) {
      block = getNextBlock(block);
      assert(block && "too large an offset!?");
    }
    return block;
  }
  // offset < 0
  auto *block = base;
  for (auto counter = 0; counter > offset; --counter) {
    block = getPreviousBlock(block);
    assert(block && "too negative an offset!?");
  }
  return block;
}

SILBasicBlock *getBlock(SILFunction *function, unsigned long index) {
  auto iterator = function->begin();
  for (unsigned long i = 0; i < index; ++i) {
    iterator = std::next(iterator);
  }
  return &*iterator;
}

SILFunction *getFunction(SILModule *module, unsigned long long index) {
  auto &list = module->getFunctionList();
  auto iter = list.begin();
  for (unsigned long i = 0; i < index; ++i) {
    iter = std::next(iter);
  }
  return &*iter;
}

// Parsing:

class ParseTestSpecification;

class ParseArgumentSpecification {
  ParseTestSpecification &outer;
  StringRef specification;
  SILInstruction *context;
  llvm::StringMap<SILValue> values;

  SILValue getTraceValue(unsigned index, SILFunction *function);

public:
  ParseArgumentSpecification(ParseTestSpecification &outer,
                             StringRef specification, SILInstruction *context,
                             llvm::StringMap<SILValue> values)
      : outer(outer), specification(specification), context(context),
        values(values) {}

  Argument parse() {
    auto argument = parseArgument();
    demandEmpty();
    return argument;
  }

private:
  // String parsing helpers:

  bool empty() { return specification.empty(); }

  bool peekPrefix(StringRef prefix) { return specification.starts_with(prefix); }

  bool consumePrefix(StringRef prefix) {
    return specification.consume_front(prefix);
  }

  bool consumeAll(StringRef remainder) {
    if (specification.size() == remainder.size())
      return specification.consume_front(remainder);
    return false;
  }

  bool consumeThrough(char separator, StringRef &substring) {
    unsigned long index = 0;
    if ((index = specification.find(separator)) == StringRef::npos)
      return false;
    substring = specification.slice(0, index);
    specification = specification.slice(index + 1, specification.size());
    return true;
  }

  void demandPrefix(StringRef prefix) {
    if (consumePrefix(prefix))
      return;
    llvm_unreachable("failed to find prefix in specification");
  }

  void demandEmpty() {
    assert(empty() && "specification unexpectedly inhabited?!");
  }

  // Argument parsing: Primitives

  std::optional<BoolArgument> parseBool() {
    if (consumeAll("true")) {
      return BoolArgument{true};
    } else if (consumeAll("false")) {
      return BoolArgument{false};
    }
    return std::nullopt;
  }

  std::optional<UIntArgument> parseUInt() {
    unsigned long long value;
    if (llvm::consumeUnsignedInteger(specification, /*radix=*/10, value))
      return std::nullopt;
    return UIntArgument{value};
  }

  std::optional<StringArgument> parseString() {
    auto retval = StringArgument{specification};
    specification = specification.drop_front(specification.size());
    return retval;
  }

  std::optional<TaggedUnion<unsigned long long, long long, StringRef>>
  parseSubscript() {
    if (!consumePrefix("["))
      return std::nullopt;
    if (peekPrefix("+") || peekPrefix("-")) {
      bool positive = false;
      if (consumePrefix("+")) {
        positive = true;
      } else {
        bool consumed = consumePrefix("-");
        assert(consumed && "peeked a +/- but can't consume one!?");
        (void)consumed;
        positive = false;
      }
      unsigned long long index = 0;
      bool consumed =
          llvm::consumeUnsignedInteger(specification, /*radix=*/10, index);
      assert(!consumed && "didn't find uint after sign!?");
      demandPrefix("]");
      long long offset = positive ? index : -index;
      return {offset};
    }
    unsigned long long index = 0;
    if (!llvm::consumeUnsignedInteger(specification, /*radix=*/10, index)) {
      demandPrefix("]");
      return {index};
    }
    StringRef string;
    if (!consumeThrough(']', string))
      llvm_unreachable("missing ] to end subscript operation!?");
    return {string};
  }

  // Argument parsing: References

  SILValue parseTraceComponent(SILFunction *within) {
    if (!consumePrefix("trace"))
      return SILValue();
    // TODO: Use a bare @trace (i.e. within == nullptr) to refer to the value
    //       which appears in the debug_value [trace] instruction.
    if (!within)
      within = context->getFunction();
    if (empty() || peekPrefix("."))
      return getTraceValue(0, within);
    if (auto subscript = parseSubscript()) {
      auto index = subscript->get<unsigned long long>();
      return getTraceValue(index, within);
    }
    llvm_unreachable("bad suffix after 'trace'!?");
  }

  std::optional<Argument> parseTraceReference(SILFunction *within) {
    auto trace = parseTraceComponent(within);
    if (!trace)
      return std::nullopt;
    if (!consumePrefix("."))
      return ValueArgument{trace};
    auto *instruction = trace.getDefiningInstruction();
    assert(instruction &&
           "attempting to access operand of non-instruction value!?");
    if (auto arg = parseOperandReference(instruction))
      return *arg;
    llvm_unreachable("bad suffix after 'trace'!?");
  }

  Operand *parseOperandComponent(SILInstruction *within) {
    if (!consumePrefix("operand"))
      return nullptr;
    if (empty()) {
      auto &operand = within->getOperandRef(0);
      return &operand;
    }
    if (auto subscript = parseSubscript()) {
      auto index = subscript->get<unsigned long long>();
      auto &operand = within->getOperandRef(index);
      return &operand;
    }
    llvm_unreachable("bad suffix after 'operand'!?");
  }

  std::optional<Argument> parseOperandReference(SILInstruction *within) {
    auto *operand = parseOperandComponent(within);
    if (!operand)
      return std::nullopt;
    return OperandArgument{operand};
  }

  SILValue parseResultComponent(SILInstruction *within) {
    if (!consumePrefix("result"))
      return nullptr;
    if (empty()) {
      auto result = within->getResult(0);
      return result;
    }
    if (auto subscript = parseSubscript()) {
      auto index = subscript->get<unsigned long long>();
      auto result = within->getResult(index);
      return result;
    }
    llvm_unreachable("bad suffix after 'result'!?");
  }

  std::optional<Argument> parseResultReference(SILInstruction *within) {
    auto result = parseResultComponent(within);
    if (!result)
      return std::nullopt;
    return ValueArgument{result};
  }

  SILArgument *parseBlockArgumentComponent(SILBasicBlock *block) {
    if (!consumePrefix("argument"))
      return nullptr;
    // If this is a bare @argument reference, it refers to the first argument
    // of the block containing the specify_test.
    if (!block) {
      block = context->getParent();
    }
    if (empty()) {
      return block->getArgument(0);
    }
    if (auto subscript = parseSubscript()) {
      auto index = subscript->get<unsigned long long>();
      return block->getArgument(index);
    }
    llvm_unreachable("bad suffix after 'argument'!?");
  }

  std::optional<Argument> parseBlockArgumentReference(SILBasicBlock *block) {
    auto *argument = parseBlockArgumentComponent(block);
    if (!argument)
      return std::nullopt;
    return BlockArgumentArgument{argument};
  }

  using InstructionContext = TaggedUnion<SILFunction *, SILBasicBlock *>;

  SILInstruction *
  parseInstructionComponent(std::optional<InstructionContext> within) {
    if (!consumePrefix("instruction"))
      return nullptr;
    auto getInstructionAtIndex = [](unsigned index, InstructionContext within) {
      if (within.isa<SILFunction *>()) {
        auto *function = within.get<SILFunction *>();
        return getInstruction(function, index);
      }
      auto *block = within.get<SILBasicBlock *>();
      return getInstruction(block, index);
    };
    if (empty() || peekPrefix(".")) {
      if (!within) {
        // If this is a bare @instruction reference, it refers to to the
        // context of the specify_test.
        return context;
      }
      return getInstructionAtIndex(0, *within);
    }
    if (auto subscript = parseSubscript()) {
      // If this is a bare @instruction[...] reference, it can refer either to
      // an instruction counting from the beginning of the function or else to
      // an instruction offset from the context of the specify_test.
      if (!within && subscript->isa<long long>()) {
        auto offset = subscript->get<long long>();
        return getInstructionOffsetFrom(context, offset);
      }
      auto index = subscript->get<unsigned long long>();
      return getInstructionAtIndex(index,
                                   within.value_or(context->getFunction()));
    }
    llvm_unreachable("bad suffix after 'instruction'!?");
  }

  std::optional<Argument>
  parseInstructionReference(std::optional<InstructionContext> within) {
    auto *instruction = parseInstructionComponent(within);
    if (!instruction)
      return std::nullopt;
    if (!consumePrefix("."))
      return InstructionArgument{instruction};
    if (auto arg = parseOperandReference(instruction))
      return arg;
    if (auto res = parseResultReference(instruction))
      return res;
    llvm_unreachable("unhandled suffix after 'instruction'!?");
  }

  SILBasicBlock *parseBlockComponent(SILFunction *within) {
    if (!consumePrefix("block"))
      return nullptr;
    if (empty() || peekPrefix(".")) {
      // If this is a bare @block reference, it refers to the block containing
      // the specify_test instruction.
      if (!within) {
        return context->getParent();
      }
      auto *block = getBlock(within, 0);
      return block;
    }
    if (auto subscript = parseSubscript()) {
      // If this is a bare @block[...] reference, it can refer either to a block
      // counting from the beginning of the function or else to a block offset
      // from the block containing the context of the specify_test.
      if (!within && subscript->isa<long long>()) {
        return getBlockOffsetFrom(context->getParent(),
                                  subscript->get<long long>());
      }
      auto index = subscript->get<unsigned long long>();
      auto *block = getBlock(within ? within : context->getFunction(), index);
      return block;
    }
    llvm_unreachable("bad suffix after 'block'!?");
  }

  std::optional<Argument> parseBlockReference(SILFunction *within) {
    auto *block = parseBlockComponent(within);
    if (!block)
      return std::nullopt;
    if (!consumePrefix("."))
      return BlockArgument{block};
    if (auto arg = parseBlockArgumentReference(block))
      return *arg;
    if (auto inst = parseInstructionReference({block}))
      return *inst;
    llvm_unreachable("unhandled suffix after 'block'!?");
  }

  SILFunction *parseFunctionComponent(SILModule *within) {
    if (!consumePrefix("function"))
      return nullptr;
    if (empty() || peekPrefix(".")) {
      // If this is a bare @function reference, it refers to the function
      // containing the specify_test instruction.
      if (!within) {
        return context->getFunction();
      }
      return ::getFunction(within, 0);
    }
    if (auto subscript = parseSubscript()) {
      if (!within) {
        within = &context->getFunction()->getModule();
      }
      if (subscript->isa<unsigned long long>()) {
        auto index = subscript->get<unsigned long long>();
        auto *fn = ::getFunction(within, index);
        return fn;
      } else {
        auto name = subscript->get<StringRef>();
        auto *specified = within->lookUpFunction(name);
        if (!specified)
          llvm_unreachable("unknown function name!?");
        return specified;
      }
    }
    llvm_unreachable("bad suffix after 'function'!?");
  }

  std::optional<Argument> parseFunctionReference(SILModule *within) {
    auto *function = parseFunctionComponent(within);
    if (!function)
      return std::nullopt;
    if (!consumePrefix("."))
      return FunctionArgument{function};
    if (auto arg = parseBlockArgumentReference(function->getEntryBlock()))
      return *arg;
    if (auto arg = parseInstructionReference({function}))
      return *arg;
    if (auto arg = parseTraceReference(function))
      return *arg;
    if (auto arg = parseBlockReference(function))
      return *arg;
    llvm_unreachable("unhandled suffix after 'function'!?");
  }

  SILValue parseValueComponent() {
    if (!peekPrefix("%"))
      return SILValue();
    auto nameEnd = specification.find_if_not(
        [](char c) { return clang::isAsciiIdentifierContinue(c); }, 1);
    if (nameEnd == StringRef::npos)
      nameEnd = specification.size();
    auto name = specification.take_front(nameEnd);
    specification = specification.drop_front(nameEnd);
    if (!empty() && !peekPrefix(".")) {
      llvm::errs() << specification << "\n";
      llvm::report_fatal_error("bad suffix on value!?");
    }
    auto value = values[name];
    if (!value) {
      llvm::errs() << "unknown value '" << name << "'\n"
                   << "have the following name->value pairs:\n";
      for (auto &pair : values) {
        llvm::errs() << pair.getKey() << "->" << pair.getValue() << "\n";
      }
      llvm::report_fatal_error("unknown value!?");
    }
    return value;
  }

  std::optional<Argument> parseValueReference() {
    auto value = parseValueComponent();
    if (!value)
      return std::nullopt;
    return ValueArgument{value};
  }

  std::optional<Argument> parseReference() {
    if (!consumePrefix("@"))
      return std::nullopt;
    if (auto arg = parseTraceReference(nullptr))
      return *arg;
    if (auto arg =
            parseOperandReference(getInstruction(context->getFunction(), 0)))
      return *arg;
    if (auto arg = parseBlockArgumentReference(nullptr))
      return *arg;
    if (auto arg = parseInstructionReference(std::nullopt))
      return *arg;
    if (auto arg = parseBlockReference(nullptr))
      return *arg;
    if (auto arg = parseFunctionReference(nullptr))
      return *arg;
    return std::nullopt;
  }

  Argument parseArgument() {
    if (auto arg = parseBool())
      return *arg;
    if (auto arg = parseUInt())
      return *arg;
    if (auto arg = parseValueReference())
      return *arg;
    if (auto arg = parseReference())
      return *arg;
    // Parse strings last--everything parses as a string.
    if (auto arg = parseString())
      return *arg;
    llvm_unreachable("bad argument specification");
  }
};

using TraceValueMap =
    llvm::SmallDenseMap<SILFunction *, llvm::SmallVector<SILValue, 4>, 32>;
/// A global map from functions to the trace instructions that they once
/// contained.
static TraceValueMap traceValues;

class ParseTestSpecification {
  friend class ParseArgumentSpecification;
  SmallVectorImpl<StringRef> &components;

  SILValue getTraceValue(unsigned index, SILFunction *function) {
    auto iterator = traceValues.find(function);
    if (iterator == traceValues.end()) {
      // llvm::SmallVector<SILValue, 4> values;
      auto &values = traceValues[function];
      findAndDeleteTraceValues(function, values);
      // auto pair = traceValues.insert({function, values});
      // assert(pair.second);
      iterator = traceValues.find(function);
    }
    return iterator->getSecond()[index];
  }

public:
  ParseTestSpecification(SmallVectorImpl<StringRef> &components)
      : components(components) {}

  void parse(UnparsedSpecification const &specification, Arguments &arguments) {
    StringRef specificationString = specification.string;
    getTestSpecificationComponents(specificationString, components);
    for (auto componentString : components) {
      ParseArgumentSpecification parser(
          *this, componentString, specification.context, specification.values);
      auto argument = parser.parse();
      arguments.storage.push_back(argument);
    }
  }
};

SILValue ParseArgumentSpecification::getTraceValue(unsigned index,
                                                   SILFunction *function) {
  return outer.getTraceValue(index, function);
}

} // anonymous namespace

// Member function implementations

void Argument::print(llvm::raw_ostream &os) {
  switch (kind) {
  case Kind::Value:
    llvm::errs() << "value:\n";
    cast<ValueArgument>(*this).getValue()->print(os);
    break;
  case Kind::Operand:
    os << "operand:\n";
    cast<OperandArgument>(*this).getValue()->print(os);
    break;
  case Kind::Instruction:
    os << "instruction:\n";
    cast<InstructionArgument>(*this).getValue()->print(os);
    break;
  case Kind::BlockArgument:
    os << "block argument:\n";
    cast<BlockArgumentArgument>(*this).getValue()->print(os);
    break;
  case Kind::Block:
    os << "block:\n";
    cast<BlockArgument>(*this).getValue()->print(os);
    break;
  case Kind::Function:
    os << "function:\n";
    cast<FunctionArgument>(*this).getValue()->print(os);
    break;
  case Kind::Bool:
    os << "bool: " << cast<BoolArgument>(*this).getValue() << "\n";
    break;
  case Kind::UInt:
    os << "uint: " << cast<UIntArgument>(*this).getValue() << "\n";
    break;
  case Kind::String:
    os << "string: " << cast<StringArgument>(*this).getValue() << "\n";
    break;
  }
}

// API

void swift::test::getTestSpecificationComponents(
    StringRef specificationString, SmallVectorImpl<StringRef> &components) {
  SmallVector<StringRef, 16> rawComponents;
  specificationString.split(rawComponents, " ");
  for (unsigned long index = 0, size = rawComponents.size(); index < size;
       ++index) {
    auto componentString = rawComponents[index].trim();
    if (componentString.empty())
      continue;
    components.push_back(componentString);
  }
}

void swift::test::getTestSpecifications(
    SILFunction *function,
    SmallVectorImpl<UnparsedSpecification> &specifications) {
  for (auto &block : *function) {
    for (SILInstruction &inst : block.deletableInstructions()) {
      if (auto *tsi = dyn_cast<SpecifyTestInst>(&inst)) {
        auto ref = tsi->getArgumentsSpecification();
        auto *anchor = findAnchorInstructionAfter(tsi);
        specifications.push_back(
            {std::string(ref.begin(), ref.end()), anchor, tsi->getValues()});
        tsi->eraseFromParent();
      }
    }
  }
}

void swift::test::parseTestArgumentsFromSpecification(
    SILFunction *function, UnparsedSpecification const &specification,
    Arguments &arguments, SmallVectorImpl<StringRef> &argumentStrings) {
  ParseTestSpecification parser(argumentStrings);
  parser.parse(specification, arguments);
}
