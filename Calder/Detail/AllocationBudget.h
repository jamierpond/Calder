#pragma once

#include <cstddef>
#include <exception>

namespace Calder
{
struct LoadOptions;
}

namespace Calder::Detail
{

struct AllocationOverBudget : std::exception
{
};

// The memory one load may still allocate for container elements, shared by
// every Reader of the load. Spending past it throws AllocationOverBudget.
class AllocationBudget
{
public:
    AllocationBudget(const LoadOptions& options, std::size_t inputSizeInBytes);

    void spend(std::size_t elementCount, std::size_t elementSizeInBytes);

private:
    std::size_t remainingBytes = 0;
};

} // namespace Calder::Detail
