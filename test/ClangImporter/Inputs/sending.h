
// Make sure that we have set the -D flag appropriately.
#ifdef __SWIFT_ATTR_SUPPORTS_SENDING
#if !__SWIFT_ATTR_SUPPORTS_SENDING
#error "Compiler should have set __SWIFT_ATTR_SUPPORTS_SENDING to 1"
#endif
#else
#error "Compiler should have defined __SWIFT_ATTR_SUPPORTS_SENDING"
#endif

#define SWIFT_SENDING __attribute__((swift_attr("sending")))

#pragma clang assume_nonnull begin

#ifdef __OBJC__

@import Foundation;

@interface MyType : NSObject
- (NSObject *)getSendingResult SWIFT_SENDING;
- (NSObject *)getSendingResultWithArgument:(NSObject *)arg SWIFT_SENDING;
- (NSObject *)getResultWithSendingArgument:(NSObject *)SWIFT_SENDING arg;
@end

SWIFT_SENDING
@interface DoesntMakeSense : NSObject
@end

NSObject *returnNSObjectFromGlobalFunction(NSObject *other);
NSObject *sendNSObjectFromGlobalFunction(NSObject *other) SWIFT_SENDING;
void sendNSObjectToGlobalFunction(NSObject *arg SWIFT_SENDING);

#endif

typedef struct {
  int state;
} NonSendableCStruct;

NonSendableCStruct
returnUserDefinedFromGlobalFunction(NonSendableCStruct other);
NonSendableCStruct
sendUserDefinedFromGlobalFunction(NonSendableCStruct other) SWIFT_SENDING;
void sendUserDefinedIntoGlobalFunction(
    NonSendableCStruct arg SWIFT_SENDING);

#pragma clang assume_nonnull end
