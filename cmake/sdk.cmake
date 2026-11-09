include(CMakePackageConfigHelpers)

# Export the actual archives and their complete link graph. Static consumers
# must not depend on this checkout, its build directories, or linker guesses.
set(SYRNCK_SDK_PRIVATE_TARGETS monocypher utf8proc blake3 libzstd_static)
if(SYRNCK_BUILD_NETWORK)
  list(APPEND SYRNCK_SDK_PRIVATE_TARGETS libcurl_static)
endif()
foreach(name core git ffi ffi_static)
  set_target_properties(syrnck_${name} PROPERTIES EXPORT_NAME ${name})
  if(NOT TARGET Syrnck::${name})
    add_library(Syrnck::${name} ALIAS syrnck_${name})
  endif()
endforeach()
if(SYRNCK_BUILD_NETWORK)
  foreach(name net git_net)
    set_target_properties(syrnck_${name} PROPERTIES EXPORT_NAME ${name})
    add_library(Syrnck::${name} ALIAS syrnck_${name})
  endforeach()
endif()
if(TARGET syrnck)
  set_target_properties(syrnck PROPERTIES EXPORT_NAME cli)
endif()
install(TARGETS ${SYRNCK_TARGETS} ${SYRNCK_SDK_PRIVATE_TARGETS} EXPORT SyrnckTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
set(SYRNCK_CMAKE_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/Syrnck")
configure_package_config_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SyrnckConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/SyrnckConfig.cmake"
  INSTALL_DESTINATION "${SYRNCK_CMAKE_INSTALL_DIR}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/SyrnckConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion)
install(EXPORT SyrnckTargets NAMESPACE Syrnck:: DESTINATION "${SYRNCK_CMAKE_INSTALL_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/SyrnckConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/SyrnckConfigVersion.cmake" DESTINATION "${SYRNCK_CMAKE_INSTALL_DIR}")
