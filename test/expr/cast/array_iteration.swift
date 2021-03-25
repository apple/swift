// RUN: %target-swift-frontend -emit-sil %s -verify

func doFoo() {}

class View {
  var subviews: Array<AnyObject>! = []
}

var rootView = View()
var v = [View(), View()]
rootView.subviews = v

_ = rootView.subviews as! [View]

for view in rootView.subviews as! [View] { // expected-warning{{immutable value 'view' was never used; consider replacing with '_' or removing it}}
  doFoo()
}

_ = (rootView.subviews!) as! [View]

_ = (rootView.subviews) as! [View]

var ao: [AnyObject] = []
_ = ao as! [View] // works


var b = Array<(String, Int)>()

for x in b { // expected-warning{{immutable value 'x' was never used; consider replacing with '_' or removing it}}
  doFoo()
}

var c : Array<(String, Int)>! = Array()

for x in c { // expected-warning{{immutable value 'x' was never used; consider replacing with '_' or removing it}}
  doFoo()
}

var d : Array<(String, Int)>? = Array()

for x in d! { // expected-warning{{immutable value 'x' was never used; consider replacing with '_' or removing it}}
  doFoo()
}
