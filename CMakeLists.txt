cmake_minimum_required(VERSION 3.24)
project(LaplaceEngine VERSION 0.2.0 LANGUAGES C CXX ASM)

# Laplace-Engine: Laplace itself, one program. Laplace-Native is built as part of it; recipes load tree-sitter grammars
# at run time. Linux (hart-server) and Windows (the development machine) build from this one file: what differs is
# the spelling of the compiler's flags (LaplaceFlags.cmake) and the libraries that stand in for libc's extras.
set(CMAKE_C_STANDARD 17)
if(DEFINED ENV{LAPLACE_NATIVE} AND NOT "$ENV{LAPLACE_NATIVE}" STREQUAL "")
  set(LAPLACE_NATIVE "$ENV{LAPLACE_NATIVE}" CACHE PATH "Laplace-Native sources")
else()
  set(LAPLACE_NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../Laplace-Native" CACHE PATH "Laplace-Native sources")
endif()
add_subdirectory(${LAPLACE_NATIVE} native EXCLUDE_FROM_ALL)
include(${LAPLACE_NATIVE}/cmake/LaplacePaths.cmake)
include(${LAPLACE_NATIVE}/cmake/LaplaceFlags.cmake)
find_package(OpenMP REQUIRED)

if(DEFINED ENV{LAPLACE_CONNINFO} AND NOT "$ENV{LAPLACE_CONNINFO}" STREQUAL "")
  set(LAPLACE_CONNINFO "$ENV{LAPLACE_CONNINFO}" CACHE STRING "the database, as a libpq connection string" FORCE)
elseif(WIN32)                                       # no Unix socket: the server on this machine, over TCP
  set(LAPLACE_CONNINFO "host=localhost port=5432 user=laplace dbname=laplace" CACHE STRING "the database, as a libpq connection string")
else()
  set(LAPLACE_CONNINFO "host=/tmp port=5432 user=laplace dbname=laplace" CACHE STRING "the database, as a libpq connection string")
endif()
laplace_path(LAPLACE_RECIPES "${CMAKE_CURRENT_SOURCE_DIR}/recipes" "recipes")

# Flags the Engine adds to the contract's: spelled for the driver, as LaplaceFlags.cmake spells them.
if(CMAKE_C_COMPILER_ID STREQUAL "IntelLLVM" AND CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
  set(ENGINE_QUIET /clang:-w /clang:-O3)
  set(ENGINE_WARN /clang:-Wno-missing-field-initializers)
  set(ENGINE_F16C /clang:-mf16c)
else()
  set(ENGINE_QUIET -w -O3 -fPIC)
  set(ENGINE_WARN -Wno-missing-field-initializers)
  set(ENGINE_F16C -mf16c)
endif()

add_library(ts_runtime STATIC ${LAPLACE_TREESITTER}/lib/src/lib.c)
target_include_directories(ts_runtime PUBLIC ${LAPLACE_TREESITTER}/lib/include PRIVATE ${LAPLACE_TREESITTER}/lib/src ${LAPLACE_TREESITTER}/lib/src/wasm)
target_compile_options(ts_runtime PRIVATE ${ENGINE_QUIET})

set(ENGINE_SOURCES src/main.c src/op.c src/os.c src/os_regex.c src/table.c src/recipe.c src/ingest.c src/db.c src/read.c src/fills.c src/pull.c src/firmware.c src/forward.c src/file.c src/structure.c src/say.c src/program.c
                   src/deploy.c src/forget.c src/tier0.c src/flags.c src/highway.c src/bench.c)
if(TARGET laplace_model)
  list(APPEND ENGINE_SOURCES src/model.c)
else()
  list(APPEND ENGINE_SOURCES src/model_none.c)
endif()

add_executable(laplace_engine ${ENGINE_SOURCES})
set_target_properties(laplace_engine PROPERTIES OUTPUT_NAME laplace)
target_include_directories(laplace_engine PRIVATE src ${LAPLACE_NATIVE}/src ${LAPLACE_BLAKE3_DIR} ${LAPLACE_ICU_DIR}/include ${LAPLACE_PG_DIR}/include)
target_link_directories(laplace_engine PRIVATE ${LAPLACE_PG_DIR}/lib)
target_compile_options(laplace_engine PRIVATE ${LP_C_FLAGS} ${ENGINE_WARN})
configure_file(src/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/laplace_config.h @ONLY)
target_include_directories(laplace_engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(laplace_engine PRIVATE laplace_text laplace_static lp_crmath BLAKE3::blake3 ts_runtime OpenMP::OpenMP_C ${LP_SYSLIBS})
if(WIN32)
  # libpq by its Windows name, and zlib from the dependency installs (dependencies.tsv: $LAPLACE_DEPS/zlib/<version>).
  target_link_libraries(laplace_engine PRIVATE libpq)
  file(GLOB LP_ZLIB_LIBDIRS "${LAPLACE_DEPS}/zlib/*/lib")
  find_library(LP_ZLIB NAMES z zlib zlibstatic HINTS ${LP_ZLIB_LIBDIRS} REQUIRED)
  get_filename_component(LP_ZLIB_LIBDIR "${LP_ZLIB}" DIRECTORY)
  target_include_directories(laplace_engine PRIVATE "${LP_ZLIB_LIBDIR}/../include")
  target_link_libraries(laplace_engine PRIVATE ${LP_ZLIB})
else()
  target_link_libraries(laplace_engine PRIVATE pq z dl)
endif()
if(TARGET laplace_model)
  target_link_libraries(laplace_engine PRIVATE laplace_model)
  set_source_files_properties(src/model.c PROPERTIES COMPILE_OPTIONS "${ENGINE_F16C}")
  set(ENGINE_MKL_RPATH ";$ENV{MKLROOT}/lib")
endif()
if(NOT WIN32)
  set_target_properties(laplace_engine PROPERTIES BUILD_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib${ENGINE_MKL_RPATH}"
                                                  INSTALL_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib${ENGINE_MKL_RPATH}")
  target_link_options(laplace_engine PRIVATE -rdynamic)
  # oneTBB's scalable allocator in place of malloc for the whole program (Research/Performance.md: tree-sitter allocates
  # every node of every parse; with glibc's arenas twelve threads parsing at once waited on malloc). Unicode's parse:
  # 21.8 s -> 13.4 s. On Windows the Universal CRT's heap serves until measured otherwise.
  find_library(TBBMALLOC_PROXY tbbmalloc_proxy HINTS $ENV{TBBROOT}/lib/intel64/gcc4.8 /opt/intel/oneapi/tbb/latest/lib/intel64/gcc4.8 REQUIRED)
  get_filename_component(TBBMALLOC_DIR ${TBBMALLOC_PROXY} DIRECTORY)
  target_link_options(laplace_engine PRIVATE -Wl,--no-as-needed ${TBBMALLOC_PROXY} -Wl,--as-needed)
  set_property(TARGET laplace_engine APPEND PROPERTY BUILD_RPATH ${TBBMALLOC_DIR})
  set_property(TARGET laplace_engine APPEND PROPERTY INSTALL_RPATH ${TBBMALLOC_DIR})
endif()

include(GNUInstallDirs)
install(TARGETS laplace_engine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
