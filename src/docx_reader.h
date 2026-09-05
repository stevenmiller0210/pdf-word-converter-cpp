#pragma once
#include "doc_model.h"
#include <string>

// Reads a .docx file's paragraph text (and heading/title styles) into a
// DocModel. Throws std::runtime_error with a human-readable message on
// failure (not a .docx / corrupt zip / no readable text).
DocModel readDocx(const std::string& path);
