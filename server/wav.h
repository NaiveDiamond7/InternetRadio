#pragma once
#include <cstdint>
#include <vector>
#include <string>

//
// ======================= WAV STRUCT =======================
//
struct WavFile {
	int sampleRate = 0;
	int channels = 0;
	int bitsPerSample = 0;
	std::vector<uint8_t> data;
};

