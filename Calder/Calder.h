#pragma once

// Entry header: the binary serialization layer — the compact "MIB1"
// wire format (Calder::Writer / Document / View) plus the
// reflection serializers toBinary / fromBinary / createFromBinary.

#include "Binary.h"
#include <Miro/Reflection/ReflectEnum.h>
#include <Miro/Reflection/ReflectMacro.h>
#include <Miro/Reflection/ReflectPolymorphic.h>
#include "SerializeBinary.h"
