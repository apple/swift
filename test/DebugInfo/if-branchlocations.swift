// RUN: %target-swift-frontend %s -emit-sil -disable-copy-propagation -emit-verbose-sil -g -o - | %FileCheck %s --check-prefixes=CHECK,CHECK-NCP
// RUN: %target-swift-frontend %s -emit-sil -enable-copy-propagation -emit-verbose-sil -g -o - | %FileCheck %s --check-prefixes=CHECK,CHECK-CP

class NSURL {}

class NSPathControlItem {
  var URL: NSURL?
}

class NSPathControl {
  var clickedPathItem: NSPathControlItem?;
}

class AppDelegate {
    
  func LogStr(_ message: String) {
  }

  func componentClicked(_ sender: AnyObject)
  {
    if let control = sender as? NSPathControl
    {
      LogStr( "Got a path control" )
      if let item = control.clickedPathItem
      {
        LogStr( "Got an NSPathControlItem" )
        // Verify that the branch's location is >= the cleanup's location.
        // (The implicit false block of the conditional
        //  below inherits the location from the condition.)
        // CHECK-CP: destroy_value [poison] {{.*}}$NSPathControlItem{{.*}}line:[[@LINE+2]]
        // CHECK: br{{.*}}line:[[@LINE+1]]
        if let url = item.URL
        {
          LogStr( "There is a url" )
        }
        // Verify that the branch's location is >= the cleanup's location.
        // CHECK-NCP: strong_release{{.*}}$NSPathControlItem{{.*}}line:[[@LINE+2]]
        // CHECK: br{{.*}}line:[[@LINE+1]]
      }
    }
  }
}
