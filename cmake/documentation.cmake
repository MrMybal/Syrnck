set(SYRNCK_DOCUMENTATION_DEFAULT ${SYRNCK_BUILD_CLI})
if(SYRNCK_BUILD_BEP_SERVICE)
  set(SYRNCK_DOCUMENTATION_DEFAULT ON)
endif()
option(SYRNCK_BUILD_DOCUMENTATION "Build and package the local bilingual CyDoc manual" ${SYRNCK_DOCUMENTATION_DEFAULT})
if(SYRNCK_BUILD_DOCUMENTATION)
  find_program(SYRNCK_NODE_EXECUTABLE NAMES node nodejs REQUIRED)
  find_program(SYRNCK_NPM_EXECUTABLE NAMES npm.cmd npm REQUIRED)
  execute_process(COMMAND "${SYRNCK_NODE_EXECUTABLE}" -p "Number(process.versions.node.split('.')[0]) >= 22"
    OUTPUT_VARIABLE SYRNCK_NODE_SUPPORTED OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT SYRNCK_NODE_SUPPORTED STREQUAL "true")
    message(FATAL_ERROR "Documentation generation requires Node.js 22 or newer")
  endif()
  set(SYRNCK_DOC_WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}/.CyDocBuild")
  set(SYRNCK_DOC_OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/Documentation")
  add_custom_command(OUTPUT "${SYRNCK_DOC_WORKSPACE}/node_modules/.syrnck-dependencies"
    COMMAND "${SYRNCK_NPM_EXECUTABLE}" ci --no-audit --no-fund --cache .cache/npm
    COMMAND "${CMAKE_COMMAND}" -E touch "${SYRNCK_DOC_WORKSPACE}/node_modules/.syrnck-dependencies"
    DEPENDS "${SYRNCK_DOC_WORKSPACE}/package.json" "${SYRNCK_DOC_WORKSPACE}/package-lock.json"
      "${SYRNCK_DOC_WORKSPACE}/vendor/cydoc-0.1.0.tgz"
    WORKING_DIRECTORY "${SYRNCK_DOC_WORKSPACE}" VERBATIM)
  file(GLOB_RECURSE SYRNCK_DOC_INPUTS CONFIGURE_DEPENDS
    "${SYRNCK_DOC_WORKSPACE}/manual/*.md" "${SYRNCK_DOC_WORKSPACE}/assets/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp")
  add_custom_command(OUTPUT "${SYRNCK_DOC_OUTPUT}/Documentation.html"
    "${SYRNCK_DOC_OUTPUT}/DocData/manifest.json" "${SYRNCK_DOC_OUTPUT}/DocData/help-topics.json"
    COMMAND "${SYRNCK_NODE_EXECUTABLE}" "${SYRNCK_DOC_WORKSPACE}/build.mjs"
    DEPENDS ${SYRNCK_DOC_INPUTS} "${SYRNCK_DOC_WORKSPACE}/node_modules/.syrnck-dependencies"
      "${SYRNCK_DOC_WORKSPACE}/build.mjs" "${SYRNCK_DOC_WORKSPACE}/reader.html"
      "${SYRNCK_DOC_WORKSPACE}/cydoc.config.json" "${SYRNCK_DOC_WORKSPACE}/topics.json"
      "${SYRNCK_DOC_WORKSPACE}/api-translations.json"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
  add_custom_target(syrnck_documentation ALL DEPENDS "${SYRNCK_DOC_OUTPUT}/Documentation.html"
    "${SYRNCK_DOC_OUTPUT}/DocData/manifest.json" "${SYRNCK_DOC_OUTPUT}/DocData/help-topics.json")
  if(SYRNCK_BUILD_CLI)
    add_custom_target(syrnck_documentation_copy
      COMMAND "${CMAKE_COMMAND}" -E copy_directory "${SYRNCK_DOC_OUTPUT}" "$<TARGET_FILE_DIR:syrnck>/Documentation"
      DEPENDS syrnck_documentation VERBATIM)
    add_dependencies(syrnck syrnck_documentation_copy)
  endif()
  install(DIRECTORY "${SYRNCK_DOC_OUTPUT}/" DESTINATION "${CMAKE_INSTALL_BINDIR}/Documentation")
endif()
