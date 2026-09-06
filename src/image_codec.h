#pragma once
#include "doc_model.h"
#include <string>

// An image turned into something a PDF can carry directly.
//
// JPEG passes through untouched: PDF's DCTDecode filter *is* JPEG, so
// re-encoding would only lose quality and time. PNG is decoded and re-packed
// as a Flate image, because PDF has no PNG filter and the alternative
// (passing the raw IDAT through with a PNG predictor) only works for a subset
// of colour types and silently produces garbage for the rest.
struct EncodedImage {
    std::string data;        // stream payload, already filtered
    std::string smask;       // optional 8-bit alpha channel, Flate-compressed
    std::string filter;      // "DCTDecode" or "FlateDecode"
    std::string colorSpace;  // "DeviceRGB" or "DeviceGray"
    int width = 0;
    int height = 0;
};

// Returns false when the format isn't one we can embed; the caller should skip
// the picture rather than abort the document.
bool encodeImageForPdf(const Image& img, EncodedImage& out);

// Reads just the pixel dimensions, for callers that need them before deciding
// on a layout. Returns false if the header can't be understood.
bool probeImageSize(const std::string& bytes, const std::string& mime, int& w, int& h);
