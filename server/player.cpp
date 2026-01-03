#include <iostream>
#include <fstream>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <queue>
#include <cstdint>

constexpr size_t BUFFER_SIZE = 4096;
constexpr int GUI_PORT = 3000;
constexpr int AUDIO_BUFFER_SIZE = 512 * 1024;  // 512KB ring buffer

// Nagłówek WAV – wartości są zawsze nadpisywane nagłówkiem z serwera
#pragma pack(push, 1)
struct WavHeader {
    char riff[4];
    uint32_t chunkSize;
    char wave[4];
    char fmt[4];
    uint32_t subchunk1Size;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char data[4];
    uint32_t dataSize;
};
#pragma pack(pop)

// Global state
std::vector<char> audio_buffer;
std::mutex audio_buffer_mutex;
size_t total_bytes_received = 0;  // Total bytes received from server (absolute position)
WavHeader current_header;
std::atomic<double> current_elapsed{0.0};
std::atomic<double> current_duration{0.0};
std::atomic<bool> is_connected{false};
std::atomic<bool> skip_requested{false};

void sendHttpRequest(int sock, const std::string& method, const std::string& path) {
    std::string request = method + " " + path + " HTTP/1.1\r\n";
    request += "Host: 127.0.0.1:9000\r\n";
    request += "Connection: close\r\n";
    request += "\r\n";
    send(sock, request.c_str(), request.size(), 0);
}

void receiveAudioThread(const std::string& server_addr, int server_port) {
    while (true) {
        int audio_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (audio_sock < 0) {
            std::cerr << "[AUDIO] Cannot create socket\n";
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        sockaddr_in server{};
        server.sin_family = AF_INET;
        server.sin_port = htons(server_port);
        inet_pton(AF_INET, server_addr.c_str(), &server.sin_addr);

        if (connect(audio_sock, (sockaddr*)&server, sizeof(server)) < 0) {
            std::cerr << "[AUDIO] Cannot connect to audio server\n";
            close(audio_sock);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        std::cout << "[AUDIO] Connected to server\n";
        is_connected = true;

        // Request audio stream
        sendHttpRequest(audio_sock, "GET", "/audio");

        // Skip HTTP headers
        std::string http_response;
        char c;
        int header_end_count = 0;
        
        while (recv(audio_sock, &c, 1, 0) == 1) {
            http_response += c;
            if (c == '\r' || c == '\n') {
                header_end_count++;
                if (header_end_count == 4)
                    break;
            } else {
                header_end_count = 0;
            }
        }

        // Read WAV header
        WavHeader header;
        ssize_t header_bytes = recv(audio_sock, reinterpret_cast<char*>(&header), sizeof(header), MSG_WAITALL);
        
        if (header_bytes != sizeof(header)) {
            std::cerr << "[AUDIO] Failed to receive WAV header\n";
            close(audio_sock);
            is_connected = false;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        // Store header info
        {
            std::lock_guard<std::mutex> lock(audio_buffer_mutex);
            current_header = header;
            current_duration = static_cast<double>(header.dataSize) / header.byteRate;
            audio_buffer.clear();
            audio_buffer.reserve(AUDIO_BUFFER_SIZE);
            total_bytes_received = 0;
        }

        std::cout << "[AUDIO] Format: " << header.numChannels << " ch, "
                  << header.sampleRate << " Hz, " << header.bitsPerSample << " bit\n";

        // Receive audio stream
        std::vector<char> recv_buffer(BUFFER_SIZE);
        while (true) {
            ssize_t received = recv(audio_sock, recv_buffer.data(), recv_buffer.size(), 0);
            if (received <= 0)
                break;

            {
                std::lock_guard<std::mutex> lock(audio_buffer_mutex);
                audio_buffer.insert(audio_buffer.end(), recv_buffer.begin(), recv_buffer.begin() + received);
                total_bytes_received += received;
                
                // Limit buffer size - keep only the last 512KB
                if (audio_buffer.size() > AUDIO_BUFFER_SIZE) {
                    size_t to_erase = audio_buffer.size() - AUDIO_BUFFER_SIZE;
                    audio_buffer.erase(audio_buffer.begin(), audio_buffer.begin() + to_erase);
                }
                
                // Debug: Log every 100KB received
                static size_t last_log = 0;
                if (total_bytes_received - last_log >= 100000) {
                    std::cout << "[AUDIO] Buffered " << total_bytes_received << " bytes (buffer size: " << audio_buffer.size() << ")\n";
                    last_log = total_bytes_received;
                }
            }

            // Update elapsed time
            double elapsed = static_cast<double>(total_bytes_received) / header.byteRate;
            current_elapsed.store(elapsed);

            // Check for skip request
            if (skip_requested.load()) {
                skip_requested.store(false);
                break;  // Exit and reconnect for next track
            }
        }

        close(audio_sock);
        is_connected = false;
        std::cout << "[AUDIO] Disconnected, reconnecting...\n";
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

void guiThread() {
    int gui_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (gui_socket < 0) {
        perror("gui socket");
        return;
    }

    int opt = 1;
    setsockopt(gui_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("0.0.0.0");
    addr.sin_port = htons(GUI_PORT);

    if (bind(gui_socket, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("gui bind");
        return;
    }

    if (listen(gui_socket, 5) < 0) {
        perror("gui listen");
        return;
    }

    std::cout << "[GUI] Server listening on port " << GUI_PORT << "\n";
    std::cout << "[GUI] Open http://localhost:" << GUI_PORT << " in your browser\n";

    while (true) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);

        int client = accept(gui_socket, (sockaddr*)&client_addr, &len);
        if (client < 0)
            continue;

        char buffer[2048] = {0};
        read(client, buffer, sizeof(buffer) - 1);

        std::string request(buffer);

        if (request.find("GET /") == 0 && request.find("GET /audio") != 0 && 
            request.find("GET /progress") != 0) {
            
            // Serve HTML page
            std::string html = "<!DOCTYPE html>\n"
                "<html lang=\"en\">\n"
                "<head>\n"
                "    <meta charset=\"UTF-8\">\n"
                "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
                "    <title>Radio Player</title>\n"
                "    <style>\n"
                "        * { margin: 0; padding: 0; box-sizing: border-box; }\n"
                "        body { \n"
                "            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;\n"
                "            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);\n"
                "            min-height: 100vh;\n"
                "            display: flex;\n"
                "            justify-content: center;\n"
                "            align-items: center;\n"
                "        }\n"
                "        .container {\n"
                "            background: #222;\n"
                "            padding: 40px;\n"
                "            border-radius: 20px;\n"
                "            box-shadow: 0 20px 60px rgba(0,0,0,0.3);\n"
                "            text-align: center;\n"
                "            color: #fff;\n"
                "            min-width: 400px;\n"
                "        }\n"
                "        h1 { \n"
                "            font-size: 2.5em;\n"
                "            margin-bottom: 10px;\n"
                "            background: linear-gradient(135deg, #667eea, #764ba2);\n"
                "            -webkit-background-clip: text;\n"
                "            -webkit-text-fill-color: transparent;\n"
                "        }\n"
                "        .status {\n"
                "            margin: 20px 0;\n"
                "            font-size: 0.9em;\n"
                "            color: #aaa;\n"
                "        }\n"
                "        .status.connected { color: #4ade80; }\n"
                "        .status.disconnected { color: #f87171; }\n"
                "        progress { \n"
                "            width: 100%;\n"
                "            height: 8px;\n"
                "            margin: 20px 0;\n"
                "            border: none;\n"
                "            border-radius: 10px;\n"
                "            background: #444;\n"
                "        }\n"
                "        progress::-webkit-progress-bar { background: #444; border-radius: 10px; }\n"
                "        progress::-webkit-progress-value { background: linear-gradient(90deg, #667eea, #764ba2); border-radius: 10px; }\n"
                "        .time-display {\n"
                "            font-size: 0.9em;\n"
                "            color: #aaa;\n"
                "            margin: 10px 0;\n"
                "        }\n"
                "        .controls {\n"
                "            display: flex;\n"
                "            gap: 10px;\n"
                "            margin-top: 30px;\n"
                "        }\n"
                "        button {\n"
                "            flex: 1;\n"
                "            padding: 15px;\n"
                "            font-size: 1.1em;\n"
                "            border: none;\n"
                "            border-radius: 10px;\n"
                "            background: linear-gradient(135deg, #667eea, #764ba2);\n"
                "            color: white;\n"
                "            cursor: pointer;\n"
                "            transition: all 0.3s;\n"
                "        }\n"
                "        button:hover { transform: translateY(-2px); box-shadow: 0 10px 20px rgba(102, 126, 234, 0.3); }\n"
                "        button:active { transform: translateY(0); }\n"
                "    </style>\n"
                "</head>\n"
                "<body>\n"
                "    <div class=\"container\">\n"
                "        <h1>Music Radio</h1>\n"
                "        <div id=\"status\" class=\"status disconnected\">● Disconnected</div>\n"
                "        <audio id=\"audio-player\" controls autoplay style=\"width: 100%; margin: 20px 0;\">\n"
                "            <source src=\"/audio\" type=\"audio/wav\">\n"
                "            Your browser does not support the audio element.\n"
                "        </audio>\n"
                "        <progress id=\"progress\" value=\"0\" max=\"1\"></progress>\n"
                "        <div class=\"time-display\" id=\"time\">0:00 / 0:00</div>\n"
                "        <div class=\"controls\">\n"
                "            <button onclick=\"skip()\">Skip Track</button>\n"
                "        </div>\n"
                "    </div>\n"
                "\n"
                "    <script>\n"
                "        function formatTime(seconds) {\n"
                "            const mins = Math.floor(seconds / 60);\n"
                "            const secs = Math.floor(seconds % 60);\n"
                "            return `${mins}:${secs.toString().padStart(2, '0')}`;\n"
                "        }\n"
                "\n"
                "        async function updateProgress() {\n"
                "            try {\n"
                "                const res = await fetch(\"/progress\");\n"
                "                const data = await res.json();\n"
                "                \n"
                "                document.getElementById(\"status\").textContent = data.connected ? \"● Connected\" : \"● Disconnected\";\n"
                "                document.getElementById(\"status\").className = data.connected ? \"status connected\" : \"status disconnected\";\n"
                "                document.getElementById(\"progress\").value = data.position || 0;\n"
                "                document.getElementById(\"time\").textContent = formatTime(data.elapsed) + \" / \" + formatTime(data.duration);\n"
                "            } catch (e) {\n"
                "                console.error(\"Update failed:\", e);\n"
                "            }\n"
                "        }\n"
                "\n"
                "        async function skip() {\n"
                "            try {\n"
                "                await fetch(\"/skip\", { method: \"POST\" });\n"
                "            } catch (e) {\n"
                "                console.error(\"Skip failed:\", e);\n"
                "            }\n"
                "        }\n"
                "\n"
                "        setInterval(updateProgress, 500);\n"
                "        updateProgress();\n"
                "    </script>\n"
                "</body>\n"
                "</html>\n";

            std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Content-Length: " + std::to_string(html.size()) + "\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n" +
                html;

            write(client, response.c_str(), response.size());
            close(client);
        }
        else if (request.find("GET /progress") == 0) {
            
            double elapsed = current_elapsed.load();
            double duration = current_duration.load();
            double position = (duration > 0.0) ? elapsed / duration : 0.0;
            bool connected = is_connected.load();

            std::string body =
                "{ \"elapsed\": " + std::to_string(elapsed) +
                ", \"duration\": " + std::to_string(duration) +
                ", \"position\": " + std::to_string(position) +
                ", \"connected\": " + (connected ? std::string("true") : std::string("false")) + " }";

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
        else if (request.find("GET /audio") == 0) {
            std::cout << "[GUI] Audio client connected\n";
            
            std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: audio/wav\r\n"
                "\r\n";

            write(client, response.c_str(), response.size());

            // Send WAV header and start client from current buffer position
            size_t client_bytes_sent = 0;
            {
                std::lock_guard<std::mutex> lock(audio_buffer_mutex);
                write(client, reinterpret_cast<const char*>(&current_header), sizeof(current_header));
                
                // Start client from current buffer position (not from beginning)
                // This makes it a "live stream" - new clients get current audio
                if (total_bytes_received > audio_buffer.size()) {
                    client_bytes_sent = total_bytes_received - audio_buffer.size();
                }
                
                std::cout << "[GUI] Sent WAV header, buffer has " << audio_buffer.size() << " bytes, total received: " << total_bytes_received << "\n";
                std::cout << "[GUI] Starting client at byte position: " << client_bytes_sent << "\n";
            }

            // Stream audio continuously from the buffer
            // Use absolute byte position (how many bytes from start of stream we've sent)
            
            while (true) {
                std::vector<char> chunk;
                size_t buffer_start_byte = 0;
                
                {
                    std::lock_guard<std::mutex> lock(audio_buffer_mutex);
                    // Which server byte does buffer[0] represent?
                    if (total_bytes_received > audio_buffer.size()) {
                        buffer_start_byte = total_bytes_received - audio_buffer.size();
                    } else {
                        buffer_start_byte = 0;
                    }
                    
                    // Offset into buffer for this client's position
                    ssize_t offset = (ssize_t)client_bytes_sent - (ssize_t)buffer_start_byte;
                    
                    // Only send if client offset is valid within current buffer
                    if (offset >= 0 && offset < (ssize_t)audio_buffer.size()) {
                        size_t to_send = std::min(size_t(4096), audio_buffer.size() - offset);
                        chunk.insert(chunk.end(),
                                   audio_buffer.begin() + offset,
                                   audio_buffer.begin() + offset + to_send);
                        client_bytes_sent += to_send;
                        
                        // Debug: Log first few sends
                        static int send_count = 0;
                        if (send_count < 5) {
                            std::cout << "[GUI] Sending chunk " << to_send << " bytes (total sent: " << client_bytes_sent << ")\n";
                            send_count++;
                        }
                    }
                }
                
                if (chunk.empty()) {
                    // Debug: Log when waiting for data
                    static int wait_count = 0;
                    if (wait_count < 3) {
                        std::cout << "[GUI] Waiting for data... (client pos: " << client_bytes_sent << ", buffer start: " << buffer_start_byte << ")\n";
                        wait_count++;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }

                if (send(client, chunk.data(), chunk.size(), MSG_NOSIGNAL) <= 0)
                    break;
            }

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

int main(int argc, char* argv[]) {
    std::string server_addr = "127.0.0.1";
    int server_port = 8080;

    if (argc > 1) server_addr = argv[1];
    if (argc > 2) server_port = std::atoi(argv[2]);

    std::cout << "[PLAYER] Connecting to server at " << server_addr << ":" << server_port << "\n";

    // Start threads
    std::thread audio_thread(receiveAudioThread, server_addr, server_port);
    std::thread gui_thread_obj(guiThread);

    audio_thread.join();
    gui_thread_obj.join();

    return 0;
}
