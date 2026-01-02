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
    setupHttpSocket();

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
    addr.sin_addr.s_addr = inet_addr("0.0.0.0");
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

void Server::setupHttpSocket() {
    http_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (http_socket < 0) {
        perror("http socket");
        exit(1);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(8080);

    if (bind(http_socket, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("http bind");
        exit(1);
    }

    if (listen(http_socket, 5) < 0) {
        perror("http listen");
        exit(1);
    }
}

void Server::httpLoop() {
    while (running) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);

        int client = accept(http_socket, (sockaddr*)&client_addr, &len);
        if (client < 0)
            continue;

        char buffer[1024] = {0};
        read(client, buffer, sizeof(buffer) - 1);

        std::string request(buffer);

        if (request.find("GET /progress") == 0) {

            double elapsed   = current_elapsed.load();
            double duration  = current_track_duration.load();
            double position  = (duration > 0.0) ? elapsed / duration : 0.0;

            std::string body =
                "{ \"elapsed\": "  + std::to_string(elapsed)  +
                ", \"duration\": " + std::to_string(duration) +
                ", \"position\": " + std::to_string(position) + " }";

            std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n" +
                body;

            write(client, response.c_str(), response.size());
        } else {
            const char* not_found =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: 0\r\n\r\n";
            write(client, not_found, strlen(not_found));
        }

        close(client);
    }
}