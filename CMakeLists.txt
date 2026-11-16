cmake_minimum_required(VERSION 3.25)

project(calder LANGUAGES CXX)

if (MSVC)
    set(CALDER_WARNINGS /W4 /WX)
else ()
    set(CALDER_WARNINGS -Wall -Werror)
endif ()

include(cmake/CPM.cmake)

# jamierpond/Miro jp/reflector-hooks: visitPacked, NumericConvert.h, and
# missing containers keep their defaults on load.
if (NOT TARGET Miro)
    CPMAddPackage(
            NAME Miro
            GITHUB_REPOSITORY jamierpond/Miro
            GIT_TAG e10294430ba132a6556b9a6c8d841cec7e2e3cfe)
endif ()

add_library(calder STATIC
        emberstore/Binary/Binary.cpp
        emberstore/Binary/BinaryReflector.cpp)

target_include_directories(calder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(calder PUBLIC cxx_std_20)
target_compile_options(calder PRIVATE ${CALDER_WARNINGS})
target_link_libraries(calder PUBLIC Miro)

if (PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_subdirectory(tests)
endif ()
