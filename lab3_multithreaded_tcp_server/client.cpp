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
#define RECONNECT_DELAY 2  // 2 секунды

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

// глобальные для клиента
int sockfd = -1;
bool connected = false;
pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;
std::string nickname = "user";

// отправка сообщения
void send_message(uint8_t type, const char* payload = nullptr) {
    Message msg;
    msg.type = type;
    if (payload) {
        msg.length = htonl(strlen(payload));
        strcpy(msg.payload, payload);
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

// поток приёма сообщений
void* receive_thread(void* arg) {
    Message msg;
    while (true) {
        pthread_mutex_lock(&sock_mutex);
        int fd = sockfd;
        bool conn = connected;
        pthread_mutex_unlock(&sock_mutex);
        if (!conn || fd < 0) {
            usleep(100000); // 0.1 сек
            continue;
        }
        int bytes = recv(fd, &msg, sizeof(msg), 0);
        if (bytes <= 0) {
            // разрыв соединения
            pthread_mutex_lock(&sock_mutex);
            connected = false;
            close(sockfd);
            sockfd = -1;
            pthread_mutex_unlock(&sock_mutex);
            std::cout << "\nСоединение потеряно. Попытка переподключения..." << std::endl;
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
            case MSG_BYE:
                pthread_mutex_lock(&sock_mutex);
                connected = false;
                close(sockfd);
                sockfd = -1;
                pthread_mutex_unlock(&sock_mutex);
                std::cout << "\rСервер завершил соединение." << std::endl;
                return nullptr;
            default:
                break;
        }
    }
    return nullptr;
}

// подключение к серверу
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

    // отправляем HELLO
    Message msg;
    msg.type = MSG_HELLO;
    msg.length = htonl(nickname.length());
    strcpy(msg.payload, nickname.c_str());
    if (send(fd, &msg, sizeof(msg), 0) < 0) {
        close(fd);
        return false;
    }

    // ждём WELCOME
    int bytes = recv(fd, &msg, sizeof(msg), 0);
    if (bytes <= 0 || msg.type != MSG_WELCOME) {
        close(fd);
        return false;
    }

    pthread_mutex_lock(&sock_mutex);
    sockfd = fd;
    connected = true;
    pthread_mutex_unlock(&sock_mutex);

    std::cout << "Подключено к серверу." << std::endl;
    return true;
}

int main() {
    std::cout << "Введите ваш ник: ";
    std::getline(std::cin, nickname);
    if (nickname.empty()) nickname = "user";

    // основной цикл подключения
    while (true) {
        if (!connected) {
            std::cout << "Попытка подключения..." << std::endl;
            if (connect_to_server()) {
                // запуск потока приёма
                pthread_t recv_thread;
                pthread_create(&recv_thread, nullptr, receive_thread, nullptr);
                pthread_detach(recv_thread);
            } else {
                sleep(RECONNECT_DELAY);
                continue;
            }
        }

        // цикл ввода сообщений, пока соединение активно
        std::string input;
        while (connected) {
            std::cout << "> ";
            std::getline(std::cin, input);
            if (input == "/quit") {
                send_message(MSG_BYE);
                pthread_mutex_lock(&sock_mutex);
                connected = false;
                close(sockfd);
                sockfd = -1;
                pthread_mutex_unlock(&sock_mutex);
                break;
            } else if (input == "/ping") {
                send_message(MSG_PING);
            } else if (!input.empty()) {
                send_message(MSG_TEXT, input.c_str());
            }
        }
        if (!connected) {
            std::cout << "Ожидание переподключения..." << std::endl;
            sleep(RECONNECT_DELAY);
        }
    }

    return 0;
}
