# ─── ODB Compilation Macro ──────────────────────────────────────────────────
#
# Generates ODB object-relational mapping code for a given entity header.
# Consumes native ODB #pragma db mappings from the entity header.
#
function(odb_generate TARGET ENTITY_HEADER)
  get_filename_component(STEM ${ENTITY_HEADER} NAME_WE)
  get_filename_component(DIR ${ENTITY_HEADER} DIRECTORY)
  set(ODB_OUT_CXX "${CMAKE_CURRENT_BINARY_DIR}/odb/${STEM}-odb.cxx")
  set(ODB_OUT_HXX "${CMAKE_CURRENT_BINARY_DIR}/odb/${STEM}-odb.hxx")

  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/odb")

  # ODB reads the original header directly; no source-rewriting bridge is used.
  add_custom_command(
    OUTPUT ${ODB_OUT_CXX} ${ODB_OUT_HXX}
    COMMAND odb
        --database pgsql
        --std c++17
        --generate-query
        --generate-schema
        --schema-format sql
        --output-dir ${CMAKE_CURRENT_BINARY_DIR}/odb
        -I${CMAKE_SOURCE_DIR}/include
        -I${DIR}
        ${ENTITY_HEADER}
    DEPENDS ${ENTITY_HEADER}
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
