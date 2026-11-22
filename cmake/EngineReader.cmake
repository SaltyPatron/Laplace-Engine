# The Engine's database reader as libraries: Laplace::engine_reader and the tree-sitter runtime
# Laplace::ts_runtime, built by Laplace-Engine and installed with its package (LaplaceEngine), so a reader such as
# Laplace-MCP links them by find_package and compiles none of the Engine's sources (consuming Laplace::os from
# LaplaceNative). Their public header is reader.h (with pg_plan.h, pg_contract.h, sql_catalog.def, walk.h,
# chess_rules.h, chess_tablebase.h), installed under include/laplace-engine.

set(LAPLACE_ENGINE_READER_HEADERS
  db/reader.h db/pg_plan.h db/pg_contract.h db/sql_catalog.def db/walk.h
  chess/chess_rules.h chess/chess_tablebase.h chess/chess_search.h)

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

  foreach(f db/read.c db/read_ids.c db/read_text.c db/pg.c db/pg_plan.c db/pg_contract.c db/walk.c
            chess/chess_rules.c chess/chess_tablebase.c chess/chess_search.c ${LAPLACE_ENGINE_READER_HEADERS})
    if(NOT EXISTS "${engine_src}/${f}")
      message(FATAL_ERROR "laplace_define_engine_reader: missing ${f} at '${engine_src}'")
    endif()
  endforeach()

  if(NOT TARGET laplace_engine_reader)
    add_library(laplace_engine_reader STATIC
      "${engine_src}/db/read.c"
      "${engine_src}/db/read_ids.c"
      "${engine_src}/db/read_text.c"
      "${engine_src}/db/pg.c"
      "${engine_src}/db/pg_plan.c"
      "${engine_src}/db/pg_contract.c"
      "${engine_src}/db/walk.c"
      "${engine_src}/chess/chess_rules.c"
      "${engine_src}/chess/chess_tablebase.c"
      "${engine_src}/chess/chess_search.c")
    set_target_properties(laplace_engine_reader PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME engine_reader)
    target_include_directories(laplace_engine_reader
      PUBLIC
        $<BUILD_INTERFACE:${engine_src}/db>
        $<BUILD_INTERFACE:${engine_src}/chess>
        $<INSTALL_INTERFACE:include/laplace-engine>
        "${LAPLACE_PG_DIR}/include"
      PRIVATE
        "${engine_src}/core"
        "${engine_src}/ingest")
    target_compile_options(laplace_engine_reader PRIVATE ${LP_C_FLAGS})
    target_link_libraries(laplace_engine_reader PUBLIC
      Laplace::os
      Laplace::text
      Laplace::laplace_static
      Laplace::math
      BLAKE3::blake3
      OpenMP::OpenMP_C
      ts_runtime)
    if(WIN32)
      target_compile_definitions(laplace_engine_reader PUBLIC _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
      target_link_libraries(laplace_engine_reader PUBLIC "${LAPLACE_PG_DIR}/lib/libpq.lib")
    else()
      target_compile_definitions(laplace_engine_reader PUBLIC _GNU_SOURCE)
      target_link_directories(laplace_engine_reader PUBLIC "${LAPLACE_PG_DIR}/lib")
      target_link_libraries(laplace_engine_reader PUBLIC pq)
    endif()
    add_library(Laplace::engine_reader ALIAS laplace_engine_reader)
  endif()
endfunction()
