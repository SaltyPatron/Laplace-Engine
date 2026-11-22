cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW) # Normalize Windows paths before excluding OS libraries.
endif()
project(LaplaceEngine VERSION 0.2.0 LANGUAGES C CXX ASM)

# Laplace-Engine: Laplace itself, one program, linked with the installed Laplace-Native package (CMAKE_PREFIX_PATH or
# LaplaceNative_DIR; a build that includes Native provides the same package, which also gives the floating-point
# contract, LP_C_FLAGS, laplace_path() and laplace_utf8_executable()). Recipes load tree-sitter grammars at run time.
set(CMAKE_C_STANDARD 17)
if(UNIX)
  add_compile_definitions(_GNU_SOURCE)
endif()
find_package(LaplaceNative CONFIG REQUIRED)
find_package(OpenMP REQUIRED)

if(DEFINED ENV{LAPLACE_CONNINFO} AND NOT "$ENV{LAPLACE_CONNINFO}" STREQUAL "")
  set(LAPLACE_CONNINFO "$ENV{LAPLACE_CONNINFO}" CACHE STRING "the database, as a libpq connection string" FORCE)
else()
  set(LAPLACE_CONNINFO "host=/tmp port=5432 user=laplace dbname=laplace" CACHE STRING "the database, as a libpq connection string")
endif()
laplace_path(LAPLACE_RECIPES "${CMAKE_CURRENT_SOURCE_DIR}/recipes" "recipes")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/EngineReader.cmake")
laplace_define_engine_reader("${CMAKE_CURRENT_SOURCE_DIR}/src")

set(ENGINE_QUERY_SOURCES
    src/db/claim_read.c
    src/core/fills.c
    src/forward/pull.c
    src/forward/pull_degrees.c
    src/forward/firmware.c
    src/forward/forward.c
    src/forward/program_couple.c
    src/forward/program.c
    src/forward/ooda.h
    src/forward/ooda.c
    src/core/bench.c
    src/db/provenance.c
    src/forward/query.h
)
set(ENGINE_SOURCES
    src/core/main.c
    src/core/op.c
    src/chess/chess.c
    src/chess/chess_cache.c
    src/chess/chess_pgn.c
    src/chess/chess_syzygy.c
    src/core/table.c
    src/ingest/recipe.c
    src/ingest/recipe_decompose.c
    src/ingest/input_frame.c
    src/ingest/ingest_probe.c
    src/ingest/ingest.c
    src/db/db_metadata.c
    src/db/db.c
    src/ingest/file.c
    src/ingest/structure_arena.c
    src/ingest/structure_compile.c
    src/ingest/structure_decode.c
    src/ingest/structure_records.c
    src/ingest/structure.c
    src/ingest/say.c
    src/db/deploy.c
    src/db/forget.c
    src/core/tier0.c
    src/core/flags.c
    src/core/highway.c
    src/ingest/grammars.c
    src/codecs/media_image.c
    src/codecs/media_audio.c
    src/codecs/media_video.c
    src/codecs/parquet_decoder.c
    src/codecs/shapefile.c
    src/codecs/git_dag.c
    src/codecs/toolchain_witness.c
    src/core/literal_sql.c
    src/ingest/file_metadata.c
    src/ingest/recipe_compile.c
    src/ingest/recipe_says.c
    src/ingest/record_journal.c
    src/ingest/record_stream.c
    src/ingest/context_stream.c
    src/ingest/record_select.c
    src/ingest/record_operands.c
    src/ingest/record_diagnostics.c
    src/ingest/record_values.c
    src/ingest/record_things.c
    src/ingest/record_surface.c
    src/core/reference_html.c
    src/ingest/record_output.c
)
list(REMOVE_ITEM ENGINE_SOURCES src/core/main.c)
if(TARGET Laplace::model)
  list(APPEND ENGINE_SOURCES src/model/model.c src/model/gguf_writer.c src/model/mold.c)
else()
  list(APPEND ENGINE_SOURCES src/model/model_none.c src/model/gguf_writer.c src/model/mold.c)
endif()

add_library(laplace_media_tp STATIC src/codecs/third_party/media_impl.c)
set_target_properties(laplace_media_tp PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(laplace_media_tp PRIVATE ${LP_C_FLAGS})
if(MSVC)
  target_compile_options(laplace_media_tp PRIVATE /w $<$<C_COMPILER_ID:IntelLLVM>:/clang:-w>)
else()
  target_compile_options(laplace_media_tp PRIVATE -w)
endif()

find_package(zstd CONFIG QUIET HINTS "D:/Libraries/zstd/1.5.7/lib/cmake/zstd" "$ENV{ZSTD_ROOT}/lib/cmake/zstd")
if(TARGET zstd::libzstd_static)
  set(LAPLACE_ZSTD_TARGET zstd::libzstd_static)
elseif(TARGET zstd::libzstd_shared)
  set(LAPLACE_ZSTD_TARGET zstd::libzstd_shared)
else()
  find_library(LAPLACE_ZSTD_LIB NAMES zstd zstd_static HINTS "D:/Libraries/zstd/1.5.7/lib" REQUIRED)
  set(LAPLACE_ZSTD_TARGET "${LAPLACE_ZSTD_LIB}")
endif()

if(WIN32)
  find_library(LAPLACE_ZLIB NAMES z zlib HINTS ENV LAPLACE_ZLIB_DIR PATH_SUFFIXES lib REQUIRED)
endif()
add_library(laplace_engine_core OBJECT ${ENGINE_SOURCES} ${ENGINE_QUERY_SOURCES})
set_target_properties(laplace_engine_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(laplace_engine_core PRIVATE ${LP_C_FLAGS} -Wno-missing-field-initializers)
target_include_directories(laplace_engine_core PRIVATE
  src src/core src/db src/ingest src/codecs src/forward src/model src/chess
  ${CMAKE_CURRENT_BINARY_DIR} ${LAPLACE_ICU_DIR}/include ${LAPLACE_PG_DIR}/include)
target_link_libraries(laplace_engine_core PRIVATE
  laplace_engine_reader Laplace::text ts_runtime OpenMP::OpenMP_C ${LAPLACE_ZSTD_TARGET})
if(TARGET Laplace::model)
  target_link_libraries(laplace_engine_core PRIVATE Laplace::model)
endif()
if(TARGET Laplace::linalg)
  target_link_libraries(laplace_engine_core PRIVATE Laplace::linalg)
endif()
if(WIN32)
  get_filename_component(LAPLACE_ZLIB_DIR ${LAPLACE_ZLIB} DIRECTORY)
  target_include_directories(laplace_engine_core PRIVATE ${LAPLACE_ZLIB_DIR}/../include)
endif()
if(NOT TARGET zstd::libzstd_static AND NOT TARGET zstd::libzstd_shared AND EXISTS "D:/Libraries/zstd/1.5.7/include")
  target_include_directories(laplace_engine_core PRIVATE "D:/Libraries/zstd/1.5.7/include")
elseif(EXISTS "D:/Libraries/zstd/1.5.7/include")
  target_include_directories(laplace_engine_core PRIVATE "D:/Libraries/zstd/1.5.7/include")
endif()

add_executable(laplace_engine src/core/main.c $<TARGET_OBJECTS:laplace_engine_core>)
laplace_utf8_executable(laplace_engine)
if(WIN32)
  target_include_directories(laplace_engine PRIVATE ${LAPLACE_ZLIB_DIR}/../include)
endif()
set_target_properties(laplace_engine PROPERTIES OUTPUT_NAME laplace)
target_include_directories(laplace_engine PRIVATE
  src src/core src/db src/ingest src/codecs src/forward src/model src/chess
  ${LAPLACE_ICU_DIR}/include ${LAPLACE_PG_DIR}/include)
target_link_directories(laplace_engine PRIVATE ${LAPLACE_PG_DIR}/lib)
target_compile_options(laplace_engine PRIVATE ${LP_C_FLAGS} -Wno-missing-field-initializers)
# A reading contract identifies source, not a compiler's executable bytes (PE/ELF
# timestamps, linkage and build paths differ between standalone and embedded builds).
# Watch all reader/native inputs, normalize checkout line endings, and frame each
# relative pathname and digest so a source change still invalidates the OS precheck.
include(cmake/ReaderContract.cmake)
laplace_reader_source_id(LAPLACE_READER_SOURCE_ID "${CMAKE_CURRENT_SOURCE_DIR}" "${LaplaceNative_SOURCE_ID}" "${LAPLACE_TREESITTER}/lib" "${ENGINE_QUERY_SOURCES};src/db/read_text.c")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/laplace_reader_source.h"
     CONTENT "#define LAPLACE_READER_SOURCE_ID \"${LAPLACE_READER_SOURCE_ID}\"\n")
configure_file(src/core/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/laplace_config.h @ONLY)
target_include_directories(laplace_engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
if(NOT WIN32)
  add_executable(input_frame_test EXCLUDE_FROM_ALL tests/input-frame.c src/ingest/input_frame.c)
  target_include_directories(input_frame_test PRIVATE src/ingest)
  target_compile_options(input_frame_test PRIVATE ${LP_C_FLAGS} -UNDEBUG)
  target_link_libraries(input_frame_test PRIVATE Laplace::os)
  add_executable(presence_frontier EXCLUDE_FROM_ALL tests/presence-frontier.c src/db/db_metadata.c src/db/db.c)
  target_include_directories(presence_frontier PRIVATE src src/core src/db src/ingest src/codecs src/forward src/model src/chess
    ${CMAKE_CURRENT_BINARY_DIR} ${LAPLACE_ICU_DIR}/include ${LAPLACE_PG_DIR}/include)
  target_compile_options(presence_frontier PRIVATE ${LP_C_FLAGS} -UNDEBUG
    -ffunction-sections -fdata-sections -Wno-unused-function -Wno-missing-field-initializers)
  target_link_options(presence_frontier PRIVATE -Wl,--gc-sections)
  target_link_libraries(presence_frontier PRIVATE laplace_engine_reader)
endif()
if(WIN32)
  # libpq's import library and zlib's from their installs (Laplace-Operations dependencies.tsv); the C runtime and
  # Win32 give what dl, m and pthread give on Linux.
  target_link_libraries(laplace_engine PRIVATE laplace_engine_reader laplace_media_tp Laplace::text ts_runtime ${LAPLACE_ZLIB} ${LAPLACE_ZSTD_TARGET})
else()
  target_link_libraries(laplace_engine PRIVATE laplace_engine_reader laplace_media_tp Laplace::text ts_runtime z ${LAPLACE_ZSTD_TARGET} m)
endif()
if(TARGET Laplace::model)
  target_link_libraries(laplace_engine PRIVATE Laplace::model)
  if(TARGET Laplace::linalg)
    target_link_libraries(laplace_engine PRIVATE Laplace::linalg)
    set_target_properties(laplace_engine PROPERTIES LINKER_LANGUAGE CXX)
  endif()
  set(ENGINE_MKL_RPATH ";$ENV{MKLROOT}/lib")
endif()
set_target_properties(laplace_engine PROPERTIES BUILD_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib${ENGINE_MKL_RPATH}"
                                                INSTALL_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib${ENGINE_MKL_RPATH}")
if(NOT WIN32)
  target_link_options(laplace_engine PRIVATE -rdynamic)
endif()
if(WIN32 AND PROJECT_IS_TOP_LEVEL)
  # The process a source is ingested in (os_run_logged): how it is started, what it inherits, how its end is read.
  enable_testing()
  add_executable(os_run tests/os_run.c)
  laplace_utf8_executable(os_run)
  target_compile_options(os_run PRIVATE ${LP_C_FLAGS})
  target_link_libraries(os_run PRIVATE Laplace::os)
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
add_executable(pg_plan_lifecycle EXCLUDE_FROM_ALL tests/pg-plan-lifecycle.c)
laplace_utf8_executable(pg_plan_lifecycle)
target_include_directories(pg_plan_lifecycle PRIVATE src/core src/ingest ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(pg_plan_lifecycle PRIVATE ${LP_C_FLAGS} -UNDEBUG)
target_link_libraries(pg_plan_lifecycle PRIVATE laplace_engine_reader)

include(GNUInstallDirs)
add_executable(reader_depth EXCLUDE_FROM_ALL tests/reader-depth.c)
laplace_utf8_executable(reader_depth)
target_include_directories(reader_depth PRIVATE src/core src/ingest ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(reader_depth PRIVATE ${LP_C_FLAGS} -UNDEBUG)
target_link_libraries(reader_depth PRIVATE laplace_engine_reader Laplace::text)
# The reader's chess (chess_rules.c): the IDs a reader such as Laplace-MCP gives a FEN and a line (tests/opening-lines.sh).
add_executable(chess_positions EXCLUDE_FROM_ALL tests/chess-positions.c)
laplace_utf8_executable(chess_positions)
target_include_directories(chess_positions PRIVATE src/core src/ingest ${CMAKE_CURRENT_BINARY_DIR})
target_compile_options(chess_positions PRIVATE ${LP_C_FLAGS} -UNDEBUG)
target_compile_definitions(chess_positions PRIVATE "LAPLACE_GRAMMARS_DEFAULT=\"${LAPLACE_GRAMMARS}\"")
target_link_libraries(chess_positions PRIVATE laplace_engine_reader Laplace::text)
# One relocatable recipe runtime is consumed by the modular CLI and managed hosts.
# Its executable is the canonical Engine, with the same compiled grammars and estate.
install(TARGETS laplace_engine EXPORT LaplaceEngineTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT LaplaceRecipeRuntime)
set_target_properties(laplace_engine PROPERTIES EXPORT_NAME engine)
# The package LaplaceEngine: the program (Laplace::engine) and the reader a service links (Laplace::engine_reader,
# ::ts_runtime) with its headers, found by find_package(LaplaceEngine CONFIG REQUIRED), which finds the
# LaplaceNative package the Engine was built against.
if(PROJECT_IS_TOP_LEVEL)
  include(CMakePackageConfigHelpers)
  add_library(laplace_engine_abi SHARED src/forward/engine_abi.c $<TARGET_OBJECTS:laplace_engine_core>)
  set_target_properties(laplace_engine_abi PROPERTIES EXPORT_NAME engine_abi POSITION_INDEPENDENT_CODE ON)
  target_compile_definitions(laplace_engine_abi PRIVATE LP_ENGINE_ABI_EXPORTS)
  target_compile_options(laplace_engine_abi PRIVATE ${LP_C_FLAGS} -Wno-missing-field-initializers)
  target_include_directories(laplace_engine_abi
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/forward>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/laplace-engine>
    PRIVATE
      src src/core src/db src/ingest src/codecs src/forward src/model src/chess
      ${CMAKE_CURRENT_BINARY_DIR} ${LAPLACE_ICU_DIR}/include ${LAPLACE_PG_DIR}/include)
  if(WIN32)
    target_include_directories(laplace_engine_abi PRIVATE ${LAPLACE_ZLIB_DIR}/../include)
    target_link_libraries(laplace_engine_abi PRIVATE laplace_engine_reader laplace_media_tp Laplace::text ts_runtime ${LAPLACE_ZLIB} ${LAPLACE_ZSTD_TARGET})
  else()
    target_link_libraries(laplace_engine_abi PRIVATE laplace_engine_reader laplace_media_tp Laplace::text ts_runtime z ${LAPLACE_ZSTD_TARGET} m)
  endif()
  target_link_directories(laplace_engine_abi PRIVATE ${LAPLACE_PG_DIR}/lib)
  if(TARGET Laplace::model)
    target_link_libraries(laplace_engine_abi PRIVATE Laplace::model)
    if(TARGET Laplace::linalg)
      target_link_libraries(laplace_engine_abi PRIVATE Laplace::linalg)
      set_target_properties(laplace_engine_abi PROPERTIES LINKER_LANGUAGE CXX)
    endif()
  endif()
  set_target_properties(laplace_engine_abi PROPERTIES
    BUILD_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib${ENGINE_MKL_RPATH}"
    INSTALL_RPATH "${LAPLACE_ICU_DIR}/lib;${LAPLACE_PG_DIR}/lib${ENGINE_MKL_RPATH}")

  set(LAPLACE_ENGINE_PACKAGE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/LaplaceEngine)
  install(TARGETS laplace_engine_reader ts_runtime laplace_engine_abi EXPORT LaplaceEngineTargets
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT LaplaceEngineReader
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT LaplaceEngineReader
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT LaplaceEngineReader)
  list(TRANSFORM LAPLACE_ENGINE_READER_HEADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/src/" OUTPUT_VARIABLE _reader_headers)
  install(FILES ${_reader_headers} "${CMAKE_CURRENT_SOURCE_DIR}/src/forward/engine_abi.h"
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/laplace-engine COMPONENT LaplaceEngineReader)
  install(FILES "${LAPLACE_TREESITTER}/lib/include/tree_sitter/api.h"
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tree_sitter COMPONENT LaplaceEngineReader)
  install(EXPORT LaplaceEngineTargets NAMESPACE Laplace:: DESTINATION ${LAPLACE_ENGINE_PACKAGE_DIR}
          COMPONENT LaplaceEngineReader)
  file(TO_CMAKE_PATH "${LaplaceNative_DIR}" LAPLACE_NATIVE_PACKAGE_DIR)   # the Native package this Engine was built against
  configure_package_config_file(cmake/LaplaceEngineConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/LaplaceEngineConfig.cmake
                                INSTALL_DESTINATION ${LAPLACE_ENGINE_PACKAGE_DIR})
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/LaplaceEngineConfigVersion.cmake COMPATIBILITY ExactVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/LaplaceEngineConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/LaplaceEngineConfigVersion.cmake
          DESTINATION ${LAPLACE_ENGINE_PACKAGE_DIR} COMPONENT LaplaceEngineReader)
endif()
option(LAPLACE_INSTALL_RECIPE_RESOURCES "Install resource inputs through CMake (standalone runtime)" ON)
if(LAPLACE_INSTALL_RECIPE_RESOURCES)
if(WIN32)
  file(REAL_PATH "${CMAKE_C_COMPILER}" _recipe_compiler)
  get_filename_component(_recipe_compiler_bin "${_recipe_compiler}" DIRECTORY)
  set(_recipe_runtime_search "${LAPLACE_ICU_DIR}/bin64;${LAPLACE_ICU_DIR}/bin;${LAPLACE_PG_DIR}/bin;${_recipe_compiler_bin}")
  foreach(_root MKLROOT TBBROOT)
    if(DEFINED ENV{${_root}} AND NOT "$ENV{${_root}}" STREQUAL "")
      list(APPEND _recipe_runtime_search "$ENV{${_root}}/bin")
    endif()
  endforeach()
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
    # MKL loads dispatch kernels dynamically. Discover peers beside the actual
    # resolved runtime, including nested builds whose parent owns MKL discovery
    # and does not export MKLROOT into this configure process.
    foreach(_library IN LISTS _libraries)
      get_filename_component(_name "${_library}" NAME)
      if(_name MATCHES "^mkl_rt([.][0-9]+)?[.]dll$")
        get_filename_component(_directory "${_library}" DIRECTORY)
        file(GLOB _dispatch "${_directory}/mkl_*.dll")
        list(APPEND _libraries ${_dispatch})
      endif()
    endforeach()
    list(FILTER _libraries EXCLUDE REGEX "^$")
    list(REMOVE_DUPLICATES _libraries)
    file(MAKE_DIRECTORY "${CMAKE_INSTALL_PREFIX}/bin")
    foreach(_library IN LISTS _libraries)
      get_filename_component(_name "${_library}" NAME)
      set(_destination "${CMAKE_INSTALL_PREFIX}/bin/${_name}")
      # Intel's imported names can be symlinks. Compare their actual bytes
      # before COPY_FILE, which can still try to replace an identical peer.
      # Repeated builds must leave an already installed runtime file alone.
      if(EXISTS "${_destination}")
        file(SHA256 "${_library}" _source_sha)
        file(SHA256 "${_destination}" _destination_sha)
        if(_source_sha STREQUAL _destination_sha)
          continue()
        endif()
      endif()
      # Preserve the imported DLL name while copying the symlink's bytes.
      # Ordinary Windows publishing does not need symlink creation privileges.
      file(COPY_FILE "${_library}" "${_destination}" ONLY_IF_DIFFERENT)
    endforeach()
  ]=])
  # Normalize and quote paths before embedding them in the install script.
  file(TO_CMAKE_PATH "${_recipe_runtime_search}" _recipe_runtime_search)
  string(REPLACE ";" "\" \"" _recipe_runtime_search "${_recipe_runtime_search}")
  set(_recipe_runtime_search "\"${_recipe_runtime_search}\"")
  string(CONFIGURE "${_recipe_runtime_install}" _recipe_runtime_install @ONLY)
  install(CODE "${_recipe_runtime_install}" COMPONENT LaplaceRecipeRuntime)
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PruneRecipeResources.cmake")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PruneRecipeResources.cmake" _recipe_resource_prune)
install(CODE "
  set(RECIPE_SOURCE \"${CMAKE_CURRENT_SOURCE_DIR}/recipes\")
  set(RECIPE_TARGET \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/laplace/recipes\")
  ${_recipe_resource_prune}
" COMPONENT LaplaceRecipeRuntime)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/recipes/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/laplace/recipes" COMPONENT LaplaceRecipeRuntime
        FILES_MATCHING PATTERN "*.recipe" PATTERN "*.format" PATTERN "*.LICENSE" PATTERN "source" PATTERN "order")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/recipes/vocabularies/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/laplace/recipes/vocabularies" COMPONENT LaplaceRecipeRuntime
        FILES_MATCHING PATTERN "*.tsv")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/grammar-sources.tsv"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/laplace" COMPONENT LaplaceRecipeRuntime)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/firmware/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/laplace/firmware" COMPONENT LaplaceRecipeRuntime
        FILES_MATCHING PATTERN "*.firmware")
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
endif()

add_executable(claim_decode EXCLUDE_FROM_ALL tests/claim-decode.c src/db/claim_read.c)
target_include_directories(claim_decode PRIVATE src/core src/db src/ingest src/forward ${CMAKE_CURRENT_BINARY_DIR} ${LAPLACE_PG_DIR}/include)
target_compile_options(claim_decode PRIVATE ${LP_C_FLAGS} -UNDEBUG)
target_link_libraries(claim_decode PRIVATE Laplace::os Laplace::laplace_static BLAKE3::blake3 OpenMP::OpenMP_C)
if(NOT WIN32)
  target_link_libraries(claim_decode PRIVATE m)
endif()

if(PROJECT_IS_TOP_LEVEL)
  if(TARGET Laplace::model)
    add_executable(codec_mold_test tests/codec-mold-test.c $<TARGET_OBJECTS:laplace_engine_core>)
    laplace_utf8_executable(codec_mold_test)
    target_compile_options(codec_mold_test PRIVATE ${LP_C_FLAGS} -UNDEBUG)
    target_include_directories(codec_mold_test PRIVATE
      src src/core src/db src/ingest src/codecs src/forward src/model src/chess
      ${CMAKE_CURRENT_BINARY_DIR} ${LAPLACE_ICU_DIR}/include ${LAPLACE_PG_DIR}/include)
    if(WIN32)
      target_include_directories(codec_mold_test PRIVATE ${LAPLACE_ZLIB_DIR}/../include)
    endif()
    target_link_directories(codec_mold_test PRIVATE ${LAPLACE_PG_DIR}/lib)
    if(WIN32)
      target_link_libraries(codec_mold_test PRIVATE laplace_engine_reader laplace_media_tp Laplace::text Laplace::model ts_runtime ${LAPLACE_ZLIB} ${LAPLACE_ZSTD_TARGET})
    else()
      target_link_libraries(codec_mold_test PRIVATE laplace_engine_reader laplace_media_tp Laplace::text Laplace::model ts_runtime z ${LAPLACE_ZSTD_TARGET} m)
    endif()
    if(TARGET Laplace::linalg)
      target_link_libraries(codec_mold_test PRIVATE Laplace::linalg)
      set_target_properties(codec_mold_test PROPERTIES LINKER_LANGUAGE CXX)
    endif()
    enable_testing()
    add_test(NAME codec_mold_test COMMAND codec_mold_test)
  endif()
  add_executable(forward_ooda_test tests/forward-ooda-test.c $<TARGET_OBJECTS:laplace_engine_core>)
  laplace_utf8_executable(forward_ooda_test)
  target_compile_options(forward_ooda_test PRIVATE ${LP_C_FLAGS} -UNDEBUG)
  target_include_directories(forward_ooda_test PRIVATE
    src src/core src/db src/ingest src/codecs src/forward src/model src/chess
    ${CMAKE_CURRENT_BINARY_DIR} ${LAPLACE_ICU_DIR}/include ${LAPLACE_PG_DIR}/include)
  if(WIN32)
    target_include_directories(forward_ooda_test PRIVATE ${LAPLACE_ZLIB_DIR}/../include)
  endif()
  target_link_directories(forward_ooda_test PRIVATE ${LAPLACE_PG_DIR}/lib)
  if(WIN32)
    target_link_libraries(forward_ooda_test PRIVATE laplace_engine_reader laplace_media_tp Laplace::text ts_runtime ${LAPLACE_ZLIB} ${LAPLACE_ZSTD_TARGET})
  else()
    target_link_libraries(forward_ooda_test PRIVATE laplace_engine_reader laplace_media_tp Laplace::text ts_runtime z ${LAPLACE_ZSTD_TARGET} m)
  endif()
  if(TARGET Laplace::model)
    target_link_libraries(forward_ooda_test PRIVATE Laplace::model)
    if(TARGET Laplace::linalg)
      target_link_libraries(forward_ooda_test PRIVATE Laplace::linalg)
      set_target_properties(forward_ooda_test PROPERTIES LINKER_LANGUAGE CXX)
    endif()
  endif()
  add_dependencies(forward_ooda_test laplace_engine_abi)
  target_compile_definitions(forward_ooda_test PRIVATE "LAPLACE_ENGINE_ABI_DLL=\"$<TARGET_FILE:laplace_engine_abi>\"")
  enable_testing()
  add_test(NAME forward_ooda_test COMMAND forward_ooda_test)
  set(Python3_FIND_REGISTRY LAST)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    enable_testing()
    add_test(NAME sql_contract_gate COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tests/sql-contract-gate.py")
  endif()
endif()
