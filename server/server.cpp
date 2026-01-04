#include "server.h"
#include "wav.h"
#include <iostream>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <iterator>
#include <chrono>
#include <sstream>
#include <cctype>
#include <sys/stat.h>
#include <cstdlib>
#include <ctime>

Server::Server(int port) : port(port) {}

Server::~Server() {
    stop();
}

//
// ======================= PORTAUDIO CALLBACK =======================
//
int portaudioCallback(
    const void*,
    void* output,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo*,
    PaStreamCallbackFlags,
    void* userData
) {
    Server* server = static_cast<Server*>(userData);
    float* out = static_cast<float*>(output);

    // Lock-free: use atomic operations to avoid blocking the real-time audio thread
    size_t pos = server->current_position.load(std::memory_order_acquire);

    for (unsigned long i = 0; i < framesPerBuffer; ++i) {
        for (int ch = 0; ch < server->current_wav.channels; ++ch) {
            if (pos + 3 <= server->current_wav.data.size()) {
                const uint8_t* p = server->current_wav.data.data() + pos;
                // Convert 24-bit PCM to float
                int32_t sample =
                    (p[0]) |
                    (p[1] << 8) |
                    (p[2] << 16);
                if (sample & 0x800000)
                    sample |= ~0xFFFFFF;
                out[i * server->current_wav.channels + ch] = sample / 8388608.0f;
                pos += 3;
            } else {
                out[i * server->current_wav.channels + ch] = 0.0f;
            }
        }
    }

    // Update position atomically without locking
    server->current_position.store(pos, std::memory_order_release);

    // Notify HTTP/TCP clients that new data is available (non-blocking)
    server->playback_cv.notify_all();

    return (pos >= server->current_wav.data.size())
           ? paComplete
           : paContinue;
}

// Ensure directory exists (mkdir -p like for single level)
static bool ensureDir(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(path.c_str(), 0755) == 0;
}

static std::string sanitizeFilename(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (c == '/' || c == '\\') continue;
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-')
            out.push_back(c);
    }
    if (out.empty()) out = "upload.wav";
    return out;
}

static long parseContentLength(const std::string& headers) {
    std::istringstream iss(headers);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.size() && (line.back() == '\r')) line.pop_back();
        std::string key = "Content-Length:";
        if (line.size() >= key.size() && std::equal(key.begin(), key.end(), line.begin(), [](char a,char b){return std::tolower(a)==std::tolower(b);} )) {
            try {
                return std::stol(line.substr(key.size()));
            } catch (...) { return -1; }
        }
    }
    return -1;
}

static std::string parseBoundary(const std::string& headers) {
    std::istringstream iss(headers);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.size() && (line.back() == '\r')) line.pop_back();
        std::string key = "Content-Type:";
        if (line.size() >= key.size() && std::equal(key.begin(), key.end(), line.begin(), [](char a,char b){return std::tolower(a)==std::tolower(b);} )) {
            auto pos = line.find("boundary=");
            if (pos != std::string::npos) {
                std::string b = line.substr(pos + 9);
                if (!b.empty() && b.front() == '"' && b.back() == '"') {
                    b = b.substr(1, b.size() - 2);
                }
                return b;
            }
        }
    }
    return {};
}

// Very simple multipart parser for single file field named "file"
static bool parseMultipartSingleFile(
    const std::string& body,
    const std::string& boundary,
    std::string& outFilename,
    std::string& outData
) {
    std::string marker = "--" + boundary;
    size_t pos = body.find(marker);
    if (pos == std::string::npos) return false;
    pos += marker.size();
    if (pos + 2 > body.size() || body.compare(pos, 2, "\r\n") != 0) return false;
    pos += 2; // skip CRLF

    // find header end of the part
    size_t header_end = body.find("\r\n\r\n", pos);
    if (header_end == std::string::npos) return false;
    std::string part_headers = body.substr(pos, header_end - pos);

    // filename from Content-Disposition
    {
        auto dispoPos = part_headers.find("Content-Disposition:");
        if (dispoPos == std::string::npos) return false;
        auto fnamePos = part_headers.find("filename=", dispoPos);
        if (fnamePos == std::string::npos) return false;
        fnamePos += 9;
        if (fnamePos < part_headers.size() && part_headers[fnamePos] == '"') {
            ++fnamePos;
            auto endq = part_headers.find('"', fnamePos);
            if (endq == std::string::npos) return false;
            outFilename = part_headers.substr(fnamePos, endq - fnamePos);
        } else {
            auto endsp = part_headers.find_first_of(";\r\n", fnamePos);
            outFilename = part_headers.substr(fnamePos, endsp - fnamePos);
        }
    }

    size_t data_start = header_end + 4;
    size_t data_end = body.find("\r\n" + marker, data_start);
    if (data_end == std::string::npos) return false;

    outData.assign(body.data() + data_start, body.data() + data_end);
    return true;
}


void Server::start() {
    running = true;
    setupHttpSocket();
    // Zainicjalizuj generator losowy dla automatycznej kolejki
    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    // Bazowa kolejka: najpierw berdly, potem sans
    enqueueTrack("berdly.wav");
    enqueueTrack("sans.wav");
    
    // Initialize PortAudio
    Pa_Initialize();

    stream_thread = std::thread(&Server::streamingLoop, this);
    http_thread   = std::thread(&Server::httpLoop, this);

    std::cout << "[SERVER] Started\n";
}

void Server::stop() {
    running = false;

    stopAudioStream();
    Pa_Terminate();

    if (http_socket > 0)
        close(http_socket);

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

    // Allow socket reuse to avoid "Address already in use" on restart
    int reuse = 1;
    if (setsockopt(http_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt SO_REUSEADDR");
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

        // Handle each client in a separate thread to avoid blocking
        std::thread([this, client]() {
            handleHttpClient(client);
        }).detach();
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

    long content_length = parseContentLength(headers);
    std::string boundary = parseBoundary(headers);

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

    if (path == "/upload" && method == "POST") {
        const long MAX_UPLOAD = 50 * 1024 * 1024; // 50MB limit
        if (content_length < 0 || content_length > MAX_UPLOAD) {
            sendHttpResponse(client, "{\"error\":\"invalid content-length\"}", "application/json", 400);
            return;
        }
        if (boundary.empty()) {
            sendHttpResponse(client, "{\"error\":\"missing boundary\"}", "application/json", 400);
            return;
        }

        // Read remaining body if not fully received
        while (static_cast<long>(body.size()) < content_length) {
            char chunk[4096];
            ssize_t r = recv(client, chunk, sizeof(chunk), 0);
            if (r <= 0) break;
            body.append(chunk, static_cast<size_t>(r));
        }

        if (static_cast<long>(body.size()) < content_length) {
            sendHttpResponse(client, "{\"error\":\"incomplete upload\"}", "application/json", 400);
            return;
        }

        // Spawn background thread to process upload asynchronously
        std::thread([this, body, boundary]() {
            std::string filename;
            std::string filedata;
            if (!parseMultipartSingleFile(body, boundary, filename, filedata)) {
                std::cerr << "[UPLOAD] Failed to parse multipart data\n";
                return;
            }

            filename = sanitizeFilename(filename);
            if (!ensureDir("uploads")) {
                std::cerr << "[UPLOAD] Cannot create uploads directory\n";
                return;
            }

            std::string outPath = "uploads/" + filename;
            std::ofstream ofs(outPath, std::ios::binary);
            if (!ofs) {
                std::cerr << "[UPLOAD] Cannot open file for writing: " << outPath << "\n";
                return;
            }
            ofs.write(filedata.data(), static_cast<std::streamsize>(filedata.size()));
            ofs.close();

            int id = enqueueTrack(outPath);
            playback_cv.notify_all();

            std::cout << "[UPLOAD] Saved " << outPath << " (" << filedata.size() << " bytes), enqueued as #" << id << "\n";
        }).detach();

        // Immediate response - processing happens in background
        sendHttpResponse(client, "{\"status\":\"processing\"}", "application/json", 202);
        return;
    }

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
        std::string filename;

        {
            std::lock_guard<std::mutex> lock(playback_mutex);
            if (current_wav.sampleRate > 0 && current_wav.channels > 0 && current_wav.bitsPerSample > 0) {
                double bytesPerSecond = current_wav.sampleRate * current_wav.channels * (current_wav.bitsPerSample / 8.0);
                duration = current_wav.data.size() / bytesPerSecond;
                size_t pos = current_position.load(std::memory_order_acquire);
                elapsed = pos / bytesPerSecond;
                if (duration > 0.0)
                    position = elapsed / duration;
                filename = current_track_name;
            }
        }

        std::string body =
            "{\"position\":" + std::to_string(position) +
            ",\"elapsed\":" + std::to_string(elapsed) +
            ",\"duration\":" + std::to_string(duration) +
            ",\"filename\":\"" + filename + "\"}";
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
                    body += "{\"id\":" + std::to_string(playlist[i].id) + ",\"index\":" + std::to_string(i) + ",\"file\":\"" + playlist[i].filename + "\"}";
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

    if (path == "/queue/move" && method == "POST") {
        // Parse "from=0&to=2" from body
        int from = -1, to = -1;
        {
            std::istringstream iss(trim(body));
            std::string token;
            while (std::getline(iss, token, '&')) {
                size_t eq = token.find('=');
                if (eq != std::string::npos) {
                    std::string key = token.substr(0, eq);
                    std::string val = token.substr(eq + 1);
                    if (key == "from") from = std::stoi(val);
                    if (key == "to") to = std::stoi(val);
                }
            }
        }

        if (from < 0 || to < 0) {
            sendHttpResponse(client, "{\"error\":\"missing from/to parameters\"}", "application/json", 400);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(playlist_mutex);
            if (static_cast<size_t>(from) >= playlist.size() || static_cast<size_t>(to) >= playlist.size()) {
                sendHttpResponse(client, "{\"error\":\"index out of range\"}", "application/json", 400);
                return;
            }

            auto it = playlist.begin();
            std::advance(it, from);
            Track track = *it;
            playlist.erase(it);

            it = playlist.begin();
            std::advance(it, to);
            playlist.insert(it, track);
        }

        sendHttpResponse(client, "{\"status\":\"moved\",\"from\":" + std::to_string(from) + ",\"to\":" + std::to_string(to) + "}", "application/json", 200);
        return;
    }

    if (path == "/queue/remove" && method == "POST") {
        // Parse "index=2" from body
        int index = -1;
        {
            std::istringstream iss(trim(body));
            std::string token;
            while (std::getline(iss, token, '&')) {
                size_t eq = token.find('=');
                if (eq != std::string::npos) {
                    std::string key = token.substr(0, eq);
                    std::string val = token.substr(eq + 1);
                    if (key == "index") index = std::stoi(val);
                }
            }
        }

        if (index < 0) {
            sendHttpResponse(client, "{\"error\":\"missing index parameter\"}", "application/json", 400);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(playlist_mutex);
            if (static_cast<size_t>(index) >= playlist.size()) {
                sendHttpResponse(client, "{\"error\":\"index out of range\"}", "application/json", 400);
                return;
            }

            auto it = playlist.begin();
            std::advance(it, index);
            playlist.erase(it);
        }

        sendHttpResponse(client, "{\"status\":\"removed\",\"index\":" + std::to_string(index) + "}", "application/json", 200);
        return;
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

static bool send_all(int sock, const uint8_t* data, size_t len) {
    size_t sent_total = 0;
    while (sent_total < len) {
        ssize_t s = send(sock, data + sent_total, len - sent_total, 0);
        if (s <= 0) return false;
        sent_total += static_cast<size_t>(s);
    }
    return true;
}

void Server::streamHttpAudio(int client) {
    std::vector<uint8_t> data;
    int sampleRate = 0;
    int channels = 0;
    int bits = 0;
    size_t start_pos = 0;

    {
        std::lock_guard<std::mutex> lock(playback_mutex);
        if (current_wav.data.empty()) {
            sendHttpResponse(client, "No audio loaded", "text/plain", 404);
            return;
        }
        data = current_wav.data; // copy for thread safety; simple for now
        sampleRate = current_wav.sampleRate;
        channels = current_wav.channels;
        bits = current_wav.bitsPerSample;
        start_pos = current_position.load(std::memory_order_acquire); // start from current live position
    }

    // Very simple WAV header writer (PCM)
    auto write_u32 = [](uint8_t* p, uint32_t v) {
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; };
    auto write_u16 = [](uint8_t* p, uint16_t v) {
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; };

    uint32_t data_size = static_cast<uint32_t>(data.size());
    uint32_t byte_rate = static_cast<uint32_t>(sampleRate * channels * (bits / 8));
    uint16_t block_align = static_cast<uint16_t>(channels * (bits / 8));

    std::vector<uint8_t> header(44, 0);
    std::memcpy(&header[0], "RIFF", 4);
    write_u32(&header[4], 36 + data_size);
    std::memcpy(&header[8], "WAVE", 4);
    std::memcpy(&header[12], "fmt ", 4);
    write_u32(&header[16], 16);
    write_u16(&header[20], 1); // PCM
    write_u16(&header[22], static_cast<uint16_t>(channels));
    write_u32(&header[24], static_cast<uint32_t>(sampleRate));
    write_u32(&header[28], byte_rate);
    write_u16(&header[32], block_align);
    write_u16(&header[34], static_cast<uint16_t>(bits));
    std::memcpy(&header[36], "data", 4);
    write_u32(&header[40], data_size);

    // Chunked transfer: stream from current live position onward
    auto send_chunk = [&](const uint8_t* ptr, size_t len) -> bool {
        if (len == 0) return true;
        char size_line[32];
        int m = std::snprintf(size_line, sizeof(size_line), "%zx\r\n", len);
        if (m <= 0) return false;
        if (!send_all(client, reinterpret_cast<const uint8_t*>(size_line), static_cast<size_t>(m))) return false;
        if (!send_all(client, ptr, len)) return false;
        return send_all(client, reinterpret_cast<const uint8_t*>("\r\n"), 2);
    };

    std::string http_header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: audio/wav\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n";

    if (!send_all(client, reinterpret_cast<const uint8_t*>(http_header.data()), http_header.size())) {
        close(client);
        return;
    }

    // Send WAV header as first chunk
    if (!send_chunk(header.data(), header.size())) { close(client); return; }

    size_t sent = start_pos;
    const size_t track_size = data.size();

    while (running) {
        size_t available = 0;
        bool track_done = false;
        bool do_skip = false;

        {
            std::unique_lock<std::mutex> lock(playback_mutex);
            playback_cv.wait_for(lock, std::chrono::milliseconds(200), [&]{
                size_t pos = current_position.load(std::memory_order_acquire);
                return !running || skip_requested || pos > sent || pos >= track_size;
            });

            size_t pos = current_position.load(std::memory_order_acquire);
            available = (pos > sent) ? (pos - sent) : 0;
            track_done = pos >= track_size;
            do_skip = skip_requested;
        }

        if (available) {
            size_t to_send = available;
            if (sent + to_send > track_size) to_send = track_size - sent;
            if (!send_chunk(data.data() + sent, to_send)) {
                close(client);
                return;
            }
            sent += to_send;
        }

        if (track_done || do_skip || !running)
            break;
    }

    // terminating chunk
    send_all(client, reinterpret_cast<const uint8_t*>("0\r\n\r\n"), 5);
    close(client);
}

//
// ======================= 24-BIT PCM → FLOAT =======================
//
inline float pcm24ToFloat(const uint8_t* p) {
    int32_t sample =
        (p[0]) |
        (p[1] << 8) |
        (p[2] << 16);

    // sign extension
    if (sample & 0x800000)
        sample |= ~0xFFFFFF;

    return sample / 8388608.0f; // 2^23
}

//
// ======================= WAV LOADER =======================
//
WavFile Server::loadWav(const std::string& filename) {
    std::ifstream f(filename, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot open WAV file: " + filename);

    char chunkId[4];
    uint32_t chunkSize;

    // RIFF
    f.read(chunkId, 4);
    if (std::strncmp(chunkId, "RIFF", 4) != 0)
        throw std::runtime_error("Not a RIFF file");

    f.read(reinterpret_cast<char*>(&chunkSize), 4);

    f.read(chunkId, 4);
    if (std::strncmp(chunkId, "WAVE", 4) != 0)
        throw std::runtime_error("Not a WAVE file");

    WavFile wav;

    while (f) {
        f.read(chunkId, 4);
        f.read(reinterpret_cast<char*>(&chunkSize), 4);

        if (std::strncmp(chunkId, "fmt ", 4) == 0) {
            uint16_t audioFormat;
            f.read(reinterpret_cast<char*>(&audioFormat), 2);
            f.read(reinterpret_cast<char*>(&wav.channels), 2);
            f.read(reinterpret_cast<char*>(&wav.sampleRate), 4);
            f.ignore(6); // byteRate + blockAlign
            f.read(reinterpret_cast<char*>(&wav.bitsPerSample), 2);

            if (audioFormat != 1)
                throw std::runtime_error("Only PCM WAV supported");

            if (wav.bitsPerSample != 24)
                throw std::runtime_error("Only 24-bit WAV supported");

            f.ignore(chunkSize - 16);
        }
        else if (std::strncmp(chunkId, "data", 4) == 0) {
            wav.data.resize(chunkSize);
            f.read(reinterpret_cast<char*>(wav.data.data()), chunkSize);
            break;
        }
        else {
            // skip unknown chunk
            f.ignore(chunkSize);
        }
    }

    if (wav.data.empty())
        throw std::runtime_error("No audio data found");

    return wav;
}

//
// ======================= STREAMING LOOP =======================
//
void Server::streamingLoop() {
    while (running) {
        {
            std::lock_guard<std::mutex> lock(playlist_mutex);
            
            // Check if we need to load a new track
            size_t pos = current_position.load(std::memory_order_acquire);
            if (pos >= current_wav.data.size() || skip_requested) {
                skip_requested = false;
                stopAudioStream();
                
                if (!playlist.empty()) {
                    Track track = playlist.front();
                    playlist.pop_front();
                    
                    try {
                        {
                            std::lock_guard<std::mutex> pb_lock(playback_mutex);
                            current_wav = loadWav(track.filename);
                            current_position.store(0, std::memory_order_release);
                            current_track_name = track.filename;
                        }
                        std::cout << "[SERVER] Now playing: " << track.filename << "\n";
                        std::cout << "  Sample rate: " << current_wav.sampleRate << " Hz\n";
                        std::cout << "  Channels:    " << current_wav.channels << "\n";
                        std::cout << "  Bit depth:   " << current_wav.bitsPerSample << "\n";
                        
                        // Start PortAudio stream for this track
                        startAudioStream();
                    }
                    catch (const std::exception& e) {
                        std::cerr << "[SERVER] Error loading track: " << e.what() << "\n";
                        stopAudioStream();
                    }
                } else {
                    // No more tracks in queue – dodaj losowo bazowe utwory,
                    // aby radio grało w kółko, gdy nic nie jest w kolejce.
                    const char* baseTracks[] = {"berdly.wav", "sans.wav"};
                    const int baseCount = 2;
                    int idx = std::rand() % baseCount;
                    int id = next_track_id++;
                    playlist.push_back({id, baseTracks[idx]});
                    std::cout << "[SERVER] Auto-enqueued: " << baseTracks[idx] << " (#" << id << ")\n";
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

//
// ======================= PORTAUDIO STREAM MANAGEMENT =======================
//
void Server::startAudioStream() {
    if (audio_stream) {
        Pa_StopStream(audio_stream);
        Pa_CloseStream(audio_stream);
        audio_stream = nullptr;
    }

    if (current_wav.sampleRate <= 0 || current_wav.channels <= 0) {
        std::cerr << "[AUDIO] Invalid WAV parameters\n";
        return;
    }

    PaError err = Pa_OpenDefaultStream(
        &audio_stream,
        0,                              // no input
        current_wav.channels,           // output channels
        paFloat32,                      // 32-bit float
        current_wav.sampleRate,         // sample rate
        256,                            // frames per buffer
        portaudioCallback,              // callback
        this                            // user data
    );

    if (err != paNoError) {
        std::cerr << "[AUDIO] Error opening stream: " << Pa_GetErrorText(err) << "\n";
        audio_stream = nullptr;
        return;
    }

    err = Pa_StartStream(audio_stream);
    if (err != paNoError) {
        std::cerr << "[AUDIO] Error starting stream: " << Pa_GetErrorText(err) << "\n";
        Pa_CloseStream(audio_stream);
        audio_stream = nullptr;
        return;
    }

    std::cout << "[AUDIO] PortAudio stream started\n";
}

void Server::stopAudioStream() {
    if (audio_stream) {
        Pa_StopStream(audio_stream);
        Pa_CloseStream(audio_stream);
        audio_stream = nullptr;
        std::cout << "[AUDIO] PortAudio stream stopped\n";
    }
}