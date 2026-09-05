// Headless CLI wrapper around the same convert.h entry points the GUI uses —
// lets the conversion engine be tested without building the GTK GUI.
// Usage: cli_test <input.docx|input.pdf> <output.pdf|output.docx>
#include "../src/convert.h"
#include <iostream>
#include <string>
#include <algorithm>

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <input.docx|input.pdf> <output.pdf|output.docx>\n";
        return 2;
    }
    std::string input = argv[1];
    std::string output = argv[2];
    std::string inLower = lower(input);

    try {
        if (endsWith(inLower, ".docx")) {
            convertWordToPdf(input, output);
        } else if (endsWith(inLower, ".pdf")) {
            convertPdfToWord(input, output);
        } else {
            std::cerr << "FAILED: ismeretlen bemeneti tipus (varhato .docx vagy .pdf)\n";
            return 2;
        }
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
    std::cout << "OK -> " << output << "\n";
    return 0;
}
