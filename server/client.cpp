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

        // sendHttpRequest(sock, "GET", "/progress");

        // char buffer[1024] = {0};
        // recv(sock, buffer, sizeof(buffer) - 1, 0);

        // std::string response(buffer);
        // size_t json_start = response.find('{');
        
        // if (json_start != std::string::npos) {
        //     std::cout << "[CONTROL] Progress: " << response.substr(json_start) << "\n";
        // }

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

    std::cout << "[CLIENT] Connected to audio stream\n";

    // First, skip HTTP headers
    std::string http_response;
    char c;
    int header_end_count = 0;
    
    std::cout << "[CLIENT] Skipping HTTP headers...\n";
    while (recv(audio_sock, &c, 1, 0) == 1) {
        http_response += c;
        if (c == '\r' || c == '\n') {
            header_end_count++;
            if (header_end_count == 4) // \r\n\r\n
                break;
        } else {
            header_end_count = 0;
        }
    }
    
    std::cout << "[CLIENT] HTTP Response Headers:\n" << http_response << "\n";

    // Now read the WAV header
    WavHeader header;
    ssize_t header_bytes = recv(audio_sock, reinterpret_cast<char*>(&header), sizeof(header), MSG_WAITALL);
    
    if (header_bytes != sizeof(header)) {
        std::cerr << "[CLIENT] Failed to receive complete WAV header (got " << header_bytes << " bytes)\n";
        close(audio_sock);
        return 1;
    }

    // Debug: Print what we received
    std::cout << "[CLIENT] WAV Header Debug:\n";
    std::cout << "  RIFF: " << std::string(header.riff, 4) << "\n";
    std::cout << "  ChunkSize: " << header.chunkSize << "\n";
    std::cout << "  WAVE: " << std::string(header.wave, 4) << "\n";
    std::cout << "  fmt: " << std::string(header.fmt, 4) << "\n";
    std::cout << "  Subchunk1Size: " << header.subchunk1Size << "\n";
    std::cout << "  AudioFormat: " << header.audioFormat << "\n";
    std::cout << "  NumChannels: " << header.numChannels << "\n";
    std::cout << "  SampleRate: " << header.sampleRate << "\n";
    std::cout << "  ByteRate: " << header.byteRate << "\n";
    std::cout << "  BlockAlign: " << header.blockAlign << "\n";
    std::cout << "  BitsPerSample: " << header.bitsPerSample << "\n";
    std::cout << "  data: " << std::string(header.data, 4) << "\n";
    std::cout << "  DataSize: " << header.dataSize << "\n";

    // Verify header
    if (std::string(header.riff, 4) != "RIFF" || std::string(header.wave, 4) != "WAVE") {
        std::cerr << "[CLIENT] ERROR: Invalid WAV header\n";
        close(audio_sock);
        return 1;
    }

    std::ofstream out("stream.wav", std::ios::binary);
    
    // Write the header as-is (server already set dataSize to max)
    out.write(reinterpret_cast<char*>(&header), sizeof(header));

    std::vector<char> buffer(BUFFER_SIZE);
    size_t totalBytes = 0;

    std::cout << "[CLIENT] Commands: 'skip' or 's' to skip, Ctrl+C to quit\n";
    std::cout << "[CLIENT] Recording stream...\n";

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

    // Update header with actual data size
    header.dataSize = totalBytes;
    header.chunkSize = 36 + totalBytes;

    out.seekp(0);
    out.write(reinterpret_cast<char*>(&header), sizeof(header));

    out.close();
    close(audio_sock);

    std::cout << "[CLIENT] Finished, saved stream.wav (" << totalBytes << " bytes)\n";
    return 0;
}