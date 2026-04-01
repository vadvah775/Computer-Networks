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

#define PORT 8080
#define MAX_PAYLOAD 1024
#define THREAD_POOL_SIZE 10

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

// структура клиента
struct ClientInfo {
    int sockfd;
    struct sockaddr_in addr;
    std::string nickname;
};

// глобальные данные
std::vector<ClientInfo> clients;          // список клиентов
std::queue<int> client_queue;             // очередь сокетов для рабочих потоков
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
bool server_running = true;

// отправка сообщения конкретному клиенту
void send_message(int sockfd, uint8_t type, const char* payload = nullptr) {
    Message msg;
    msg.type = type;
    if (payload) {
        msg.length = htonl(strlen(payload));
        strcpy(msg.payload, payload);
    } else {
        msg.length = htonl(0);
        msg.payload[0] = '\0';
    }
    send(sockfd, &msg, sizeof(msg), 0);
}

// широковещательная рассылка (всем, кроме отправителя)
void broadcast_message(const char* payload, int sender_sock) {
    pthread_mutex_lock(&clients_mutex);
    for (const auto& client : clients) {
        if (client.sockfd != sender_sock) {
            send_message(client.sockfd, MSG_TEXT, payload);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

// удаление клиента из списка
void remove_client(int sockfd) {
    pthread_mutex_lock(&clients_mutex);
    clients.erase(std::remove_if(clients.begin(), clients.end(),
        [sockfd](const ClientInfo& c) { return c.sockfd == sockfd; }),
        clients.end());
    pthread_mutex_unlock(&clients_mutex);
}

// оабочий поток
void* worker_thread(void* arg) {
    while (server_running) {
        // получаем сокет из очереди
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

        // получаем адрес клиента
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        getpeername(client_fd, (struct sockaddr*)&client_addr, &addr_len);
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

        // обработка HELLO
        Message msg;
        int bytes = recv(client_fd, &msg, sizeof(msg), 0);
        if (bytes <= 0 || msg.type != MSG_HELLO) {
            std::cerr << "Ошибка: ожидался HELLO от " << client_ip << std::endl;
            close(client_fd);
            continue;
        }
        msg.length = ntohl(msg.length);
        std::string nickname(msg.payload);

        // О=отправляем WELCOME
        send_message(client_fd, MSG_WELCOME);

        // добавляем клиента в список
        ClientInfo new_client{client_fd, client_addr, nickname};
        pthread_mutex_lock(&clients_mutex);
        clients.push_back(new_client);
        pthread_mutex_unlock(&clients_mutex);
        std::cout << "Client connected: " << client_ip << ":" << ntohs(client_addr.sin_port)
                  << " (" << nickname << ")" << std::endl;

        // цикл обработки сообщений от клиента
        bool client_active = true;
        while (client_active && server_running) {
            bytes = recv(client_fd, &msg, sizeof(msg), 0);
            if (bytes <= 0) {
                std::cout << "Client disconnected: " << client_ip << ":" << ntohs(client_addr.sin_port)
                          << " (" << nickname << ")" << std::endl;
                break;
            }
            msg.length = ntohl(msg.length);

            switch (msg.type) {
                case MSG_TEXT:
                    std::cout << nickname << ": " << msg.payload << std::endl;
                    broadcast_message(msg.payload, client_fd);
                    break;
                case MSG_PING:
                    send_message(client_fd, MSG_PONG);
                    break;
                case MSG_BYE:
                    std::cout << "Client disconnected: " << client_ip << ":" << ntohs(client_addr.sin_port)
                              << " (" << nickname << ")" << std::endl;
                    client_active = false;
                    break;
                default:
                    std::cerr << "Неизвестный тип сообщения от " << nickname << std::endl;
                    break;
            }
        }

        // удаляем клиента из списка и закрываем сокет
        remove_client(client_fd);
        close(client_fd);
    }
    return nullptr;
}

int main() {
    int server_fd;
    struct sockaddr_in server_addr;

    // создание сокета
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "Ошибка создания сокета" << std::endl;
        return 1;
    }

    //настройка адреса
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // привязка
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Ошибка привязки" << std::endl;
        close(server_fd);
        return 1;
    }

    // ожидание подключений (очередь до 10)
    if (listen(server_fd, 10) < 0) {
        std::cerr << "Ошибка listen" << std::endl;
        close(server_fd);
        return 1;
    }

    std::cout << "Сервер запущен на порту " << PORT << std::endl;

    // создание пула рабочих потоков
    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; ++i) {
        pthread_create(&threads[i], nullptr, worker_thread, nullptr);
    }

    // основной цикл приёма подключений
    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            if (server_running) std::cerr << "Ошибка accept" << std::endl;
            continue;
        }

        // помещаем сокет в очередь
        pthread_mutex_lock(&queue_mutex);
        client_queue.push(client_fd);
        pthread_cond_signal(&queue_cond);
        pthread_mutex_unlock(&queue_mutex);
    }

    // завершение
    server_running = false;
    pthread_cond_broadcast(&queue_cond);
    for (int i = 0; i < THREAD_POOL_SIZE; ++i) {
        pthread_join(threads[i], nullptr);
    }

    close(server_fd);
    return 0;
}
