cmake_minimum_required(VERSION 3.22)
project(cex_contracts LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)

set(PROTO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/proto)
file(GLOB_RECURSE PROTO_FILES
  ${PROTO_ROOT}/*.proto
)

set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})

set(PROTOC $<TARGET_FILE:protobuf::protoc>)
set(GRPC_CPP_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)

set(GENERATED_SRCS "")
set(GENERATED_HDRS "")

foreach(PROTO ${PROTO_FILES})
  file(RELATIVE_PATH REL ${PROTO_ROOT} ${PROTO})
  get_filename_component(REL_DIR ${REL} DIRECTORY)
  get_filename_component(PROTO_NAME ${REL} NAME_WE)

  file(MAKE_DIRECTORY ${GEN_DIR}/${REL_DIR})

  set(PB_CC   ${GEN_DIR}/${REL_DIR}/${PROTO_NAME}.pb.cc)
  set(PB_H    ${GEN_DIR}/${REL_DIR}/${PROTO_NAME}.pb.h)
  set(GRPC_CC ${GEN_DIR}/${REL_DIR}/${PROTO_NAME}.grpc.pb.cc)
  set(GRPC_H  ${GEN_DIR}/${REL_DIR}/${PROTO_NAME}.grpc.pb.h)

  add_custom_command(
    OUTPUT ${PB_CC} ${PB_H} ${GRPC_CC} ${GRPC_H}
    COMMAND ${PROTOC}
      --proto_path=${PROTO_ROOT}
      --cpp_out=${GEN_DIR}
      --grpc_out=${GEN_DIR}
      --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
      ${REL}
    DEPENDS ${PROTO}
    WORKING_DIRECTORY ${PROTO_ROOT}
    COMMENT "Generating C++ proto/grpc for ${REL}"
    VERBATIM
  )

  list(APPEND GENERATED_SRCS ${PB_CC} ${GRPC_CC})
  list(APPEND GENERATED_HDRS ${PB_H} ${GRPC_H})
endforeach()

add_library(contracts_proto STATIC
  ${GENERATED_SRCS}
  ${GENERATED_HDRS}
)

target_include_directories(contracts_proto PUBLIC
  ${GEN_DIR}
)

target_link_libraries(contracts_proto PUBLIC
  protobuf::libprotobuf
  gRPC::grpc++
)
