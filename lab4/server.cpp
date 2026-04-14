#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <vector>
#include <queue>
#include <algorithm>
#include <stdint.h>
#include <string>

#define PORT 8080
#define MAX_PAYLOAD 1024
#define THREAD_POOL_SIZE 10

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

// Информация о клиенте
struct ClientInfo {
    int sockfd;
    struct sockaddr_in addr;
    std::string nickname;
    bool authenticated;
};

// Глобальные данные
std::vector<ClientInfo> clients;
std::queue<int> client_queue;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
bool server_running = true;

// Функции логирования OSI
void log_osi(int layer, const char* action) {
    const char* layer_name;
    switch(layer) {
        case 4: layer_name = "Transport"; break;
        case 5: layer_name = "Session"; break;
        case 6: layer_name = "Presentation"; break;
        case 7: layer_name = "Application"; break;
        default: layer_name = "Unknown";
    }
    std::cout << "[Layer " << layer << " - " << layer_name << "] " << action << std::endl;
}

// Отправка сообщения клиенту
void send_message(int sockfd, uint8_t type, const char* payload = nullptr) {
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
    log_osi(7, "prepare response");
    log_osi(6, "serialize Message");
    log_osi(4, "send()");
    send(sockfd, &msg, sizeof(msg), 0);
}

// Широковещательная рассылка (всем, кроме отправителя)
void broadcast_message(const std::string& formatted_message, int sender_sock) {
    pthread_mutex_lock(&clients_mutex);
    for (const auto& client : clients) {
        if (client.sockfd != sender_sock && client.authenticated) {
            send_message(client.sockfd, MSG_TEXT, formatted_message.c_str());
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Удаление клиента из списка
void remove_client(int sockfd) {
    pthread_mutex_lock(&clients_mutex);
    auto it = std::find_if(clients.begin(), clients.end(),
        [sockfd](const ClientInfo& c) { return c.sockfd == sockfd; });
    if (it != clients.end()) {
        std::cout << "User " << it->nickname << " disconnected" << std::endl;
        clients.erase(it);
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Рабочий поток (обслуживает клиента)
void* worker_thread(void* arg) {
    while (server_running) {
        // Извлечение сокета из очереди
        pthread_mutex_lock(&queue_mutex);
        while (client_queue.empty() && server_running) {
            pthread_cond_wait(&queue_cond, &queue_mutex);
        }
        if (!server_running) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }
        int client_fd = client_queue.front();
        client_queue.pop();
        pthread_mutex_unlock(&queue_mutex);

        // Получение адреса клиента
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        getpeername(client_fd, (struct sockaddr*)&client_addr, &addr_len);
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        int client_port = ntohs(client_addr.sin_port);

        // Ожидание HELLO
        Message msg;
        int bytes = recv(client_fd, &msg, sizeof(msg), 0);
        log_osi(4, "recv()");
        if (bytes <= 0 || msg.type != MSG_HELLO) {
            log_osi(6, "deserialize Message");
            std::cerr << "Expected HELLO, closing" << std::endl;
            close(client_fd);
            continue;
        }
        log_osi(6, "deserialize Message type=HELLO");
        send_message(client_fd, MSG_WELCOME);

        // Аутентификация (ожидание MSG_AUTH)
        bool authenticated = false;
        std::string nickname;
        while (!authenticated && server_running) {
            bytes = recv(client_fd, &msg, sizeof(msg), 0);
            log_osi(4, "recv()");
            if (bytes <= 0) break;
            log_osi(6, "deserialize Message");
            if (msg.type != MSG_AUTH) {
                log_osi(5, "session: waiting for AUTH, ignoring");
                send_message(client_fd, MSG_ERROR, "Authenticate first");
                close(client_fd);
                break;
            }
            msg.length = ntohl(msg.length);
            nickname = std::string(msg.payload);
            bool valid = true;
            if (nickname.empty()) {
                send_message(client_fd, MSG_ERROR, "Nickname cannot be empty");
                valid = false;
            } else {
                pthread_mutex_lock(&clients_mutex);
                for (const auto& c : clients) {
                    if (c.nickname == nickname) {
                        send_message(client_fd, MSG_ERROR, "Nickname already taken");
                        valid = false;
                        break;
                    }
                }
                pthread_mutex_unlock(&clients_mutex);
            }
            if (!valid) {
                close(client_fd);
                break;
            }
            authenticated = true;
            log_osi(5, "authentication success");
        }
        if (!authenticated) {
            close(client_fd);
            continue;
        }

        // Добавление клиента в список
        ClientInfo new_client{client_fd, client_addr, nickname, true};
        pthread_mutex_lock(&clients_mutex);
        clients.push_back(new_client);
        pthread_mutex_unlock(&clients_mutex);
        std::cout << "User " << nickname << " connected from " << client_ip << ":" << client_port << std::endl;

        // Цикл обработки сообщений от авторизованного клиента
        bool active = true;
        while (active && server_running) {
            bytes = recv(client_fd, &msg, sizeof(msg), 0);
            log_osi(4, "recv()");
            if (bytes <= 0) break;
            log_osi(6, "deserialize Message");
            msg.length = ntohl(msg.length);

            switch (msg.type) {
                case MSG_TEXT:
                    log_osi(7, "handle MSG_TEXT");
                    {
                        std::string formatted = "[" + nickname + "]: " + msg.payload;
                        std::cout << formatted << std::endl;
                        broadcast_message(formatted, client_fd);
                    }
                    break;
                case MSG_PRIVATE:
                    log_osi(7, "handle MSG_PRIVATE");
                    {
                        std::string payload(msg.payload);
                        size_t colon = payload.find(':');
                        if (colon == std::string::npos) {
                            send_message(client_fd, MSG_ERROR, "Invalid private message format");
                            break;
                        }
                        std::string target = payload.substr(0, colon);
                        std::string message = payload.substr(colon + 1);
                        pthread_mutex_lock(&clients_mutex);
                        bool found = false;
                        for (const auto& c : clients) {
                            if (c.nickname == target && c.authenticated) {
                                std::string private_msg = "[PRIVATE][" + nickname + "]: " + message;
                                send_message(c.sockfd, MSG_TEXT, private_msg.c_str());
                                found = true;
                                break;
                            }
                        }
                        pthread_mutex_unlock(&clients_mutex);
                        if (!found) {
                            send_message(client_fd, MSG_ERROR, ("User " + target + " not found").c_str());
                        }
                    }
                    break;
                case MSG_PING:
                    log_osi(7, "handle MSG_PING");
                    send_message(client_fd, MSG_PONG);
                    break;
                case MSG_BYE:
                    log_osi(7, "handle MSG_BYE");
                    active = false;
                    break;
                default:
                    log_osi(7, "unknown message type");
                    send_message(client_fd, MSG_ERROR, "Unknown command");
                    break;
            }
        }
        remove_client(client_fd);
        close(client_fd);
    }
    return nullptr;
}

int main() {
    int server_fd;
    struct sockaddr_in server_addr;

    // Создание TCP сокета
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    // Настройка адреса сервера
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // Привязка сокета
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); close(server_fd); return 1;
    }
    // Ожидание подключений
    if (listen(server_fd, 10) < 0) {
        perror("listen"); close(server_fd); return 1;
    }
    std::cout << "Server listening on port " << PORT << std::endl;

    // Создание пула рабочих потоков
    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; ++i) {
        pthread_create(&threads[i], nullptr, worker_thread, nullptr);
    }

    // Основной цикл приёма подключений
    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) { perror("accept"); continue; }
        pthread_mutex_lock(&queue_mutex);
        client_queue.push(client_fd);
        pthread_cond_signal(&queue_cond);
        pthread_mutex_unlock(&queue_mutex);
    }

    // Очистка (необязательно, для полноты)
    server_running = false;
    pthread_cond_broadcast(&queue_cond);
    for (int i = 0; i < THREAD_POOL_SIZE; ++i) {
        pthread_join(threads[i], nullptr);
    }
    close(server_fd);
    return 0;
}