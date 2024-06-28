//===--- FuzzyStringMatcher.cpp -------------------------------------------===//
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

#include "swift/Basic/Assertions.h"
#include "swift/IDE/FuzzyStringMatcher.h"
#include "clang/Basic/CharInfo.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"

using namespace swift;
using namespace swift::ide;
using clang::toUppercase;
using clang::toLowercase;
using clang::isUppercase;
using clang::isLowercase;

FuzzyStringMatcher::FuzzyStringMatcher(StringRef pattern_)
    : pattern(pattern_), charactersInPattern(1 << (sizeof(char) * 8)) {
  lowercasePattern.reserve(pattern.size());
  unsigned upperCharCount = 0;
  for (char c : pattern) {
    char lower = toLowercase(c);
    upperCharCount += (c == lower) ? 0 : 1;
    lowercasePattern.push_back(lower);
    charactersInPattern.set(static_cast<unsigned char>(lower));
    charactersInPattern.set(static_cast<unsigned char>(toUppercase(c)));
  }
  assert(pattern.size() == lowercasePattern.size());

  // FIXME: pull out the magic constants.
  // This depends on the inner details of the matching algorithm and will need
  // to be updated if we substantially alter it.
  if (pattern.size() == 1) {
    maxScore = 3.0 +  // uppercase match
               0.001; // size bonus
  } else {
    maxScore = 0.25 +                           // percent match bonus
               2.5 +                            // match at start bonus
               pattern.size() * pattern.size(); // max run length score
    if (upperCharCount)                         // max uppercase match score
      maxScore += (upperCharCount + 1) * (upperCharCount + 1);
    maxScore *= 1.1 * 2.5 * 1.2; // exact prefix match bonus
  }
}

bool FuzzyStringMatcher::matchesCandidate(StringRef candidate) const {
  unsigned patternLength = pattern.size();
  unsigned candidateLength = candidate.size();
  if (patternLength > candidateLength)
    return false;

  // Do all of the pattern characters match the candidate in order?
  unsigned pidx = 0, cidx = 0;
  while (pidx < patternLength && cidx < candidateLength) {
    char c = candidate[cidx];
    char p = lowercasePattern[pidx];
    if (p == c || p == toLowercase(c))
      ++pidx;
    ++cidx;
  }

  return pidx == patternLength;
}

static bool isTokenizingChar(char c) {
  switch (c) {
  case '/':
  case '.':
  case '_':
  case '+':
  case '-':
  case ':':
  case ',':
  case ' ':
  case '(':
  case ')':
  case '!':
  case '?':
    return true;
  default:
    return false;
  }
}

namespace {
/// A simple index range.
struct Range {
  unsigned location;
  unsigned length;
};
} // end anonymous namespace

static void
populateTokenTable(SmallVectorImpl<Range> &tokens,
                   llvm::MutableArrayRef<unsigned> characterToTokenIndex,
                   StringRef candidate) {
  unsigned start = 0;
  characterToTokenIndex[0] = 0;

  for (unsigned cidx = 1; cidx < candidate.size(); ++cidx) {
    char current = candidate[cidx];
    char prev = candidate[cidx - 1];

    // Is this a special tokenizing character like '_', or the start of a camel
    // case word?  The uppercase character should start a new token.
    if (isTokenizingChar(prev) ||
        (isUppercase(current) && !isUppercase(prev)) ||
        (clang::isDigit(current) && !clang::isDigit(prev))) {
      tokens.push_back({start, cidx - start});
      start = cidx;


    } else if (isLowercase(current) && isUppercase(prev) && start != cidx - 1) {
      // Or is this the end of a run of uppercase characters?
      // E.g. in NSWindow, the 'W' should start a new token.
      tokens.push_back({start, cidx - start - 1});
      characterToTokenIndex[cidx - 1] = tokens.size();
      start = cidx - 1;
    }
    characterToTokenIndex[cidx] = tokens.size();
  }

  tokens.push_back({start, static_cast<unsigned>(candidate.size() - start)});
}

static constexpr unsigned notFound = ~0U;

namespace {
/// The candidate-specific matching data and algorithms.
struct CandidateSpecificMatcher {
  // The following StringRefs are owned by FuzzyStringMatcher and must outlive
  // this object.
  StringRef pattern;
  StringRef lowercasePattern;
  StringRef candidate;
  SmallVector<char, 128> lowercaseCandidate;
  SmallVector<unsigned, 128> jumpTable; ///< The next matching character index.
  SmallVector<Range, 128> tokens; ///< Tokenized ranges from the candidate.
  SmallVector<unsigned, 128> characterToTokenIndex;
  SmallVector<Range, 128> runs;

  CandidateSpecificMatcher(StringRef pattern, StringRef lowercasePattern,
                           StringRef candidate,
                           const llvm::BitVector &charactersInPattern,
                           unsigned &firstPatternPos);

  /// Calculates the candidate's score, matching the candidate from
  /// \p firstPatternPos or later.
  ///
  /// This drives scoreCandidateTrial by trying the possible matches.
  double scoreCandidate(unsigned firstPatternPos);

  /// Calculates the candidate's score, matching the candidate from
  /// exactly \p firstPatternPos.
  double scoreCandidateTrial(unsigned firstPatternPos);
};
} // end anonymous namespace

double FuzzyStringMatcher::scoreCandidate(StringRef candidate) const {
  double finalScore = 0.0;
  if (candidate.empty() || pattern.empty() || candidate.size() < pattern.size())
    return finalScore;

  // Single character pattern matching should be simple and fast.  Just look at
  // the first character.
  if (pattern.size() == 1) {
    char c = candidate[0];
    if (c == pattern[0] && isUppercase(c)) {
      finalScore = 3.0;
    } else if (toLowercase(c) == lowercasePattern[0]) {
      finalScore = 2.0;
    }

    // Make sure shorter results come first;
    if (finalScore)
      finalScore += (1 / static_cast<double>(candidate.size())) * (1 / 1000.0);

    if (normalize)
      finalScore /= maxScore;

    return finalScore;
  }

  // FIXME: path separators would be handled here, jumping straight to the last
  // component if the pattern doesn't contain a separator.

  unsigned firstPatternPos = 0;
  CandidateSpecificMatcher CSM(pattern, lowercasePattern, candidate,
                               charactersInPattern, firstPatternPos);
  finalScore = CSM.scoreCandidate(firstPatternPos);

  if (normalize)
    finalScore /= maxScore;

  return finalScore;
}

CandidateSpecificMatcher::CandidateSpecificMatcher(
    StringRef pattern, StringRef lowercasePattern, StringRef candidate,
    const llvm::BitVector &charactersInPattern, unsigned &firstPatternPos)
    : pattern(pattern), lowercasePattern(lowercasePattern),
      candidate(candidate) {

  assert(!pattern.empty() && pattern.size() <= candidate.size());
  assert(pattern.size() == lowercasePattern.size());

  // Build a table that points at the next pattern character so we skip
  // through candidate faster.
  unsigned candidateLength = candidate.size();
  jumpTable.resize(candidateLength);
  lowercaseCandidate.resize(candidateLength);
  unsigned lastPatternPos = notFound;
  for (unsigned cidx = candidateLength - 1;; --cidx) {
    char c = candidate[cidx];
    lowercaseCandidate[cidx] = toLowercase(c);
    jumpTable[cidx] = lastPatternPos;
    if (charactersInPattern[static_cast<unsigned char>(c)])
      lastPatternPos = cidx;

    if (!cidx)
      break;
  }
  firstPatternPos = lastPatternPos;

  // Build the token table.
  characterToTokenIndex.resize(candidate.size());
  populateTokenTable(tokens, characterToTokenIndex, candidate);
}

double CandidateSpecificMatcher::scoreCandidate(unsigned firstPatternPos) {
  double finalScore = 0.0;

  // The outer matching loop. We run multiple trials so that "a_b_c_abc"
  // matching "abc" is matched on the "abc" part instead of the "a_b_c" part.
  while (firstPatternPos != notFound) {
    // Quickly skip to the first character that matches. We need
    // the loop in case the first pattern-character in the
    // candidate is not the first character in the pattern.
    while (firstPatternPos != notFound) {
      if (lowercasePattern[0] == lowercaseCandidate[firstPatternPos])
        break;
      firstPatternPos = jumpTable[firstPatternPos];
    }
    if (firstPatternPos == notFound)
      break;

    double trialScore = scoreCandidateTrial(firstPatternPos);

    if (trialScore > finalScore) {
      finalScore = trialScore;
      // FIXME: update output ranges, if necessary
    }

    firstPatternPos = jumpTable[firstPatternPos];
  }

  return finalScore;
}

static double scoreRun(unsigned runStart, unsigned runLength,
                       unsigned prevTokenStart, unsigned tokenIndex,
                       unsigned uppercaseMatches, bool isTokenizingChar) {
  if (runLength == 0)
    return 0.0;

  // We really don't like not matching at token starts, but if it's a long match
  // give some credit.
  if (runStart != prevTokenStart && !isTokenizingChar) {
    if (runLength < 5) {
      return (runLength < 3) ? 0.0 : runLength;
    }

    // For really long matches, we'll give a high score.  Pretend it's a bit
    // shorter.
    runLength -= 2;
  }

  // Bonus if the match is the first or second token.
  double prefixBonus = (runStart == 0) ? 2.5 : ((tokenIndex < 2) ? 1.0 : 0.0);
  double uppercaseBonus =
      uppercaseMatches ? (uppercaseMatches + 1) * (uppercaseMatches + 1) : 0.0;

  return (runLength * runLength) + uppercaseBonus + prefixBonus;
}

double
CandidateSpecificMatcher::scoreCandidateTrial(unsigned firstPatternPos) {
  double trialScore = 0.0; /// We run multiple trials so that "a_b_c_abc"
                           /// matching "abc" is matched on the "abc" part
                           /// instead of the "a_b_c" part.
  unsigned uppercaseMatches = 0;
  unsigned cidx = firstPatternPos;
  unsigned pidx = 0;
  unsigned runLength = 0;
  unsigned runStart = cidx;
  unsigned nonTokenRuns = 0;
  unsigned camelCaseLen = 0;
  unsigned camelCaseLastToken = 0;
  double camelCaseStartBonus = 0.0;
  unsigned camelCaseSkips = 0;

  unsigned patternLength = pattern.size();
  unsigned candidateLength = candidate.size();

  while (pidx < patternLength && cidx < candidateLength) {
    char lowerPatternChar = lowercasePattern[pidx];
    char lowerCandidateChar = lowercaseCandidate[cidx];
    unsigned nextCidx = jumpTable[cidx];
    bool matched = lowerPatternChar == lowerCandidateChar;
    if (matched) {
      if (isUppercase(pattern[pidx]) && isUppercase(candidate[cidx])) {
        ++uppercaseMatches;
      }

      ++runLength;
      ++pidx;
      if (pidx < patternLength)
        lowerPatternChar = lowercasePattern[pidx];
    }

    // If we're skipping forward and were running, the run ended.
    if (((cidx + 1) != nextCidx) || !matched) {
      if (runLength) {
        double runValue =
            scoreRun(runStart, runLength,
                     tokens[characterToTokenIndex[runStart]].location,
                     characterToTokenIndex[runStart], uppercaseMatches,
                     isTokenizingChar(candidate[runStart]));

        // If it's a poor match in the middle of a token, see if the next char
        // starts a token and also matches. If so, use it.
        if (runLength == 1 && pidx > 1 && runValue == 0.0 &&
            nextCidx != notFound &&
            characterToTokenIndex[runStart] < tokens.size() - 1) {
          bool foundIt = false;
          unsigned charToCheck = matched ? nextCidx : cidx;
          while (charToCheck != notFound) {
            if (tokens[characterToTokenIndex[charToCheck]].location ==
                    charToCheck &&
                lowercasePattern[pidx - 1] == lowercaseCandidate[charToCheck]) {
              foundIt = true;
              break;
            }
            charToCheck = jumpTable[charToCheck];
          }

          if (foundIt) {
            --pidx;
            lowerPatternChar = lowercasePattern[pidx];
            runStart = cidx = charToCheck;
            runLength = 0;
            continue;
          }
        }

        // We really don't like matches that don't start at a token.
        if (runValue == 0.0) {
          ++nonTokenRuns;

        } else {
          unsigned tokenIndex = characterToTokenIndex[runStart];
          if (runStart == tokens[tokenIndex].location ||
              isTokenizingChar(lowerCandidateChar)) {
            camelCaseLen += runLength;

            // Bonus for matching the beginning of the candidate.
            if (tokenIndex <= 1) {
              camelCaseStartBonus = 2.0;

              // Penalty for skipping a token.
            } else if (tokenIndex != camelCaseLastToken + 1) {
              camelCaseSkips += tokenIndex - camelCaseLastToken - 1;
            }

            camelCaseLastToken = tokenIndex;

            if (isTokenizingChar(lowerCandidateChar) && runLength == 1) {
              --camelCaseLastToken;
            }
          }
        }

        // Accumulate run and reset for next run.
        trialScore += runValue;
        runs.push_back({runStart, runLength});
        uppercaseMatches = 0;
        runLength = 0;
      }

      runStart = nextCidx;
    }

    cidx = nextCidx;
  }

  // The trial is done, did we find a match?
  // FIXME: this can happen spuriously in foo => ufDownOb.
  if (pidx != patternLength)
    return 0.0;

  // Okay, we found a match.

  // FIXME: this code is largely duplicated with the previous block. There are
  // some subtle differences that can be seen if you try to remove this one and
  // check for pidx == patternLength for the other block.
  if (runLength) {
    double runValue = scoreRun(
        runStart, runLength, tokens[characterToTokenIndex[runStart]].location,
        characterToTokenIndex[runStart], uppercaseMatches,
        isTokenizingChar(candidate[runStart]));

    // If it's a poor match in the middle of a token, see if the next char
    // starts a token and also matches. If so, use it.
    if (runLength == 1 && runValue == 0.0) {
      unsigned nextCidx = jumpTable[runStart];
      if (nextCidx != notFound &&
          characterToTokenIndex[runStart] < tokens.size() - 1) {
        bool foundIt = false;
        while (nextCidx != notFound) {
          if (tokens[characterToTokenIndex[nextCidx]].location == nextCidx &&
              lowercasePattern[pidx - 1] == lowercaseCandidate[nextCidx]) {
            foundIt = true;
            break;
          }
          nextCidx = jumpTable[nextCidx];
        }

        if (foundIt) {
          runStart = nextCidx;
          uppercaseMatches +=
              (isUppercase(pattern[pidx - 1]) &&
               isUppercase(candidate[runStart])) ? 1 : 0;
          runValue = scoreRun(runStart, runLength,
                              tokens[characterToTokenIndex[runStart]].location,
                              characterToTokenIndex[runStart], uppercaseMatches,
                              isTokenizingChar(candidate[runStart]));
        }
      }
    }

    // We really don't like matches that don't start at a token.
    if (runValue == 0.0) {
      ++nonTokenRuns;

    } else {
      unsigned tokenIndex = characterToTokenIndex[runStart];
      if (runStart == tokens[tokenIndex].location ||
          isTokenizingChar(lowercaseCandidate[runStart])) {
        camelCaseLen += runLength;

        if (tokenIndex <= 1) {
          // Bonus for matching the beginning of the candidate.
          camelCaseStartBonus = 2.0;
        } else if (tokenIndex != camelCaseLastToken + 1) {
          // Penalty for skipping a token.
          camelCaseSkips += tokenIndex - camelCaseLastToken - 1;
        }
      }
    }

    // Accumulate run.
    trialScore += runValue;
    runs.push_back({runStart, runLength});
  }

  // Unless there were bad matches, prefer camel case matches.
  if (nonTokenRuns == 0 && camelCaseSkips < 3) {
    double camelCaseScore = (camelCaseLen * camelCaseLen) + camelCaseStartBonus;
    if (camelCaseSkips == 1) {
      camelCaseScore *= 0.9;
    } else if (camelCaseSkips == 2) {
      camelCaseScore *= 0.8;
    }

    if (trialScore < camelCaseScore) {
      // Camel case matched better.
      trialScore = camelCaseScore;
    }
  }

  // FIXME: using the range up to a dot is silly when candidate isn't a file.
  auto dotLoc = candidate.find_last_of('.');
  unsigned baseNameLength =
      dotLoc != StringRef::npos && dotLoc > 1 ? dotLoc : candidateLength;

  // FIXME: file type bonus if we're checking a file path.

  // Add a bit for the percentage of the candidate matched.
  trialScore += patternLength / static_cast<double>(baseNameLength) * 0.25;

  // Exact matches are even better.
  if (patternLength >= baseNameLength && !runs.empty() &&
      runs[0].location == 0) {
    trialScore *= 1.1;
  }

  // Exact prefix matches are the best.
  if (!runs.empty() && runs[0].location == 0 && runs[0].length == patternLength) {
    trialScore *= 2.5;
    // Case sensitive exact prefix matches are the best of the best.
    if (candidate.starts_with(pattern))
      trialScore *= 1.2;
  }

  // FIXME: popular/unpopular API.

  // We really don't like matches that don't start at a token.
  switch (nonTokenRuns) {
  case 0:
    break;
  case 1:
    trialScore *= 0.8125;
    break;
  case 2:
    trialScore *= 0.5;
    break;
  case 3:
    trialScore *= 0.25;
    break;
  default:
    trialScore *= 0.0625;
    break;
  }

  // FIXME: matched ranges output.

  return trialScore;
}
