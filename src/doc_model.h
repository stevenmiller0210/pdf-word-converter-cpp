#pragma once
#include <string>
#include <vector>

// A deliberately simple in-memory document model shared by both conversion
// directions: a flat list of paragraphs, each with a style tag. This is
// enough to preserve heading hierarchy (so a converted document still reads
// as structured, not a wall of identical text) without attempting full
// layout/formatting fidelity — see README for why that's an explicit,
// disclosed scope decision rather than an oversight.
enum class ParagraphStyle {
    Normal,
    Title,
    Heading1,
    Heading2,
    Heading3,
};

struct Paragraph {
    std::string text;
    ParagraphStyle style = ParagraphStyle::Normal;
};

struct DocModel {
    std::vector<Paragraph> paragraphs;
};
