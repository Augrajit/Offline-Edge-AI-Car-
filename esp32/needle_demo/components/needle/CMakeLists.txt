file(GLOB ND_SRCS "${CMAKE_CURRENT_LIST_DIR}/../../../../engine/src/*.c")
idf_component_register(SRCS ${ND_SRCS}
                       INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../../engine/include")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
# target_compile_definitions(${COMPONENT_LIB} PRIVATE ND_PROFILE)
