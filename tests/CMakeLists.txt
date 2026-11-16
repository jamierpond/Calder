if (NOT TARGET NanoTest)
    CPMAddPackage(
            NAME NanoTest
            GITHUB_REPOSITORY eyalamirmusic/NanoTest
            GIT_TAG main)
endif ()

add_executable(CalderTests
        BinaryReflectorTests.cpp BinaryFormatTests.cpp
        BinaryBenchmarkTests.cpp SchemaEvolutionTests.cpp
        BinaryHeaderTests.cpp PackedStructArrayTests.cpp)
target_link_libraries(CalderTests PRIVATE
        NanoTest NanoTestMain calder)
target_compile_options(CalderTests PRIVATE ${CALDER_WARNINGS})
nano_discover_tests(CalderTests)
