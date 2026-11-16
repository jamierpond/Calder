#include "../Calder.h"

#include <algorithm>

namespace Calder::Detail
{

AllocationBudget::AllocationBudget(const LoadOptions& options,
                                   std::size_t inputSizeInBytes)
{
    constexpr auto defaultBytesPerInputByte = std::size_t {64};
    constexpr auto smallestDefaultLimit = std::size_t {64} << 20;

    if (options.maximumAllocationInBytes)
    {
        remainingBytes = *options.maximumAllocationInBytes;
        return;
    }

    auto scaledLimit = LoadOptions::noAllocationLimit;

    if (inputSizeInBytes
        <= LoadOptions::noAllocationLimit / defaultBytesPerInputByte)
    {
        scaledLimit = inputSizeInBytes * defaultBytesPerInputByte;
    }

    remainingBytes = std::max(scaledLimit, smallestDefaultLimit);
}

void AllocationBudget::spend(std::size_t elementCount,
                             std::size_t elementSizeInBytes)
{
    if (remainingBytes == LoadOptions::noAllocationLimit)
        return;

    if (elementSizeInBytes != 0
        && elementCount > remainingBytes / elementSizeInBytes)
    {
        throw AllocationOverBudget {};
    }

    remainingBytes -= elementCount * elementSizeInBytes;
}

} // namespace Calder::Detail
