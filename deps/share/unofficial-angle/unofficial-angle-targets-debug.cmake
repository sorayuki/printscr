#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "unofficial::angle::libEGL" for configuration "Debug"
set_property(TARGET unofficial::angle::libEGL APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(unofficial::angle::libEGL PROPERTIES
  IMPORTED_IMPLIB_DEBUG "${_IMPORT_PREFIX}/debug/lib/libEGL.lib"
  IMPORTED_LINK_DEPENDENT_LIBRARIES_DEBUG "unofficial::angle::libGLESv2"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/debug/bin/libEGL.dll"
  )

list(APPEND _cmake_import_check_targets unofficial::angle::libEGL )
list(APPEND _cmake_import_check_files_for_unofficial::angle::libEGL "${_IMPORT_PREFIX}/debug/lib/libEGL.lib" "${_IMPORT_PREFIX}/debug/bin/libEGL.dll" )

# Import target "unofficial::angle::libGLESv2" for configuration "Debug"
set_property(TARGET unofficial::angle::libGLESv2 APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(unofficial::angle::libGLESv2 PROPERTIES
  IMPORTED_IMPLIB_DEBUG "${_IMPORT_PREFIX}/debug/lib/libGLESv2.lib"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/debug/bin/libGLESv2.dll"
  )

list(APPEND _cmake_import_check_targets unofficial::angle::libGLESv2 )
list(APPEND _cmake_import_check_files_for_unofficial::angle::libGLESv2 "${_IMPORT_PREFIX}/debug/lib/libGLESv2.lib" "${_IMPORT_PREFIX}/debug/bin/libGLESv2.dll" )

# Import target "unofficial::angle::libANGLE" for configuration "Debug"
set_property(TARGET unofficial::angle::libANGLE APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(unofficial::angle::libANGLE PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C;CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/debug/lib/ANGLE.lib"
  )

list(APPEND _cmake_import_check_targets unofficial::angle::libANGLE )
list(APPEND _cmake_import_check_files_for_unofficial::angle::libANGLE "${_IMPORT_PREFIX}/debug/lib/ANGLE.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
