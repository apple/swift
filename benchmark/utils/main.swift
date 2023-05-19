//===--- main.swift -------------------------------------------*- swift -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// This is just a driver for performance overview tests.

import TestsUtils
import DriverUtils
import Ackermann
import AngryPhonebook
import AnyHashableWithAClass
import Array2D
import ArrayAppend
import ArrayInClass
import ArrayLiteral
import ArrayOfGenericPOD
import ArrayOfGenericRef
import ArrayOfPOD
import ArrayOfRef
import ArraySetElement
import ArraySubscript
import BinaryFloatingPointConversionFromBinaryInteger
import BinaryFloatingPointProperties
import BitCount
import Breadcrumbs
import BucketSort
import BufferFill
import BufferFind
import ByteSwap
import COWTree
import COWArrayGuaranteedParameterOverhead
import CString
import CSVParsing
import Calculator
import CaptureProp
import ChaCha
import ChainedFilterMap
import CharacterLiteralsLarge
import CharacterLiteralsSmall
import CharacterProperties
import CharacterRecognizer
import Chars
import ClassArrayGetter
import CodableTest
import Combos
import CreateObjects
import CxxSetToCollection
import CxxStringConversion
import CxxVectorSum
import DataBenchmarks
import DeadArray
import DevirtualizeProtocolComposition
import DictOfArraysToArrayOfDicts
import DictTest
import DictTest2
import DictTest3
import DictTest4
import DictTest4Legacy
import DictionaryBridge
import DictionaryBridgeToObjC
import DictionaryCompactMapValues
import DictionaryCopy
import DictionaryGroup
import DictionaryKeysContains
import DictionaryLiteralTest
import DictionaryOfAnyHashableStrings
import DictionaryRemove
import DictionarySubscriptDefault
import DictionarySwap
#if canImport(_Differentiation)
import Differentiation
#endif
import Diffing
import DiffingMyers
import DropFirst
import DropLast
import DropWhile
import ErrorHandling
import Exclusivity
import ExistentialPerformance
import Fibonacci
import FindStringNaive
import FlattenList
import FloatingPointConversion
import FloatingPointParsing
import FloatingPointPrinting
import Hanoi
import Hash
import Histogram
import HTTP2StateMachine
import IndexPathTest
import InsertCharacter
import IntegerParsing
import Integrate
import IterateData
import Join
import KeyPathPerformanceTests
import LazyFilter
import LinkedList
import LuhnAlgoEager
import LuhnAlgoLazy
import MapReduce
import Memset
import MirrorTest
import MonteCarloE
import MonteCarloPi
import NaiveRangeReplaceableCollectionConformance
import NibbleSort
import NIOChannelPipeline
import NSDictionaryCastToSwift
import NSErrorTest
#if canImport(Darwin)
import NSStringConversion
#endif
import NopDeinit
import ObjectAllocation
#if canImport(Darwin)
import ObjectiveCBridging
import ObjectiveCBridgingStubs
#if !(SWIFT_PACKAGE || Xcode)
import ObjectiveCNoBridgingStubs
#endif
#endif
import ObserverClosure
import ObserverForwarderStruct
import ObserverPartiallyAppliedMethod
import ObserverUnappliedMethod
import OpaqueConsumingUsers
import OpenClose
import Phonebook
import PointerArithmetics
import PolymorphicCalls
import PopFront
import PopFrontGeneric
import Prefix
import PrefixWhile
import Prims
import PrimsNonStrongRef
import PrimsSplit
import ProtocolConformance
import ProtocolDispatch
import ProtocolDispatch2
import QueueTest
import RC4
import RGBHistogram
import Radix2CooleyTukey
import RandomShuffle
import RandomTree
import RandomValues
import RangeAssignment
import RangeIteration
import RangeOverlaps
import RangeReplaceableCollectionPlusDefault
//import ReadAccessor
import RecursiveOwnedParameter
import ReduceInto
import RemoveWhere
import ReversedCollections
import RomanNumbers
import SIMDRandomMask
import SIMDReduceInteger
import SequenceAlgos
import SetTests
import SevenBoom
import Sim2DArray
//import SimpleArraySpecialization
import SortArrayInClass
import SortIntPyramids
import SortLargeExistentials
import SortLettersInPlace
import SortStrings
import StackPromo
import StaticArray
import StrComplexWalk
import StrToInt
import StringBuilder
import StringComparison
import StringDistance
import StringEdits
import StringEnum
import StringInterpolation
import StringMatch
import StringRemoveDupes
import StringRepeating
import StringReplaceSubrange
import StringSplitting
import StringSwitch
import StringTests
import StringWalk
import SubstringTest
import Suffix
import SuperChars
import TwoSum
import TypeFlood
import UTF8Decode
import UTF16Decode
import Walsh
import WordCount
import XorLoop

register(Ackermann.benchmarks)
register(AngryPhonebook.benchmarks)
register(AnyHashableWithAClass.benchmarks)
register(Array2D.benchmarks)
register(ArrayAppend.benchmarks)
register(ArrayInClass.benchmarks)
register(ArrayLiteral.benchmarks)
register(ArrayOfGenericPOD.benchmarks)
register(ArrayOfGenericRef.benchmarks)
register(ArrayOfPOD.benchmarks)
register(ArrayOfRef.benchmarks)
register(ArraySetElement.benchmarks)
register(ArraySubscript.benchmarks)
register(BinaryFloatingPointConversionFromBinaryInteger.benchmarks)
register(BinaryFloatingPointProperties.benchmarks)
register(BitCount.benchmarks)
register(Breadcrumbs.benchmarks)
register(BucketSort.benchmarks)
register(BufferFill.benchmarks)
register(BufferFind.benchmarks)
register(ByteSwap.benchmarks)
register(COWTree.benchmarks)
register(COWArrayGuaranteedParameterOverhead.benchmarks)
register(CString.benchmarks)
register(CSVParsing.benchmarks)
register(Calculator.benchmarks)
register(CaptureProp.benchmarks)
register(ChaCha.benchmarks)
register(ChainedFilterMap.benchmarks)
register(CharacterLiteralsLarge.benchmarks)
register(CharacterLiteralsSmall.benchmarks)
register(CharacterProperties.benchmarks)
register(CharacterRecognizer.benchmarks)
register(Chars.benchmarks)
register(CodableTest.benchmarks)
register(Combos.benchmarks)
register(ClassArrayGetter.benchmarks)
register(CreateObjects.benchmarks)
register(CxxSetToCollection.benchmarks)
register(CxxStringConversion.benchmarks)
register(CxxVectorSum.benchmarks)
register(DataBenchmarks.benchmarks)
register(DeadArray.benchmarks)
register(DevirtualizeProtocolComposition.benchmarks)
register(DictOfArraysToArrayOfDicts.benchmarks)
register(DictTest.benchmarks)
register(DictTest2.benchmarks)
register(DictTest3.benchmarks)
register(DictTest4.benchmarks)
register(DictTest4Legacy.benchmarks)
register(DictionaryBridge.benchmarks)
register(DictionaryBridgeToObjC.benchmarks)
register(DictionaryCompactMapValues.benchmarks)
register(DictionaryCopy.benchmarks)
register(DictionaryGroup.benchmarks)
register(DictionaryKeysContains.benchmarks)
register(DictionaryLiteralTest.benchmarks)
register(DictionaryOfAnyHashableStrings.benchmarks)
register(DictionaryRemove.benchmarks)
register(DictionarySubscriptDefault.benchmarks)
register(DictionarySwap.benchmarks)
#if canImport(_Differentiation)
register(Differentiation.benchmarks)
#endif
register(Diffing.benchmarks)
register(DiffingMyers.benchmarks)
register(DropFirst.benchmarks)
register(DropLast.benchmarks)
register(DropWhile.benchmarks)
register(ErrorHandling.benchmarks)
register(Exclusivity.benchmarks)
register(ExistentialPerformance.benchmarks)
register(Fibonacci.benchmarks)
register(FindStringNaive.benchmarks)
register(FlattenList.benchmarks)
register(FloatingPointConversion.benchmarks)
register(FloatingPointParsing.benchmarks)
register(FloatingPointPrinting.benchmarks)
register(Hanoi.benchmarks)
register(Hash.benchmarks)
register(Histogram.benchmarks)
register(HTTP2StateMachine.benchmarks)
register(IndexPathTest.benchmarks)
register(InsertCharacter.benchmarks)
register(IntegerParsing.benchmarks)
register(Integrate.benchmarks)
register(IterateData.benchmarks)
register(Join.benchmarks)
register(LazyFilter.benchmarks)
register(KeyPathPerformanceTests.benchmarks)
register(LinkedList.benchmarks)
register(LuhnAlgoEager.benchmarks)
register(LuhnAlgoLazy.benchmarks)
register(MapReduce.benchmarks)
register(Memset.benchmarks)
register(MirrorTest.benchmarks)
register(MonteCarloE.benchmarks)
register(MonteCarloPi.benchmarks)
register(NaiveRangeReplaceableCollectionConformance.benchmarks)
register(NSDictionaryCastToSwift.benchmarks)
register(NSErrorTest.benchmarks)
#if canImport(Darwin)
register(NSStringConversion.benchmarks)
#endif
register(NibbleSort.benchmarks)
register(NIOChannelPipeline.benchmarks)
register(NopDeinit.benchmarks)
register(ObjectAllocation.benchmarks)
#if canImport(Darwin)
register(ObjectiveCBridging.benchmarks)
register(ObjectiveCBridgingStubs.benchmarks)
#if !(SWIFT_PACKAGE || Xcode)
register(ObjectiveCNoBridgingStubs.benchmarks)
#endif
#endif
register(ObserverClosure.benchmarks)
register(ObserverForwarderStruct.benchmarks)
register(ObserverPartiallyAppliedMethod.benchmarks)
register(ObserverUnappliedMethod.benchmarks)
register(OpaqueConsumingUsers.benchmarks)
register(OpenClose.benchmarks)
register(Phonebook.benchmarks)
register(PointerArithmetics.benchmarks)
register(PolymorphicCalls.benchmarks)
register(PopFront.benchmarks)
register(PopFrontGeneric.benchmarks)
register(Prefix.benchmarks)
register(PrefixWhile.benchmarks)
register(Prims.benchmarks)
register(PrimsNonStrongRef.benchmarks)
register(PrimsSplit.benchmarks)
register(ProtocolConformance.benchmarks)
register(ProtocolDispatch.benchmarks)
register(ProtocolDispatch2.benchmarks)
register(QueueTest.benchmarks)
register(RC4.benchmarks)
register(RGBHistogram.benchmarks)
register(Radix2CooleyTukey.benchmarks)
register(RandomShuffle.benchmarks)
register(RandomTree.benchmarks)
register(RandomValues.benchmarks)
register(RangeAssignment.benchmarks)
register(RangeIteration.benchmarks)
register(RangeOverlaps.benchmarks)
register(RangeReplaceableCollectionPlusDefault.benchmarks)
// TODO: rdar://92120528
//register(ReadAccessor.benchmarks)
register(RecursiveOwnedParameter.benchmarks)
register(ReduceInto.benchmarks)
register(RemoveWhere.benchmarks)
register(ReversedCollections.benchmarks)
register(RomanNumbers.benchmarks)
register(SIMDRandomMask.benchmarks)
register(SIMDReduceInteger.benchmarks)
register(SequenceAlgos.benchmarks)
register(SetTests.benchmarks)
register(SevenBoom.benchmarks)
register(Sim2DArray.benchmarks)
//register(SimpleArraySpecialization.benchmarks)
register(SortArrayInClass.benchmarks)
register(SortIntPyramids.benchmarks)
register(SortLargeExistentials.benchmarks)
register(SortLettersInPlace.benchmarks)
register(SortStrings.benchmarks)
register(StackPromo.benchmarks)
register(StaticArray.benchmarks)
register(StrComplexWalk.benchmarks)
register(StrToInt.benchmarks)
register(StringBuilder.benchmarks)
register(StringComparison.benchmarks)
register(StringDistance.benchmarks)
register(StringEdits.benchmarks)
register(StringEnum.benchmarks)
register(StringInterpolation.benchmarks)
register(StringMatch.benchmarks)
register(StringRemoveDupes.benchmarks)
register(StringRepeating.benchmarks)
register(StringReplaceSubrange.benchmarks)

if #available(macOS 10.15, iOS 13, tvOS 13, watchOS 6, *) {
  register(StringSplitting.benchmarks)
}

register(StringSwitch.benchmarks)
register(StringTests.benchmarks)
register(StringWalk.benchmarks)
register(SubstringTest.benchmarks)
register(Suffix.benchmarks)
register(SuperChars.benchmarks)
register(TwoSum.benchmarks)
register(TypeFlood.benchmarks)
register(UTF8Decode.benchmarks)
register(UTF16Decode.benchmarks)
register(Walsh.benchmarks)
register(WordCount.benchmarks)
register(XorLoop.benchmarks)

main()
