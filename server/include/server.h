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

    std::vector<int> clients;
    std::mutex clients_mutex;

    std::deque<Track> playlist;
    std::mutex playlist_mutex;

    std::atomic<bool> skip_requested{false};
    std::atomic<int> next_track_id{1};

    PaStream* audio_stream = nullptr;
    WavFile current_wav;
    std::string current_track_name;
    std::atomic<size_t> current_position{0};
    std::mutex playback_mutex;
    std::condition_variable playback_cv;

    std::thread accept_thread;
    std::thread stream_thread;
    std::thread http_thread;

    void acceptLoop();
    void streamingLoop();
    void httpLoop();

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

    friend int portaudioCallback(const void*, void*, unsigned long, const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void*);
};