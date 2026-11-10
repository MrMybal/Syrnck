if(SYRNCK_BUILD_BEP_SERVICE)
  if(SYRNCK_SANITIZE)
    message(FATAL_ERROR "The BEP executable requires a nonsanitized native library; qualify the core sanitizer build separately")
  endif()
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  find_program(SYRNCK_GO_EXECUTABLE NAMES go PATHS "${CMAKE_CURRENT_SOURCE_DIR}/.tools/go/bin" REQUIRED)
  if(CMAKE_CONFIGURATION_TYPES)
    set(SYRNCK_BEP_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
  else()
    set(SYRNCK_BEP_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  set(SYRNCK_BEP_EXECUTABLE "${SYRNCK_BEP_BUILD_DIR}/syrnck-bep${CMAKE_EXECUTABLE_SUFFIX}")
  file(RELATIVE_PATH SYRNCK_BEP_LIBRARY_RELATIVE "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
  file(GLOB_RECURSE SYRNCK_BEP_GO_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/bindings/go/*.go"
    "${CMAKE_CURRENT_SOURCE_DIR}/bindings/syncthing/*.go")
  list(FILTER SYRNCK_BEP_GO_INPUTS EXCLUDE REGEX "_test[.]go$")
  file(GLOB SYRNCK_BEP_OPTIONAL_SUMS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bindings/go/go.sum")
  add_custom_command(OUTPUT "${SYRNCK_BEP_EXECUTABLE}" "${SYRNCK_BEP_BUILD_DIR}/bep-licenses/manifest.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_bep_service.py"
      --go "${SYRNCK_GO_EXECUTABLE}" --library "$<TARGET_FILE:syrnck_ffi>"
      --output "${SYRNCK_BEP_EXECUTABLE}" --runtime-library-relative "${SYRNCK_BEP_LIBRARY_RELATIVE}"
    DEPENDS syrnck_ffi ${SYRNCK_BEP_GO_INPUTS}
      "${CMAKE_CURRENT_SOURCE_DIR}/include/syrnck.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/bindings/go/go.mod" ${SYRNCK_BEP_OPTIONAL_SUMS}
      "${CMAKE_CURRENT_SOURCE_DIR}/bindings/syncthing/go.mod" "${CMAKE_CURRENT_SOURCE_DIR}/bindings/syncthing/go.sum"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_bep_service.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
  add_custom_target(syrnck_bep_service ALL DEPENDS "${SYRNCK_BEP_EXECUTABLE}"
    "${SYRNCK_BEP_BUILD_DIR}/bep-licenses/manifest.json")
  if(TARGET syrnck)
    add_dependencies(syrnck syrnck_bep_service)
  endif()
  if(TARGET syrnck_documentation)
    add_dependencies(syrnck_bep_service syrnck_documentation)
    # Both executables normally share one runtime directory. Order the copies
    # so Ninja cannot replace the same documentation files concurrently. Keep
    # the BEP copy for builds with a separately placed native CLI as well.
    if(TARGET syrnck_documentation_copy)
      add_dependencies(syrnck_bep_service syrnck_documentation_copy)
    endif()
    add_custom_command(TARGET syrnck_bep_service POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_directory
      "${SYRNCK_DOC_OUTPUT}" "${SYRNCK_BEP_BUILD_DIR}/Documentation" VERBATIM)
  endif()
  install(PROGRAMS "${SYRNCK_BEP_EXECUTABLE}" DESTINATION "${CMAKE_INSTALL_BINDIR}")
  install(DIRECTORY "${SYRNCK_BEP_BUILD_DIR}/bep-licenses/" DESTINATION "${SYRNCK_LICENSE_DIR}/bep")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/examples/bep-service.json"
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/bep-untrusted-service.json"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/Syrnck/examples")
  if(SYRNCK_BUILD_TESTS)
    add_test(NAME bep_distribution COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/bep_distribution.py"
      --cmake "${CMAKE_COMMAND}" --build "${CMAKE_CURRENT_BINARY_DIR}" --config "$<CONFIG>"
      --bindir "${CMAKE_INSTALL_BINDIR}" --licenses "${SYRNCK_LICENSE_DIR}/bep"
      --examples "${CMAKE_INSTALL_DATADIR}/Syrnck/examples"
      --executable "syrnck-bep${CMAKE_EXECUTABLE_SUFFIX}")
    set_tests_properties(bep_distribution PROPERTIES TIMEOUT 90 RESOURCE_LOCK install_distribution)
  endif()
endif()
