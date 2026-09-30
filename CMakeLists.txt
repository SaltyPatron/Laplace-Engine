cmake_minimum_required(VERSION 3.24)
project(LaplaceEngine VERSION 0.2.0 LANGUAGES C CXX ASM)

# Laplace-Engine: Laplace itself, one program. Laplace-Native is built as part of it; recipes load tree-sitter grammars
# at run time.
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
  set(LAPLACE_CONNINFO "$ENV{LAPLACE_CONNINFO}" CACHE STRING "the database, as a libpq connection string")
else()
  set(LAPLACE_CONNINFO "host=/tmp port=5432 user=laplace dbname=laplace_engine" CACHE STRING "the database, as a libpq connection string")
endif()
laplace_path(LAPLACE_RECIPES "${CMAKE_CURRENT_SOURCE_DIR}/recipes" "recipes")

add_library(ts_runtime STATIC ${LAPLACE_TREESITTER}/lib/src/lib.c)
target_include_directories(ts_runtime PUBLIC ${LAPLACE_TREESITTER}/lib/include PRIVATE ${LAPLACE_TREESITTER}/lib/src ${LAPLACE_TREESITTER}/lib/src/wasm)
target_compile_options(ts_runtime PRIVATE -w -O3 -fPIC)

set(ENGINE_SOURCES src/main.c src/table.c src/recipe.c src/records.c src/elements.c src/members.c src/vocab.c src/ingest.c src/db.c src/read.c src/fills.c src/pull.c src/firmware.c src/forward.c src/file.c
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
target_compile_options(laplace_engine PRIVATE ${LP_C_FLAGS} -Wno-missing-field-initializers)
configure_file(src/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/laplace_config.h @ONLY)
target_include_directories(laplace_engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(laplace_engine PRIVATE laplace_text laplace_static lp_crmath BLAKE3::blake3 ts_runtime pq OpenMP::OpenMP_C z dl m pthread)
if(TARGET laplace_model)
  target_link_libraries(laplace_engine PRIVATE laplace_model)
  set_source_files_properties(src/model.c PROPERTIES COMPILE_OPTIONS "-mf16c")
  set(ENGINE_MKL_RPATH ";$ENV{MKLROOT}/lib")
endif()
set_target_properties(laplace_engine PROPERTIES BUILD_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib${ENGINE_MKL_RPATH}"
                                                INSTALL_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib${ENGINE_MKL_RPATH}")
target_link_options(laplace_engine PRIVATE -rdynamic)

include(GNUInstallDirs)
install(TARGETS laplace_engine RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
