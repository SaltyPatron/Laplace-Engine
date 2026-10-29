# Installed recipe resources belong to the package. Overlay installation must
# not leave removed declarations available to the source/recipe selector.
if(NOT DEFINED RECIPE_SOURCE OR NOT DEFINED RECIPE_TARGET)
  message(FATAL_ERROR "Recipe publication requires its source and target directories")
endif()
cmake_path(ABSOLUTE_PATH RECIPE_SOURCE NORMALIZE)
cmake_path(ABSOLUTE_PATH RECIPE_TARGET NORMALIZE)
if(RECIPE_SOURCE STREQUAL RECIPE_TARGET OR NOT IS_DIRECTORY "${RECIPE_SOURCE}")
  message(FATAL_ERROR "Recipe publication requires a distinct existing source")
endif()
if(IS_DIRECTORY "${RECIPE_TARGET}")
  file(GLOB_RECURSE _installed LIST_DIRECTORIES false RELATIVE "${RECIPE_TARGET}" "${RECIPE_TARGET}/*")
  foreach(_relative IN LISTS _installed)
    if(_relative MATCHES "[.](recipe|format|LICENSE)$|(^|/)(source|order)$|^vocabularies/.*[.]tsv$")
      if(NOT EXISTS "${RECIPE_SOURCE}/${_relative}")
        # Remove individual package resources, never a user's unrelated files
        # or a directory reached through a computed recursive deletion.
        file(REMOVE "${RECIPE_TARGET}/${_relative}")
        message(STATUS "Removed obsolete recipe resource: ${_relative}")
      endif()
    endif()
  endforeach()
endif()
