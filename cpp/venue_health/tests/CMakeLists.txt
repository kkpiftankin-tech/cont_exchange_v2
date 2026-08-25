if (NOT BUILD_TESTING)
    return()
endif ()

find_package(GTest CONFIG QUIET)
if (NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.15.2
    )
    FetchContent_MakeAvailable(googletest)
endif ()

add_library(venue_health_lib STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/app/service.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/domain/entities/circuit_breaker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/domain/entities/venue_state.cpp
)

target_include_directories(venue_health_lib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

add_executable(venue_health_tests
        domain/circuit_breaker_test.cpp
        domain/venue_state_test.cpp
        app/service_test.cpp
)

target_include_directories(venue_health_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(venue_health_tests PRIVATE
        venue_health_lib
        GTest::gtest_main
)

add_test(NAME venue_health_tests COMMAND venue_health_tests)
