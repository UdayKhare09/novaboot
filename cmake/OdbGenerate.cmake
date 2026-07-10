# ─── ODB Compilation Macro ──────────────────────────────────────────────────
#
# Generates ODB object-relational mapping code for a given entity header.
# Translates standard C++26 database attributes to ODB pragmas at build time.
#
function(odb_generate TARGET ENTITY_HEADER)
  get_filename_component(STEM ${ENTITY_HEADER} NAME_WE)
  get_filename_component(DIR ${ENTITY_HEADER} DIRECTORY)
  set(ODB_OUT_CXX "${CMAKE_CURRENT_BINARY_DIR}/odb/${STEM}-odb.cxx")
  set(ODB_OUT_HXX "${CMAKE_CURRENT_BINARY_DIR}/odb/${STEM}-odb.hxx")

  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/odb_bridge")
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/odb")

  # 1. Run the Python bridge script to translate C++26 annotations to ODB pragmas
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/odb_bridge/${STEM}.h
    COMMAND python3 ${CMAKE_SOURCE_DIR}/cmake/odb_bridge.py ${ENTITY_HEADER} ${CMAKE_CURRENT_BINARY_DIR}/odb_bridge/${STEM}.h
    DEPENDS ${ENTITY_HEADER} ${CMAKE_SOURCE_DIR}/cmake/odb_bridge.py
    COMMENT "Translating C++26 annotations to ODB pragmas for ${ENTITY_HEADER}"
    VERBATIM
  )

  # 2. Run the ODB compiler on the shadow header file
  add_custom_command(
    OUTPUT ${ODB_OUT_CXX} ${ODB_OUT_HXX}
    COMMAND odb
        --database pgsql
        --std c++17
        --generate-query
        --generate-schema
        --schema-format sql
        --output-dir ${CMAKE_CURRENT_BINARY_DIR}/odb
        -I.
        -I${CMAKE_SOURCE_DIR}/include
        ${STEM}.h
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/odb_bridge/${STEM}.h
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/odb_bridge
    COMMENT "Generating ODB mapping for ${STEM}.h"
    VERBATIM
  )

  # Add generated ODB source file to target
  target_sources(${TARGET} PRIVATE ${ODB_OUT_CXX})

  # Set compiler include paths: ODB generated headers and the original entity headers
  target_include_directories(${TARGET} PRIVATE 
      "${CMAKE_CURRENT_BINARY_DIR}/odb"
      "${DIR}"
  )
endfunction()
