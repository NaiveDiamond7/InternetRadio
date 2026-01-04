#include "server.h"
#include <iostream>

int main() {
    Server server(9000);
    server.start();

    std::cout << "Press ENTER to stop server...\n";
    std::cin.get();

    server.stop();
    return 0;
}