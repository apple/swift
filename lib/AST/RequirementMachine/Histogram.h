//===--- Histogram.h - Simple histogram for statistics ----------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/ArrayRef.h"

#ifndef SWIFT_HISTOGRAM_H
#define SWIFT_HISTOGRAM_H

#include "swift/Basic/Assertions.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <vector>

namespace swift {

namespace rewriting {

/// A simple histogram that supports two operations: recording an element, and
/// printing a graphical representation.
///
/// Used by the rewrite system to record various statistics, which are dumped
/// when the -analyze-requirement-machine frontend flag is used.
class Histogram {
  unsigned Size;
  unsigned Start;
  std::vector<unsigned> Buckets;
  unsigned OverflowBucket;
  unsigned MaxValue = 0;

  const unsigned MaxWidth = 40;

  /// This is not really the base-10 logarithm, but rather the number of digits
  /// in \p x when printed in base 10.
  ///
  /// So log10(0) == 1, log10(1) == 1, log10(10) == 2, etc.
  static unsigned log10(unsigned x) {
    if (x == 0)
      return 1;
    unsigned result = 0;
    while (x != 0) {
      result += 1;
      x /= 10;
    }
    return result;
  }

public:
  /// Creates a histogram with \p size buckets, where the numbering begins at
  /// \p start.
  Histogram(unsigned size, unsigned start = 0)
      : Size(size),
        Start(start),
        Buckets(size, 0),
        OverflowBucket(0) {
  }

  /// Adds an entry to the histogram. Asserts if the \p value is smaller than
  /// Start. The value is allowed to be greater than or equal to Start + Size,
  /// in which case it's counted in the OverflowBucket.
  void add(unsigned value) {
    ASSERT(value >= Start);
    value -= Start;
    if (value >= Size)
      ++OverflowBucket;
    else
      ++Buckets[value];

    if (value > MaxValue)
      MaxValue = value;
  }

  /// Print a nice-looking graphical representation of the histogram.
  void dump(llvm::raw_ostream &out, const StringRef labels[] = {}) {
    unsigned sumValues = 0;
    unsigned maxValue = 0;
    for (unsigned i = 0; i < Size; ++i) {
      sumValues += Buckets[i];
      maxValue = std::max(maxValue, Buckets[i]);
    }
    sumValues += OverflowBucket;
    maxValue = std::max(maxValue, OverflowBucket);

    size_t maxLabelWidth = 3 + log10(Size + Start);
    if (labels != nullptr) {
      for (unsigned i = 0; i < Size; ++i)
        maxLabelWidth = std::max(labels[i].size(), maxLabelWidth);
    }

    out << std::string(maxLabelWidth, ' ') << " |";
    out << std::string(std::min(MaxWidth, maxValue), ' ');
    out << maxValue << "\n";

    out << std::string(maxLabelWidth, ' ') << " |";
    out << std::string(std::min(MaxWidth, maxValue), ' ');
    out << "*\n";

    out << std::string(maxLabelWidth, '-') << "-+-";
    out << std::string(std::min(MaxWidth, maxValue), '-') << "\n";

    auto scaledValue = [&](unsigned value) {
      if (maxValue > MaxWidth) {
        if (value != 0) {
          // If the value is non-zero, print at least one '#'.
          return std::max(1U, (value * MaxWidth) / maxValue);
        }
      }
      return value;
    };

    for (unsigned i = 0; i < Size; ++i) {
      if (labels) {
        out << std::string(maxLabelWidth - labels[i].size(), ' ');
        out << labels[i];
      } else {
        unsigned key = i + Start;
        out << std::string(maxLabelWidth - log10(key), ' ');
        out << key;
      }
      out << " | ";

      unsigned value = scaledValue(Buckets[i]);
      out << std::string(value, '#') << "\n";
    }

    if (OverflowBucket > 0) {
      out << ">= " << (Size + Start) << " | ";

      unsigned value = scaledValue(OverflowBucket);
      out << std::string(value, '#') << "\n";
    }

    out << std::string(maxLabelWidth, '-') << "-+-";
    out << std::string(std::min(MaxWidth, maxValue), '-') << "\n";

    out << std::string(maxLabelWidth, ' ') << " | ";
    out << "Total: " << sumValues << "\n";
    out << std::string(maxLabelWidth, ' ') << " | ";
    out << "Max:   " << MaxValue << "\n";
  }
};

}  // end namespace rewriting

}  // end namespace swift

#endif