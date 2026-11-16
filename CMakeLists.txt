cmake_minimum_required(VERSION 3.25)

project(calder LANGUAGES CXX)

if (MSVC)
    set(CALDER_WARNINGS /W4 /WX)
else ()
    set(CALDER_WARNINGS -Wall -Werror)
endif ()

include(cmake/CPM.cmake)

# jamierpond/Miro jp/packed-structs-on-hooks: visitPacked, NumericConvert.h,
# missing containers keep their defaults on load, and packed struct arrays
# (including MIRO_REFLECT_EXTERNAL structs).
if (NOT TARGET Miro)
    CPMAddPackage(
            NAME Miro
            GITHUB_REPOSITORY jamierpond/Miro
            GIT_TAG 8180bbc75a0f7acc04b19298c1420920f4bbc605)
endif ()

add_library(calder STATIC
        Calder/Binary.cpp
        Calder/BinaryReflector.cpp)

target_include_directories(calder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(calder PUBLIC cxx_std_20)
target_compile_options(calder PRIVATE ${CALDER_WARNINGS})
target_link_libraries(calder PUBLIC Miro)

if (PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_subdirectory(tests)
endif ()
