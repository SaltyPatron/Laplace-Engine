# Shared Engine platform and database-reader targets consumed by Laplace-Engine,
# its test executables, and Laplace-MCP without duplicating source lists or
# platform link boilerplate.

function(laplace_define_engine_reader engine_src)
  if(NOT TARGET ts_runtime)
    add_library(ts_runtime STATIC EXCLUDE_FROM_ALL "${LAPLACE_TREESITTER}/lib/src/lib.c")
    target_include_directories(ts_runtime PUBLIC "${LAPLACE_TREESITTER}/lib/include"
                               PRIVATE "${LAPLACE_TREESITTER}/lib/src" "${LAPLACE_TREESITTER}/lib/src/wasm")
    if(WIN32)
      target_compile_options(ts_runtime PRIVATE /w /O2)
    else()
      target_compile_options(ts_runtime PRIVATE -w -O3 -fPIC)
    endif()
  endif()

  foreach(f os.c os.h read.c read_ids.c read_text.c pg.c pg_plan.c pg_plan.h walk.c walk.h engine.h chess_rules.c chess_rules.h chess_tablebase.c chess_tablebase.h)
    if(NOT EXISTS "${engine_src}/${f}")
      message(FATAL_ERROR "laplace_define_engine_reader: missing ${f} at '${engine_src}'")
    endif()
  endforeach()

  if(NOT TARGET laplace_engine_os)
    add_library(laplace_engine_os STATIC EXCLUDE_FROM_ALL "${engine_src}/os.c")
    set_target_properties(laplace_engine_os PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(WIN32)
      target_include_directories(laplace_engine_os BEFORE PUBLIC "${engine_src}/win")
      target_compile_definitions(laplace_engine_os PUBLIC _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    endif()
    target_include_directories(laplace_engine_os PUBLIC "${engine_src}" PRIVATE "${LAPLACE_NATIVE}/src")
    target_compile_options(laplace_engine_os PRIVATE ${LP_C_FLAGS})
    if(NOT WIN32)
      target_link_libraries(laplace_engine_os PUBLIC dl pthread)
    endif()
  endif()

  if(NOT TARGET laplace_engine_reader)
    add_library(laplace_engine_reader STATIC EXCLUDE_FROM_ALL
      "${engine_src}/read.c"
      "${engine_src}/read_ids.c"
      "${engine_src}/read_text.c"
      "${engine_src}/pg.c"
      "${engine_src}/pg_plan.c"
      "${engine_src}/walk.c"
      "${engine_src}/chess_rules.c"
      "${engine_src}/chess_tablebase.c")
    set_target_properties(laplace_engine_reader PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(laplace_engine_reader PUBLIC
      "${engine_src}"
      "${LAPLACE_NATIVE}/include"
      "${LAPLACE_PG_DIR}/include")
    target_compile_options(laplace_engine_reader PRIVATE ${LP_C_FLAGS})
    target_link_libraries(laplace_engine_reader PUBLIC
      laplace_engine_os
      laplace_text
      laplace_static
      lp_crmath
      BLAKE3::blake3
      OpenMP::OpenMP_C
      ts_runtime)
    if(WIN32)
      target_link_libraries(laplace_engine_reader PUBLIC "${LAPLACE_PG_DIR}/lib/libpq.lib")
    else()
      target_link_directories(laplace_engine_reader PUBLIC "${LAPLACE_PG_DIR}/lib")
      target_link_libraries(laplace_engine_reader PUBLIC pq)
    endif()
  endif()
endfunction()
