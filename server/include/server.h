#pragma once
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <string>
#include <cstdint>
#include <portaudio.h>
#include "track.h"
#include "wav.h"

class Server {
public:
    Server(int port);
    ~Server();

    void start();
    void stop();

private:
    int port;
    int server_socket;
    int http_socket{-1};
    std::atomic<bool> running{false};

    // Klienci strumienia
    std::vector<int> clients;
    std::mutex clients_mutex;

    // Kolejka utworów
    std::deque<Track> playlist;
    std::mutex playlist_mutex;

    // Flagi sterujące
    std::atomic<bool> skip_requested{false};
    std::atomic<int> next_track_id{1};

    // PortAudio stream i stan odtwarzania
    PaStream* audio_stream = nullptr;
    WavFile current_wav;
    std::string current_track_name; // nazwa aktualnie odtwarzanego utworu
    std::atomic<size_t> current_position{0};
    std::mutex playback_mutex;
    std::condition_variable playback_cv;

    // Wątki
    std::thread accept_thread;
    std::thread stream_thread;
    std::thread http_thread;

    // Metody wątków
    void acceptLoop();
    void streamingLoop();
    void httpLoop();

    // Pomocnicze
    void setupSocket();
    void setupHttpSocket();
    WavFile loadWav(const std::string& filename);
    void sendToClients(const uint8_t* buffer, size_t size);
    void handleHttpClient(int client);
    void sendHttpResponse(int client, const std::string& body, const std::string& contentType = "text/plain", int status = 200);
    int enqueueTrack(const std::string& filename);
    void streamHttpAudio(int client);
    void startAudioStream();
    void stopAudioStream();

    // PortAudio callback (static wrapper)
    friend int portaudioCallback(const void*, void*, unsigned long, const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*);
};