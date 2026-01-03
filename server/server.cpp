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

    playlist.push_back({1, "berdly.wav"});
    playlist.push_back({2, "paranoia_intro.wav"});
    playlist.push_back({3, "give_the_anarchist_a_cigarette.wav"});
    playlist.push_back({4, "keine_lust.wav"});

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

        // WAV DEBUGGING ----------------------------------------------
        // Debug: Print what we read
        std::cout << "[STREAM] Playing: " << current.filename << "\n";
        std::cout << "[STREAM] WAV Header Debug:\n";
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
        std::cout << "  Current file position: " << file.tellg() << "\n";
        // ------------------------------------------------------------

        if (header.audioFormat != 1) {
            std::cerr << "[STREAM] Not PCM WAV\n";
            file.close();
            continue;
        }
        
        // Check if fmt chunk is larger than 16 bytes
        if (header.subchunk1Size != 16) {
            std::cout << "[STREAM] WARNING: fmt chunk is " << header.subchunk1Size 
                      << " bytes (expected 16)\n";
            
            // Reposition: go back to right after the fmt chunk header (position 20)
            // Then skip the ENTIRE fmt chunk data
            file.seekg(20, std::ios::beg);  // Position after "fmt " and size
            file.seekg(header.subchunk1Size, std::ios::cur);  // Skip entire fmt data
            
            // Now read the next chunk header (should be "data")
            char data_marker[4];
            uint32_t data_size;
            file.read(data_marker, 4);
            file.read(reinterpret_cast<char*>(&data_size), 4);
            
            std::cout << "[STREAM] After repositioning: chunk = '" << std::string(data_marker, 4) 
                      << "', size = " << data_size << "\n";
            std::cout << "[STREAM] File position now: " << file.tellg() << "\n";
            
            // If it's not "data", it might be another chunk (like "LIST" or "fact")
            // Keep reading chunks until we find "data"
            while (std::string(data_marker, 4) != "data" && file.good()) {
                std::cout << "[STREAM] Skipping chunk '" << std::string(data_marker, 4) 
                          << "' of size " << data_size << "\n";
                file.seekg(data_size, std::ios::cur);  // Skip this chunk's data
                file.read(data_marker, 4);
                file.read(reinterpret_cast<char*>(&data_size), 4);
            }
            
            if (std::string(data_marker, 4) == "data") {
                std::cout << "[STREAM] Found data chunk: size = " << data_size << "\n";
                // Update header with correct data info
                std::memcpy(header.data, data_marker, 4);
                header.dataSize = data_size;
            } else {
                std::cerr << "[STREAM] ERROR: Could not find 'data' chunk\n";
                file.close();
                continue;
            }
        }
        
        // Final verification - we should be at the data section now
        std::cout << "[STREAM] Final header state:\n";
        std::cout << "  data marker: '" << std::string(header.data, 4) << "'\n";
        std::cout << "  data size: " << header.dataSize << "\n";
        std::cout << "  file position: " << file.tellg() << " (start of audio data)\n";
        
        if (std::string(header.data, 4) != "data") {
            std::cerr << "[STREAM] ERROR: Still not at 'data' chunk!\n";
            file.close();
            continue;
        }
        
        // Store current WAV header for new HTTP clients
        {
            std::lock_guard<std::mutex> lock(wav_header_mutex);
            current_wav_header = header;
            // Set dataSize to max for streaming (unknown length)
            current_wav_header.dataSize = 0xFFFFFFFF;
            current_wav_header.chunkSize = 0xFFFFFFFF;
        }

        current_track_size = header.dataSize;
        current_byte_offset = 0;
        current_elapsed = 0.0;
        current_track_duration = static_cast<double>(header.dataSize) / header.byteRate;

        std::vector<char> buffer(AUDIO_BLOCK);

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

            {
                std::lock_guard<std::mutex> lock(http_audio_clients_mutex);
                for (auto it = http_audio_clients.begin(); it != http_audio_clients.end();) {
                    ssize_t sent = send(*it, buffer.data(), bytesRead, MSG_NOSIGNAL);
                    if (sent <= 0) {
                        close(*it);
                        it = http_audio_clients.erase(it);
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
    addr.sin_addr.s_addr = inet_addr("0.0.0.0");
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

        char buffer[2048] = {0};
        read(client, buffer, sizeof(buffer) - 1);

        std::string request(buffer);

        if (request.find("GET /") == 0 && request.find("GET /progress") != 0 && 
            request.find("GET /audio") != 0 && request.find("GET /skip") != 0) {
            
            // Serve index.html
            std::ifstream html_file("index.html", std::ios::binary);
            if (html_file.is_open()) {
                std::string body((std::istreambuf_iterator<char>(html_file)),
                                 std::istreambuf_iterator<char>());
                html_file.close();

                std::string response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html\r\n"
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
        else if (request.find("GET /audio") == 0) {
            
            // Send HTTP headers for audio stream
            const char* headers =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: audio/wav\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n";
            
            write(client, headers, strlen(headers));
            
            // Send WAV header to the client
            {
                std::lock_guard<std::mutex> lock(wav_header_mutex);
                write(client, reinterpret_cast<const char*>(&current_wav_header), sizeof(current_wav_header));
            }
            
            // Add client to HTTP audio streaming list
            {
                std::lock_guard<std::mutex> lock(http_audio_clients_mutex);
                http_audio_clients.push_back(client);
            }
            
            // Don't close - let streaming loop handle it
            continue;
        }
        else if (request.find("GET /progress") == 0) {

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
            close(client);
        }
        else if (request.find("POST /skip") == 0) {

            skip_requested.store(true);

            std::string body = "{ \"status\": \"skipped\" }";

            std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n" +
                body;

            write(client, response.c_str(), response.size());
            close(client);
        }
        else {
            const char* not_found =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: 0\r\n\r\n";
            write(client, not_found, strlen(not_found));
            close(client);
        }
    }
}