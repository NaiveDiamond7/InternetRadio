#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <portaudio.h>

//
// ======================= WAV STRUCT =======================
//
struct WavFile {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    std::vector<uint8_t> data;
};

//
// ======================= WAV LOADER =======================
//
WavFile loadWav(const std::string& filename) {
    std::ifstream f(filename, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot open WAV file");

    char chunkId[4];
    uint32_t chunkSize;

    // RIFF
    f.read(chunkId, 4);
    if (std::strncmp(chunkId, "RIFF", 4) != 0)
        throw std::runtime_error("Not a RIFF file");

    f.read(reinterpret_cast<char*>(&chunkSize), 4);

    f.read(chunkId, 4);
    if (std::strncmp(chunkId, "WAVE", 4) != 0)
        throw std::runtime_error("Not a WAVE file");

    WavFile wav;

    while (f) {
        f.read(chunkId, 4);
        f.read(reinterpret_cast<char*>(&chunkSize), 4);

        if (std::strncmp(chunkId, "fmt ", 4) == 0) {
            uint16_t audioFormat;
            f.read(reinterpret_cast<char*>(&audioFormat), 2);
            f.read(reinterpret_cast<char*>(&wav.channels), 2);
            f.read(reinterpret_cast<char*>(&wav.sampleRate), 4);
            f.ignore(6); // byteRate + blockAlign
            f.read(reinterpret_cast<char*>(&wav.bitsPerSample), 2);

            if (audioFormat != 1)
                throw std::runtime_error("Only PCM WAV supported");

            if (wav.bitsPerSample != 24)
                throw std::runtime_error("Only 24-bit WAV supported");

            f.ignore(chunkSize - 16);
        }
        else if (std::strncmp(chunkId, "data", 4) == 0) {
            wav.data.resize(chunkSize);
            f.read(reinterpret_cast<char*>(wav.data.data()), chunkSize);
            break;
        }
        else {
            // skip unknown chunk
            f.ignore(chunkSize);
        }
    }

    if (wav.data.empty())
        throw std::runtime_error("No audio data found");

    return wav;
}

//
// ======================= 24-BIT PCM → FLOAT =======================
//
inline float pcm24ToFloat(const uint8_t* p) { // conversion cause portaudio works best with float
    int32_t sample =
        (p[0]) |
        (p[1] << 8) |
        (p[2] << 16);

    // sign extension
    if (sample & 0x800000)
        sample |= ~0xFFFFFF;

    return sample / 8388608.0f; // 2^23
}

//
// ======================= PLAYBACK STATE =======================
//
struct PlaybackState {
    const std::vector<uint8_t>* data;
    size_t position = 0;
    int channels = 0;
};

//
// ======================= PORTAUDIO CALLBACK =======================
//
static int audioCallback(
    const void*,
    void* output,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo*,
    PaStreamCallbackFlags,
    void* userData
) {
    auto* state = static_cast<PlaybackState*>(userData);
    float* out = static_cast<float*>(output);

    for (unsigned long i = 0; i < framesPerBuffer; ++i) {
        for (int ch = 0; ch < state->channels; ++ch) {
            if (state->position + 3 <= state->data->size()) {
                out[i * state->channels + ch] =
                    pcm24ToFloat(&(*state->data)[state->position]);
                state->position += 3;
            } else {
                out[i * state->channels + ch] = 0.0f;
            }
        }
    }

    return (state->position >= state->data->size())
           ? paComplete
           : paContinue;
}

//
// ======================= MAIN =======================
//
int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: wav_player <file.wav>\n";
        return 1;
    }

    try {
        WavFile wav = loadWav(argv[1]);

        std::cout << "Playing WAV:\n";
        std::cout << "  Sample rate: " << wav.sampleRate << " Hz\n";
        std::cout << "  Channels:    " << wav.channels << "\n";
        std::cout << "  Bit depth:   " << wav.bitsPerSample << "\n";

        Pa_Initialize();

        PlaybackState state;
        state.data = &wav.data;
        state.channels = wav.channels;

        PaStream* stream;
        Pa_OpenDefaultStream(
            &stream,
            0,
            wav.channels,
            paFloat32,
            wav.sampleRate,
            256,                // low-latency buffer
            audioCallback,
            &state
        );

        Pa_StartStream(stream);

        while (Pa_IsStreamActive(stream))
            Pa_Sleep(10);

        Pa_StopStream(stream);
        Pa_CloseStream(stream);
        Pa_Terminate();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}