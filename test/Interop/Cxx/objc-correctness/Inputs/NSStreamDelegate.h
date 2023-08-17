#include "CFOptions.h"

typedef CF_OPTIONS(int, NSStreamEvent) {
  NSStreamEventNone = 0,
  NSStreamEventOpenCompleted = 1UL << 0,
  NSStreamEventHasBytesAvailable = 1UL << 1,
  NSStreamEventHasSpaceAvailable = 1UL << 2,
  NSStreamEventErrorOccurred = 1UL << 3,
  NSStreamEventEndEncountered = 1UL << 4
};

@protocol NSObject
@end

__attribute__((objc_root_class))
@interface NSObject <NSObject>
@end

@interface NSStream : NSObject
@end

@protocol NSStreamDelegate <NSObject>
@optional
- (void)stream:(NSStream *)aStream handleEvent:(NSStreamEvent)eventCode;
@end
