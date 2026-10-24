# V8 from Debian / Ubuntu's libnode-dev (Node 22: V8 12.4; Node 24: 13.6),
# plus the upstream V8 headers Node leaves out (ALKA_V8_EXTRA_INCLUDE; see
# fetch-v8-headers.sh). Node builds V8 without pointer compression or the
# sandbox: no ABI defines -- but with GCC, which ignores the preserve_most
# convention V8's headers declare for cppgc's slow paths, so clang >= 17
# must not assume it (COMPONENT_BUILD: in V8's headers it gates only that).
#   cmake -DALKA_V8_TARGET=LibnodeV8 -DCMAKE_PROJECT_INCLUDE=ci/LibnodeV8.cmake
#         -DALKA_V8_EXTRA_INCLUDE=<dir> ...
if(NOT TARGET LibnodeV8)
  find_library(ALKA_LIBNODE NAMES node REQUIRED)
  find_path(ALKA_LIBNODE_INCLUDE v8.h PATH_SUFFIXES node nodejs REQUIRED)
  add_library(LibnodeV8 INTERFACE IMPORTED GLOBAL)
  set_target_properties(LibnodeV8 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ALKA_LIBNODE_INCLUDE};${ALKA_V8_EXTRA_INCLUDE}"
    INTERFACE_COMPILE_DEFINITIONS "$<$<CXX_COMPILER_ID:Clang>:COMPONENT_BUILD>"
    INTERFACE_LINK_LIBRARIES "${ALKA_LIBNODE}")
endif()
