@import Foundation;

@interface Base : NSObject
- (void)objCBaseMethod;
@property (nonatomic, strong) NSString *prop;
@end

@protocol Pettable
@end

@interface PettableContainer<T : id<Pettable>> : NSObject
@end
