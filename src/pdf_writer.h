#pragma once
#include "doc_model.h"
#include <string>

// Writes a DocModel out as a standalone PDF (A4, base-14 Helvetica/
// Helvetica-Bold, hand-written PDF syntax — no external PDF library).
// Headings/Title get a larger bold font; everything else wraps as plain
// paragraphs across as many pages as needed. Throws std::runtime_error on
// failure to write the output file.
void writePdf(const std::string& path, const DocModel& doc);
