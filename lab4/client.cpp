#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <string>
#include <stdint.h>

#define PORT 8080
#define MAX_PAYLOAD 1024
#define RECONNECT_DELAY 2

// Структура сообщения
typedef struct {
    uint32_t length;
    uint8_t type;
    char payload[MAX_PAYLOAD];
} Message;

// Типы сообщений
enum {
    MSG_HELLO = 1,
    MSG_WELCOME = 2,
    MSG_TEXT = 3,
    MSG_PING = 4,
    MSG_PONG = 5,
    MSG_BYE = 6,
    MSG_AUTH = 7,
    MSG_PRIVATE = 8,
    MSG_ERROR = 9,
    MSG_SERVER_INFO = 10
};

// Глобальные переменные клиента
int sockfd = -1;
bool connected = false;
pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;
std::string nickname;

// Отправка сообщения
void send_message(uint8_t type, const char* payload = nullptr) {
    Message msg;
    msg.type = type;
    if (payload) {
        msg.length = htonl(strlen(payload));
        strncpy(msg.payload, payload, MAX_PAYLOAD - 1);
        msg.payload[MAX_PAYLOAD - 1] = '\0';
    } else {
        msg.length = htonl(0);
        msg.payload[0] = '\0';
    }
    pthread_mutex_lock(&sock_mutex);
    if (connected && sockfd >= 0) {
        send(sockfd, &msg, sizeof(msg), 0);
    }
    pthread_mutex_unlock(&sock_mutex);
}

// Поток приёма сообщений от сервера
void* receive_thread(void* arg) {
    Message msg;
    while (true) {
        pthread_mutex_lock(&sock_mutex);
        int fd = sockfd;
        bool conn = connected;
        pthread_mutex_unlock(&sock_mutex);
        if (!conn || fd < 0) {
            usleep(100000);
            continue;
        }
        int bytes = recv(fd, &msg, sizeof(msg), 0);
        if (bytes <= 0) {
            pthread_mutex_lock(&sock_mutex);
            connected = false;
            close(sockfd);
            sockfd = -1;
            pthread_mutex_unlock(&sock_mutex);
            std::cout << "\n[SYSTEM]: Connection lost. Reconnecting..." << std::endl;
            break;
        }
        msg.length = ntohl(msg.length);
        switch (msg.type) {
            case MSG_TEXT:
                std::cout << "\r" << msg.payload << std::endl << "> " << std::flush;
                break;
            case MSG_PONG:
                std::cout << "\rPONG" << std::endl << "> " << std::flush;
                break;
            case MSG_ERROR:
                std::cout << "\r[ERROR]: " << msg.payload << std::endl << "> " << std::flush;
                break;
            case MSG_SERVER_INFO:
                std::cout << "\r[SERVER]: " << msg.payload << std::endl << "> " << std::flush;
                break;
            case MSG_BYE:
                pthread_mutex_lock(&sock_mutex);
                connected = false;
                close(sockfd);
                sockfd = -1;
                pthread_mutex_unlock(&sock_mutex);
                std::cout << "\r[SERVER]: Disconnected" << std::endl;
                return nullptr;
            default:
                break;
        }
    }
    return nullptr;
}

// Подключение к серверу и аутентификация
bool connect_to_server() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(fd);
        return false;
    }

    // Отправка HELLO
    Message msg;
    msg.type = MSG_HELLO;
    msg.length = htonl(0);
    msg.payload[0] = '\0';
    if (send(fd, &msg, sizeof(msg), 0) < 0) {
        close(fd);
        return false;
    }

    // Ожидание WELCOME
    int bytes = recv(fd, &msg, sizeof(msg), 0);
    if (bytes <= 0 || msg.type != MSG_WELCOME) {
        close(fd);
        return false;
    }

    // Отправка AUTH с ником
    msg.type = MSG_AUTH;
    msg.length = htonl(nickname.length());
    strcpy(msg.payload, nickname.c_str());
    if (send(fd, &msg, sizeof(msg), 0) < 0) {
        close(fd);
        return false;
    }

    // Проверка ответа (может прийти ERROR)
    fd_set readfds;
    struct timeval tv = {1, 0};
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    if (select(fd + 1, &readfds, NULL, NULL, &tv) > 0) {
        bytes = recv(fd, &msg, sizeof(msg), 0);
        if (bytes > 0 && msg.type == MSG_ERROR) {
            std::cerr << "Authentication failed: " << msg.payload << std::endl;
            close(fd);
            return false;
        }
    }

    pthread_mutex_lock(&sock_mutex);
    sockfd = fd;
    connected = true;
    pthread_mutex_unlock(&sock_mutex);
    std::cout << "Connected as " << nickname << std::endl;
    return true;
}

int main() {
    // Ввод ника
    std::cout << "Enter your nickname: ";
    std::getline(std::cin, nickname);
    if (nickname.empty()) nickname = "Anonymous";

    // Попытка подключения и аутентификации
    if (!connect_to_server()) {
        std::cerr << "Failed to authenticate. Exiting." << std::endl;
        return 1;
    }

    // Запуск потока приёма сообщений
    pthread_t recv_thread;
    pthread_create(&recv_thread, nullptr, receive_thread, nullptr);
    pthread_detach(recv_thread);

    // Основной цикл ввода команд
    std::string input;
    while (connected) {
        std::cout << "> ";
        std::getline(std::cin, input);
        if (input == "/quit") {
            send_message(MSG_BYE);
            break;
        } else if (input == "/ping") {
            send_message(MSG_PING);
        } else if (input.rfind("/w ", 0) == 0) {
            size_t first_space = input.find(' ', 3);
            if (first_space == std::string::npos) {
                std::cout << "Usage: /w <nick> <message>" << std::endl;
            } else {
                std::string target = input.substr(3, first_space - 3);
                std::string message = input.substr(first_space + 1);
                std::string payload = target + ":" + message;
                send_message(MSG_PRIVATE, payload.c_str());
            }
        } else if (!input.empty()) {
            send_message(MSG_TEXT, input.c_str());
        }
    }

    // Закрытие сокета
    pthread_mutex_lock(&sock_mutex);
    if (sockfd >= 0) close(sockfd);
    pthread_mutex_unlock(&sock_mutex);
    return 0;
}