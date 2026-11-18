# The Engine's platform layer and database reader as libraries: Laplace::engine_os, Laplace::engine_reader and the
# tree-sitter runtime Laplace::ts_runtime, built by Laplace-Engine and installed with its package (LaplaceEngine), so a
# reader such as Laplace-MCP links them by find_package and compiles none of the Engine's sources. Their public header
# is reader.h (with os.h, pg_plan.h, walk.h, chess_rules.h, chess_tablebase.h), installed under include/laplace-engine.

set(LAPLACE_ENGINE_READER_HEADERS reader.h os.h pg_plan.h walk.h chess_rules.h chess_tablebase.h)

function(laplace_define_engine_reader engine_src)
  if(NOT TARGET ts_runtime)
    add_library(ts_runtime STATIC "${LAPLACE_TREESITTER}/lib/src/lib.c")
    set_target_properties(ts_runtime PROPERTIES EXPORT_NAME ts_runtime)
    target_include_directories(ts_runtime PUBLIC $<BUILD_INTERFACE:${LAPLACE_TREESITTER}/lib/include> $<INSTALL_INTERFACE:include>
                               PRIVATE "${LAPLACE_TREESITTER}/lib/src" "${LAPLACE_TREESITTER}/lib/src/wasm")
    if(WIN32)
      target_compile_options(ts_runtime PRIVATE /w /O2)
    else()
      target_compile_options(ts_runtime PRIVATE -w -O3 -fPIC)
    endif()
    add_library(Laplace::ts_runtime ALIAS ts_runtime)
  endif()

  foreach(f os.c read.c read_ids.c read_text.c pg.c pg_plan.c walk.c chess_rules.c chess_tablebase.c ${LAPLACE_ENGINE_READER_HEADERS})
    if(NOT EXISTS "${engine_src}/${f}")
      message(FATAL_ERROR "laplace_define_engine_reader: missing ${f} at '${engine_src}'")
    endif()
  endforeach()

  if(NOT TARGET laplace_engine_os)
    add_library(laplace_engine_os STATIC "${engine_src}/os.c")
    set_target_properties(laplace_engine_os PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME engine_os)
    if(WIN32)
      target_include_directories(laplace_engine_os BEFORE PUBLIC
        $<BUILD_INTERFACE:${engine_src}/win> $<INSTALL_INTERFACE:include/laplace-engine/win>)
      target_compile_definitions(laplace_engine_os PUBLIC _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    endif()
    target_include_directories(laplace_engine_os PUBLIC $<BUILD_INTERFACE:${engine_src}> $<INSTALL_INTERFACE:include/laplace-engine>)
    target_compile_options(laplace_engine_os PRIVATE ${LP_C_FLAGS})
    if(NOT WIN32)
      target_link_libraries(laplace_engine_os PUBLIC dl pthread)
    endif()
    add_library(Laplace::engine_os ALIAS laplace_engine_os)
  endif()

  if(NOT TARGET laplace_engine_reader)
    add_library(laplace_engine_reader STATIC
      "${engine_src}/read.c"
      "${engine_src}/read_ids.c"
      "${engine_src}/read_text.c"
      "${engine_src}/pg.c"
      "${engine_src}/pg_plan.c"
      "${engine_src}/walk.c"
      "${engine_src}/chess_rules.c"
      "${engine_src}/chess_tablebase.c")
    set_target_properties(laplace_engine_reader PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME engine_reader)
    target_include_directories(laplace_engine_reader PUBLIC
      $<BUILD_INTERFACE:${engine_src}> $<INSTALL_INTERFACE:include/laplace-engine>
      "${LAPLACE_PG_DIR}/include")
    target_compile_options(laplace_engine_reader PRIVATE ${LP_C_FLAGS})
    target_link_libraries(laplace_engine_reader PUBLIC
      laplace_engine_os
      Laplace::text
      Laplace::laplace_static
      Laplace::math
      BLAKE3::blake3
      OpenMP::OpenMP_C
      ts_runtime)
    if(WIN32)
      target_link_libraries(laplace_engine_reader PUBLIC "${LAPLACE_PG_DIR}/lib/libpq.lib")
    else()
      target_link_directories(laplace_engine_reader PUBLIC "${LAPLACE_PG_DIR}/lib")
      target_link_libraries(laplace_engine_reader PUBLIC pq)
    endif()
    add_library(Laplace::engine_reader ALIAS laplace_engine_reader)
  endif()
endfunction()
