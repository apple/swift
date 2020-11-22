#define MY_ENUM(_name) \
  enum _name _name; \
  enum __attribute__((enum_extensibility(open))) _name
#define MY_EXHAUSTIVE_ENUM(_name) \
  enum _name _name; \
  enum __attribute__((enum_extensibility(closed))) _name


#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmicrosoft-enum-forward-reference"
typedef MY_ENUM(RegularEnum) {
  RegularEnumA,
  RegularEnumB
};
typedef MY_EXHAUSTIVE_ENUM(ExhaustiveEnum) {
  ExhaustiveEnumA,
  ExhaustiveEnumB
};

typedef MY_ENUM(RegularEnumTurnedExhaustive) {
  RegularEnumTurnedExhaustiveA,
  RegularEnumTurnedExhaustiveB
} __attribute__((enum_extensibility(closed)));

enum AnotherRegularEnumTurnedExhaustive {
  AnotherRegularEnumTurnedExhaustiveA,
  AnotherRegularEnumTurnedExhaustiveB
} __attribute__((enum_extensibility(open))) __attribute__((enum_extensibility(closed)));

typedef MY_ENUM(RegularEnumTurnedExhaustiveThenBackViaAPINotes) {
  RegularEnumTurnedExhaustiveThenBackViaAPINotesA,
  RegularEnumTurnedExhaustiveThenBackViaAPINotesB
} __attribute__((enum_extensibility(closed)));

typedef MY_ENUM(ForwardDeclaredTurnedExhaustive);
enum ForwardDeclaredTurnedExhaustive {
  ForwardDeclaredTurnedExhaustiveA,
  ForwardDeclaredTurnedExhaustiveB
} __attribute__((enum_extensibility(closed)));

enum __attribute__((enum_extensibility(open))) ForwardDeclaredOnly;
enum __attribute__((enum_extensibility(closed))) ForwardDeclaredOnly;
enum ForwardDeclaredOnly {
  ForwardDeclaredOnlyA,
  ForwardDeclaredOnlyB
};

enum __attribute__((enum_extensibility(closed))) UnavailableCases {
  UnavailableCasesA,
  UnavailableCasesB,
  UnavailableCasesThisIsTheUnavailableOne __attribute__((availability(swift, unavailable)))
};
#pragma clang diagnostic pop