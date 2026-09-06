#pragma once
#include "doc_model.h"
#include <string>

// Writes a DocModel out as a standalone A4 PDF, in hand-written PDF syntax —
// no external PDF library.
//
// Everything is laid out here, because a PDF has no concept of flowing
// content: word wrapping (using the embedded font's real advance widths),
// page breaks, list markers, table geometry and image placement all happen
// before a single byte is emitted.
//
// Text is set in a subsetted TrueType face embedded as a CID font with
// Identity-H encoding, so there is no code-page ceiling on what can be
// written, and a /ToUnicode CMap keeps the result selectable and
// searchable. Bold/italic runs use separate faces, falling back to the
// regular one when the system has no such variant.
//
// Throws std::runtime_error if no usable TrueType font can be found or the
// output file cannot be written.
void writePdf(const std::string& path, const DocModel& doc);
