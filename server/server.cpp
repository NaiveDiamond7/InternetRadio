#include "server.h"
#include "wav.h"
#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <fstream>
#include <chrono>
#include <vector>

constexpr size_t AUDIO_BLOCK = 4096;

Server::Server(int port) : port(port) {}

Server::~Server() {
    stop();
}

void Server::start() {
    running = true;
    setupSocket();

    accept_thread = std::thread(&Server::acceptLoop, this);
    stream_thread = std::thread(&Server::streamingLoop, this);
    http_thread   = std::thread(&Server::httpLoop, this);

    std::cout << "[SERVER] Started\n";
}

void Server::stop() {
    running = false;

    if (server_socket > 0)
        close(server_socket);

    if (accept_thread.joinable()) accept_thread.join();
    if (stream_thread.joinable()) stream_thread.join();
    if (http_thread.joinable())   http_thread.join();

    std::cout << "[SERVER] Stopped\n";
}

void Server::setupSocket() {
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("socket");
        exit(1);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);

    if (bind(server_socket, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }

    if (listen(server_socket, 10) < 0) {
        perror("listen");
        exit(1);
    }
}

void Server::acceptLoop() {
    while (running) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);

        int client = accept(server_socket, (sockaddr*)&client_addr, &len);
        if (client < 0) continue;

        std::lock_guard<std::mutex> lock(clients_mutex);
        clients.push_back(client);

        std::cout << "[SERVER] Client connected (" << clients.size() << ")\n";
    }
}

void Server::streamingLoop() {
    while (running) {

        Track current;
        {
            std::lock_guard<std::mutex> lock(playlist_mutex);
            if (playlist.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            current = playlist.front();
        }

        std::ifstream file(current.filename, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[STREAM] Cannot open " << current.filename << "\n";
            std::lock_guard<std::mutex> lock(playlist_mutex);
            playlist.pop_front();
            continue;
        }

        WavHeader header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));

        if (header.audioFormat != 1) {
            std::cerr << "[STREAM] Not PCM WAV\n";
            file.close();
            continue;
        }

        current_track_size = header.dataSize;
        current_byte_offset = 0;
        current_elapsed = 0.0;
        current_track_duration = static_cast<double>(header.dataSize) / header.byteRate;

        std::vector<char> buffer(AUDIO_BLOCK);

        std::cout << "[STREAM] Playing: " << current.filename << "\n";

        while (running && !skip_requested) {

            file.read(buffer.data(), buffer.size());
            std::streamsize bytesRead = file.gcount();
            if (bytesRead <= 0)
                break;

            {
                std::lock_guard<std::mutex> lock(clients_mutex);
                for (auto it = clients.begin(); it != clients.end();) {
                    ssize_t sent = send(*it, buffer.data(), bytesRead, MSG_NOSIGNAL);
                    if (sent <= 0) {
                        close(*it);
                        it = clients.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            current_byte_offset += bytesRead;
            double elapsed = current_elapsed.load();
            elapsed += static_cast<double>(bytesRead) / header.byteRate;
            current_elapsed.store(elapsed);

            auto delay = std::chrono::duration<double>(
                static_cast<double>(bytesRead) / header.byteRate);
            std::this_thread::sleep_for(delay);
        }

        skip_requested = false;
        file.close();

        {
            std::lock_guard<std::mutex> lock(playlist_mutex);
            playlist.pop_front();
        }

        std::cout << "[STREAM] Track finished\n";
    }
}

void Server::httpLoop() {
    while (running) {
        // TODO:
        // - obsługa REST API
        // - /queue
        // - /skip
        // - /progress
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}