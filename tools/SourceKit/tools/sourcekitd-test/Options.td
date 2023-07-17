//===--- Options.td -------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

include "llvm/Option/OptParser.td"

def req : Separate<["-"], "req">,
  HelpText<"Request kind">;
def req_EQ : Joined<["-"], "req=">, Alias<req>;

def id : Separate<["-"], "id">,
  HelpText<"If this is an async request, an ID that can be used to cancel the request">;
def id_EQ : Joined<["-"], "id=">, Alias<id>;

def primary_file : Separate<["-"], "primary-file">,
  HelpText<"Primary file path">;

def offset : Separate<["-"], "offset">,
  HelpText<"byte offset">;
def offset_EQ : Joined<["-"], "offset=">, Alias<offset>;

def length : Separate<["-"], "length">,
  HelpText<"length of text referred to by pos / offset">;
def length_EQ : Joined<["-"], "length=">, Alias<length>;

def pos : Separate<["-"], "pos">,
  HelpText<"line:col">;
def pos_EQ : Joined<["-"], "pos=">, Alias<pos>;

def line : Separate<["-"], "line">,
  HelpText<"line">;
def line_EQ : Joined<["-"], "line=">, Alias<line>;

def using_swift_args : Flag<["-"], "using-swift-args">,
  HelpText<"Interpret the compiler arguments as Swift compiler arguments">;

def replace : Separate<["-"], "replace">,
  HelpText<"replace text ('text')">;
def replace_EQ : Joined<["-"], "replace=">, Alias<replace>;

def module : Separate<["-"], "module">,
  HelpText<"Module name">;

def header : Separate<["-"], "header">,
  HelpText<"Header path">;

def usr : Separate<["-"], "usr">,
  HelpText<"USR">;

def text_input : Separate<["-"], "text-input">,
  HelpText<"File to get source text from">;

def pass_as_sourcetext : Flag<["-"], "pass-as-sourcetext">,
  HelpText<"Read source file and pass the contents with key.sourcetext">;

def cache_path: Separate<["-"], "cache-path">, HelpText<"cache path">;
def cache_path_EQ : Joined<["-"], "cache-path=">, Alias<cache_path>;

def req_opts : CommaJoined<["-"], "req-opts=">,
  HelpText<"Pass the comma separated options in <arg> as request specific options">,
  MetaVarName<"<arg>">;

def rename_spec : Separate<["-"], "rename-spec">,
  HelpText<"The JSON format rename spec file (req=syntactic-rename)">;
def rename_spec_EQ : Joined<["-"], "rename-spec=">, Alias<rename_spec>;

def check_interface_is_ascii : Flag<["-"], "check-interface-ascii">,
  HelpText<"Check that the module interface text is ASCII">;

def json_request_path: Separate<["-"], "json-request-path">,
  HelpText<"path to read a request in JSON format">;

def dont_print_request : Flag<["-"], "dont-print-request">,
  HelpText<"Don't print the request">;

def print_response_as_json : Flag<["-"], "print-response-as-json">,
  HelpText<"Print the response as JSON output">;

def print_raw_response : Flag<["-"], "print-raw-response">,
  HelpText<"Dump the response to stdout">;

def dont_print_response : Flag<["-"], "dont-print-response">,
  HelpText<"Do not print the response">;

def group_name : Separate<["-"], "group-name">,
  HelpText<"Module group name to print">;

def simplified_demangling : Flag<["-"], "simplified-demangling">,
  HelpText<"Use simplified demangling for the 'demangle' request">;

def synthesized_extension : Flag<["-"], "synthesized-extension">,
  HelpText<"Print synthesized extensions when generating interface">;

def cursor_action : Flag<["-"], "cursor-action">,
  HelpText<"Print available actions on the code element under cursor">;

def interested_usr : Separate<["-"], "interested-usr">,
  HelpText<"Interested USR to calculate the containing group">;

def async : Flag<["-"], "async">,
  HelpText<"Perform request asynchronously">;

def disable_implicit_concurrency_module_import : Flag<["-"],
    "disable-implicit-concurrency-module-import">,
  HelpText<"Disable implicit import of the _Concurrency module">;

def disable_implicit_string_processing_module_import : Flag<["-"],
    "disable-implicit-string-processing-module-import">,
  HelpText<"Disable implicit import of the _StringProcessing module">;

def enable_implicit_backtracing_module_import : Flag<["-"],
    "enable-implicit-backtracing-module-import">,
  HelpText<"Enable implicit import of the _Backtracing module">;

def disable_implicit_backtracing_module_import : Flag<["-"],
    "disable-implicit-backtracing-module-import">,
  HelpText<"Disable implicit import of the _Backtracing module">;

def end_pos : Separate<["-"], "end-pos">, HelpText<"line:col">;
def end_pos_EQ : Joined<["-"], "end-pos=">, Alias<end_pos>;

def swift_version : Separate<["-"], "swift-version">, HelpText<"the swift version to use">;
def swift_version_EQ : Joined<["-"], "swift-version=">, Alias<swift_version>;

def pass_version_as_string : Flag<["-"], "pass-version-as-string">,
  HelpText<"Pass swift-version as string">;

def swift_name : Separate<["-"], "swift-name">,
  HelpText<"Swift name to translate from">;

def objc_name : Separate<["-"], "objc-name">,
  HelpText<"Objective-C name to translate from">;

def name : Separate<["-"], "name">,
  HelpText<"The preferred name when performing semantic refactoring">;

def objc_selector : Separate<["-"], "objc-selector">,
  HelpText<"Objective-C selector name to translate from">;

def cancel_on_subsequent_request : Separate<["-"], "cancel-on-subsequent-request">,
  HelpText<"Whether to cancel if there is a subsequent request using the same AST">;
def cancel_on_subsequent_request_EQ : Joined<["-"], "cancel-on-subsequent-request=">,
  Alias<cancel_on_subsequent_request>;

def time_request : Flag<["-"], "time-request">,
  HelpText<"Print the time taken to process the request">;

def measure_instructions : Flag<["-"], "measure-instructions">,
  HelpText<"Measure how many instructions the execution of this request took in the SourceKit process.">;

def repeat_request : Separate<["-"], "repeat-request">,
  HelpText<"Repeat the request n times">, MetaVarName<"<n>">;
def repeat_request_EQ : Joined<["-"], "repeat-request=">, Alias<repeat_request>;

def vfs_files : CommaJoined<["-"], "vfs-files=">,
  HelpText<"Injects a VFS into the request, overlaying files specified by the given <name>=<target> pairs over the real filesystem. Prefix destination with '@' to pass as sourcetext.">;

def vfs_name : Separate<["-"], "vfs-name">,
  HelpText<"Specify a virtual filesystem name">;

def index_store_path : Separate<["-"], "index-store-path">,
  HelpText<"Index Store path to use for indexing">;

def index_unit_output_path : Separate<["-"], "index-unit-output-path">,
  HelpText<"Index Store unit output path to use for indexing">;

def suppress_config_request : Flag<["-"], "suppress-config-request">,
  HelpText<"Suppress the default global configuration request, that is otherwise sent before any other request (except for the global-config request itself)">;

def module_cache_path: Separate<["-"], "module-cache-path">, HelpText<"module cache path">;
def module_cache_path_EQ : Joined<["-"], "module-cache-path=">, Alias<module_cache_path>;

def simulate_long_request : Separate<["-"], "simulate-long-request">,
  HelpText<"Simulate that the request takes x ms longer to execute. The request can be cancelled while waiting this duration">;
def simulate_long_request_EQ : Joined<["-"], "simulate-long-request=">, Alias<simulate_long_request>;

def shell: Flag<["-"], "shell">,
  HelpText<"Run shell command">;

def cancel : Separate<["-"], "cancel">,
  HelpText<"Cancel all async requests with the given ID">;
def cancel_EQ : Joined<["-"], "cancel=">, Alias<cancel>;

def help : Flag<["-", "--"], "help">,
  HelpText<"Display available options">;

def h : Flag<["-"], "h">, Alias<help>;
