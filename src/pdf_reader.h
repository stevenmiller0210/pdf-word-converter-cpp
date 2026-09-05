#pragma once
#include "doc_model.h"
#include <string>

// Extracts a PDF's text (via poppler's `pdftotext -layout`) into a flat
// DocModel — one paragraph per non-blank line. No layout/formatting is
// recovered, matching this tool's documented scope (see README).
//
// Throws std::runtime_error("NO_TEXT_EXTRACTED") specifically when the PDF
// has no extractable text at all (a scanned/image-only PDF) — callers
// should catch this to show a distinct, honest message instead of a
// generic failure. Any other failure (not a real PDF, pdftotext missing)
// throws std::runtime_error with a different message.
DocModel readPdf(const std::string& path);
