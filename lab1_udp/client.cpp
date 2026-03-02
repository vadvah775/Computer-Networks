#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BUFFER_SIZE 1024

int main() {
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    socklen_t addr_len = sizeof(server_addr);
    
    // создание UDP сокета
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "Ошибка создания сокета" << std::endl;
        return 1;
    }
    
    // настройка адреса сервера
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    std::cout << "Введите сообщение (или 'exit' для выхода):" << std::endl;
    
    while (true) {
        std::cout << "> ";
        std::cin.getline(buffer, BUFFER_SIZE);
        
        if (strcmp(buffer, "exit") == 0) {
            break;
        }
        
        // отправка сообщения серверу
        sendto(sockfd, buffer, strlen(buffer), 0,
               (struct sockaddr*)&server_addr, sizeof(server_addr));
        
        // получение ответа от сервера
        int bytes_received = recvfrom(sockfd, buffer, BUFFER_SIZE - 1, 0,
                                      (struct sockaddr*)&server_addr, &addr_len);
        if (bytes_received < 0) {
            std::cerr << "Ошибка получения ответа" << std::endl;
            continue;
        }
        
        buffer[bytes_received] = '\0';
        std::cout << "Ответ от сервера: " << buffer << std::endl;
    }
    
    close(sockfd);
    return 0;
}