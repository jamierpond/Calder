if (NOT TARGET NanoTest)
    CPMAddPackage(
            NAME NanoTest
            GITHUB_REPOSITORY eyalamirmusic/NanoTest
            GIT_TAG main)
endif ()

add_executable(CalderTests
        BinaryReflectorTests.cpp BinaryFormatTests.cpp
        BinaryBenchmarkTests.cpp SchemaEvolutionTests.cpp
        BinaryHeaderTests.cpp PackedStructArrayTests.cpp
        PublicApiTests.cpp FieldProblemTests.cpp)
target_link_libraries(CalderTests PRIVATE
        NanoTest NanoTestMain calder)
target_compile_options(CalderTests PRIVATE ${CALDER_WARNINGS})
nano_discover_tests(CalderTests)

# The C++ writes sample files and reads them back; the Python decoder must
# read the same trees from them.
add_executable(CalderCrossCheckWriter CrossCheck/WriteSamples.cpp)
target_link_libraries(CalderCrossCheckWriter PRIVATE calder)
target_compile_options(CalderCrossCheckWriter PRIVATE ${CALDER_WARNINGS})

find_package(Python3 3.10 COMPONENTS Interpreter)

if (Python3_FOUND)
    add_test(NAME "Cross-check: the Python decoder reads what the C++ reads"
            COMMAND Python3::Interpreter
            ${CMAKE_CURRENT_SOURCE_DIR}/CrossCheck/CompareWithPython.py
            $<TARGET_FILE:CalderCrossCheckWriter>
            ${CMAKE_CURRENT_BINARY_DIR}/CrossCheckSamples)
    add_test(NAME "Python decoder: unit tests"
            COMMAND Python3::Interpreter -m unittest calder.test_calder
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/python)
endif ()

# Replaces the global operator new to measure a load's peak memory, so it gets
# an executable of its own.
add_executable(CalderAllocationBudgetTests AllocationBudgetTests.cpp)
target_link_libraries(CalderAllocationBudgetTests PRIVATE
        NanoTest NanoTestMain calder)
target_compile_options(CalderAllocationBudgetTests PRIVATE ${CALDER_WARNINGS})
nano_discover_tests(CalderAllocationBudgetTests)
