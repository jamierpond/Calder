#pragma once

// Entry header: the binary serialization layer — the compact "CLD1"
// wire format (Detail::ByteWriter / Document / View) plus the
// reflection serializers toBinary / fromBinary / createFromBinary.

#include "Binary.h"
#include <Miro/Reflection/ReflectEnum.h>
#include <Miro/Reflection/ReflectMacro.h>
#include <Miro/Reflection/ReflectPolymorphic.h>
#include "SerializeBinary.h"
