// Enum usage that is bitwise-able and assignable in C++, aka how CF_OPTIONS
// does things.

#if __has_attribute(enum_extensibility)
#define __CF_ENUM_ATTRIBUTES __attribute__((enum_extensibility(open)))
#define __CF_CLOSED_ENUM_ATTRIBUTES __attribute__((enum_extensibility(closed)))
#define __CF_OPTIONS_ATTRIBUTES __attribute__((flag_enum,enum_extensibility(open)))
#else
#define __CF_ENUM_ATTRIBUTES
#define __CF_CLOSED_ENUM_ATTRIBUTES
#define __CF_OPTIONS_ATTRIBUTES
#endif

// explicitly use extern "C" rather than setting it in the modulemap file as
// would be the case with Foundation's modulemap.
extern "C" {

#define CF_OPTIONS(_type, _name) _type __attribute__((availability(swift, unavailable))) _name; enum __CF_OPTIONS_ATTRIBUTES : _name
#define NS_OPTIONS(_type, _name) CF_OPTIONS(_type, _name)
#define NS_REFINED_FOR_SWIFT __attribute__((swift_private))
#define UIKIT_EXTERN extern "C" __attribute__((visibility("default")))

typedef NS_OPTIONS(unsigned long, NSBinarySearchingOptions) {
	NSBinarySearchingFirstEqual = (1UL << 8),
	NSBinarySearchingLastEqual = (1UL << 9),
	NSBinarySearchingInsertionIndex = (1UL << 10),
};

typedef NS_OPTIONS(unsigned long, NSAttributedStringFormattingOptions) {
  NSAttributedStringFormattingInsertArgumentAttributesWithoutMerging = 1 << 0,
  NSAttributedStringFormattingApplyReplacementIndexAttribute = 1 << 1,
} NS_REFINED_FOR_SWIFT;

@interface NSAttributedString
@end

@interface NSAttributedString (NSAttributedStringFormatting)
- (instancetype)initWithOptions:(NSAttributedStringFormattingOptions)options
    NS_REFINED_FOR_SWIFT;
@end

UIKIT_EXTERN
@interface UIPrinter

typedef NS_OPTIONS(long, UIPrinterJobTypes) {
  UIPrinterJobTypeUnknown = 0,
  UIPrinterJobTypeDocument = 1 << 0,
  UIPrinterJobTypeEnvelope = 1 << 1,
  UIPrinterJobTypeLabel = 1 << 2,
  UIPrinterJobTypePhoto = 1 << 3,
  UIPrinterJobTypeReceipt = 1 << 4,
  UIPrinterJobTypeRoll = 1 << 5,
  UIPrinterJobTypeLargeFormat = 1 << 6,
  UIPrinterJobTypePostcard = 1 << 7
};

@end
}

typedef NS_OPTIONS(unsigned long, Foo) {
  NS_SWIFT_NAMED_OptionOne __attribute__((swift_name("SwiftOptionOne"))) = 0,
  NS_SWIFT_NAMED_OptionTwo __attribute__((swift_name("SwiftOptionTwo"))) = 1
                                                                           << 0,
  NS_SWIFT_NAMED_OptionThree
  __attribute__((swift_name("SwiftOptionThree"))) = 1 << 1,
  NS_SWIFT_NAMED_OptionFour
  __attribute__((swift_name("SwiftOptionFour"))) = NS_SWIFT_NAMED_OptionOne |
                                                   NS_SWIFT_NAMED_OptionTwo
};

typedef NS_OPTIONS(unsigned long, Bar) {
  API_NOTES_NAMED_OptionOne = 0,
  API_NOTES_NAMED_OptionTwo = 1 << 0,
  API_NOTES_NAMED_OptionThree = 1 << 1,
  API_NOTES_NAMED_OptionFour = API_NOTES_NAMED_OptionOne |
                               API_NOTES_NAMED_OptionTwo
};

typedef NS_OPTIONS(unsigned long, Baz) { Baz1, Baz2 };

struct HasNSOptionField {
  Bar bar;
};

@interface HasNSOptionFieldObjC
@property Bar bar;
@end

@interface HasNSOptionFieldObjC2
- (void)setBar:(Bar)bar;
@end

Baz CFunctionReturningNSOption();
void CFunctionTakingNSOption(Baz);

@interface NSOptionTypeCheckTest
+ (Baz)methodReturningNSOption;
+ (void)methodTakingNSOption:(Baz)baz;
@end
