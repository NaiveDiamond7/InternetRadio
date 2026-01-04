#pragma once

#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <string>
#include <chrono>

#include "track.h"

// Prosty serwer "radia" HTTP:
// - /queue  (GET/POST)  – kolejka plików WAV po nazwie
// - /audio  (GET)       – aktualny utwór jako plik WAV
// - /progress (GET)     – JSON z postępem (elapsed, duration, position)
// - /skip   (POST)      – przeskoczenie do następnego utworu
// - /        (GET)      – index.html z GUI

class Server {
public:
    explicit Server(int port);
    ~Server();

    void start();
    void stop();

private:
    int port;
    int http_socket{-1};
    std::atomic<bool> running{false};

    // Kolejka utworów (tylko nazwy plików)
    std::deque<Track> playlist;
    std::mutex playlist_mutex;

    // Sterowanie odtwarzaniem
    std::atomic<bool> skip_requested{false};
    std::atomic<int> next_track_id{1};

    // Aktualnie odtwarzany utwór (czas liczony z zegara)
    std::mutex playback_mutex;
    std::condition_variable playback_cv;
    std::string current_track;
    double current_track_duration{0.0};
    std::chrono::steady_clock::time_point current_track_start{};
    bool track_playing{false};

    // Wątki
    std::thread stream_thread;
    std::thread http_thread;

    // Metody wątków
    void streamingLoop();
    void httpLoop();

    // Pomocnicze
    void setupHttpSocket();
    double computeTrackDuration(const std::string& filename);
    void handleHttpClient(int client);
    void sendHttpResponse(int client,
                          const std::string& body,
                          const std::string& contentType = "text/plain",
                          int status = 200);
    int enqueueTrack(const std::string& filename);
    void streamHttpAudio(int client);
};