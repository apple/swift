//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
// This file provides link compatibility shims to work through some ABI
// changes.
//===----------------------------------------------------------------------===//
#include "swift/Runtime/Config.h"

#define OLD_SYMBOL(NAME) \
  SWIFT_EXPORT_FROM(swift_Concurrency) extern "C" void const * const NAME = nullptr;

OLD_SYMBOL($ss4TaskV6HandleVMn)
OLD_SYMBOL($ss4TaskV8PriorityOMa)
OLD_SYMBOL($ss4TaskV8PriorityO11unspecifiedyA2DmFWC)
OLD_SYMBOL($ss6detach8priority9operations4TaskV6HandleVy_xs5NeverOGAE8PriorityO_xyYaYbcntlF)
OLD_SYMBOL($ss6detach8priority9operations4TaskV6HandleVy_xs5NeverOGAE8PriorityO_xyYaYbcntlFfA_)
OLD_SYMBOL($ss18UnsafeContinuationVMn)
OLD_SYMBOL($s13AsyncIterators0A8SequencePTl)
OLD_SYMBOL($s7Elements13AsyncSequencePTl)
OLD_SYMBOL($s7Elements21AsyncIteratorProtocolPTl)
OLD_SYMBOL($ss13AsyncSequenceMp)
OLD_SYMBOL($ss13AsyncSequenceP04makeA8Iterator0aD0QzyF)
OLD_SYMBOL($ss13AsyncSequenceP04makeA8Iterator0aD0QzyFTq)
OLD_SYMBOL($ss13AsyncSequenceP0A8IteratorAB_s0aC8ProtocolTn)
OLD_SYMBOL($ss13AsyncSequenceTL)
OLD_SYMBOL($ss19CheckedContinuationVMn)
OLD_SYMBOL($ss13AsyncSequenceP04makeA8Iterator0aD0QzyFTj)
OLD_SYMBOL($ss18UnsafeContinuationVMa)
OLD_SYMBOL($ss19CheckedContinuationV6resume8throwingyq_n_tF)
OLD_SYMBOL($ss19CheckedContinuationV6resume9returningyxn_tF)
OLD_SYMBOL($ss19CheckedContinuationVMa)
OLD_SYMBOL($ss21AsyncIteratorProtocolMp)
OLD_SYMBOL($ss21AsyncIteratorProtocolP4next7ElementQzSgyYaKFTj)
OLD_SYMBOL($ss21AsyncIteratorProtocolP4next7ElementQzSgyYaKFTjTu)
OLD_SYMBOL($ss21AsyncIteratorProtocolP4next7ElementQzSgyYaKFTq)
OLD_SYMBOL($ss27withTaskCancellationHandler7handler9operationxyyYbXE_xyYaKXEtYaKlF)
OLD_SYMBOL($ss27withTaskCancellationHandler7handler9operationxyyYbXE_xyYaKXEtYaKlFTu)
OLD_SYMBOL($ss31withCheckedThrowingContinuation8function_xSS_ys0bD0Vyxs5Error_pGXEtYaKlF)
OLD_SYMBOL($ss31withCheckedThrowingContinuation8function_xSS_ys0bD0Vyxs5Error_pGXEtYaKlFTu)
OLD_SYMBOL($ss4TaskV17checkCancellationyyKFZ)
OLD_SYMBOL($ss5ActorMp)
OLD_SYMBOL($ss4TaskV8PriorityOMn)
OLD_SYMBOL($ss12TaskPriorityVMn)
