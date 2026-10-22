#include "model/flm_gateway.h"
#include "under_ui/console_agent.h"
#include <iostream>

int main() {
    std::cout << "Starting Watcher...\n";

    StreamQueue queue;
    FlmGateway gateway(queue, "127.0.0.1", 52625);
    ConsoleAgent agent(gateway, queue);

    agent.run(); // Блокирующий вызов

    std::cout << "Watcher stopped.\n";
    return 0;
}