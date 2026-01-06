#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "unofficial::angle::libEGL" for configuration "Release"
set_property(TARGET unofficial::angle::libEGL APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(unofficial::angle::libEGL PROPERTIES
  IMPORTED_IMPLIB_RELEASE "${_IMPORT_PREFIX}/lib/libEGL.lib"
  IMPORTED_LINK_DEPENDENT_LIBRARIES_RELEASE "unofficial::angle::libGLESv2"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/libEGL.dll"
  )

list(APPEND _cmake_import_check_targets unofficial::angle::libEGL )
list(APPEND _cmake_import_check_files_for_unofficial::angle::libEGL "${_IMPORT_PREFIX}/lib/libEGL.lib" "${_IMPORT_PREFIX}/bin/libEGL.dll" )

# Import target "unofficial::angle::libGLESv2" for configuration "Release"
set_property(TARGET unofficial::angle::libGLESv2 APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(unofficial::angle::libGLESv2 PROPERTIES
  IMPORTED_IMPLIB_RELEASE "${_IMPORT_PREFIX}/lib/libGLESv2.lib"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/bin/libGLESv2.dll"
  )

list(APPEND _cmake_import_check_targets unofficial::angle::libGLESv2 )
list(APPEND _cmake_import_check_files_for_unofficial::angle::libGLESv2 "${_IMPORT_PREFIX}/lib/libGLESv2.lib" "${_IMPORT_PREFIX}/bin/libGLESv2.dll" )

# Import target "unofficial::angle::libANGLE" for configuration "Release"
set_property(TARGET unofficial::angle::libANGLE APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(unofficial::angle::libANGLE PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C;CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/ANGLE.lib"
  )

list(APPEND _cmake_import_check_targets unofficial::angle::libANGLE )
list(APPEND _cmake_import_check_files_for_unofficial::angle::libANGLE "${_IMPORT_PREFIX}/lib/ANGLE.lib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
