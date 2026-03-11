#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdint.h>

#define PORT 8080
#define MAX_PAYLOAD 1024
#define NICKNAME "user"

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
    int sockfd;
    struct sockaddr_in server_addr;
    Message msg;
    
    // создание TCP сокета
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        std::cerr << "Ошибка создания сокета" << std::endl;
        return 1;
    }
    
    // настройка адреса сервера
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    // подключение к серверу
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Ошибка подключения" << std::endl;
        close(sockfd);
        return 1;
    }
    
    std::cout << "Connected" << std::endl;
    
    // отправка MSG_HELLO
    msg.length = htonl(strlen(NICKNAME));
    msg.type = MSG_HELLO;
    strcpy(msg.payload, NICKNAME);
    send(sockfd, &msg, sizeof(msg), 0);
    
    // получение MSG_WELCOME
    int bytes = recv(sockfd, &msg, sizeof(msg), 0);
    if (bytes <= 0 || msg.type != MSG_WELCOME) {
        std::cerr << "Ошибка: ожидался WELCOME" << std::endl;
        close(sockfd);
        return 1;
    }
    
    char server_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &server_addr.sin_addr, server_ip, INET_ADDRSTRLEN);
    std::cout << "Welcome " << server_ip << ":" << PORT << std::endl;
    
    // настройка неблокирующего ввода
    fd_set readfds;
    struct timeval tv;
    
    while (true) {
        FD_ZERO(&readfds);
        FD_SET(0, &readfds);  // stdin
        FD_SET(sockfd, &readfds);
        
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        if (select(sockfd + 1, &readfds, NULL, NULL, &tv) < 0) {
            std::cerr << "Ошибка select" << std::endl;
            break;
        }
        
        // проверка ввода пользователя
        if (FD_ISSET(0, &readfds)) {
            std::string input;
            std::getline(std::cin, input);
            
            if (input == "/quit") {
                msg.type = MSG_BYE;
                msg.length = htonl(0);
                send(sockfd, &msg, sizeof(msg), 0);
                break;
            }
            else if (input == "/ping") {
                msg.type = MSG_PING;
                msg.length = htonl(0);
                send(sockfd, &msg, sizeof(msg), 0);
            }
            else {
                msg.type = MSG_TEXT;
                msg.length = htonl(input.length());
                strcpy(msg.payload, input.c_str());
                send(sockfd, &msg, sizeof(msg), 0);
            }
        }
        
        // проверка данных от сервера
        if (FD_ISSET(sockfd, &readfds)) {
            bytes = recv(sockfd, &msg, sizeof(msg), 0);
            
            if (bytes <= 0) {
                std::cout << "Disconnected" << std::endl;
                break;
            }
            
            msg.length = ntohl(msg.length);
            
            switch (msg.type) {
                case MSG_TEXT:
                    std::cout << msg.payload << std::endl;
                    break;
                    
                case MSG_PONG:
                    std::cout << "PONG" << std::endl;
                    break;
                    
                case MSG_BYE:
                    std::cout << "Disconnected" << std::endl;
                    close(sockfd);
                    return 0;
                    
                default:
                    break;
            }
        }
    }
    
    close(sockfd);
    return 0;
}