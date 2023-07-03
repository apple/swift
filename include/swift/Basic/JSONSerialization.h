//===--- JSONSerialization.h - JSON serialization support -------*- C++ -*-===//
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
/// \file
/// Provides an interface for serializing to JSON.
/// \note This does not include support for deserializing JSON; since JSON is
/// a subset of YAML, use LLVM's YAML parsing support instead.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_JSONSERIALIZATION_H
#define SWIFT_BASIC_JSONSERIALIZATION_H

/* #include "swift/Basic/LLVM.h" */
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <vector>

namespace swift {
namespace json {

/// This class should be specialized by any type that needs to be converted
/// to/from a JSON object.  For example:
///
///     struct ObjectTraits<MyStruct> {
///       static void mapping(Output &out, MyStruct &s) {
///         out.mapRequired("name", s.name);
///         out.mapRequired("size", s.size);
///         out.mapOptional("age",  s.age);
///       }
///     };
template<class T>
struct ObjectTraits {
  // Must provide:
  // static void mapping(Output &out, T &fields);
  // Optionally may provide:
  // static StringRef validate(Output &out, T &fields);
};


/// This class should be specialized by any integral type that converts
/// to/from a JSON scalar where there is a one-to-one mapping between
/// in-memory values and a string in JSON.  For example:
///
///     struct ScalarEnumerationTraits<Colors> {
///         static void enumeration(Output &out, Colors &value) {
///           out.enumCase(value, "red",   cRed);
///           out.enumCase(value, "blue",  cBlue);
///           out.enumCase(value, "green", cGreen);
///         }
///       };
template<typename T>
struct ScalarEnumerationTraits {
  // Must provide:
  // static void enumeration(Output &out, T &value);
};


/// This class should be specialized by any integer type that is a union
/// of bit values and the JSON representation is an array of
/// strings.  For example:
///
///      struct ScalarBitSetTraits<MyFlags> {
///        static void bitset(Output &out, MyFlags &value) {
///          out.bitSetCase(value, "big",   flagBig);
///          out.bitSetCase(value, "flat",  flagFlat);
///          out.bitSetCase(value, "round", flagRound);
///        }
///      };
template<typename T>
struct ScalarBitSetTraits {
  // Must provide:
  // static void bitset(Output &out, T &value);
};


/// This class should be specialized by type that requires custom conversion
/// to/from a json scalar.  For example:
///
///    template<>
///    struct ScalarTraits<MyType> {
///      static void output(const MyType &val, llvm::raw_ostream &out) {
///        // stream out custom formatting
///        out << llvm::format("%x", val);
///      }
///      static bool mustQuote(StringRef) { return true; }
///    };
template<typename T>
struct ScalarTraits {
  // Must provide:
  //
  // Function to write the value as a string:
  //static void output(const T &value, void *ctxt, llvm::raw_ostream &out);
  //
  // Function to determine if the value should be quoted.
  //static bool mustQuote(StringRef);
};

/// This is an optimized form of ScalarTraits in case the scalar value is
/// already present in a memory buffer.  For example:
///
///    template<>
///    struct ScalarReferenceTraits<MyType> {
///      static StringRef stringRef(const MyType &val) {
///        // Retrieve scalar value from memory
///        return value.stringValue;
///      }
///      static bool mustQuote(StringRef) { return true; }
///    };
template<typename T>
struct ScalarReferenceTraits {
  // Must provide:
  //
  // Function to return a string representation of the value.
  // static StringRef stringRef(const T &value);
  //
  // Function to determine if the value should be quoted.
  // static bool mustQuote(StringRef);
};

/// This class should be specialized by any type that can be 'null' in JSON.
/// For example:
///
///    template<>
///    struct NullableTraits<MyType *> > {
///      static bool isNull(MyType *&ptr) {
///        return !ptr;
///      }
///      static MyType &get(MyType *&ptr) {
///        return *ptr;
///      }
///    };
template<typename T>
struct NullableTraits {
  // Must provide:
  //
  // Function to return true if the value is 'null'.
  // static bool isNull(const T &Val);
  //
  // Function to return a reference to the unwrapped value.
  // static T::value_type &get(const T &Val);
};


/// This class should be specialized by any type that needs to be converted
/// to/from a JSON array.  For example:
///
///    template<>
///    struct ArrayTraits< std::vector<MyType> > {
///      static size_t size(Output &out, std::vector<MyType> &seq) {
///        return seq.size();
///      }
///      static MyType& element(Output &, std::vector<MyType> &seq, size_t index) {
///        if (index >= seq.size())
///          seq.resize(index+1);
///        return seq[index];
///      }
///    };
template<typename T>
struct ArrayTraits {
  // Must provide:
  // static size_t size(Output &out, T &seq);
  // static T::value_type& element(Output &out, T &seq, size_t index);
};

// Only used by compiler if both template types are the same
template <typename T, T>
struct SameType;

// Only used for better diagnostics of missing traits
template <typename T>
struct MissingTrait;

// Test if ScalarEnumerationTraits<T> is defined on type T.
template <class T>
struct has_ScalarEnumerationTraits
{
  using Signature_enumeration = void (*)(class Output &, T &);

  template <typename U>
  static char test(SameType<Signature_enumeration, &U::enumeration>*);

  template <typename U>
  static double test(...);

public:
  static bool const value =
  (sizeof(test<ScalarEnumerationTraits<T> >(nullptr)) == 1);
};


// Test if ScalarBitSetTraits<T> is defined on type T.
template <class T>
struct has_ScalarBitSetTraits
{
  using Signature_bitset = void (*)(class Output &, T &);

  template <typename U>
  static char test(SameType<Signature_bitset, &U::bitset>*);

  template <typename U>
  static double test(...);

public:
  static bool const value = (sizeof(test<ScalarBitSetTraits<T> >(nullptr)) == 1);
};


// Test if ScalarTraits<T> is defined on type T.
template <class T>
struct has_ScalarTraits
{
  using Signature_output = void (*)(const T &, llvm::raw_ostream &);
  using Signature_mustQuote = bool (*)(llvm::StringRef);

  template <typename U>
  static char test(SameType<Signature_output, &U::output> *,
                   SameType<Signature_mustQuote, &U::mustQuote> *);

  template <typename U>
  static double test(...);

public:
  static bool const value =
  (sizeof(test<ScalarTraits<T>>(nullptr, nullptr)) == 1);
};

// Test if ScalarReferenceTraits<T> is defined on type T.
template <class T>
struct has_ScalarReferenceTraits
{
  using Signature_stringRef = llvm::StringRef (*)(const T &);
  using Signature_mustQuote = bool (*)(llvm::StringRef);

  template <typename U>
  static char test(SameType<Signature_stringRef, &U::stringRef> *,
                   SameType<Signature_mustQuote, &U::mustQuote> *);

  template <typename U>
  static double test(...);

public:
  static bool const value =
  (sizeof(test<ScalarReferenceTraits<T>>(nullptr, nullptr)) == 1);
};

// Test if ObjectTraits<T> is defined on type T.
template <class T>
struct has_ObjectTraits
{
  using Signature_mapping = void (*)(class Output &, T &);

  template <typename U>
  static char test(SameType<Signature_mapping, &U::mapping>*);

  template <typename U>
  static double test(...);

public:
  static bool const value = (sizeof(test<ObjectTraits<T> >(nullptr)) == 1);
};

// Test if ObjectTraits<T>::validate() is defined on type T.
template <class T>
struct has_ObjectValidateTraits
{
  using Signature_validate = llvm::StringRef (*)(class Output &, T &);

  template <typename U>
  static char test(SameType<Signature_validate, &U::validate>*);

  template <typename U>
  static double test(...);

public:
  static bool const value = (sizeof(test<ObjectTraits<T> >(nullptr)) == 1);
};



// Test if ArrayTraits<T> is defined on type T.
template <class T>
struct has_ArrayMethodTraits
{
  using Signature_size = size_t (*)(class Output &, T &);

  template <typename U>
  static char test(SameType<Signature_size, &U::size>*);
  
  template <typename U>
  static double test(...);
  
public:
  static bool const value =  (sizeof(test<ArrayTraits<T> >(nullptr)) == 1);
};

// Test if ArrayTraits<T> is defined on type T
template<typename T>
struct has_ArrayTraits : public std::integral_constant<bool,
    has_ArrayMethodTraits<T>::value > { };

// Test if NullableTraits<T> is defined on type T.
template <class T>
struct has_NullableTraits
{
  using Signature_isNull = bool (*)(T &);

  template <typename U>
  static char test(SameType<Signature_isNull, &U::isNull> *);

  template <typename U>
  static double test(...);

public:
  static bool const value =
  (sizeof(test<NullableTraits<T>>(nullptr)) == 1);
};

inline bool isNumber(llvm::StringRef S) {
  static const char DecChars[] = "0123456789";
  if (S.find_first_not_of(DecChars) == llvm::StringRef::npos)
    return true;

  llvm::Regex FloatMatcher(
      "^(\\.[0-9]+|[0-9]+(\\.[0-9]*)?)([eE][-+]?[0-9]+)?$");
  if (FloatMatcher.match(S))
    return true;

  return false;
}

inline bool isNumeric(llvm::StringRef S) {
  if ((S.front() == '-' || S.front() == '+') && isNumber(S.drop_front()))
    return true;

  if (isNumber(S))
    return true;

  return false;
}

inline bool isNull(llvm::StringRef S) { return S.equals("null"); }

inline bool isBool(llvm::StringRef S) {
  return S.equals("true") || S.equals("false");
}

template<typename T>
struct missingTraits : public std::integral_constant<bool,
    !has_ScalarEnumerationTraits<T>::value
 && !has_ScalarBitSetTraits<T>::value
 && !has_ScalarTraits<T>::value
 && !has_ScalarReferenceTraits<T>::value
 && !has_NullableTraits<T>::value
 && !has_ObjectTraits<T>::value
 && !has_ArrayTraits<T>::value> {};

template<typename T>
struct validatedObjectTraits : public std::integral_constant<bool,
    has_ObjectTraits<T>::value
 && has_ObjectValidateTraits<T>::value> {};

template<typename T>
struct unvalidatedObjectTraits : public std::integral_constant<bool,
    has_ObjectTraits<T>::value
&& !has_ObjectValidateTraits<T>::value> {};

class Output {
public:
  using UserInfoMap = std::map<void *, void *>;

private:
  enum State {
    ArrayFirstValue,
    ArrayOtherValue,
    ObjectFirstKey,
    ObjectOtherKey
  };

  llvm::raw_ostream &Stream;
  llvm::SmallVector<State, 8> StateStack;
  bool PrettyPrint;
  bool NeedBitValueComma;
  bool EnumerationMatchFound;
  UserInfoMap UserInfo;

public:
  Output(llvm::raw_ostream &os, UserInfoMap UserInfo = {},
         bool PrettyPrint = true)
      : Stream(os), PrettyPrint(PrettyPrint), NeedBitValueComma(false),
        EnumerationMatchFound(false), UserInfo(UserInfo) {}
  virtual ~Output() = default;

  UserInfoMap &getUserInfo() {
    return UserInfo;
  }

  unsigned beginArray();
  bool preflightElement(unsigned, void *&);
  void postflightElement(void*);
  void endArray();
  bool canElideEmptyArray();

  void beginObject();
  void endObject();
  bool preflightKey(llvm::StringRef, bool, bool, bool &, void *&);
  void postflightKey(void*);

  void beginEnumScalar();
  bool matchEnumScalar(const char*, bool);
  void endEnumScalar();

  bool beginBitSetScalar(bool &);
  bool bitSetMatch(const char*, bool);
  void endBitSetScalar();

  void scalarString(llvm::StringRef &, bool);
  void null();

  template <typename T>
  void enumCase(T &Val, const char* Str, const T ConstVal) {
    if (matchEnumScalar(Str, Val == ConstVal)) {
      Val = ConstVal;
    }
  }

  template <typename T>
  void bitSetCase(T &Val, const char* Str, const T ConstVal) {
    if (bitSetMatch(Str, (Val & ConstVal) == ConstVal)) {
      Val = Val | ConstVal;
    }
  }

  template <typename T>
  void maskedBitSetCase(T &Val, const char *Str, T ConstVal, T Mask) {
    if (bitSetMatch(Str, (Val & Mask) == ConstVal))
      Val = Val | ConstVal;
  }

  template <typename T>
  void maskedBitSetCase(T &Val, const char *Str, uint32_t ConstVal,
                        uint32_t Mask) {
    if (bitSetMatch(Str, (Val & Mask) == ConstVal))
      Val = Val | ConstVal;
  }

  template <typename T>
  void mapRequired(llvm::StringRef Key, T &Val) {
    this->processKey(Key, Val, true);
  }

  template <typename T>
  typename std::enable_if<has_ArrayTraits<T>::value, void>::type
  mapOptional(llvm::StringRef Key, T &Val) {
    // omit key/value instead of outputting empty array
    if (this->canElideEmptyArray() && !(Val.begin() != Val.end()))
      return;
    this->processKey(Key, Val, false);
  }

  template <typename T>
  void mapOptional(llvm::StringRef Key, llvm::Optional<T> &Val) {
    processKeyWithDefault(Key, Val, llvm::Optional<T>(), /*Required=*/false);
  }

  template <typename T>
  typename std::enable_if<!has_ArrayTraits<T>::value, void>::type
  mapOptional(llvm::StringRef Key, T &Val) {
    this->processKey(Key, Val, false);
  }

  template <typename T>
  void mapOptional(llvm::StringRef Key, T &Val, const T &Default) {
    this->processKeyWithDefault(Key, Val, Default, false);
  }

private:
  template <typename T>
  void processKeyWithDefault(llvm::StringRef Key, llvm::Optional<T> &Val,
                             const llvm::Optional<T> &DefaultValue,
                             bool Required) {
    assert(!DefaultValue.has_value() &&
           "Optional<T> shouldn't have a value!");
    void *SaveInfo;
    bool UseDefault;
    const bool sameAsDefault = !Val.has_value();
    if (!Val.has_value())
      Val = T();
    if (this->preflightKey(Key, Required, sameAsDefault, UseDefault,
                           SaveInfo)) {
      jsonize(*this, Val.value(), Required);
      this->postflightKey(SaveInfo);
    } else {
      if (UseDefault)
        Val = DefaultValue;
    }
  }

  template <typename T>
  void processKeyWithDefault(llvm::StringRef Key, T &Val, const T &DefaultValue,
                             bool Required) {
    void *SaveInfo;
    bool UseDefault;
    const bool sameAsDefault = Val == DefaultValue;
    if (this->preflightKey(Key, Required, sameAsDefault, UseDefault,
                           SaveInfo)) {
      jsonize(*this, Val, Required);
      this->postflightKey(SaveInfo);
    } else {
      if (UseDefault)
        Val = DefaultValue;
    }
  }

  template <typename T>
  void processKey(llvm::StringRef Key, T &Val, bool Required) {
    void *SaveInfo;
    bool UseDefault;
    if (this->preflightKey(Key, Required, false, UseDefault, SaveInfo)) {
      jsonize(*this, Val, Required);
      this->postflightKey(SaveInfo);
    }
  }

private:
  void indent();
};

template <typename T> struct ArrayTraits<std::vector<T>> {
  static size_t size(Output &out, std::vector<T> &seq) { return seq.size(); }

  static T &element(Output &out, std::vector<T> &seq, size_t index) {
    if (index >= seq.size())
      seq.resize(index + 1);
    return seq[index];
  }
};

template<>
struct ScalarReferenceTraits<bool> {
  static llvm::StringRef stringRef(const bool &);
  static bool mustQuote(llvm::StringRef) { return false; }
};

template <>
struct ScalarReferenceTraits<llvm::StringRef> {
  static llvm::StringRef stringRef(const llvm::StringRef &);
  static bool mustQuote(llvm::StringRef S) { return true; }
};

template<>
struct ScalarReferenceTraits<std::string> {
  static llvm::StringRef stringRef(const std::string &);
  static bool mustQuote(llvm::StringRef S) { return true; }
};

template<>
struct ScalarTraits<uint8_t> {
  static void output(const uint8_t &, llvm::raw_ostream &);
  static bool mustQuote(llvm::StringRef) { return false; }
};

template<>
struct ScalarTraits<uint16_t> {
  static void output(const uint16_t &, llvm::raw_ostream &);
  static bool mustQuote(llvm::StringRef) { return false; }
};

template<>
struct ScalarTraits<uint32_t> {
  static void output(const uint32_t &, llvm::raw_ostream &);
  static bool mustQuote(llvm::StringRef) { return false; }
};

#if defined(_MSC_VER)
// In MSVC, 'unsigned long' is 32bit size and different from uint32_t,
// and it is used to define swift::sys::ProcessId.
template<>
struct ScalarTraits<unsigned long> {
  static void output(const unsigned long &, llvm::raw_ostream &);
  static bool mustQuote(llvm::StringRef) { return false; }
};
#endif

template<>
struct ScalarTraits<uint64_t> {
  static void output(const uint64_t &, llvm::raw_ostream &);
  static bool mustQuote(llvm::StringRef) { return false; }
};

template<>
struct ScalarTraits<int8_t> {
  static void output(const int8_t &, llvm::raw_ostream &);
  static bool mustQuote(llvm::StringRef) { return false; }
};

template<>
struct ScalarTraits<int16_t> {
  static void output(const int16_t &, llvm::raw_ostream &);
  static bool mustQuote(llvm::StringRef) { return false; }
};

template<>
struct ScalarTraits<int32_t> {
  static void output(const int32_t &, llvm::raw_ostream &);
  static bool mustQuote(llvm::StringRef) { return false; }
};

template<>
struct ScalarTraits<int64_t> {
  static void output(const int64_t &, llvm::raw_ostream &);
  static bool mustQuote(llvm::StringRef) { return false; }
};

template<>
struct ScalarTraits<float> {
  static void output(const float &, llvm::raw_ostream &);
  static bool mustQuote(llvm::StringRef) { return false; }
};

template<>
struct ScalarTraits<double> {
  static void output(const double &, llvm::raw_ostream &);
  static bool mustQuote(llvm::StringRef) { return false; }
};

template<typename T>
typename std::enable_if<has_ScalarEnumerationTraits<T>::value,void>::type
jsonize(Output &out, T &Val, bool) {
  out.beginEnumScalar();
  ScalarEnumerationTraits<T>::enumeration(out, Val);
  out.endEnumScalar();
}

template<typename T>
typename std::enable_if<has_ScalarBitSetTraits<T>::value,void>::type
jsonize(Output &out, T &Val, bool) {
  bool DoClear;
  if (out.beginBitSetScalar(DoClear)) {
    if (DoClear)
      Val = static_cast<T>(0);
    ScalarBitSetTraits<T>::bitset(out, Val);
    out.endBitSetScalar();
  }
}


template<typename T>
typename std::enable_if<has_ScalarTraits<T>::value,void>::type
jsonize(Output &out, T &Val, bool) {
  {
    llvm::SmallString<64> Storage;
    llvm::raw_svector_ostream Buffer(Storage);
    Buffer.SetUnbuffered();
    ScalarTraits<T>::output(Val, Buffer);
    llvm::StringRef Str = Buffer.str();
    out.scalarString(Str, ScalarTraits<T>::mustQuote(Str));
  }
}

template <typename T>
typename std::enable_if<has_ScalarReferenceTraits<T>::value, void>::type
jsonize(Output &out, T &Val, bool) {
  llvm::StringRef Str = ScalarReferenceTraits<T>::stringRef(Val);
  out.scalarString(Str, ScalarReferenceTraits<T>::mustQuote(Str));
}

template<typename T>
typename std::enable_if<has_NullableTraits<T>::value,void>::type
jsonize(Output &out, T &Obj, bool) {
  if (NullableTraits<T>::isNull(Obj))
    out.null();
  else
    jsonize(out, NullableTraits<T>::get(Obj), true);
}


template<typename T>
typename std::enable_if<validatedObjectTraits<T>::value, void>::type
jsonize(Output &out, T &Val, bool) {
  out.beginObject();
  {
    llvm::StringRef Err = ObjectTraits<T>::validate(out, Val);
    if (!Err.empty()) {
      llvm::errs() << Err << "\n";
      assert(Err.empty() && "invalid struct trying to be written as json");
    }
  }
  ObjectTraits<T>::mapping(out, Val);
  out.endObject();
}

template<typename T>
typename std::enable_if<unvalidatedObjectTraits<T>::value, void>::type
jsonize(Output &out, T &Val, bool) {
  out.beginObject();
  ObjectTraits<T>::mapping(out, Val);
  out.endObject();
}

template<typename T>
typename std::enable_if<missingTraits<T>::value, void>::type
jsonize(Output &out, T &Val, bool) {
  char missing_json_trait_for_type[sizeof(MissingTrait<T>)];
}

template<typename T>
typename std::enable_if<has_ArrayTraits<T>::value,void>::type
jsonize(Output &out, T &Seq, bool) {
  {
    out.beginArray();
    unsigned count = ArrayTraits<T>::size(out, Seq);
    for (unsigned i=0; i < count; ++i) {
      void *SaveInfo;
      if (out.preflightElement(i, SaveInfo)) {
        jsonize(out, ArrayTraits<T>::element(out, Seq, i), true);
        out.postflightElement(SaveInfo);
      }
    }
    out.endArray();
  }
}

// Define non-member operator<< so that Output can stream out a map.
template <typename T>
inline
typename
std::enable_if<swift::json::has_ObjectTraits<T>::value, Output &>::type
operator<<(Output &yout, T &map) {
  jsonize(yout, map, true);
  return yout;
}

// Define non-member operator<< so that Output can stream out an array.
template <typename T>
inline
typename
std::enable_if<swift::json::has_ArrayTraits<T>::value, Output &>::type
operator<<(Output &yout, T &seq) {
  jsonize(yout, seq, true);
  return yout;
}

} // end namespace json
} // end namespace swift

#endif // SWIFT_BASIC_JSONSERIALIZATION_H
