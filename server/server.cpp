#include "server.h"
#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>

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
    addr.sin_addr.s_addr = INADDR_ANY;
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
        // TODO:
        // - sprawdź czy kolejka niepusta
        // - otwórz WAV
        // - czytaj blokami
        // - wysyłaj do klientów
        // - obsłuż skip_requested

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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