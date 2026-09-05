#pragma once
#include "doc_model.h"
#include <string>

// Writes a DocModel out as a minimal, valid .docx (OOXML WordprocessingML)
// package at `path`. Throws std::runtime_error on failure (can't create
// scratch dir, `zip` missing/failed, ...).
void writeDocx(const std::string& path, const DocModel& doc);
