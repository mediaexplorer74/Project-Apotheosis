#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Apotheosis: parse a MAPPED image (RVAs are offsets from its allocation base).
// Bounds checks reject malformed headers; the caller must still guard inaccessible pages
// with SEH. PE32 and PE32+ have different export-directory offsets (96 and 112).
namespace ApoMappedPe {
inline bool Fits(size_t size, size_t offset, size_t bytes)
{
    return offset <= size && bytes <= size - offset;
}

template<typename T> inline bool Read(const unsigned char* image, size_t size, size_t offset, T& value)
{
    if (!Fits(size, offset, sizeof(T)))
        return false;
    memcpy(&value, image + offset, sizeof(T));
    return true;
}

inline bool ReadName(const void* base, size_t mappedSize, char* out, size_t capacity)
{
    if (!out || !capacity)
        return false;
    out[0] = 0;
    if (!base)
        return false;
    const unsigned char* image = static_cast<const unsigned char*>(base);
    uint16_t dosMagic = 0, optionalSize = 0, magic = 0;
    int32_t ntOffset = 0;
    uint32_t signature = 0, imageSize = 0, directoryCount = 0;
    if (!Read(image, mappedSize, 0, dosMagic) || dosMagic != 0x5a4d
        || !Read(image, mappedSize, 0x3c, ntOffset) || ntOffset < 0x40)
        return false;
    size_t nt = static_cast<size_t>(ntOffset);
    if (!Fits(mappedSize, nt, 24) || !Read(image, mappedSize, nt, signature)
        || signature != 0x00004550 || !Read(image, mappedSize, nt + 20, optionalSize))
        return false;
    size_t optional = nt + 24;
    if (!Fits(mappedSize, optional, optionalSize) || optionalSize < 64
        || !Read(image, mappedSize, optional, magic))
        return false;
    size_t directory = magic == 0x10b ? 96 : magic == 0x20b ? 112 : 0;
    if (!directory || optionalSize < directory + 8
        || !Read(image, mappedSize, optional + 56, imageSize)
        || !imageSize || imageSize > mappedSize
        || !Fits(imageSize, optional, optionalSize)
        || !Read(image, imageSize, optional + directory - 4, directoryCount) || !directoryCount)
        return false;
    uint32_t exportRva = 0, exportSize = 0, nameRva = 0;
    if (!Read(image, imageSize, optional + directory, exportRva)
        || !Read(image, imageSize, optional + directory + 4, exportSize)
        || !exportRva || exportSize < 40 || !Fits(imageSize, exportRva, exportSize)
        || !Read(image, imageSize, static_cast<size_t>(exportRva) + 12, nameRva)
        || !nameRva || nameRva >= imageSize)
        return false;
    size_t available = imageSize - nameRva;
    // Validate the whole bounded string even when the output needs truncation.
    for (size_t i = 0; i < available; ++i) {
        unsigned char c = image[nameRva + i];
        if (!c) {
            size_t length = i < capacity - 1 ? i : capacity - 1;
            memcpy(out, image + nameRva, length);
            out[length] = 0;
            return length != 0;
        }
        if (c < 32 || c >= 127)
            return false;
    }
    return false;
}
} // namespace ApoMappedPe
