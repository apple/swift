//===--- StringExtras.cpp - String Utilities ------------------------------===//
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
//
// This file implements utilities for working with words and camelCase
// names.
//
//===----------------------------------------------------------------------===//

#include "swift/Basic/StringExtras.h"
#include "clang/Basic/CharInfo.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

using namespace swift;
using namespace camel_case;

using llvm::StringRef;

bool swift::canBeArgumentLabel(StringRef identifier) {
  return llvm::StringSwitch<bool>(identifier)
    .Case("inout", false)
    .Case("$", false)
    .Default(true);
}

bool swift::canBeMemberName(StringRef identifier) {
  return llvm::StringSwitch<bool>(identifier)
    .Case("init", false)
    .Case("Protocol", false)
    .Case("self", false)
    .Case("Type", false)
    .Default(true);
}

bool swift::isPreposition(StringRef word) {
#define PREPOSITION(Word)                                                      \
  if (word.equals_insensitive(#Word))                                          \
    return true;
#include "PartsOfSpeech.def"

  return false;
}

PartOfSpeech swift::getPartOfSpeech(StringRef word) {
  // FIXME: This implementation is woefully inefficient.
#define PREPOSITION(Word)                                                      \
  if (word.equals_insensitive(#Word))                                          \
    return PartOfSpeech::Preposition;
#define VERB(Word)                                                             \
  if (word.equals_insensitive(#Word))                                          \
    return PartOfSpeech::Verb;
#include "PartsOfSpeech.def"

  // Identify gerunds, which always end in "ing".
  if (word.endswith("ing") && word.size() > 4) {
    StringRef possibleVerb = word.drop_back(3);

    // If what remains is a verb, we have a gerund.
    if (getPartOfSpeech(possibleVerb) == PartOfSpeech::Verb)
      return PartOfSpeech::Gerund;

    // Try adding an "e" and look for that as a verb.
    if (possibleVerb.back() != 'e') {
      SmallString<16> possibleVerbWithE;
      possibleVerbWithE += possibleVerb;
      possibleVerbWithE += 'e';
      if (getPartOfSpeech(possibleVerbWithE) == PartOfSpeech::Verb)
        return PartOfSpeech::Gerund;
    }

    // If there is a repeated letter at the back, drop that second
    // instance of that letter and try again.
    unsigned count = possibleVerb.size();
    if (possibleVerb[count-1] == possibleVerb[count-2] &&
        getPartOfSpeech(possibleVerb.drop_back()) == PartOfSpeech::Verb)
      return PartOfSpeech::Gerund;
  }

  // "auto" tends to be used as a verb prefix.
  if (startsWithIgnoreFirstCase(word, "auto") && word.size() > 4) {
    if (getPartOfSpeech(word.substr(4)) == PartOfSpeech::Verb)
      return PartOfSpeech::Verb;
  }

  // "re" can prefix a verb.
  if (startsWithIgnoreFirstCase(word, "re") && word.size() > 2) {
    if (getPartOfSpeech(word.substr(2)) == PartOfSpeech::Verb)
      return PartOfSpeech::Verb;
  }

  // "de" can prefix a verb.
  if (startsWithIgnoreFirstCase(word, "de") && word.size() > 2) {
    if (getPartOfSpeech(word.substr(2)) == PartOfSpeech::Verb)
      return PartOfSpeech::Verb;
  }

  return PartOfSpeech::Unknown;
}

/// Whether the given word is a plural s
static bool isPluralSuffix(StringRef word) {
  return word == "s" || word == "es" || word == "ies";
}

void WordIterator::computeNextPosition() const {
  assert(Position < String.size() && "Already at end of string");

  unsigned i = Position, n = String.size();

  // Treat _ as a word on its own. Don't coalesce.
  if (String[i] == '_') {
    NextPosition = i + 1;
    NextPositionValid = true;
    return;
  }

  // Skip over any uppercase letters at the beginning of the word.
  while (i < n && clang::isUppercase(String[i]))
    ++i;

  // If there was more than one uppercase letter, this is an
  // acronym.
  if (i - Position > 1) {
    // If we hit the end of the string, that's it. Otherwise, this
    // word ends before the last uppercase letter if the next word is alphabetic
    // (URL_Loader) or after the last uppercase letter if it's not (UTF_8).

    // Collect the lowercase letters up to the next word.
    unsigned endOfNext = i;
    while (endOfNext < n && clang::isLowercase(String[endOfNext]))
      ++endOfNext;

    // If the next word is a plural suffix, add it on.
    if (i == n || 
        (isPluralSuffix(String.slice(i, endOfNext)) &&
         String.slice(i-1, endOfNext) != "Is"))
      NextPosition = endOfNext;
    else if (clang::isLowercase(String[i]))
      NextPosition = i-1;
    else
      NextPosition = i;

    NextPositionValid = true;
    return;
  }

  // Skip non-uppercase letters.
  while (i < n && !clang::isUppercase(String[i]) && String[i] != '_')
    ++i;

  NextPosition = i;
  NextPositionValid = true;
}

void WordIterator::computePrevPosition() const {
  assert(Position > 0 && "Already at beginning of string");

  unsigned i = Position;

  // While we see non-uppercase letters, keep moving back.
  while (i > 0 && !clang::isUppercase(String[i-1]) && String[i-1] != '_')
    --i;

  // If what we found is a plural suffix, keep going.
  unsigned effectiveEndPosition = Position;
  if (i > 0 && isPluralSuffix(String.slice(i, Position))) {
    effectiveEndPosition = i;
    while (i > 0 && !clang::isUppercase(String[i-1]) && String[i-1] != '_')
      --i;
  }

  // If we found any lowercase letters, this was a normal camel case
  // word (not an acronym).
  if (i < effectiveEndPosition) {
    // If we hit the beginning of the string, that's it. Otherwise, this
    // word starts with an uppercase letter if the next word is alphabetic
    // (URL_Loader) or after the last uppercase letter if it's not (UTF_8).
    PrevPosition = i;
    if (i != 0 && clang::isLowercase(String[i]) && String[i-1] != '_')
      --PrevPosition;
    PrevPositionValid = true;
    return;
  }

  // Treat _ as a word on its own. Don't coalesce.
  if (String[i-1] == '_') {
    PrevPosition = i - 1;
    PrevPositionValid = true;
    return;
  }

  // There were no lowercase letters, so this is an acronym. Keep
  // skipping uppercase letters.
  while (i > 0 && clang::isUppercase(String[i-1]))
    --i;

  PrevPosition = i;
  PrevPositionValid = true;
}

StringRef camel_case::getFirstWord(StringRef string) {
  if (string.empty())
    return "";

  return *WordIterator(string, 0);
}

StringRef camel_case::getLastWord(StringRef string) {
  if (string.empty())
    return "";

  return *--WordIterator(string, string.size());
}

bool camel_case::sameWordIgnoreFirstCase(StringRef word1, StringRef word2) {
  if (word1.size() != word2.size())
    return false;

  if (clang::toLowercase(word1[0]) != clang::toLowercase(word2[0]))
    return false;

  return word1.substr(1) == word2.substr(1);
}

bool camel_case::startsWithIgnoreFirstCase(StringRef word1, StringRef word2) {
  if (word1.size() < word2.size())
    return false;

  if (clang::toLowercase(word1[0]) != clang::toLowercase(word2[0]))
    return false;

  return word1.substr(1, word2.size() - 1) == word2.substr(1);
}

StringRef camel_case::toLowercaseWord(StringRef string,
                                      SmallVectorImpl<char> &scratch) {
  if (string.empty())
    return string;

  // Already lowercase.
  if (!clang::isUppercase(string[0]))
    return string;

  // Acronym doesn't get lowercased.
  if (string.size() > 1 && clang::isUppercase(string[1]))
    return string;

  // Lowercase the first letter, append the rest.
  scratch.clear();
  scratch.push_back(clang::toLowercase(string[0]));
  scratch.append(string.begin() + 1, string.end());
  return StringRef(scratch.data(), scratch.size());
}

StringRef camel_case::toSentencecase(StringRef string, 
                                     SmallVectorImpl<char> &scratch) {
  if (string.empty())
    return string;

  // Can't be uppercased.
  if (!clang::isLowercase(string[0]))
    return string;

  // Uppercase the first letter, append the rest.
  scratch.clear();
  scratch.push_back(clang::toUppercase(string[0]));
  scratch.append(string.begin() + 1, string.end());
  return StringRef(scratch.data(), scratch.size());  
}

StringRef camel_case::dropPrefix(StringRef string) {

  unsigned firstLower = 0, n = string.size();

  if (n < 4)
    return string;

  for (; firstLower < n; ++firstLower) {
    if (!clang::isUppercase(string[firstLower]))
      break;
  }

  if (firstLower == n)
    return string;

  if (firstLower >= 3 && firstLower <= 4)
    return string.substr(firstLower - 1);

  return string;
}

StringRef camel_case::appendSentenceCase(SmallVectorImpl<char> &buffer,
                                         StringRef string) {
  // Trivial case: empty string.
  if (string.empty())
    return StringRef(buffer.data(), buffer.size());

  // Uppercase the first letter, append the rest.
  buffer.push_back(clang::toUppercase(string[0]));
  buffer.append(string.begin() + 1, string.end());
  return StringRef(buffer.data(), buffer.size());
}

size_t camel_case::findWord(StringRef string, StringRef word) {
  assert(!word.empty());
  assert(clang::isUppercase(word[0]));

  // Scan forward until we find the word as a complete word.
  size_t startingIndex = 0;
  while (true) {
    size_t index = string.find(word, startingIndex);
    if (index == StringRef::npos)
      return StringRef::npos;

    // If any of the following checks fail, we want to start searching
    // past the end of the match.  (This assumes that the word doesn't
    // end with a prefix of itself, e.g. "LikableLike".)
    startingIndex = index + word.size();

    // We assume that we don't have to check if the match starts a new
    // word in the string.

    // If we find the word, check whether it's a valid match.
    StringRef suffix = string.substr(index);
    if (!suffix.empty() && clang::isLowercase(suffix[0]))
      continue;

    return index;
  }
}

/// Skip a type suffix that can be dropped.
static llvm::Optional<StringRef> skipTypeSuffix(StringRef typeName) {
  if (typeName.empty())
    return llvm::None;

  auto lastWord = camel_case::getLastWord(typeName);

  // "Type" suffix.
  if (lastWord == "Type" && typeName.size() > 4) {
    return typeName.drop_back(4);
  }

  // "Ref" suffix.
  if (lastWord == "Ref" && typeName.size() > 3) {
    return typeName.drop_back(3);
  }

  // "Mask" suffix.
  if (lastWord == "Mask" && typeName.size() > 4) {
    return typeName.drop_back(4);
  }

  // \d+D for dimensionality.
  if (typeName.back() == 'D' && typeName.size() > 1) {
    unsigned firstDigit = typeName.size() - 1;
    while (firstDigit > 0) {
      if (!isdigit(typeName[firstDigit-1])) break;
      --firstDigit;
    }

    if (firstDigit < typeName.size()-1) {
      return typeName.substr(0, firstDigit);
    }
  }

  // _t.
  if (typeName.size() > 2 && typeName.endswith("_t")) {
    return typeName.drop_back(2);
  }
  return llvm::None;
}

/// Match a word within a name to a word within a type.
static bool matchNameWordToTypeWord(StringRef nameWord, StringRef typeWord) {
  // If the name word is longer, there's no match.
  if (nameWord.size() > typeWord.size()) return false;

  // If the name word is shorter, try for a partial match.
  if (nameWord.size() < typeWord.size()) {
    // We can match the suffix of the type so long as everything preceding the
    // match is neither a lowercase letter nor a '_'. This ignores type
    // prefixes for acronyms, e.g., the 'NS' in 'NSURL'.
    if (typeWord.endswith_insensitive(nameWord) &&
        !clang::isLowercase(typeWord[typeWord.size() - nameWord.size()])) {
      // Check that everything preceding the match is neither a lowercase letter
      // nor a '_'.
      for (unsigned i = 0, n = nameWord.size(); i != n; ++i) {
        if (clang::isLowercase(typeWord[i]) || typeWord[i] == '_') return false;
      }

      return true;
    }

    // We can match a prefix so long as everything following the match is
    // a number.
    if (typeWord.startswith_insensitive(nameWord)) {
      for (unsigned i = nameWord.size(), n = typeWord.size(); i != n; ++i) {
        if (!clang::isDigit(typeWord[i])) return false;
      }

      return true;
    }

    return false;
  }

  // Check for an exact match.
  return nameWord.equals_insensitive(typeWord);
}

/// Match the beginning of the name to the given type name.
StringRef swift::matchLeadingTypeName(StringRef name,
                                      OmissionTypeName typeName) {
  // Match the camelCase beginning of the name to the
  // ending of the type name.
  auto nameWords = camel_case::getWords(name);
  auto typeWords = camel_case::getWords(typeName.Name);
  auto nameWordIter = nameWords.begin();
  auto typeWordRevIter = typeWords.rbegin();

  // Find the last instance of the first word in the name within
  // the words in the type name.
  typeWordRevIter = std::find_if(
      typeWordRevIter, typeWords.rend(), [nameWordIter](StringRef word) {
        return matchNameWordToTypeWord(*nameWordIter, word);
      });

  // If we didn't find the first word in the name at all, we're
  // done.
  if (typeWordRevIter == typeWords.rend())
    return name;

  // Now, match from the first word up until the end of the type name.
  std::advance(nameWordIter, 1);
  WordIterator typeMismatch = typeWords.end(), nameMismatch = nameWords.end();
  std::tie(typeMismatch, nameMismatch) =
      std::mismatch(typeWordRevIter.base(), typeWords.end(), nameWordIter,
                    nameWords.end(), matchNameWordToTypeWord);

  // If we didn't reach the end of the type name, don't match.
  if (typeMismatch != typeWords.end())
    return name;

  // Chop of the beginning of the name.
  return nameMismatch.getRestOfStr();
}

const char *swift::copyCString(StringRef string,
                               llvm::BumpPtrAllocator &Allocator) {
  if (string.empty())
    return "";
  char *memory = Allocator.Allocate<char>(string.size() + 1);
  memcpy(memory, string.data(), string.size());
  memory[string.size()] = '\0';
  return memory;
}

void InheritedNameSet::add(StringRef name) {
  Names.insert(name);
}

bool InheritedNameSet::contains(StringRef name) const {
  auto set = this;
  do {
    if (set->Names.count(name) > 0) return true;
    set = set->Parent;
  } while (set);

  return false;
}

/// Wrapper for camel_case::toLowercaseWord that uses string scratch space.
StringRef camel_case::toLowercaseWord(StringRef string,
                                      StringScratchSpace &scratch){
  llvm::SmallString<32> scratchStr;
  StringRef result = toLowercaseWord(string, scratchStr);
  if (string == result)
    return string;

  return scratch.copyString(result);
}

/// Omit needless words from the beginning of a name.
static StringRef omitNeedlessWordsFromPrefix(StringRef name,
                                             OmissionTypeName type) {
  if (type.empty())
    return name;

  // Match the result type to the beginning of the name.
  StringRef newName = matchLeadingTypeName(name, type);
  if (newName == name)
    return name;

  auto firstWord = camel_case::getFirstWord(newName);

  // If we have a preposition, we can chop off type information at the
  // beginning of the name.
  if (getPartOfSpeech(firstWord) == PartOfSpeech::Preposition &&
      newName.size() > firstWord.size()) {
    // If the preposition was "by" and is followed by a gerund, also remove
    // "by".
    if (firstWord == "By") {
      StringRef nextWord = camel_case::getFirstWord(
                             newName.substr(firstWord.size()));
      if (nextWord.endswith("ing")) {
        return newName.substr(firstWord.size());
      }
    }

    return newName;
  }

  return name;
}

/// Identify certain vacuous names to which we do not want to reduce any name.
static bool isVacuousName(StringRef name) {
  return camel_case::sameWordIgnoreFirstCase(name, "get") ||
         camel_case::sameWordIgnoreFirstCase(name, "for") ||
         camel_case::sameWordIgnoreFirstCase(name, "set") ||
         camel_case::sameWordIgnoreFirstCase(name, "using") ||
         camel_case::sameWordIgnoreFirstCase(name, "with");
}

/// Determine whether the given text matches a property name.
static bool textMatchesPropertyName(StringRef text,
                                    const InheritedNameSet *allPropertyNames) {
  if (!allPropertyNames) return false;

  SmallString<16> localScratch;
  auto name = camel_case::toLowercaseWord(text, localScratch);

  // A property with exactly this name.
  if (allPropertyNames->contains(name)) return true;

  // From here on, we'll be working with scratch space.
  if (name.data() != localScratch.data())
    localScratch = name;

  if (localScratch.back() == 'y') {
    // If the last letter is a 'y', try 'ies'.
    localScratch.pop_back();
    localScratch += "ies";
    if (allPropertyNames->contains(localScratch)) return true;
  } else {
    // Otherwise, add an 's' and try again.
    localScratch += 's';
    if (allPropertyNames->contains(localScratch)) return true;

    // Alternatively, try to add 'es'.
    localScratch.pop_back();
    localScratch += "es";
    if (allPropertyNames->contains(localScratch)) return true;
  }

  return false;
}

namespace {
/// Describes the role that a particular name has within a
/// signature, which can affect how we omit needless words.
enum class NameRole {
  /// The base name of a function or method.
  BaseName,

  /// The first parameter of a function or method.
  FirstParameter,

  // Subsequent parameters in a function or method.
  SubsequentParameter,

  // The name of a property.
  Property,

  // A partial name; used internally.
  Partial,
};
} // end anonymous namespace

static StringRef
omitTrailingTypeNameWithSpecialCases(StringRef name,
                                     OmissionTypeName typeName,
                                     NameRole role,
                                     const InheritedNameSet *allPropertyNames);

/// Returns the iterator pointing to the first word in \p name that starts the
/// match for \p typeName (anchored at the end of \p name).
///
/// If there is no match, returns the end WordIterator for \p name.
static Words::iterator matchTypeNameFromBackWithSpecialCases(
    StringRef name, OmissionTypeName typeName,
    const InheritedNameSet *allPropertyNames) {
  // Get the camel-case words in the name and type name.
  auto nameWords = camel_case::getWords(name);
  auto typeWords = camel_case::getWords(typeName.Name);

  // Match the last words in the type name to the last words in the
  // name.
  auto nameWordRevIter = nameWords.rbegin(),
    nameWordRevIterBegin = nameWordRevIter,
    nameWordRevIterEnd = nameWords.rend();
  auto typeWordRevIter = typeWords.rbegin(),
    typeWordRevIterEnd = typeWords.rend();

  while (nameWordRevIter != nameWordRevIterEnd &&
         typeWordRevIter != typeWordRevIterEnd) {
    // If the names match, continue.
    auto nameWord = *nameWordRevIter;
    if (matchNameWordToTypeWord(nameWord, *typeWordRevIter)) {
      ++nameWordRevIter;
      ++typeWordRevIter;
      continue;
    }

    // Special case: "Indexes" and "Indices" in the name match
    // "IndexSet" in the type.
    if ((matchNameWordToTypeWord(nameWord, "Indexes") ||
         matchNameWordToTypeWord(nameWord, "Indices")) &&
        *typeWordRevIter == "Set") {
      auto nextTypeWordRevIter = typeWordRevIter;
      ++nextTypeWordRevIter;
      if (nextTypeWordRevIter != typeWordRevIterEnd &&
          matchNameWordToTypeWord("Index", *nextTypeWordRevIter)) {
        ++nameWordRevIter;
        typeWordRevIter = nextTypeWordRevIter;
        ++typeWordRevIter;
        continue;
      }
    }

    // Special case: "Index" in the name matches "Int" or "Integer" in the type.
    if (matchNameWordToTypeWord(nameWord, "Index") &&
        (matchNameWordToTypeWord("Int", *typeWordRevIter) ||
         matchNameWordToTypeWord("Integer", *typeWordRevIter))) {
      ++nameWordRevIter;
      ++typeWordRevIter;
      continue;
    }

    // Special case: "ObjectValue" in the name matches "Object" in the
    // type.
    if (matchNameWordToTypeWord("Object", *typeWordRevIter) &&
        matchNameWordToTypeWord(nameWord, "Value")) {
      auto nextNameWordRevIter = std::next(nameWordRevIter);
      if (nextNameWordRevIter != nameWordRevIterEnd &&
          matchNameWordToTypeWord(*nextNameWordRevIter, "Object")) {
        nameWordRevIter = nextNameWordRevIter;
        ++nameWordRevIter;
        ++typeWordRevIter;
        continue;
      }
    }

    // Special case: if the word in the name ends in 's', and we have
    // a collection element type, see if this is a plural.
    if (!typeName.CollectionElement.empty() && nameWord.size() > 2 &&
        nameWord.back() == 's') {
      // Check <element name>s.
      auto shortenedNameWord = nameWordRevIter.base().getPriorStr().drop_back();
      auto newShortenedNameWord = omitTrailingTypeNameWithSpecialCases(
          shortenedNameWord, typeName.CollectionElement, NameRole::Partial,
          allPropertyNames);

      if (shortenedNameWord != newShortenedNameWord) {
        unsigned targetSize = newShortenedNameWord.size();
        auto newIter = std::make_reverse_iterator(WordIterator(name,
                                                               targetSize));
#ifndef NDEBUG
        while (nameWordRevIter.base().getPosition() > targetSize)
          ++nameWordRevIter;
        assert(nameWordRevIter == newIter);
#else
        nameWordRevIter = newIter;
#endif
        continue;
      }
    }

    // If this is a skippable suffix, skip it and keep looking.
    if (nameWordRevIter == nameWordRevIterBegin) {
      if (auto withoutSuffix = skipTypeSuffix(typeName.Name)) {
        typeName.Name = *withoutSuffix;
        typeWords = camel_case::getWords(typeName.Name);
        typeWordRevIter = typeWords.rbegin();
        typeWordRevIterEnd = typeWords.rend();
        continue;
      }
    }

    break;
  }

  return nameWordRevIter.base();
}

static StringRef
omitSelfTypeFromBaseName(StringRef name, OmissionTypeName typeName,
                         const InheritedNameSet *allPropertyNames,
                         StringScratchSpace &scratch) {
  // If we have no name or no type name, there is nothing to do.
  if (name.empty() || typeName.empty()) return name;

  // Deliberately drop the collection element from typeName.
  typeName.CollectionElement = StringRef();

  auto nameWords = camel_case::getWords(name);
  llvm::Optional<llvm::iterator_range<WordIterator>> matchingRange;

  // Search backwards for the type name, whether anchored at the end or not.
  for (auto nameReverseIter = nameWords.rbegin();
       nameReverseIter != nameWords.rend();
       ++nameReverseIter) {
    StringRef matchName = nameReverseIter.base().getPriorStr();
    auto matchIter = matchTypeNameFromBackWithSpecialCases(matchName, typeName,
                                                           allPropertyNames);
    auto matchIterInFullName = WordIterator(name, matchIter.getPosition());
    if (matchIterInFullName != nameReverseIter.base()) {
      matchingRange = llvm::make_range(matchIterInFullName,
                                       nameReverseIter.base());
      break;
    }

    // Note: This behavior fell out of a previous implementation of
    // omit-needless-words, even though it probably wasn't intentional. At this
    // point, though, it could be source-breaking to change it.
    while (auto withoutSuffix = skipTypeSuffix(typeName.Name))
      typeName.Name = *withoutSuffix;
  }

  // If we matched nothing, or if the type name was all the way at the start
  // of the base name, don't strip anything.
  if (!matchingRange || matchingRange->begin() == nameWords.begin())
    return name;
  assert(matchingRange->begin() != matchingRange->end() &&
         "should not have been considered a match");

  // Don't strip just "Error" at the end of a base name.
  // FIXME: Is this still relevant?
  if (matchingRange->end() == nameWords.end() &&
      std::next(matchingRange->begin()) == nameWords.end() &&
      *matchingRange->begin() == "Error") {
    return name;
  }

  // Only strip a type name that follows a verb.
  switch (getPartOfSpeech(*std::prev(matchingRange->begin()))) {
  case PartOfSpeech::Verb:
    break;
  case PartOfSpeech::Preposition:
  case PartOfSpeech::Gerund:
  case PartOfSpeech::Unknown:
    return name;
  }

  // Splice together the parts before and after the matched
  // type. For example, if we matched "ViewController" in
  // "dismissViewControllerAnimated", stitch together
  // "dismissAnimated".

  // Don't prune redundant type information from the base name if
  // there is a corresponding property (either singular or plural).
  StringRef removedText = name.substr(matchingRange->begin().getPosition(),
                                      matchingRange->end().getPosition());
  if (textMatchesPropertyName(removedText, allPropertyNames))
    return name;

  SmallString<16> newName = matchingRange->begin().getPriorStr();
  newName += matchingRange->end().getRestOfStr();

  // If we ended up with something that can't be a member name, do nothing.
  if (!canBeMemberName(newName))
    return name;

  // If we ended up with a vacuous name like "get" or "set", do nothing.
  if (isVacuousName(newName))
    return name;

  // We're done.
  return scratch.copyString(newName);
}

static StringRef
omitTrailingTypeNameWithSpecialCases(StringRef name, OmissionTypeName typeName,
                                     NameRole role,
                                     const InheritedNameSet *allPropertyNames) {
  // If we have no name or no type name, there is nothing to do.
  if (name.empty() || typeName.empty()) return name;

  auto nameWords = camel_case::getWords(name);
  Words::iterator matchIter =
      matchTypeNameFromBackWithSpecialCases(name, typeName, allPropertyNames);

  if (matchIter == nameWords.end())
    return name;

  // Handle complete name matches.
  if (matchIter == nameWords.begin()) {
    // If we're doing a partial match or we have an initial
    // parameter, return the empty string.
    if (role == NameRole::Partial || role == NameRole::FirstParameter)
      return "";

    // Leave the name alone.
    return name;
  }

  // Don't strip just "Error".
  if (std::next(matchIter) == nameWords.end() && *matchIter == "Error")
    return name;

  switch (role) {
  case NameRole::Property:
    // Always strip off type information.
    break;

  case NameRole::FirstParameter:
  case NameRole::Partial:
  case NameRole::SubsequentParameter: {
    // Classify the part of speech of the word before the type information we
    // would strip off. We only want to strip it if the previous word is a
    // preposition, verb, or gerund.
    auto previousWordIter = std::prev(matchIter);
    if (getPartOfSpeech(*previousWordIter) == PartOfSpeech::Unknown)
      return name;
    break;
  }

  case NameRole::BaseName: {
    // Classify the part of speech of the word before the type
    // information we would strip off.
    auto previousWordIter = std::prev(matchIter);
    switch (getPartOfSpeech(*previousWordIter)) {
    case PartOfSpeech::Preposition:
      // If there's nothing preceding the preposition, don't strip anything.
      if (previousWordIter == nameWords.begin())
        return name;
      break;

    case PartOfSpeech::Verb:
    case PartOfSpeech::Gerund:
      // Don't prune redundant type information from the base name if
      // there is a corresponding property (either singular or plural).
      if (textMatchesPropertyName(matchIter.getRestOfStr(), allPropertyNames))
        return name;
      break;

    case PartOfSpeech::Unknown:
      // Assume it's a noun or adjective; don't strip anything.
      return name;
    }

    break;
  }
  }

  // Strip off the part of the name that is redundant with
  // type information.
  StringRef newName = matchIter.getPriorStr();

  switch (role) {
  case NameRole::BaseName:
  case NameRole::Property:
    // If we ended up with something that can't be a member name, do nothing.
    if (!canBeMemberName(newName))
      return name;

    // If we ended up with a vacuous name like "get" or "set", do nothing.
    if (isVacuousName(newName))
      return name;

    break;

  case NameRole::SubsequentParameter:
  case NameRole::FirstParameter:
  case NameRole::Partial:
    break;
  }

  // We're done.
  return newName;
}

StringRef camel_case::toLowercaseInitialisms(StringRef string,
                                             StringScratchSpace &scratch) {
  llvm::SmallString<32> scratchStr;
  StringRef result = toLowercaseInitialisms(string, scratchStr);
  if (string == result)
    return string;
  return scratch.copyString(result);
}

StringRef
camel_case::toLowercaseInitialisms(StringRef string,
                                   SmallVectorImpl<char> &scratch) {

  if (string.empty())
    return string;

  // Already lowercase.
  if (!clang::isUppercase(string[0]))
    return string;

  // Lowercase until we hit the an uppercase letter followed by a
  // non-uppercase letter.
  scratch.clear();
  scratch.reserve(string.size());
  for (unsigned i = 0, n = string.size(); i != n; ++i) {
    // If the next character is not uppercase, stop.
    if (i < n - 1 && !clang::isUppercase(string[i+1])) {
      // If the next non-uppercase character was not a letter, we seem
      // to have a plural, or we're at the beginning, we should still
      // lowercase the character we're on.
      if (i == 0 || !clang::isLetter(string[i+1]) ||
          isPluralSuffix(camel_case::getFirstWord(string.substr(i+1)))) {
        scratch.push_back(clang::toLowercase(string[i]));
        ++i;
      }

      scratch.append(string.substr(i).begin(), string.substr(i).end());
      break;
    }

    scratch.push_back(clang::toLowercase(string[i]));
  }

  return {scratch.data(), scratch.size()};
}

/// Determine whether the given word occurring before the given
/// preposition results in a conflict that suppresses preposition
/// splitting.
static bool wordConflictsBeforePreposition(StringRef word,
                                           StringRef preposition) {
  if (camel_case::sameWordIgnoreFirstCase(preposition, "in") &&
      camel_case::sameWordIgnoreFirstCase(word, "plug"))
    return true;

  return false;
}

/// Determine whether the given word occurring after the given
/// preposition results in a conflict that suppresses preposition
/// splitting.
static bool wordConflictsAfterPreposition(StringRef word,
                                          StringRef preposition) {
  if (camel_case::sameWordIgnoreFirstCase(preposition, "with")) {
    if (camel_case::sameWordIgnoreFirstCase(word, "error") ||
        camel_case::sameWordIgnoreFirstCase(word, "no"))
      return true;
  }

  if (camel_case::sameWordIgnoreFirstCase(preposition, "to")) {
    if (camel_case::sameWordIgnoreFirstCase(word, "visible") ||
        camel_case::sameWordIgnoreFirstCase(word, "backing"))
      return true;
  }

  if (camel_case::sameWordIgnoreFirstCase(preposition, "from")) {
    if (camel_case::sameWordIgnoreFirstCase(word, "backing"))
      return true;
  }

  return false;
}

/// When splitting based on a preposition, whether we should place the
/// preposition on the argument label (vs. on the base name).
static bool shouldPlacePrepositionOnArgLabel(StringRef beforePreposition,
                                             StringRef preposition,
                                             StringRef afterPreposition) {
  // X/Y/Z often used as coordinates and should be the labels.
  if (afterPreposition == "X" ||
      afterPreposition == "Y" ||
      afterPreposition == "Z")
    return false;

  return true;
}

/// Determine whether the word preceding the preposition is part of an
/// "extended" preposition, such as "compatible with".
static bool priorWordExtendsPreposition(StringRef preceding,
                                        StringRef preposition) {
  // compatible with
  if (camel_case::sameWordIgnoreFirstCase(preceding, "compatible") &&
      camel_case::sameWordIgnoreFirstCase(preposition, "with"))
    return true;

  // best matching
  if (camel_case::sameWordIgnoreFirstCase(preceding, "best") &&
      camel_case::sameWordIgnoreFirstCase(preposition, "matching"))
    return true;

  // according to
  if (camel_case::sameWordIgnoreFirstCase(preceding, "according") &&
      camel_case::sameWordIgnoreFirstCase(preposition, "to"))
    return true;

  // bound by
  if (camel_case::sameWordIgnoreFirstCase(preceding, "bound") &&
      camel_case::sameWordIgnoreFirstCase(preposition, "by"))
    return true;

  // separated by
  if (camel_case::sameWordIgnoreFirstCase(preceding, "separated") &&
      camel_case::sameWordIgnoreFirstCase(preposition, "by"))
    return true;

  return false;
}

/// Determine whether the preposition in a split is "vacuous", and
/// should be removed.
static bool isVacuousPreposition(StringRef beforePreposition,
                                 StringRef preposition,
                                 StringRef afterPreposition,
                                 const OmissionTypeName &paramType) {
  // Only consider "with" or "using" to be potentially vacuous.
  if (!camel_case::sameWordIgnoreFirstCase(preposition, "with") &&
      !camel_case::sameWordIgnoreFirstCase(preposition, "using"))
    return false;

  // If the preposition is "with" followed by "zone", never consider
  // it vacuous.
  if (camel_case::sameWordIgnoreFirstCase(preposition, "with") &&
      camel_case::sameWordIgnoreFirstCase(
        camel_case::getFirstWord(afterPreposition), "zone"))
    return false;

  // If the parameter has a default argument, it's vacuous.
  if (paramType.hasDefaultArgument()) return true;

  // If the parameter is of function type, it's vacuous.
  if (paramType.isFunction()) return true;

  return false;
}

namespace {
using ReverseWordIterator = std::reverse_iterator<camel_case::WordIterator>;
} // end anonymous namespace

/// Find the last preposition in the given word.
static ReverseWordIterator findLastPreposition(ReverseWordIterator first,
                                               ReverseWordIterator last,
                                               bool recursive = false) {
  // Find the last preposition.
  auto result =
    std::find_if(first, last,
                 [](StringRef word) {
                   return getPartOfSpeech(word) == PartOfSpeech::Preposition;
                 });

  // If the preposition is "of", look for a previous preposition.
  if (!recursive && result != last &&
      camel_case::sameWordIgnoreFirstCase(*result, "of")) {
    auto prevPreposition = findLastPreposition(std::next(result), last,
                                               /*recursive=*/true);
    if (prevPreposition != last &&
        !camel_case::sameWordIgnoreFirstCase(*prevPreposition, "of") &&
        !camel_case::sameWordIgnoreFirstCase(*prevPreposition, "for"))
      return prevPreposition;
  }

  return result;
}

/// Split the base name after the last preposition, if there is one.
static bool splitBaseNameAfterLastPreposition(
    StringRef &baseName,
    StringRef &argName,
    const OmissionTypeName &paramType) {
  // Scan backwards for a preposition.
  auto nameWords = camel_case::getWords(baseName);
  auto nameWordRevIterBegin = nameWords.rbegin(),
    nameWordRevIterEnd = nameWords.rend();

  // Find the last preposition.
  auto nameWordRevIter = findLastPreposition(nameWordRevIterBegin,
                                             nameWordRevIterEnd);

  if (nameWordRevIter == nameWordRevIterEnd) return false;

  // We found a split point.
  auto preposition = *nameWordRevIter;

  // If we have a conflict with the word before the preposition, don't
  // split.
  if (std::next(nameWordRevIter) != nameWordRevIterEnd &&
      wordConflictsBeforePreposition(*std::next(nameWordRevIter), preposition))
    return false;

  // If we have a conflict with the word after the preposition, don't
  // split.
  if (nameWordRevIter != nameWordRevIterBegin &&
      wordConflictsAfterPreposition(*std::prev(nameWordRevIter), preposition))
    return false;

  // If the word preceding the preposition extends the preposition, it
  // will never be dropped.
  if (std::next(nameWordRevIter) != nameWordRevIterEnd &&
      priorWordExtendsPreposition(*std::next(nameWordRevIter), preposition)) {
    ++nameWordRevIter;
    preposition = StringRef((*nameWordRevIter).begin(),
                            preposition.size() + (*nameWordRevIter).size());
  }

  // Determine whether we should drop the preposition.
  StringRef beforePreposition(baseName.begin(),
                              preposition.begin() - baseName.begin());
  StringRef afterPreposition(preposition.end(),
                             baseName.end() - preposition.end());
  bool dropPreposition = isVacuousPreposition(beforePreposition,
                                              preposition,
                                              afterPreposition,
                                              paramType);

  // By default, put the preposition on the argument label.
  bool prepositionOnArgLabel =
    shouldPlacePrepositionOnArgLabel(beforePreposition, preposition,
                                     afterPreposition);
  if (prepositionOnArgLabel)
    ++nameWordRevIter;

  unsigned startOfArgumentLabel = nameWordRevIter.base().getPosition();
  unsigned endOfBaseName = startOfArgumentLabel;

  // If we're supposed to drop the preposition, do so.
  if (dropPreposition) {
    if (prepositionOnArgLabel)
      startOfArgumentLabel += preposition.size();
    else {
      endOfBaseName -= preposition.size();
    }
  }
  if (endOfBaseName == 0) return false;

  // If the base name is vacuous or is a keyword and there are two or
  // fewer words in the base name, don't split.
  auto newBaseName = baseName.substr(0, endOfBaseName);
  {
    auto newWords = camel_case::getWords(newBaseName);
    auto newWordsIter = newWords.begin();
    bool isKeyword = !canBeMemberName(*newWordsIter);
    bool isVacuous = isVacuousName(*newWordsIter);
    if (isKeyword || isVacuous) {
      // Just one word?
      ++newWordsIter;
      if (newWordsIter == newWords.end()) return false;

      // Or two words, if it's vacuous.
      ++newWordsIter;
      if (newWordsIter == newWords.end() && isVacuous) return false;

      // Okay: there is enough in the base name.
    }
  }

  // Update the argument label and base name.
  argName = baseName.substr(startOfArgumentLabel);
  baseName = newBaseName;

  return true;
}

/// Split the base name, if it makes sense.
static bool splitBaseName(StringRef &baseName, StringRef &argName,
                          const OmissionTypeName &paramType,
                          StringRef paramName) {
  // If there is already an argument label, do nothing.
  if (!argName.empty()) return false;

  // Try splitting a Boolean "Animated".
  if (paramType.isBoolean() &&
      camel_case::getLastWord(baseName) == "Animated") {
    baseName = baseName.drop_back(strlen("Animated"));
    argName = "animated";
    return true;
  }

  // Don't split anything that starts with "set".
  if (camel_case::getFirstWord(baseName) == "set")
    return false;

  // Don't split a method that looks like an action (with a "sender"
  // of type AnyObject).
  if (paramName == "sender" &&
      camel_case::getLastWord(paramType.Name) == "Object")
    return false;

  // Try splitting after the last preposition.
  if (splitBaseNameAfterLastPreposition(baseName, argName, paramType))
    return true;

  return false;
}

bool swift::omitNeedlessWords(
    StringRef &baseName, MutableArrayRef<StringRef> argNames,
    StringRef firstParamName, OmissionTypeName givenResultType,
    OmissionTypeName contextType, ArrayRef<OmissionTypeName> paramTypes,
    bool returnsSelf, bool isProperty, const InheritedNameSet *allPropertyNames,
    llvm::Optional<unsigned> completionHandlerIndex,
    llvm::Optional<StringRef> completionHandlerName,
    StringScratchSpace &scratch) {
  bool anyChanges = false;
  OmissionTypeName resultType = returnsSelf ? contextType : givenResultType;

  /// Local function that lowercases all of the base names and
  /// argument names before returning.
  auto lowercaseAcronymsForReturn = [&] {
    StringRef newBaseName = toLowercaseInitialisms(baseName, scratch);
    if (baseName.data() != newBaseName.data()) {
      baseName = newBaseName;
      anyChanges = true;
    }

    for (StringRef &argName : argNames) {
      StringRef newArgName = toLowercaseInitialisms(argName, scratch);
      if (argName.data() != newArgName.data()) {
        argName = newArgName;
        anyChanges = true;
      }
    }

    return anyChanges;
  };

  // If the result type matches the context, remove the context type from the
  // prefix of the name.
  bool resultTypeMatchesContext = (resultType == contextType);
  if (resultTypeMatchesContext) {
    StringRef newBaseName = omitNeedlessWordsFromPrefix(baseName, contextType);
    if (newBaseName != baseName) {
      baseName = newBaseName;
      anyChanges = true;
    }
  }

  // Strip the context type from the base name of a method.
  if (!isProperty) {
    StringRef newBaseName = omitSelfTypeFromBaseName(baseName, contextType,
                                                     allPropertyNames, scratch);
    if (newBaseName != baseName) {
      baseName = newBaseName;
      anyChanges = true;
    }
  }

  if (paramTypes.empty()) {
    if (resultTypeMatchesContext) {
      StringRef newBaseName = omitTrailingTypeNameWithSpecialCases(
          baseName, resultType, NameRole::Property, allPropertyNames);
      if (newBaseName != baseName) {
        baseName = newBaseName;
        anyChanges = true;
      }
    }

    return lowercaseAcronymsForReturn();
  }

  if (camel_case::getFirstWord(baseName) == "set") {
    StringRef newBaseName = omitTrailingTypeNameWithSpecialCases(
        baseName, contextType, NameRole::Property, allPropertyNames);
    if (newBaseName != baseName) {
      baseName = newBaseName;
      anyChanges = true;
    }
  }

  // If the base name of a method imported as "async" starts with the word
  // "get", drop the "get".
  bool isAsync = completionHandlerIndex.has_value();
  if (isAsync && camel_case::getFirstWord(baseName) == "get" &&
      baseName.size() > 3) {
    baseName = baseName.substr(3);
    anyChanges = true;
  }

  // If needed, split the base name.
  if (!argNames.empty() &&
      splitBaseName(baseName, argNames[0], paramTypes[0], firstParamName))
    anyChanges = true;

  // If this is an asynchronous function where the completion handler is
  // the first parameter, strip off WithCompletion(Handler) from the base name.
  if (isAsync && *completionHandlerIndex == 0) {
    if (auto newBaseName = stripWithCompletionHandlerSuffix(baseName)) {
      baseName = *newBaseName;
      anyChanges = true;
    }
  }

  // For a method imported as "async", drop the "Asynchronously" suffix from
  // the base name. It is redundant with 'async'.
  const StringRef asynchronously = "Asynchronously";
  if (isAsync && camel_case::getLastWord(baseName) == asynchronously &&
      baseName.size() > asynchronously.size()) {
    baseName = baseName.drop_back(asynchronously.size());
    anyChanges = true;
  }

  // If this is an asynchronous function where the completion handler is
  // past the first parameter the corresponding name has some additional
  // information prior to the completion-handled suffix, append that
  // additional text to the base name.
  if (isAsync && *completionHandlerIndex >= 1 && completionHandlerName) {
    if (auto extraParamText = stripWithCompletionHandlerSuffix(
            *completionHandlerName)) {
      SmallString<32> newBaseName;
      newBaseName += baseName;
      appendSentenceCase(newBaseName, *extraParamText);
      baseName = scratch.copyString(newBaseName);
      anyChanges = true;
    }
  }

  // Omit needless words based on parameter types.
  for (unsigned i = 0, n = argNames.size(); i != n; ++i) {
    // If there is no corresponding parameter, there is nothing to
    // omit.
    if (i >= paramTypes.size()) continue;

    // Omit needless words based on the type of the parameter.
    NameRole role = i > 0 ? NameRole::SubsequentParameter
      : argNames[0].empty() ? NameRole::BaseName
      : baseName == "init" ? NameRole::SubsequentParameter
      : paramTypes[0].hasDefaultArgument() ? NameRole::SubsequentParameter
      : NameRole::FirstParameter;

    // Omit needless words from the name.
    StringRef name = role == NameRole::BaseName ? baseName : argNames[i];
    StringRef newName = omitTrailingTypeNameWithSpecialCases(
        name, paramTypes[i], role,
        role == NameRole::BaseName ? allPropertyNames : nullptr);

    if (name == newName) continue;

    // Record this change.
    anyChanges = true;
    if (role == NameRole::BaseName) {
      baseName = newName;
    } else {
      argNames[i] = newName;
    }
  }

  return lowercaseAcronymsForReturn();
}

llvm::Optional<StringRef>
swift::stripWithCompletionHandlerSuffix(StringRef name) {
  if (name.endswith("WithCompletionHandler")) {
    return name.drop_back(strlen("WithCompletionHandler"));
  }

  if (name.endswith("WithCompletion")) {
    return name.drop_back(strlen("WithCompletion"));
  }

  if (name.endswith("WithCompletionBlock")) {
    return name.drop_back(strlen("WithCompletionBlock"));
  }

  if (name.endswith("WithBlock")) {
    return name.drop_back(strlen("WithBlock"));
  }

  if (name.endswith("WithReplyTo")) {
    return name.drop_back(strlen("WithReplyTo"));
  }

  if (name.endswith("WithReply")) {
    return name.drop_back(strlen("WithReply"));
  }

  return llvm::None;
}

void swift::writeEscaped(llvm::StringRef Str, llvm::raw_ostream &OS) {
  for (unsigned i = 0, e = Str.size(); i != e; ++i) {
    unsigned char c = Str[i];

    switch (c) {
      case '\\':
        OS << '\\' << '\\';
        break;
      case '\t':
        OS << '\\' << 't';
        break;
      case '\n':
        OS << '\\' << 'n';
        break;
      case '"':
        OS << '\\' << '"';
        break;
      default:
        OS << c;
        break;
    }
  }
}

bool swift::pathStartsWith(StringRef prefix, StringRef path) {
  auto prefixIt = llvm::sys::path::begin(prefix),
       prefixEnd = llvm::sys::path::end(prefix);
  for (auto pathIt = llvm::sys::path::begin(path),
            pathEnd = llvm::sys::path::end(path);
       prefixIt != prefixEnd && pathIt != pathEnd; ++prefixIt, ++pathIt) {
    if (*prefixIt != *pathIt)
      return false;
  }
  return prefixIt == prefixEnd;
}
