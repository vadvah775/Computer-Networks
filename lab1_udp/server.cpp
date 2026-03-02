#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define BUFFER_SIZE 1024

int main() {
    struct sockaddr_in server_addr, client_addr;
    char buffer[BUFFER_SIZE];
    socklen_t client_len = sizeof(client_addr);
    
    // создание UDP сокета
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        std::cerr << "Ошибка создания сокета" << std::endl;
        return 1;
    }
    
    // настройка адреса сервера
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    // привязка сокета
    if (bind(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Ошибка привязки" << std::endl;
        close(sockfd);
        return 1;
    }
    
    std::cout << "UDP сервер запущен на порту " << PORT << std::endl;
    
    while (true) {
        // получение сообщения от клиента
        int bytes_received = recvfrom(sockfd, buffer, BUFFER_SIZE - 1, 0,
                                      (struct sockaddr*)&client_addr, &client_len);
        if (bytes_received < 0) {
            std::cerr << "Ошибка получения данных" << std::endl;
            continue;
        }
        
        buffer[bytes_received] = '\0';
        
        // вывод информации о клиенте
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "Получено от " << client_ip << ":" 
                  << ntohs(client_addr.sin_port) << " - " << buffer << std::endl;
        
        // отправкаответа
        sendto(sockfd, buffer, bytes_received, 0,
               (struct sockaddr*)&client_addr, client_len);
    }
    
    close(sockfd);
    return 0;
}
