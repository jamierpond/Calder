cmake_minimum_required(VERSION 3.25)

project(calder LANGUAGES CXX)

if (MSVC)
    set(CALDER_WARNINGS /W4 /WX)
else ()
    set(CALDER_WARNINGS -Wall -Werror)
endif ()

include(cmake/CPM.cmake)

# jamierpond/Miro jp/calder-emberstore: Miro's develop plus the open pull
# requests Calder needs (visitPacked, missing containers keep their defaults,
# range-checked numbers, compile-time type names, packed struct arrays).
if (NOT TARGET Miro)
    CPMAddPackage(
            NAME Miro
            GITHUB_REPOSITORY jamierpond/Miro
            GIT_TAG 65d6c7b02aa16d0191dea8046fa406e6bf2ee2be)
endif ()

add_library(calder STATIC
        Calder/Detail/Wire.cpp
        Calder/Detail/Reflectors.cpp
        Calder/Detail/View.cpp
        Calder/Detail/File.cpp
        Calder/Detail/AtomicWrite.cpp
        Calder/Detail/JournalWriter.cpp)

target_include_directories(calder PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(calder PUBLIC cxx_std_20)
target_compile_options(calder PRIVATE ${CALDER_WARNINGS})
target_link_libraries(calder PUBLIC Miro)

if (PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_subdirectory(tests)
endif ()
