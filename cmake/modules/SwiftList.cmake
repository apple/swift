# Module import guard
if(DEFINED SWIFT_LIST_MODULE_LOADED)
  return()
endif()
set(SWIFT_LIST_MODULE_LOADED TRUE)


include(SwiftUtils)


function(list_subtract lhs rhs result_var)
  set(result)
  foreach(item IN LISTS lhs)
    if(NOT "${item}" IN_LIST rhs)
      list(APPEND result "${item}")
    endif()
  endforeach()

  set(${result_var} "${result}" PARENT_SCOPE)
endfunction()


function(list_intersect lhs rhs result_var)
  set(result)
  foreach(item IN LISTS lhs)
    if("${item}" IN_LIST rhs)
      list(APPEND result "${item}")
    endif()
  endforeach()

  set(${result_var} "${result}" PARENT_SCOPE)
endfunction()


function(list_union lhs rhs result_var)
  set(result)
  foreach(item IN LISTS lhs rhs)
    if(NOT "${item}" IN_LIST result)
      list(APPEND result "${item}")
    endif()
  endforeach()

  set(${result_var} "${result}" PARENT_SCOPE)
endfunction()


function(_list_add_string_suffix input_list suffix result_var)
  set(result)
  foreach(element ${input_list})
    list(APPEND result "${element}${suffix}")
  endforeach()

  set(${result_var} "${result}" PARENT_SCOPE)
endfunction()


function(_list_escape_for_shell input_list result_var)
  set(result "")
  foreach(element ${input_list})
    string(REPLACE " " "\\ " element "${element}")
    set(result "${result}${element} ")
  endforeach()

  set(${result_var} "${result}" PARENT_SCOPE)
endfunction()


function(list_replace input_list old new)
  set(replaced_list)
  foreach(item ${${input_list}})
    if("${item}" STREQUAL "${old}")
      list(APPEND replaced_list "${new}")
    else()
      list(APPEND replaced_list "${item}")
    endif()
  endforeach()

  set(${input_list} "${replaced_list}" PARENT_SCOPE)
endfunction()


function(precondition_list_empty l message)
  precondition(l NEGATE MESSAGE "${message}")
endfunction()

