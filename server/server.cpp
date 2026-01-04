#include "server.h"

#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <cctype>
#include <sys/stat.h>

Server::Server(int port) : port(port) {}

Server::~Server() {
    stop();
}

// Proste wyliczenie czasu trwania WAV
// Zakładamy: 48 kHz, stereo, 24-bit (tak jak w przykładowych plikach).
double Server::computeTrackDuration(const std::string& filename) {
    struct stat st{};
    if (stat(filename.c_str(), &st) != 0)
        return 0.0;

    if (st.st_size <= 44) // nagłówek WAV
        return 0.0;

    const double bytesPerSecond = 48000.0 * 2.0 * 3.0; // sr * channels * bytesPerSample
    return (st.st_size - 44) / bytesPerSecond;
}


void Server::start() {
    running = true;
    setupHttpSocket();

    stream_thread = std::thread(&Server::streamingLoop, this);
    http_thread   = std::thread(&Server::httpLoop, this);

    std::cout << "[SERVER] Started on port " << port << "\n";
}

void Server::stop() {
    running = false;

    if (http_socket >= 0) {
        close(http_socket);
        http_socket = -1;
    }

    if (stream_thread.joinable()) stream_thread.join();
    if (http_thread.joinable())   http_thread.join();

    std::cout << "[SERVER] Stopped\n";
}

void Server::setupHttpSocket() {
    http_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (http_socket < 0) {
        perror("http socket");
        exit(1);
    }

    int reuse = 1;
    if (setsockopt(http_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        exit(1);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("0.0.0.0");
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(http_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
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

        handleHttpClient(client);
    }
}

void Server::sendHttpResponse(int client, const std::string& body, const std::string& contentType, int status) {
    const char* statusText = status == 200 ? "OK" : (status == 404 ? "Not Found" : "OK");
    std::string header =
        "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n" +
        "Content-Type: " + contentType + "\r\n" +
        "Content-Length: " + std::to_string(body.size()) + "\r\n" +
        "Connection: close\r\n\r\n";

    send(client, header.c_str(), header.size(), 0);
    send(client, body.c_str(), body.size(), 0);
    close(client);
}

void Server::handleHttpClient(int client) {
    char buffer[4096];
    ssize_t n = recv(client, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        close(client);
        return;
    }
    buffer[n] = '\0';

    std::string request(buffer);

    // Split headers/body
    size_t header_end = request.find("\r\n\r\n");
    std::string headers = header_end != std::string::npos ? request.substr(0, header_end) : request;
    std::string body = header_end != std::string::npos ? request.substr(header_end + 4) : std::string();

    // Parse first line: METHOD PATH HTTP/1.1
    size_t method_end = headers.find(' ');
    if (method_end == std::string::npos) {
        close(client);
        return;
    }
    size_t path_end = headers.find(' ', method_end + 1);
    if (path_end == std::string::npos) {
        close(client);
        return;
    }

    std::string method = headers.substr(0, method_end);
    std::string path = headers.substr(method_end + 1, path_end - method_end - 1);

    auto trim = [](std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        return s.substr(i);
    };

    // /upload zostało usunięte dla uproszczenia – dodajemy tylko po nazwie pliku przez /queue

    if (path == "/" || path == "/index.html") {
        std::ifstream f("index.html", std::ios::binary);
        if (!f) {
            sendHttpResponse(client, "index not found", "text/plain", 404);
            return;
        }
        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        sendHttpResponse(client, body, "text/html", 200);
        return;
    }

    if (path == "/progress") {
        double duration = 0.0;
        double elapsed = 0.0;
        double position = 0.0;

        {
            std::lock_guard<std::mutex> lock(playback_mutex);
            if (track_playing && current_track_duration > 0.0) {
                duration = current_track_duration;
                auto now = std::chrono::steady_clock::now();
                elapsed = std::chrono::duration<double>(now - current_track_start).count();
                if (elapsed < 0.0) elapsed = 0.0;
                if (elapsed > duration) elapsed = duration;
                position = duration > 0.0 ? (elapsed / duration) : 0.0;
            }
        }

        std::string body =
            "{\"position\":" + std::to_string(position) +
            ",\"elapsed\":" + std::to_string(elapsed) +
            ",\"duration\":" + std::to_string(duration) + "}";
        sendHttpResponse(client, body, "application/json", 200);
        return;
    }

    if (path == "/skip") {
        if (method == "POST")
            skip_requested = true;
        sendHttpResponse(client, "{\"status\":\"skip\"}", "application/json", 200);
        return;
    }

    if (path == "/queue") {
        if (method == "GET") {
            std::string body = "{\"queue\": [";
            {
                std::lock_guard<std::mutex> lock(playlist_mutex);
                for (size_t i = 0; i < playlist.size(); ++i) {
                    body += "\"" + playlist[i].filename + "\"";
                    if (i + 1 < playlist.size()) body += ",";
                }
            }
            body += "]}";
            sendHttpResponse(client, body, "application/json", 200);
            return;
        }

        if (method == "POST") {
            std::string fname = trim(body);
            size_t nl = fname.find_first_of("\r\n");
            if (nl != std::string::npos) fname = fname.substr(0, nl);
            if (fname.empty()) {
                sendHttpResponse(client, "{\"error\":\"filename required\"}", "application/json", 400);
                return;
            }
            int id = enqueueTrack(fname);
            // Wake streaming loop in case it was idle
            playback_cv.notify_all();
            sendHttpResponse(client, "{\"enqueued\":" + std::to_string(id) + ",\"file\":\"" + fname + "\"}", "application/json", 200);
            return;
        }
    }

    if (path == "/audio") {
        if (method == "GET") {
            streamHttpAudio(client);
            return;
        }
    }

    sendHttpResponse(client, "Not Found", "text/plain", 404);
}
 
int Server::enqueueTrack(const std::string& filename) {
    std::lock_guard<std::mutex> lock(playlist_mutex);
    int id = next_track_id++;
    playlist.push_back({id, filename});
    return id;
}

void Server::streamHttpAudio(int client) {
    std::string filename;
    {
        std::lock_guard<std::mutex> lock(playback_mutex);
        if (!track_playing || current_track.empty()) {
            sendHttpResponse(client, "No track playing", "text/plain", 404);
            return;
        }
        filename = current_track;
    }

    std::ifstream f(filename, std::ios::binary);
    if (!f) {
        sendHttpResponse(client, "Cannot open file", "text/plain", 404);
        return;
    }

    f.seekg(0, std::ios::end);
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);

    if (size <= 0) {
        sendHttpResponse(client, "Empty file", "text/plain", 500);
        return;
    }

    std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: audio/wav\r\n"
        "Content-Length: " + std::to_string(size) + "\r\n"
        "Connection: close\r\n\r\n";

    if (send(client, header.c_str(), header.size(), 0) <= 0) {
        close(client);
        return;
    }

    char buf[4096];
    while (f) {
        f.read(buf, sizeof(buf));
        std::streamsize n = f.gcount();
        if (n <= 0) break;
        if (send(client, buf, static_cast<size_t>(n), 0) <= 0)
            break;
    }

    close(client);
}

// Prosta pętla "odtwarzania":
// bierze kolejne pliki z kolejki, wylicza czas trwania
// i czeka, aż minie ten czas albo ktoś wciśnie /skip.
void Server::streamingLoop() {
    while (running) {
        Track next;

        {
            std::lock_guard<std::mutex> lock(playlist_mutex);
            if (playlist.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            next = playlist.front();
            playlist.pop_front();
            skip_requested = false;
        }

        double duration = computeTrackDuration(next.filename);
        if (duration <= 0.0) {
            std::cerr << "[SERVER] Cannot compute duration for: " << next.filename << "\n";
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(playback_mutex);
            current_track = next.filename;
            current_track_duration = duration;
            current_track_start = std::chrono::steady_clock::now();
            track_playing = true;
        }

        std::cout << "[SERVER] Now playing: " << next.filename
                  << " (" << duration << " s)" << "\n";

        while (running) {
            bool do_skip = skip_requested.load();
            double elapsed = 0.0;
            double dur = 0.0;

            {
                std::lock_guard<std::mutex> lock(playback_mutex);
                if (!track_playing) break;
                dur = current_track_duration;
                auto now = std::chrono::steady_clock::now();
                elapsed = std::chrono::duration<double>(now - current_track_start).count();
            }

            if (do_skip || elapsed >= dur)
                break;

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        {
            std::lock_guard<std::mutex> lock(playback_mutex);
            track_playing = false;
        }
    }
}