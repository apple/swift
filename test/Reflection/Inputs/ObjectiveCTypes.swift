import Foundation

public class OC : NSObject {
  public let nsObject: NSObject
  public let nsString: NSString
  public let cfString: CFString
  public init(nsObject: NSObject, nsString: NSString, cfString: CFString) {
    self.nsObject = nsObject
    self.nsString = nsString
    self.cfString = cfString
  }
}

public class GenericOC<T> : NSObject {
  public let ocnss: GenericOC<NSString>
  public let occfs: GenericOC<CFString>
  public init(nss: GenericOC<NSString>, cfs: GenericOC<CFString>) {
    self.ocnss = nss
    self.occfs = cfs
  }
}
