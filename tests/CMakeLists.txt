if (NOT TARGET NanoTest)
    CPMAddPackage(
            NAME NanoTest
            GITHUB_REPOSITORY eyalamirmusic/NanoTest
            GIT_TAG main)
endif ()

add_executable(CalderTests
        Binary/BinaryReflectorTests.cpp Binary/BinaryFormatTests.cpp
        Binary/BinaryBenchmarkTests.cpp Binary/SchemaEvolutionTests.cpp
        Binary/BinaryHeaderTests.cpp)
target_link_libraries(CalderTests PRIVATE
        NanoTest NanoTestMain calder)
target_compile_options(CalderTests PRIVATE ${CALDER_WARNINGS})
nano_discover_tests(CalderTests)
