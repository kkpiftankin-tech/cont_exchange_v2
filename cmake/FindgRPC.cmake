# =============================================================================
# FindgRPC.cmake — minimal Find module for gRPC C++.
#
# Покрывает случаи, когда upstream gRPCConfig.cmake отсутствует
# (Ubuntu apt-пакеты libgrpc++-dev/protobuf-compiler-grpc), но есть
# pkg-config интерфейс. На macOS (brew install grpc) тоже работает.
#
# Defines imported targets matching upstream gRPCConfig naming:
#   gRPC::grpc++              — shared library
#   gRPC::grpc_cpp_plugin     — protoc plugin executable
#
# Usage in CMakeLists.txt:
#   list(PREPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
#   find_package(gRPC REQUIRED)
#   target_link_libraries(my_target PUBLIC gRPC::grpc++)
# =============================================================================

find_package(PkgConfig REQUIRED)

pkg_check_modules(PC_GRPCXX QUIET grpc++)

if(NOT PC_GRPCXX_FOUND)
  message(FATAL_ERROR
    "grpc++ not found via pkg-config. "
    "Install libgrpc++-dev (Ubuntu/Debian apt) or grpc (Homebrew on macOS).")
endif()

find_library(GRPCXX_LIBRARY
  NAMES grpc++
  HINTS ${PC_GRPCXX_LIBRARY_DIRS}
)

find_path(GRPCXX_INCLUDE_DIR
  NAMES grpcpp/grpcpp.h
  HINTS ${PC_GRPCXX_INCLUDE_DIRS}
)

find_program(GRPC_CPP_PLUGIN_EXECUTABLE
  NAMES grpc_cpp_plugin
  HINTS ${PC_GRPCXX_PREFIX}/bin /usr/bin /usr/local/bin
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(gRPC
  REQUIRED_VARS GRPCXX_LIBRARY GRPCXX_INCLUDE_DIR GRPC_CPP_PLUGIN_EXECUTABLE
  VERSION_VAR PC_GRPCXX_VERSION
)

if(gRPC_FOUND)
  if(NOT TARGET gRPC::grpc++)
    add_library(gRPC::grpc++ UNKNOWN IMPORTED)
    set_target_properties(gRPC::grpc++ PROPERTIES
      IMPORTED_LOCATION "${GRPCXX_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${GRPCXX_INCLUDE_DIR}"
      INTERFACE_LINK_LIBRARIES "${PC_GRPCXX_LINK_LIBRARIES}"
    )
  endif()

  if(NOT TARGET gRPC::grpc_cpp_plugin)
    add_executable(gRPC::grpc_cpp_plugin IMPORTED)
    set_target_properties(gRPC::grpc_cpp_plugin PROPERTIES
      IMPORTED_LOCATION "${GRPC_CPP_PLUGIN_EXECUTABLE}"
    )
  endif()
endif()

mark_as_advanced(GRPCXX_LIBRARY GRPCXX_INCLUDE_DIR GRPC_CPP_PLUGIN_EXECUTABLE)
