cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW) # Normalize Windows paths before excluding OS libraries.
endif()
project(LaplaceEngine VERSION 0.2.0 LANGUAGES C CXX ASM)

# Laplace-Engine: Laplace itself, one program. Laplace-Native is built as part of it; recipes load tree-sitter grammars
# at run time.
set(CMAKE_C_STANDARD 17)
if(DEFINED ENV{LAPLACE_NATIVE} AND NOT "$ENV{LAPLACE_NATIVE}" STREQUAL "")
  set(LAPLACE_NATIVE "$ENV{LAPLACE_NATIVE}" CACHE PATH "Laplace-Native sources")
else()
  set(LAPLACE_NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../Laplace-Native" CACHE PATH "Laplace-Native sources")
endif()
if(NOT TARGET laplace_static)
  add_subdirectory(${LAPLACE_NATIVE} native EXCLUDE_FROM_ALL)
endif()
include(${LAPLACE_NATIVE}/cmake/LaplacePaths.cmake)
include(${LAPLACE_NATIVE}/cmake/LaplaceFlags.cmake)
include(${LAPLACE_NATIVE}/cmake/LaplaceWindowsUtf8.cmake)
find_package(OpenMP REQUIRED)

if(DEFINED ENV{LAPLACE_CONNINFO} AND NOT "$ENV{LAPLACE_CONNINFO}" STREQUAL "")
  set(LAPLACE_CONNINFO "$ENV{LAPLACE_CONNINFO}" CACHE STRING "the database, as a libpq connection string" FORCE)
else()
  set(LAPLACE_CONNINFO "host=/tmp port=5432 user=laplace dbname=laplace" CACHE STRING "the database, as a libpq connection string")
endif()
laplace_path(LAPLACE_RECIPES "${CMAKE_CURRENT_SOURCE_DIR}/recipes" "recipes")

add_library(ts_runtime STATIC ${LAPLACE_TREESITTER}/lib/src/lib.c)
target_include_directories(ts_runtime PUBLIC ${LAPLACE_TREESITTER}/lib/include PRIVATE ${LAPLACE_TREESITTER}/lib/src ${LAPLACE_TREESITTER}/lib/src/wasm)
if(WIN32)
  target_compile_options(ts_runtime PRIVATE /w /O2)
else()
  target_compile_options(ts_runtime PRIVATE -w -O3 -fPIC)
endif()

set(ENGINE_SOURCES
    src/main.c
    src/op.c
    src/os.c
    src/pg.c
    src/pg_plan.c
    src/table.c
    src/recipe.c
    src/ingest.c
    src/db.c
    src/read.c
    src/read_text.c
    src/fills.c
    src/pull.c
    src/firmware.c
    src/forward.c
    src/file.c
    src/structure.c
    src/say.c
    src/program.c
    src/deploy.c
    src/forget.c
    src/tier0.c
    src/flags.c
    src/highway.c
    src/bench.c
    src/provenance.c
    src/walk.c
    src/chess.c
    src/grammars.c
    src/literal_sql.c
    src/file_metadata.c
    src/recipe_compile.c
    src/record_stream.c
    src/context_stream.c
    src/record_select.c
    src/record_operands.c
    src/record_diagnostics.c
    src/record_values.c
    src/record_surface.c
    src/reference_html.c
    src/record_output.c
)
if(TARGET laplace_model)
  list(APPEND ENGINE_SOURCES src/model.c)
else()
  list(APPEND ENGINE_SOURCES src/model_none.c)
endif()

if(WIN32)
  find_library(LAPLACE_ZLIB NAMES z zlib HINTS ENV LAPLACE_ZLIB_DIR PATH_SUFFIXES lib REQUIRED)
endif()
add_executable(laplace_engine ${ENGINE_SOURCES})
laplace_utf8_executable(laplace_engine)
if(WIN32)
  # POSIX headers the Windows C runtime lacks (src/win: byte order, regex), and POSIX regex itself from musl's
  # sources at their pin (Laplace-Operations dependencies.tsv): TRE, leftmost-longest, as glibc matches on Linux.
  set(LAPLACE_MUSL "$ENV{LAPLACE_DEPSRC}/musl" CACHE PATH "musl sources (POSIX regex on Windows)")
  if(NOT EXISTS "${LAPLACE_MUSL}/src/regex/regcomp.c")
    message(FATAL_ERROR "LAPLACE_MUSL: no musl sources at ${LAPLACE_MUSL}")
  endif()
  add_library(posix_regex STATIC ${LAPLACE_MUSL}/src/regex/regcomp.c ${LAPLACE_MUSL}/src/regex/regexec.c
                                 ${LAPLACE_MUSL}/src/regex/tre-mem.c ${LAPLACE_MUSL}/src/regex/regerror.c)
  target_include_directories(posix_regex BEFORE PRIVATE src/win src/win/musl)
  target_compile_options(posix_regex PRIVATE ${LP_C_FLAGS} /w)
  target_compile_definitions(posix_regex PRIVATE restrict=__restrict hidden= RE_DUP_MAX=255 CHARCLASS_NAME_MAX=14)
  target_include_directories(laplace_engine BEFORE PRIVATE src/win)
  target_link_libraries(laplace_engine PRIVATE posix_regex)
  get_filename_component(LAPLACE_ZLIB_DIR ${LAPLACE_ZLIB} DIRECTORY)
  target_include_directories(laplace_engine PRIVATE ${LAPLACE_ZLIB_DIR}/../include)
endif()
set_target_properties(laplace_engine PROPERTIES OUTPUT_NAME laplace)
target_include_directories(laplace_engine PRIVATE src ${LAPLACE_NATIVE}/src ${LAPLACE_BLAKE3_DIR} ${LAPLACE_ICU_DIR}/include ${LAPLACE_PG_DIR}/include)
target_link_directories(laplace_engine PRIVATE ${LAPLACE_PG_DIR}/lib)
target_compile_options(laplace_engine PRIVATE ${LP_C_FLAGS} -Wno-missing-field-initializers)
# A reading contract identifies source, not a compiler's executable bytes (PE/ELF
# timestamps, linkage and build paths differ between standalone and embedded builds).
# Watch all reader/native inputs, normalize checkout line endings, and frame each
# relative pathname and digest so a source change still invalidates the OS precheck.
set(_reader_contract "")
foreach(_reader_root IN ITEMS "${CMAKE_CURRENT_SOURCE_DIR}" "${LAPLACE_NATIVE}" "${LAPLACE_TREESITTER}/lib")
  file(GLOB_RECURSE _reader_inputs CONFIGURE_DEPENDS RELATIVE "${_reader_root}"
       "${_reader_root}/src/*.c" "${_reader_root}/src/*.h" "${_reader_root}/src/*.inc"
       "${_reader_root}/include/*.h" "${_reader_root}/manifest/*.tsv" "${_reader_root}/manifest/*.toml")
  list(SORT _reader_inputs)
  foreach(_reader_input IN LISTS _reader_inputs)
    set(_reader_path "${_reader_root}/${_reader_input}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_reader_path}")
    file(READ "${_reader_path}" _reader_text)
    string(REPLACE "\r\n" "\n" _reader_text "${_reader_text}")
    string(SHA256 _reader_digest "${_reader_text}")
    string(LENGTH "${_reader_input}" _reader_length)
    string(APPEND _reader_contract "${_reader_length}:${_reader_input}:${_reader_digest}\n")
  endforeach()
  string(APPEND _reader_contract "\n")
endforeach()
string(SHA256 LAPLACE_READER_SOURCE_ID "${_reader_contract}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/laplace_reader_source.h"
     CONTENT "#define LAPLACE_READER_SOURCE_ID \"${LAPLACE_READER_SOURCE_ID}\"\n")
configure_file(src/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/laplace_config.h @ONLY)
target_include_directories(laplace_engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
if(NOT WIN32)
  add_executable(file_reference_local EXCLUDE_FROM_ALL tests/file-reference-local.c src/table.c)
  target_include_directories(file_reference_local PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}
    ${LAPLACE_NATIVE}/include ${LAPLACE_NATIVE}/src ${LAPLACE_BLAKE3_DIR}
    ${LAPLACE_ICU_DIR}/include ${LAPLACE_PG_DIR}/include)
  target_compile_options(file_reference_local PRIVATE ${LP_C_FLAGS} -UNDEBUG
    -ffunction-sections -fdata-sections -Wno-unused-function -Wno-missing-field-initializers)
  target_link_options(file_reference_local PRIVATE -Wl,--gc-sections)
  target_link_libraries(file_reference_local PRIVATE laplace_static lp_crmath BLAKE3::blake3 OpenMP::OpenMP_C)
  add_executable(presence_frontier EXCLUDE_FROM_ALL tests/presence-frontier.c src/db.c src/pg.c)
  target_include_directories(presence_frontier PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}
    ${LAPLACE_NATIVE}/include ${LAPLACE_NATIVE}/src ${LAPLACE_BLAKE3_DIR}
    ${LAPLACE_ICU_DIR}/include ${LAPLACE_PG_DIR}/include)
  target_compile_options(presence_frontier PRIVATE ${LP_C_FLAGS} -UNDEBUG
    -ffunction-sections -fdata-sections -Wno-unused-function -Wno-missing-field-initializers)
  target_link_options(presence_frontier PRIVATE -Wl,--gc-sections)
  target_link_libraries(presence_frontier PRIVATE laplace_static lp_crmath BLAKE3::blake3 pq OpenMP::OpenMP_C)
endif()
if(WIN32)
  # libpq's import library and zlib's from their installs (Laplace-Operations dependencies.tsv); the C runtime and
  # Win32 give what dl, m and pthread give on Linux.
  target_link_libraries(laplace_engine PRIVATE laplace_text laplace_static lp_crmath BLAKE3::blake3 ts_runtime
                        ${LAPLACE_PG_DIR}/lib/libpq.lib OpenMP::OpenMP_C ${LAPLACE_ZLIB})
else()
  target_link_libraries(laplace_engine PRIVATE laplace_text laplace_static lp_crmath BLAKE3::blake3 ts_runtime pq OpenMP::OpenMP_C z dl m pthread)
endif()
if(TARGET laplace_model)
  target_link_libraries(laplace_engine PRIVATE laplace_model)
  if(WIN32)
    set_source_files_properties(src/model.c PROPERTIES COMPILE_OPTIONS "/clang:-mf16c")
  else()
    set_source_files_properties(src/model.c PROPERTIES COMPILE_OPTIONS "-mf16c")
  endif()
  set(ENGINE_MKL_RPATH ";$ENV{MKLROOT}/lib")
endif()
set_target_properties(laplace_engine PROPERTIES BUILD_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib${ENGINE_MKL_RPATH}"
                                                INSTALL_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib${ENGINE_MKL_RPATH}")
if(NOT WIN32)
  target_link_options(laplace_engine PRIVATE -rdynamic)
endif()
if(WIN32)
  # The process a source is ingested in (os_run_logged): how it is started, what it inherits, how its end is read.
  enable_testing()
  add_executable(os_run tests/os_run.c src/os.c)
  laplace_utf8_executable(os_run)
  target_include_directories(os_run BEFORE PRIVATE src/win src ${LAPLACE_NATIVE}/src)
  target_compile_options(os_run PRIVATE ${LP_C_FLAGS})
  target_link_libraries(os_run PRIVATE shlwapi)
  add_test(NAME os_run COMMAND os_run)
endif()
# oneTBB's scalable allocator in place of malloc for the whole program (Research/Performance.md: tree-sitter allocates
# every node of every parse; with glibc's arenas twelve threads parsing at once waited on malloc). Unicode's parse:
# 21.8 s -> 13.4 s.
if(WIN32)
  # On Windows the proxy replaces the CRT's allocator when the program references its marker symbol.
  find_library(TBBMALLOC_PROXY tbbmalloc_proxy HINTS $ENV{TBBROOT}/lib $ENV{TBBROOT}/lib/intel64/vc14 REQUIRED)
  target_link_libraries(laplace_engine PRIVATE ${TBBMALLOC_PROXY})
  target_link_options(laplace_engine PRIVATE /INCLUDE:__TBB_malloc_proxy)
else()
  find_library(TBBMALLOC_PROXY tbbmalloc_proxy HINTS $ENV{TBBROOT}/lib/intel64/gcc4.8 /opt/intel/oneapi/tbb/latest/lib/intel64/gcc4.8 REQUIRED)
  get_filename_component(TBBMALLOC_DIR ${TBBMALLOC_PROXY} DIRECTORY)
  target_link_options(laplace_engine PRIVATE -Wl,--no-as-needed ${TBBMALLOC_PROXY} -Wl,--as-needed)
  set_property(TARGET laplace_engine APPEND PROPERTY BUILD_RPATH ${TBBMALLOC_DIR})
  set_property(TARGET laplace_engine APPEND PROPERTY INSTALL_RPATH ${TBBMALLOC_DIR})
endif()

# The transport plan owner is exercised against a real configured server.
add_executable(pg_plan_lifecycle EXCLUDE_FROM_ALL tests/pg-plan-lifecycle.c src/pg_plan.c)
laplace_utf8_executable(pg_plan_lifecycle)
target_include_directories(pg_plan_lifecycle PRIVATE src ${LAPLACE_NATIVE}/include ${LAPLACE_PG_DIR}/include)
target_compile_options(pg_plan_lifecycle PRIVATE ${LP_C_FLAGS})
if(WIN32)
  target_compile_options(pg_plan_lifecycle PRIVATE /UNDEBUG)
  target_link_libraries(pg_plan_lifecycle PRIVATE laplace_static ${LAPLACE_PG_DIR}/lib/libpq.lib)
else()
  target_compile_options(pg_plan_lifecycle PRIVATE -UNDEBUG)
  target_link_libraries(pg_plan_lifecycle PRIVATE laplace_static pq)
endif()

include(GNUInstallDirs)
add_executable(reader_depth EXCLUDE_FROM_ALL tests/reader-depth.c src/pg.c src/pg_plan.c)
laplace_utf8_executable(reader_depth)
target_include_directories(reader_depth PRIVATE src ${LAPLACE_NATIVE}/include ${LAPLACE_PG_DIR}/include)
target_compile_options(reader_depth PRIVATE ${LP_C_FLAGS})
if(WIN32)
  target_include_directories(reader_depth BEFORE PRIVATE src/win)
  target_sources(reader_depth PRIVATE src/os.c)
  target_compile_options(reader_depth PRIVATE /UNDEBUG)
  target_link_libraries(reader_depth PRIVATE laplace_static lp_crmath BLAKE3::blake3 ${LAPLACE_PG_DIR}/lib/libpq.lib OpenMP::OpenMP_C shlwapi)
else()
  target_compile_options(reader_depth PRIVATE -UNDEBUG)
  target_link_libraries(reader_depth PRIVATE laplace_static lp_crmath BLAKE3::blake3 pq OpenMP::OpenMP_C)
endif()
# One relocatable recipe runtime is consumed by the modular CLI and managed hosts.
# Its executable is the canonical Engine, with the same compiled grammars and estate.
install(TARGETS laplace_engine
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT LaplaceRecipeRuntime)
if(WIN32)
  file(REAL_PATH "${CMAKE_C_COMPILER}" _recipe_compiler)
  get_filename_component(_recipe_compiler_bin "${_recipe_compiler}" DIRECTORY)
  # MKL loads CPU dispatch kernels dynamically; they are absent from import tables.
  file(GLOB _recipe_mkl_dispatch "$ENV{MKLROOT}/bin/mkl_*.dll")
  set(_recipe_runtime_search "${LAPLACE_ICU_DIR}/bin64;${LAPLACE_ICU_DIR}/bin;${LAPLACE_PG_DIR}/bin;${_recipe_compiler_bin};$ENV{MKLROOT}/bin;$ENV{TBBROOT}/bin")
  set(_recipe_runtime_install [=[
    if(POLICY CMP0207)
      cmake_policy(SET CMP0207 NEW)
    endif()
    file(GET_RUNTIME_DEPENDENCIES EXECUTABLES "$<TARGET_FILE:laplace_engine>"
         RESOLVED_DEPENDENCIES_VAR _libraries UNRESOLVED_DEPENDENCIES_VAR _missing
         DIRECTORIES @_recipe_runtime_search@
         PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
         POST_EXCLUDE_REGEXES "[Ww][Ii][Nn][Dd][Oo][Ww][Ss]/[Ss][Yy][Ss][Tt][Ee][Mm]32/")
    if(_missing)
      message(FATAL_ERROR "Unresolved Native recipe runtime libraries: ${_missing}")
    endif()
    list(APPEND _libraries @_recipe_mkl_dispatch@)
    file(MAKE_DIRECTORY "${CMAKE_INSTALL_PREFIX}/bin")
    foreach(_library IN LISTS _libraries)
      get_filename_component(_name "${_library}" NAME)
      # Preserve the imported DLL name while copying the symlink's bytes.
      # Ordinary Windows publishing does not need symlink creation privileges.
      file(COPY_FILE "${_library}" "${CMAKE_INSTALL_PREFIX}/bin/${_name}" ONLY_IF_DIFFERENT)
    endforeach()
  ]=])
  # Normalize and quote paths before embedding them in the install script.
  file(TO_CMAKE_PATH "${_recipe_runtime_search}" _recipe_runtime_search)
  file(TO_CMAKE_PATH "${_recipe_mkl_dispatch}" _recipe_mkl_dispatch)
  string(REPLACE ";" "\" \"" _recipe_runtime_search "${_recipe_runtime_search}")
  set(_recipe_runtime_search "\"${_recipe_runtime_search}\"")
  string(REPLACE ";" "\" \"" _recipe_mkl_dispatch "${_recipe_mkl_dispatch}")
  set(_recipe_mkl_dispatch "\"${_recipe_mkl_dispatch}\"")
  string(CONFIGURE "${_recipe_runtime_install}" _recipe_runtime_install @ONLY)
  install(CODE "${_recipe_runtime_install}" COMPONENT LaplaceRecipeRuntime)
endif()
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/recipes/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/laplace/recipes" COMPONENT LaplaceRecipeRuntime
        FILES_MATCHING PATTERN "*.recipe" PATTERN "*.format" PATTERN "*.LICENSE" PATTERN "source" PATTERN "order")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/grammar-sources.tsv"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/laplace" COMPONENT LaplaceRecipeRuntime)
install(CODE "
  foreach(_grammar xml lp_words)
    if(NOT EXISTS \"${LAPLACE_GRAMMARS}/libtree-sitter-\${_grammar}${CMAKE_SHARED_LIBRARY_SUFFIX}\")
      message(FATAL_ERROR \"Native recipe grammar bundle is incomplete: ${LAPLACE_GRAMMARS}/libtree-sitter-\${_grammar}${CMAKE_SHARED_LIBRARY_SUFFIX}\")
    endif()
  endforeach()
" COMPONENT LaplaceRecipeRuntime)
install(DIRECTORY "${LAPLACE_GRAMMARS}/" DESTINATION "${CMAKE_INSTALL_BINDIR}/grammars"
        COMPONENT LaplaceRecipeRuntime FILES_MATCHING
        PATTERN "libtree-sitter-*.dll" PATTERN "libtree-sitter-*.so" PATTERN "libtree-sitter-*.dylib")
