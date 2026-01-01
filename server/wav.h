#pragma once
#include <cstdint>

#pragma pack(push, 1)
struct WavHeader {
    char riff[4];          // "RIFF"
    uint32_t chunkSize;
    char wave[4];          // "WAVE"
    char fmt[4];           // "fmt "
    uint32_t subchunk1Size;
    uint16_t audioFormat;  // 1 = PCM
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char data[4];          // "data"
    uint32_t dataSize;
};
#pragma pack(pop)