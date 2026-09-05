#pragma once
#include <string>

// High-level entry points used by both the GUI and the CLI test harness.
// Both throw std::runtime_error on failure; convertPdfToWord specifically
// throws with the message "NO_TEXT_EXTRACTED" for a scanned/image-only PDF
// (no text to recover) so callers can show a distinct, honest message.
void convertWordToPdf(const std::string& inputPath, const std::string& outputPath);
void convertPdfToWord(const std::string& inputPath, const std::string& outputPath);
