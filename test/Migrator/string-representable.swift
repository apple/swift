// REQUIRES: objc_interop
// RUN: %empty-directory(%t.mod)
// RUN: %target-swift-frontend -swift-version 4 -emit-module -o %t.mod/Cities.swiftmodule %S/Inputs/Cities.swift -module-name Cities -parse-as-library
// RUN: %empty-directory(%t) && %target-swift-frontend -c -swift-version 4 -update-code -primary-file %s  -I %t.mod -api-diff-data-file %S/Inputs/string-representable.json -emit-migrated-file-path %t/string-representable.swift.result -disable-migrator-fixits -o /dev/null -F %S/mock-sdk
// RUN: diff -u %S/string-representable.swift.expected %t/string-representable.swift.result

import Cities
import Bar

func foo(_ c: Container) -> String {
  c.Value = ""
  c.addingAttributes(["a": "b", "a": "b", "a": "b"])
  c.addingAttributes(["a": "b", "a": "b", "a": "b"])
  c.adding(attributes: ["a": 1, "a": 2, "a": 3])
  c.adding(optionalAttributes: ["a": 1, "a": 2, "a": 3])
  _ = Container(optionalAttributes: [:])
  _ = Container(optionalAttrArray: [])
  _ = Container(optionalAttributes: nil)
  _ = Container(optionalAttrArray: nil)
  c.adding(attrArray: ["key1", "key2"])
  c.add(single: "")
  c.add(singleOptional: "")
  c.add(singleOptional: nil)
  _ = c.getAttrDictionary()
  _ = c.getOptionalAttrDictionary()
  _ = c.getSingleAttr()
  _ = c.getOptionalSingleAttr()
  _ = c.getAttrArray()
  _ = c.getOptionalAttrArray()

  c.addingAttributes(c.getAttrDictionary())
  c.adding(optionalAttributes: c.getAttrDictionary())

  c.attrDict = ["a": "b", "a": "b", "a": "b"]
  c.attrArr = ["key1", "key2"]
  _ = c.attrArr
  _ = c.attrDict
  c.adding(attributes: c.attrDict)
  _ = Container(optionalAttrArray: c.attrArr)
  c.adding(optionalAttributes: c.optionalAttrDict)
  _ = GlobalAttribute
  c.Value = GlobalAttribute
  c.optionalAttrDict = nil
  return c.Value
}

class C: BarForwardDeclaredClass {}

func revert(_ a: AwesomeCityAttribute, b: Wrapper.Attribute) {
  _ = AwesomeCityAttribute(rawValue: "somevalue")
  _ = AwesomeCityAttribute.init(rawValue: "somevalue")
  _ = AwesomeCityAttribute("somevalue")
  _ = AwesomeCityAttribute.init("somevalue")
  _ = a.rawValue
  _ = Wrapper.Attribute(rawValue: "somevalue")
  _ = Wrapper.Attribute.init(rawValue: "somevalue")
  _ = b.rawValue
  _ = Wrapper.Attribute.KnownAttr.rawValue
  _ = Wrapper.Attribute("somevalue")
  _ = Wrapper.Attribute.init("somevalue")
}


func bar(_ c: Container) {
  let attr: AliasAttribute = ""
  c.add(single: attr)
}

public class SubContainer: Container {
  public override func adding(optionalAttributes subname: [String: Any]?) {}
  public override func adding(attributes myname: [String: Any]) {}
  public override func adding(attrArray: [String]) {}
  public override func add(single: String) {}
  public override func add(singleOptional: String?) {}
}
