# Each layer of src/ may include only itself and the layers below it (CLAUDE.md). Linking catches most mistakes,
# but not a header-only include, because every layer shares one include root; this reads every #include and fails
# on one a layer may not use.
#   cmake -DSRC=<repo>/src -P tests/layers.cmake

set(allowed_platform   platform)
set(allowed_base       base platform)
set(allowed_storage    storage base platform)
set(allowed_core       core storage base platform)
set(allowed_wire       wire core storage base platform)
set(allowed_events     events wire core storage base platform)
set(allowed_objects    objects events wire core storage base platform)
set(allowed_http       http core storage base platform)
set(allowed_api        api objects http events wire core storage base platform)          # not federation
set(allowed_federation federation http events wire core storage base platform)          # not api, not objects
set(allowed_service    service api federation objects http events wire core storage base platform)

set(layers platform base storage core wire events objects http api federation service)
set(failures 0)
foreach(layer IN LISTS layers)
  file(GLOB files "${SRC}/${layer}/*.h" "${SRC}/${layer}/*.hpp" "${SRC}/${layer}/*.cpp")
  foreach(file IN LISTS files)
    file(STRINGS "${file}" includes REGEX "^#include \"[a-z]+/")
    foreach(line IN LISTS includes)
      string(REGEX REPLACE "^#include \"([a-z]+)/.*" "\\1" used "${line}")
      list(FIND layers "${used}" known)
      list(FIND allowed_${layer} "${used}" ok)
      if(known GREATER -1 AND ok EQUAL -1)
        file(RELATIVE_PATH rel "${SRC}" "${file}")
        message(SEND_ERROR "${rel}: ${layer}/ may not include ${used}/ (${line})")
        math(EXPR failures "${failures} + 1")
      endif()
    endforeach()
  endforeach()
endforeach()
if(failures GREATER 0)
  message(FATAL_ERROR "${failures} include(s) cross a layer they may not")
endif()
message(STATUS "every layer includes only what it may")
