// RUN: %target-typecheck-verify-swift

func doFoo() {}

class View {
  var subviews: Array<AnyObject>! = []
}

var rootView = View()
var v = [View(), View()]
rootView.subviews = v

for view:View in rootView.subviews { // expected-error{{cannot convert sequence element type 'AnyObject' to expected type 'View'}}
  doFoo()
}
