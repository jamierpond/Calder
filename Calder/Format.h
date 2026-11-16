#pragma once

// For tools that read Calder's bytes directly, such as a byte-layout viewer
// or a store with its own journal files: the wire format (Detail::Tag,
// ByteReader, parseValueHeader, ObjectEntryCursor, Document) and journal
// frames (inspectJournal, makeJournal, makeJournalSetFrame). These live in
// Calder::Detail and change without notice; apps use Calder.h alone.

#include "Calder.h"
