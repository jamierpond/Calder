#pragma once

#include "Wire.h"

namespace Calder::Detail
{

struct ViewAccess
{
    static View makeView(const Document& document, const ViewPosition& position);
    static const Document& getDocument(const View& view);
    static const ViewPosition& getPosition(const View& view);
};

ValueHeader readViewHeader(const Document& document, const ViewPosition& position);

} // namespace Calder::Detail
