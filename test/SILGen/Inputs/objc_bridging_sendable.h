@import Foundation;

@interface NSBlah: NSObject
- (void) takeSendable: (id __attribute__((swift_attr("@Sendable")))) x;
@property(readonly) id __attribute__((swift_attr("@Sendable"))) x;
- (nullable __attribute__((swift_attr("@Sendable"))) id)test:(NSError *_Nullable __autoreleasing * _Nullable)error;
@end
