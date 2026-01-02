#pragma once
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include "track.h"

class Server {
public:
    Server(int port);
    ~Server();

    void start();
    void stop();

private:
    int port;
    int server_socket;
    int http_socket;

    std::atomic<bool> running{false};

    // Klienci strumienia
    std::vector<int> clients;
    std::mutex clients_mutex;

    // HTTP clients (audio stream)
    std::vector<int> http_audio_clients;
    std::mutex http_audio_clients_mutex;

    // Kolejka utworów
    std::deque<Track> playlist;
    std::mutex playlist_mutex;

    // Flagi sterujące
    std::atomic<bool> skip_requested{false};

    // Śledzenie przebiegu utworu
    std::atomic<size_t> current_byte_offset{0};
    std::atomic<size_t> current_track_size{0};
    std::atomic<double> current_track_duration{0.0};
    std::atomic<double> current_elapsed{0.0};


    // Wątki
    std::thread accept_thread;
    std::thread stream_thread;
    std::thread http_thread;

    // Metody wątków
    void setupSocket();
    void setupHttpSocket();
    void acceptLoop();
    void streamingLoop();
    void httpLoop();
};