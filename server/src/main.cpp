#include "server.h"
#include <iostream>

#ifndef DEFAULT_HTTP_PORT
#define DEFAULT_HTTP_PORT 8080
#endif

int main(int argc, char** argv) {
    int httpPort = DEFAULT_HTTP_PORT;
    if (argc >= 2) {
        try {
            httpPort = std::stoi(argv[1]);
        } catch (...) {
            std::cerr << "Nieprawidłowy port: " << argv[1]
                      << " – używam domyślnego " << DEFAULT_HTTP_PORT << "\n";
            httpPort = DEFAULT_HTTP_PORT;
        }
    }

    std::cout << "Starting HTTP server on port " << httpPort << "...\n";
    std::cout << "Open UI in browser: http://127.0.0.1:" << httpPort << "/" << "\n";

    Server server(httpPort);
    server.start();

    std::cout << "Press ENTER to stop server...\n";
    std::cin.get();

    server.stop();
    return 0;
}