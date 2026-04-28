#include <iostream>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <vector>
#include <queue>
#include <map>
#include <algorithm>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <json/json.h>   

#define PORT 8080
#define MAX_NAME 32
#define MAX_PAYLOAD 256
#define THREAD_POOL_SIZE 10

// Расширенный формат сообщения
typedef struct {
    uint32_t length;                 // длина полезной части
    uint8_t  type;                   // тип сообщения
    uint32_t msg_id;                 // уникальный идентификатор
    char     sender[MAX_NAME];       // отправитель
    char     receiver[MAX_NAME];     // получатель ("" для broadcast)
    time_t   timestamp;              // время создания
    char     payload[MAX_PAYLOAD];   // данные
} MessageEx;

// Типы сообщений (расширенные)
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
    MSG_SERVER_INFO = 10,
    MSG_LIST = 11,
    MSG_HISTORY = 12,
    MSG_HISTORY_DATA = 13,
    MSG_HELP = 14
};

// Структура клиента
struct ClientInfo {
    int sockfd;
    struct sockaddr_in addr;
    std::string nickname;
    bool authenticated;
};

// Офлайн-сообщение
struct OfflineMsg {
    char sender[MAX_NAME];
    char receiver[MAX_NAME];
    char text[MAX_PAYLOAD];
    time_t timestamp;
    uint32_t msg_id;
};

// Глобальные данные
std::vector<ClientInfo> clients;
std::queue<int> client_queue;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t queue_cond = PTHREAD_COND_INITIALIZER;
bool server_running = true;

// Офлайн-очередь: ключ - ник получателя, значение - список офлайн-сообщений
std::map<std::string, std::vector<OfflineMsg>> offline_queue;
pthread_mutex_t offline_mutex = PTHREAD_MUTEX_INITIALIZER;

// Генератор msg_id
uint32_t next_msg_id = 1;
pthread_mutex_t id_mutex = PTHREAD_MUTEX_INITIALIZER;

// Файл истории
std::string history_file = "chat_history.json";
pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

// Логирование TCP/IP
void log_tcpip_incoming(const char* data, int bytes, const char* src_ip, const char* dst_ip) {
    std::cout << "[Network Access] frame arrived from NIC" << std::endl;
    std::cout << "[Internet] simulated IP hdr: src=" << src_ip << " dst=" << dst_ip << " proto=6" << std::endl;
    std::cout << "[Transport] simulated TCP hdr: recv() " << bytes << " bytes via TCP" << std::endl;
    std::cout << "[Application] deserialize MessageEx -> " << data << std::endl;
}

void log_tcpip_outgoing(const char* action, const char* dst_ip) {
    std::cout << "[Application] " << action << std::endl;
    std::cout << "[Transport] send() via TCP" << std::endl;
    std::cout << "[Internet] destination ip = " << dst_ip << std::endl;
    std::cout << "[Network Access] frame sent to network interface" << std::endl;
}

// Преобразование time_t в строку
std::string time_to_string(time_t t) {
    char buf[32];
    struct tm* tm_info = localtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buf);
}

// Сохранение сообщения в JSON-файл истории
void save_to_history(uint32_t msg_id, time_t timestamp, const std::string& sender,
                     const std::string& receiver, uint8_t type, const std::string& text,
                     bool delivered, bool is_offline) {
    pthread_mutex_lock(&history_mutex);
    Json::Value root;
    Json::Reader reader;
    std::ifstream infile(history_file);
    if (infile.is_open() && reader.parse(infile, root)) {
        infile.close();
    } else {
        root = Json::arrayValue;
    }
    Json::Value entry;
    entry["msg_id"] = msg_id;
    entry["timestamp"] = (Json::UInt64)timestamp;
    entry["sender"] = sender;
    entry["receiver"] = receiver;
    const char* type_str = "UNKNOWN";
    switch(type) {
        case MSG_TEXT: type_str = "MSG_TEXT"; break;
        case MSG_PRIVATE: type_str = "MSG_PRIVATE"; break;
        case MSG_SERVER_INFO: type_str = "MSG_SERVER_INFO"; break;
        default: type_str = "MSG_UNKNOWN";
    }
    entry["type"] = type_str;
    entry["text"] = text;
    entry["delivered"] = delivered;
    entry["is_offline"] = is_offline;
    root.append(entry);
    std::ofstream outfile(history_file);
    outfile << root.toStyledString();
    outfile.close();
    pthread_mutex_unlock(&history_mutex);
}

// Загрузка последних N сообщений из истории
std::vector<std::string> load_history(int n) {
    pthread_mutex_lock(&history_mutex);
    std::vector<std::string> result;
    Json::Value root;
    Json::Reader reader;
    std::ifstream infile(history_file);
    if (infile.is_open() && reader.parse(infile, root) && root.isArray()) {
        infile.close();
        int total = root.size();
        int start = (n > total) ? 0 : total - n;
        for (int i = start; i < total; ++i) {
            Json::Value& e = root[i];
            uint32_t id = e["msg_id"].asUInt();
            time_t ts = (time_t)e["timestamp"].asUInt64();
            std::string sender = e["sender"].asString();
            std::string receiver = e["receiver"].asString();
            std::string type_str = e["type"].asString();
            std::string text = e["text"].asString();
            bool is_offline = e["is_offline"].asBool();
            std::string line = "[" + time_to_string(ts) + "][id=" + std::to_string(id) + "]";
            if (type_str == "MSG_PRIVATE") {
                if (is_offline) line += "[OFFLINE]";
                line += "[" + sender + " -> " + receiver + "]: " + text;
            } else if (type_str == "MSG_TEXT") {
                line += "[" + sender + "]: " + text;
            } else if (type_str == "MSG_SERVER_INFO") {
                line += "[SERVER]: " + text;
            } else {
                line += "[UNKNOWN]";
            }
            result.push_back(line);
        }
    }
    pthread_mutex_unlock(&history_mutex);
    return result;
}

// Отправка сообщения в формате MessageEx
void send_message_ex(int sockfd, uint8_t type, uint32_t msg_id,
                     const char* sender, const char* receiver,
                     const char* payload) {
    MessageEx msg;
    msg.type = type;
    msg.msg_id = msg_id;
    strncpy(msg.sender, sender, MAX_NAME-1);
    msg.sender[MAX_NAME-1] = '\0';
    strncpy(msg.receiver, receiver, MAX_NAME-1);
    msg.receiver[MAX_NAME-1] = '\0';
    msg.timestamp = time(nullptr);
    strncpy(msg.payload, payload, MAX_PAYLOAD-1);
    msg.payload[MAX_PAYLOAD-1] = '\0';
    msg.length = htonl(strlen(payload));  // только полезная нагрузка (не вся структура)
    send(sockfd, &msg, sizeof(msg), 0);
}

// Получить IP-адрес клиента
std::string get_client_ip(int sockfd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getpeername(sockfd, (struct sockaddr*)&addr, &len);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);
    return std::string(ip);
}

// Доставка офлайн-сообщений при подключении клиента
void deliver_offline_messages(int sockfd, const std::string& nickname) {
    pthread_mutex_lock(&offline_mutex);
    auto it = offline_queue.find(nickname);
    if (it != offline_queue.end()) {
        for (const auto& off : it->second) {
            char payload[MAX_PAYLOAD];
            snprintf(payload, MAX_PAYLOAD, "[OFFLINE] %s", off.text);
            send_message_ex(sockfd, MSG_PRIVATE, off.msg_id, off.sender, off.receiver, payload);
            // Помечаем как доставленное в истории
            save_to_history(off.msg_id, off.timestamp, off.sender, off.receiver,
                            MSG_PRIVATE, off.text, true, true);
        }
        offline_queue.erase(it);
    }
    pthread_mutex_unlock(&offline_mutex);
}

// Широковещательная рассылка
void broadcast_message(const std::string& formatted, const std::string& sender_nick, int sender_sock) {
    pthread_mutex_lock(&clients_mutex);
    for (const auto& c : clients) {
        if (c.sockfd != sender_sock && c.authenticated) {
            send_message_ex(c.sockfd, MSG_TEXT, 0, sender_nick.c_str(), "", formatted.c_str());
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

// Рабочий поток
void* worker_thread(void* arg) {
    while (server_running) {
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

        std::string client_ip = get_client_ip(client_fd);
        log_tcpip_incoming("HELLO", sizeof(MessageEx), client_ip.c_str(), "127.0.0.1");

        // Начальный обмен HELLO/WELCOME
        MessageEx msg;
        int bytes = recv(client_fd, &msg, sizeof(msg), 0);
        if (bytes <= 0 || msg.type != MSG_HELLO) {
            std::cerr << "Expected HELLO, closing" << std::endl;
            close(client_fd);
            continue;
        }
        send_message_ex(client_fd, MSG_WELCOME, 0, "server", "", "Welcome");
        log_tcpip_outgoing("MSG_WELCOME", client_ip.c_str());

        // Аутентификация
        bool authenticated = false;
        std::string nickname;
        while (!authenticated && server_running) {
            bytes = recv(client_fd, &msg, sizeof(msg), 0);
            log_tcpip_incoming("AUTH", bytes, client_ip.c_str(), "127.0.0.1");
            if (bytes <= 0) break;
            if (msg.type != MSG_AUTH) {
                send_message_ex(client_fd, MSG_ERROR, 0, "server", "", "Authenticate first");
                close(client_fd);
                break;
            }
            nickname = std::string(msg.payload);
            bool valid = true;
            if (nickname.empty()) {
                send_message_ex(client_fd, MSG_ERROR, 0, "server", "", "Nickname cannot be empty");
                valid = false;
            } else {
                pthread_mutex_lock(&clients_mutex);
                for (const auto& c : clients) {
                    if (c.nickname == nickname) {
                        send_message_ex(client_fd, MSG_ERROR, 0, "server", "", "Nickname already taken");
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
            std::cout << "Authentication success: " << nickname << std::endl;
            log_tcpip_incoming("authentication success", 0, client_ip.c_str(), "127.0.0.1");
        }
        if (!authenticated) {
            close(client_fd);
            continue;
        }

        // Добавляем клиента
        ClientInfo new_client{client_fd, {}, nickname, true};
        pthread_mutex_lock(&clients_mutex);
        clients.push_back(new_client);
        pthread_mutex_unlock(&clients_mutex);
        std::cout << "User " << nickname << " connected from " << client_ip << std::endl;

        // Доставка офлайн-сообщений
        deliver_offline_messages(client_fd, nickname);

        // Цикл обработки сообщений
        bool active = true;
        while (active && server_running) {
            bytes = recv(client_fd, &msg, sizeof(msg), 0);
            log_tcpip_incoming("message", bytes, client_ip.c_str(), "127.0.0.1");
            if (bytes <= 0) break;

            switch (msg.type) {
                case MSG_TEXT: {
                    std::string text = msg.payload;
                    std::string formatted = "[" + nickname + "]: " + text;
                    std::cout << formatted << std::endl;
                    // Сохраняем в историю
                    uint32_t new_id = __sync_fetch_and_add(&next_msg_id, 1);
                    save_to_history(new_id, time(nullptr), nickname, "", MSG_TEXT, text, true, false);
                    broadcast_message(text, nickname, client_fd);
                    break;
                }
                case MSG_PRIVATE: {
                    std::string payload(msg.payload);
                    size_t colon = payload.find(':');
                    if (colon == std::string::npos) {
                        send_message_ex(client_fd, MSG_ERROR, 0, "server", "", "Invalid private message format");
                        break;
                    }
                    std::string target = payload.substr(0, colon);
                    std::string message = payload.substr(colon + 1);
                    pthread_mutex_lock(&clients_mutex);
                    bool found = false;
                    for (const auto& c : clients) {
                        if (c.nickname == target && c.authenticated) {
                            std::string private_msg = "[PRIVATE][" + nickname + "->" + target + "]: " + message;
                            send_message_ex(c.sockfd, MSG_PRIVATE, 0, nickname.c_str(), target.c_str(), private_msg.c_str());
                            found = true;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&clients_mutex);
                    if (!found) {
                        // Офлайн-сохранение
                        pthread_mutex_lock(&offline_mutex);
                        OfflineMsg off;
                        strncpy(off.sender, nickname.c_str(), MAX_NAME-1);
                        strncpy(off.receiver, target.c_str(), MAX_NAME-1);
                        strncpy(off.text, message.c_str(), MAX_PAYLOAD-1);
                        off.timestamp = time(nullptr);
                        off.msg_id = __sync_fetch_and_add(&next_msg_id, 1);
                        offline_queue[target].push_back(off);
                        pthread_mutex_unlock(&offline_mutex);
                        // Сохраняем в историю как недоставленное
                        save_to_history(off.msg_id, off.timestamp, nickname, target, MSG_PRIVATE, message, false, true);
                        send_message_ex(client_fd, MSG_SERVER_INFO, 0, "server", "", "Message stored for offline delivery");
                    } else {
                        // Сохраняем в историю как доставленное
                        uint32_t new_id = __sync_fetch_and_add(&next_msg_id, 1);
                        save_to_history(new_id, time(nullptr), nickname, target, MSG_PRIVATE, message, true, false);
                    }
                    break;
                }
                case MSG_PING:
                    send_message_ex(client_fd, MSG_PONG, 0, "server", "", "pong");
                    log_tcpip_outgoing("MSG_PONG", client_ip.c_str());
                    break;
                case MSG_LIST: {
                    std::string list;
                    pthread_mutex_lock(&clients_mutex);
                    for (const auto& c : clients) {
                        if (c.authenticated) list += c.nickname + "\n";
                    }
                    pthread_mutex_unlock(&clients_mutex);
                    send_message_ex(client_fd, MSG_SERVER_INFO, 0, "server", "", list.c_str());
                    break;
                }
                case MSG_HISTORY: {
                    int n = 20; // default
                    std::string param(msg.payload);
                    if (!param.empty()) {
                        n = std::stoi(param);
                        if (n <= 0) n = 20;
                    }
                    auto history = load_history(n);
                    std::string result;
                    for (const auto& line : history) {
                        result += line + "\n";
                    }
                    send_message_ex(client_fd, MSG_HISTORY_DATA, 0, "server", "", result.c_str());
                    break;
                }
                case MSG_BYE:
                    active = false;
                    break;
                default:
                    send_message_ex(client_fd, MSG_ERROR, 0, "server", "", "Unknown command");
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

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind"); close(server_fd); return 1;
    }
    if (listen(server_fd, 10) < 0) {
        perror("listen"); close(server_fd); return 1;
    }
    std::cout << "Server listening on port " << PORT << std::endl;

    pthread_t threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; ++i) {
        pthread_create(&threads[i], nullptr, worker_thread, nullptr);
    }

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

    server_running = false;
    pthread_cond_broadcast(&queue_cond);
    for (int i = 0; i < THREAD_POOL_SIZE; ++i) {
        pthread_join(threads[i], nullptr);
    }
    close(server_fd);
    return 0;
}