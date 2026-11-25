# The translated game lives only in the developer's build/recomp/gen; the
# kit never tracks generated code (spec section 11). Generated C is platform
# independent, so every preset (macOS, iOS) compiles the same directory.
set(POP_GEN_DIR ${POP_BUILD_ROOT}/recomp/gen)
if(POP_TRANSLATE STREQUAL "STUB")
  # A link-only translation for builds without game code (CI).
  set(POP_GEN_DIR ${CMAKE_BINARY_DIR}/stub-gen)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${POP_ROOT}/tools/gen_stub_translation.py --out ${POP_GEN_DIR}
    RESULT_VARIABLE POP_STUB_RESULT)
  if(NOT POP_STUB_RESULT EQUAL 0)
    message(FATAL_ERROR "tools/gen_stub_translation.py failed")
  endif()
endif()
set(POP_HAVE_GEN OFF)
# POP_REAL_GEN: the translation is the game's, not the link-only stub. Tests
# that call into generated functions by address are defined only then.
set(POP_REAL_GEN OFF)
if(POP_TRANSLATE STREQUAL "OFF")
  message(STATUS "POP_TRANSLATE=OFF: targets that need the generated code are not defined")
elseif(EXISTS ${POP_GEN_DIR}/table.c)
  set(POP_HAVE_GEN ON)
  if(NOT POP_TRANSLATE STREQUAL "STUB")
    set(POP_REAL_GEN ON)
  endif()
  message(STATUS "Translation: ${POP_GEN_DIR}")
elseif(POP_TRANSLATE STREQUAL "ON")
  message(FATAL_ERROR "POP_TRANSLATE=ON but no translation: run tools/build.py --regenerate")
else()
  message(STATUS "No translation found: hosts and game-backed tests are not defined")
endif()

if(POP_HAVE_GEN)
  file(GLOB POP_GEN_SOURCES CONFIGURE_DEPENDS ${POP_GEN_DIR}/chunk_*.c ${POP_GEN_DIR}/table.c)
  add_library(recomp_gen STATIC ${POP_GEN_SOURCES})
  set_target_properties(recomp_gen PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${POP_ARCHIVE_DIR} OUTPUT_NAME recomp_gen)
  # -I<gen> for x86.h beside the sources, -I<root> for the canonical copy,
  # -I<runtime> for intrinsics.h: the same three the shell script passed.
  target_include_directories(recomp_gen PRIVATE ${POP_GEN_DIR} ${POP_ROOT} ${POP_ROOT}/runtime)
  target_include_directories(recomp_gen INTERFACE ${POP_GEN_DIR})
  target_compile_options(recomp_gen PRIVATE ${POP_WARN_GEN})
  # A game's native replacements. funcs.h includes this header before it
  # defines FN_<addr>, so every call site, tail call and jump-table case for a
  # replaced address goes to the native function instead.
  if(RECOMP_OVERRIDE_HEADER)
    if(NOT EXISTS ${RECOMP_OVERRIDE_HEADER})
      message(FATAL_ERROR "RECOMP_OVERRIDE_HEADER does not exist: ${RECOMP_OVERRIDE_HEADER}")
    endif()
    target_compile_definitions(recomp_gen PRIVATE
      RECOMP_OVERRIDE_HEADER="${RECOMP_OVERRIDE_HEADER}")
    get_filename_component(_override_dir ${RECOMP_OVERRIDE_HEADER} DIRECTORY)
    target_include_directories(recomp_gen PRIVATE ${_override_dir})
    message(STATUS "Native overrides: ${RECOMP_OVERRIDE_HEADER}")
  endif()
  pop_optimize(recomp_gen 2)
  # ThinLTO over the translated code: a guest CALL is a C call into another chunk and every RET asks
  # recomp_is_call_return (table.c), so without it none of that inlines across files.
  option(RECOMP_GEN_LTO "ThinLTO for the translated code" ON)
  if(RECOMP_GEN_LTO AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(recomp_gen PRIVATE -flto=thin)
  endif()
  # Profile-guided optimisation of the translated code. RECOMP_PGO_GENERATE=1 in the environment builds an
  # instrumented app (run it with LLVM_PROFILE_FILE=<dir>/%p.profraw, merge with `xcrun llvm-profdata merge`);
  # a merged profile at <build root>/pgo/game.profdata is then used by every later build.
  set(POP_GEN_PGO_FLAGS "")
  if(DEFINED ENV{RECOMP_PGO_GENERATE} AND NOT "$ENV{RECOMP_PGO_GENERATE}" STREQUAL "")
    set(POP_GEN_PGO_FLAGS -fprofile-instr-generate)
    set(POP_GEN_PGO_LINK -fprofile-instr-generate)
    message(STATUS "Translated code: PGO instrumented build")
  elseif(DEFINED POP_BUILD_ROOT AND EXISTS "${POP_BUILD_ROOT}/pgo/game.profdata")
    set(POP_GEN_PGO_FLAGS "-fprofile-instr-use=${POP_BUILD_ROOT}/pgo/game.profdata" -Wno-profile-instr-unprofiled
        -Wno-profile-instr-out-of-date -Wno-profile-instr-missing)
    message(STATUS "Translated code: PGO with ${POP_BUILD_ROOT}/pgo/game.profdata")
  endif()
  if(POP_GEN_PGO_FLAGS)
    target_compile_options(recomp_gen PRIVATE ${POP_GEN_PGO_FLAGS})
  endif()
  # game.toml [translate] store_hooks = false: guest stores in the translation skip the watch/dirty hooks.
  if(DEFINED RECOMP_STORE_HOOKS AND NOT RECOMP_STORE_HOOKS)
    target_compile_definitions(recomp_gen PRIVATE RECOMP_NO_STORE_HOOKS=1)
  endif()
  # Auxiliary modules (game.toml [modules.aux.<key>]) are translated into
  # gen/aux-<key>/ with their own funcs.h and prefixed tables, so each is its
  # own library; their table.c registers with the runtime's module registry.
  set(POP_GEN_AUX_TARGETS "")
  file(GLOB POP_GEN_AUX_DIRS CONFIGURE_DEPENDS LIST_DIRECTORIES true ${POP_GEN_DIR}/aux-*)
  foreach(dir ${POP_GEN_AUX_DIRS})
    if(NOT EXISTS ${dir}/table.c)
      continue()
    endif()
    get_filename_component(key ${dir} NAME)
    string(REPLACE "aux-" "recomp_gen_" aux_target ${key})
    file(GLOB aux_sources CONFIGURE_DEPENDS ${dir}/chunk_*.c ${dir}/table.c)
    add_library(${aux_target} STATIC ${aux_sources})
    set_target_properties(${aux_target} PROPERTIES
      ARCHIVE_OUTPUT_DIRECTORY ${POP_ARCHIVE_DIR} OUTPUT_NAME ${aux_target})
    target_include_directories(${aux_target} PRIVATE ${dir} ${POP_GEN_DIR} ${POP_ROOT} ${POP_ROOT}/runtime)
    # A module's own funcs.h reads the same overrides header, so a native
    # replacement can stand in for one of its functions as for the image's.
    if(RECOMP_OVERRIDE_HEADER)
      target_compile_definitions(${aux_target} PRIVATE
        RECOMP_OVERRIDE_HEADER="${RECOMP_OVERRIDE_HEADER}")
      target_include_directories(${aux_target} PRIVATE ${_override_dir})
    elseif(RECOMP_NATIVE_HEADER AND NOT POP_TRANSLATE STREQUAL "STUB")
      # The game's native replacements reach its auxiliary modules too (NFS Undercover's msvcr80 qsort).
      target_compile_definitions(${aux_target} PRIVATE RECOMP_OVERRIDE_HEADER="${RECOMP_NATIVE_HEADER}")
    endif()
    target_compile_options(${aux_target} PRIVATE ${POP_WARN_GEN})
    pop_optimize(${aux_target} 2)
    if(RECOMP_GEN_LTO AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
      target_compile_options(${aux_target} PRIVATE -flto=thin)
    endif()
    if(POP_GEN_PGO_FLAGS)
      target_compile_options(${aux_target} PRIVATE ${POP_GEN_PGO_FLAGS})
    endif()
    if(DEFINED RECOMP_STORE_HOOKS AND NOT RECOMP_STORE_HOOKS)
      target_compile_definitions(${aux_target} PRIVATE RECOMP_NO_STORE_HOOKS=1)
    endif()
    list(APPEND POP_GEN_AUX_TARGETS ${aux_target})
    message(STATUS "Auxiliary module translation: ${dir}")
  endforeach()
  # The game's native replacements (game.toml [translate] native).
  if(RECOMP_NATIVE_HEADER AND NOT POP_TRANSLATE STREQUAL "STUB")
    target_sources(recomp_gen PRIVATE ${RECOMP_NATIVE_SOURCES})
    # A replacement must round exactly as the instructions it replaces: no fused multiply-add.
    set_source_files_properties(${RECOMP_NATIVE_SOURCES} PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
    target_compile_definitions(recomp_gen PRIVATE RECOMP_OVERRIDE_HEADER="${RECOMP_NATIVE_HEADER}")
  endif()
endif()

# The portable spelling of -Wl,-force_load: every generated object is kept
# whether or not anything references it, because the dispatch table is
# reached by address.
function(pop_link_gen target)
  target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,recomp_gen>")
  if(POP_GEN_PGO_LINK)
    target_link_options(${target} PRIVATE ${POP_GEN_PGO_LINK})
  endif()
  if(RECOMP_GEN_LTO AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND APPLE)
    # Keep ThinLTO's per-module results between links: an unchanged chunk is not optimised again.
    target_link_options(${target} PRIVATE -flto=thin "-Wl,-cache_path_lto,${CMAKE_BINARY_DIR}/lto-cache")
  endif()
  foreach(aux_target ${POP_GEN_AUX_TARGETS})
    target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${aux_target}>")
  endforeach()
  target_include_directories(${target} PRIVATE ${POP_GEN_DIR})
endfunction()
