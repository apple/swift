// If you're modifying this file to add or modify function signatures, you
// should be notifying the maintainer of PlaygroundLogger and also the
// maintainer of lib/Sema/PlaygroundTransform.cpp.

struct SourceRange {
  let sl : Int
  let el : Int
  let sc : Int
  let ec : Int
  var text : String {
    return "[\(sl):\(sc)-\(el):\(ec)]"
  }
}

struct ModuleFileIdentifier {
  let moduleId : Int
  let fileId : Int
  var text : String {
    return "[\(moduleId):\(fileId)]"
  }
}

class LogRecord {
  let text : String

  init(api : String, object : Any, name : String, id : Int, range : SourceRange, moduleName : String, filePath : String) {
    var object_description : String = ""
    print(object, terminator: "", to: &object_description)
    text = range.text + " " + api + "[" + name + "='" + object_description + "'" + " module: " + moduleName + ". file: " + filePath + "]"
  }
  init(api : String, object : Any, name : String, id : Int, range : SourceRange, moduleFileId : ModuleFileIdentifier) {
    var object_description : String = ""
    print(object, terminator: "", to: &object_description)
    text = moduleFileId.text + " " + range.text + " " + api + "[" + name + "='" + object_description + "']"
  }
  init(api : String, object : Any, name : String, range : SourceRange, moduleFileId : ModuleFileIdentifier) {
    var object_description : String = ""
    print(object, terminator: "", to: &object_description)
    text = moduleFileId.text + " " + range.text + " " + api + "[" + name + "='" + object_description + "']"
  }
  init(api : String, object: Any, range : SourceRange, moduleFileId : ModuleFileIdentifier) {
    var object_description : String = ""
    print(object, terminator: "", to: &object_description)
    text = moduleFileId.text + " " + range.text + " " + api + "['" + object_description + "']"
  }
  init(api : String, range : SourceRange, moduleName : String, filePath: String) {
    text = range.text + " " + api + " " + " module: " + moduleName + ". file: " + filePath
  }
  init(api: String, range : SourceRange, moduleFileId : ModuleFileIdentifier) {
    text = moduleFileId.text + " " + range.text + " " + api
  }
}

public func __builtin_log<T>(_ object : T, _ name : String, _ sl : Int, _ el : Int, _ sc : Int, _ ec: Int, _ moduleId : Int, _ fileId : Int) -> AnyObject? {
  let moduleFileId = ModuleFileIdentifier(moduleId:moduleId, fileId:fileId)
  return LogRecord(api:"__builtin_log", object:object, name:name, range:SourceRange(sl:sl, el:el, sc:sc, ec:ec), moduleFileId:moduleFileId)
}

public func __builtin_log_with_id<T>(_ object : T, _ name : String, _ id : Int, _ sl : Int, _ el : Int, _ sc : Int, _ ec: Int, _ moduleId : Int, _ fileId : Int) -> AnyObject? {
  let moduleFileId = ModuleFileIdentifier(moduleId:moduleId, fileId:fileId)
  return LogRecord(api:"__builtin_log", object:object, name:name, id:id, range:SourceRange(sl:sl, el:el, sc:sc, ec:ec), moduleFileId:moduleFileId)
}

public func __builtin_log_with_id_extended<T>(_ object: T, _ name: String, _ id: Int, _ sl: Int, _ el: Int, _ sc: Int, _ ec: Int, _ moduleName: String, _ filePath: String) -> AnyObject? {
  return LogRecord(api:"__builtin_log_with_id_extended", object:object, name:name, id:id, range:SourceRange(sl:sl, el:el, sc:sc, ec:ec), moduleName:moduleName, filePath:filePath)
}

public func __builtin_log_scope_entry(_ sl : Int, _ el : Int, _ sc : Int, _ ec: Int, _ moduleId : Int, _ fileId : Int) -> AnyObject? {
  let moduleFileId = ModuleFileIdentifier(moduleId:moduleId, fileId:fileId)
  return LogRecord(api:"__builtin_log_scope_entry", range:SourceRange(sl:sl, el:el, sc:sc, ec:ec), moduleFileId:moduleFileId)
}

public func __builtin_log_scope_entry_extended(_ sl : Int, _ el : Int, _ sc : Int, _ ec: Int, _ moduleName: String, _ filePath: String) -> AnyObject? {
  return LogRecord(api:"__builtin_log_scope_entry_extended", range:SourceRange(sl:sl, el:el, sc:sc, ec:ec), moduleName:moduleName, filePath:filePath)
}

public func __builtin_log_scope_exit(_ sl : Int, _ el : Int, _ sc : Int, _ ec: Int, _ moduleId : Int, _ fileId : Int) -> AnyObject? {
  let moduleFileId = ModuleFileIdentifier(moduleId:moduleId, fileId:fileId)
  return LogRecord(api:"__builtin_log_scope_exit", range:SourceRange(sl:sl, el:el, sc:sc, ec:ec), moduleFileId:moduleFileId)
}

public func __builtin_log_scope_exit_extended(_ sl : Int, _ el : Int, _ sc : Int, _ ec: Int, _ moduleName: String, _ filePath: String) -> AnyObject? {
  return LogRecord(api:"__builtin_log_scope_exit_extended", range:SourceRange(sl:sl, el:el, sc:sc, ec:ec), moduleName:moduleName, filePath:filePath)
}

public func __builtin_postPrint(_ sl : Int, _ el : Int, _ sc : Int, _ ec: Int, _ moduleId : Int, _ fileId : Int) -> AnyObject? {
  let moduleFileId = ModuleFileIdentifier(moduleId:moduleId, fileId:fileId)
  return LogRecord(api:"__builtin_postPrint", range:SourceRange(sl:sl, el:el, sc:sc, ec:ec), moduleFileId:moduleFileId)
}

public func __builtin_postPrint_extended(_ sl : Int, _ el : Int, _ sc : Int, _ ec: Int, _ moduleName: String, _ filePath: String) -> AnyObject? {
  return LogRecord(api:"__builtin_postPrint_extended", range:SourceRange(sl:sl, el:el, sc:sc, ec:ec), moduleName:moduleName, filePath:filePath)
}

public func __builtin_send_data(_ object:AnyObject?) {
  print((object as! LogRecord).text)
}


