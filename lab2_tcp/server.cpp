#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdint.h>

#define PORT 8080
#define MAX_PAYLOAD 1024

typedef struct {
    uint32_t length;
    uint8_t type;
    char payload[MAX_PAYLOAD];
} Message;

enum {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_TEXT = 3,
    MSG_PING = 4,
    MSG_PONG = 5,
    MSG_BYE = 6
};

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    Message msg;
    
    // создание TCP сокета
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Ошибка создания сокета" << std::endl;
        return 1;
    }
    
    // настройка адреса сервера
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    // привязка сокета
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Ошибка привязки" << std::endl;
        close(server_fd);
        return 1;
    }
    
    // ожидание подключения (одного клиента)
    if (listen(server_fd, 1) < 0) {
        std::cerr << "Ошибка listen" << std::endl;
        close(server_fd);
        return 1;
    }
    
    std::cout << "TCP сервер запущен на порту " << PORT << std::endl;
    std::cout << "Ожидание подключения..." << std::endl;
    
    // принятие клиента
    client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
    if (client_fd < 0) {
        std::cerr << "Ошибка accept" << std::endl;
        close(server_fd);
        return 1;
    }
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    std::cout << "Client connected" << std::endl;
    
    // получение MSG_HELLO
    int bytes = recv(client_fd, &msg, sizeof(msg), 0);
    if (bytes <= 0 || msg.type != MSG_HELLO) {
        std::cerr << "Ошибка: ожидался HELLO" << std::endl;
        close(client_fd);
        close(server_fd);
        return 1;
    }
    
    std::cout << "[" << client_ip << ":" << ntohs(client_addr.sin_port) 
              << "]: " << msg.payload << std::endl;
    
    // отправка MSG_WELCOME
    msg.length = htonl(0);  // только type, payload пустой
    msg.type = MSG_WELCOME;
    send(client_fd, &msg, sizeof(msg), 0);
    
    // основной цикл обработки сообщений
    while (true) {
        bytes = recv(client_fd, &msg, sizeof(msg), 0);
        
        if (bytes <= 0) {
            std::cout << "Client disconnected" << std::endl;
            break;
        }
        
        msg.length = ntohl(msg.length);
        
        switch (msg.type) {
            case MSG_TEXT:
                std::cout << "[" << client_ip << ":" << ntohs(client_addr.sin_port) 
                          << "]: " << msg.payload << std::endl;
                break;
                
            case MSG_PING:
                msg.type = MSG_PONG;
                msg.length = htonl(0);
                send(client_fd, &msg, sizeof(msg), 0);
                break;
                
            case MSG_BYE:
                std::cout << "Client disconnected" << std::endl;
                close(client_fd);
                break;
                
            default:
                std::cout << "Неизвестный тип сообщения: " << (int)msg.type << std::endl;
                break;
        }
        
        if (msg.type == MSG_BYE) break;
    }
    
    close(client_fd);
    close(server_fd);
    return 0;
}