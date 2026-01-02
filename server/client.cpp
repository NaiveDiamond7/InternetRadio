#include <iostream>
#include <fstream>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <thread>
#include <chrono>

constexpr size_t BUFFER_SIZE = 4096;

#pragma pack(push, 1)
struct WavHeader {
    char riff[4] = {'R','I','F','F'};
    uint32_t chunkSize = 0;
    char wave[4] = {'W','A','V','E'};
    char fmt[4]  = {'f','m','t',' '};
    uint32_t subchunk1Size = 16;
    uint16_t audioFormat = 1;
    uint16_t numChannels = 2;
    uint32_t sampleRate = 48000;
    uint32_t byteRate = sampleRate * numChannels * 2;
    uint16_t blockAlign = numChannels * 2;
    uint16_t bitsPerSample = 24;
    char data[4] = {'d','a','t','a'};
    uint32_t dataSize = 0;
};
#pragma pack(pop)

int audio_sock = -1;

void sendHttpRequest(int sock, const std::string& method, const std::string& path) {
    std::string request = method + " " + path + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1:8080\r\n";
    request += "Connection: close\r\n";
    request += "\r\n";
    
    send(sock, request.c_str(), request.size(), 0);
}

void fetchProgress() {
    while (true) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::cerr << "[CONTROL] Cannot create socket\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(8080);
        inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

        if (connect(sock, (sockaddr*)&server, sizeof(server)) < 0) {
            std::cerr << "[CONTROL] Cannot connect to control server\n";
            close(sock);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        sendHttpRequest(sock, "GET", "/progress");

        char buffer[1024] = {0};
        recv(sock, buffer, sizeof(buffer) - 1, 0);

        std::string response(buffer);
        size_t json_start = response.find('{');
        
        if (json_start != std::string::npos) {
            std::cout << "[CONTROL] Progress: " << response.substr(json_start) << "\n";
        }

        close(sock);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void handleUserInput() {
    std::string command;
    
    while (std::cin >> command) {
        if (command == "skip" || command == "s") {
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) {
                std::cerr << "[CONTROL] Cannot create socket\n";
                continue;
            }

            sockaddr_in server{};
            server.sin_family = AF_INET;
            server.sin_port = htons(8080);
            inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

            if (connect(sock, (sockaddr*)&server, sizeof(server)) < 0) {
                std::cerr << "[CONTROL] Cannot connect to control server\n";
                close(sock);
                continue;
            }

            sendHttpRequest(sock, "POST", "/skip");

            char buffer[256] = {0};
            recv(sock, buffer, sizeof(buffer) - 1, 0);
            
            std::cout << "[CONTROL] Skip sent\n";
            close(sock);
        }
    }
}

int main() {
    audio_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (audio_sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &server.sin_addr);

    if (connect(audio_sock, (sockaddr*)&server, sizeof(server)) < 0) {
        perror("connect");
        return 1;
    }

    // Send HTTP GET /audio request
    sendHttpRequest(audio_sock, "GET", "/audio");

    std::ofstream out("stream.wav", std::ios::binary);
    WavHeader header;
    out.write(reinterpret_cast<char*>(&header), sizeof(header));

    std::vector<char> buffer(BUFFER_SIZE);
    size_t totalBytes = 0;

    std::cout << "[CLIENT] Connected to audio stream\n";
    std::cout << "[CLIENT] Commands: 'skip' or 's' to skip, 'q' to quit\n";

    // Start control threads
    std::thread progress_thread(fetchProgress);
    std::thread input_thread(handleUserInput);
    
    progress_thread.detach();
    input_thread.detach();

    // Read audio stream
    while (true) {
        ssize_t received = recv(audio_sock, buffer.data(), buffer.size(), 0);
        if (received <= 0)
            break;

        out.write(buffer.data(), received);
        totalBytes += received;
    }

    header.dataSize = totalBytes;
    header.chunkSize = 36 + totalBytes;

    out.seekp(0);
    out.write(reinterpret_cast<char*>(&header), sizeof(header));

    out.close();
    close(audio_sock);

    std::cout << "[CLIENT] Finished, saved stream.wav (" << totalBytes << " bytes)\n";
    return 0;
}