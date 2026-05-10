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
#include <random>
#include <getopt.h>
#include <json/json.h>

#define PORT 8080
#define MAX_NAME 32
#define MAX_PAYLOAD 256
#define THREAD_POOL_SIZE 10
#define MAX_LAST_IDS 32

typedef struct {
    uint32_t length;
    uint8_t  type;
    uint32_t msg_id;
    char     sender[MAX_NAME];
    char     receiver[MAX_NAME];
    time_t   timestamp;
    char     payload[MAX_PAYLOAD];
} MessageEx;

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
    MSG_HELP = 14,
    MSG_ACK = 15
};

struct ClientInfo {
    int sockfd;
    struct sockaddr_in addr;
    std::string nickname;
    bool authenticated;
    std::vector<uint32_t> last_ids;
};

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

std::map<std::string, std::vector<OfflineMsg>> offline_queue;
pthread_mutex_t offline_mutex = PTHREAD_MUTEX_INITIALIZER;

uint32_t next_msg_id = 1;
pthread_mutex_t id_mutex = PTHREAD_MUTEX_INITIALIZER;

std::string history_file = "chat_history.json";
pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

// Параметры эмуляции
int sim_delay = 50;
double sim_drop = 0.4;
double sim_corrupt = 0.4;
std::mt19937 rng(std::random_device{}());

// Логирование
void log_tcpip(const char* prefix, const char* msg) {
    std::cout << "[" << prefix << "] " << msg << std::endl;
}

void log_recv(int bytes, const char* src_ip, const char* dst_ip, const char* desc) {
    std::cout << "[Network Access] frame arrived from NIC" << std::endl;
    std::cout << "[Internet] simulated IP hdr: src=" << src_ip << " dst=" << dst_ip << " proto=6" << std::endl;
    std::cout << "[Transport] recv() " << bytes << " bytes via TCP" << std::endl;
    std::cout << "[Application] " << desc << std::endl;
}

void log_send(const char* action, const char* dst_ip) {
    std::cout << "[Application] " << action << std::endl;
    std::cout << "[Transport] send() via TCP" << std::endl;
    std::cout << "[Internet] destination ip = " << dst_ip << std::endl;
    std::cout << "[Network Access] frame sent to network interface" << std::endl;
}

// Эмуляция помех
bool emulate_network(MessageEx& msg, int bytes, const char* client_ip) {
    if (sim_delay > 0) {
        log_tcpip("Transport[SIM]", ("DELAY applied: " + std::to_string(sim_delay) + " ms").c_str());
        usleep(sim_delay * 1000);
    }
    if (sim_drop > 0.0) {
        std::uniform_real_distribution<> dis(0.0, 1.0);
        if (dis(rng) < sim_drop) {
            log_tcpip("Transport[SIM]", ("DROP (id=" + std::to_string(msg.msg_id) + ", rate=" + std::to_string(sim_drop) + ")").c_str());
            return false;
        }
    }
    if (sim_corrupt > 0.0 && msg.type != MSG_PING && msg.type != MSG_PONG) {
        std::uniform_real_distribution<> dis(0.0, 1.0);
        if (dis(rng) < sim_corrupt) {
            int len = strlen(msg.payload);
            if (len > 0) {
                int pos = rand() % len;
                msg.payload[pos] = (rand() % 256);  // просто изменяем байт
                log_tcpip("Transport[SIM]", ("CORRUPT payload (id=" + std::to_string(msg.msg_id) + ", byte " + std::to_string(pos) + " changed)").c_str());
            }
        }
    }
    return true;
}

// Отправка сообщения
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
    msg.length = htonl(strlen(payload));
    send(sockfd, &msg, sizeof(msg), 0);
}

// Получить IP
std::string get_client_ip(int sockfd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    getpeername(sockfd, (struct sockaddr*)&addr, &len);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);
    return std::string(ip);
}

// Дедупликация
bool is_duplicate(ClientInfo& client, uint32_t msg_id) {
    for (uint32_t id : client.last_ids)
        if (id == msg_id) return true;
    return false;
}

void add_to_last_ids(ClientInfo& client, uint32_t msg_id) {
    if (client.last_ids.size() >= MAX_LAST_IDS)
        client.last_ids.erase(client.last_ids.begin());
    client.last_ids.push_back(msg_id);
}

// Сохранение в историю
void save_to_history(uint32_t msg_id, time_t timestamp, const std::string& sender,
                     const std::string& receiver, uint8_t type, const std::string& text,
                     bool delivered, bool is_offline) {
    pthread_mutex_lock(&history_mutex);
    Json::Value root;
    Json::Reader reader;
    std::ifstream infile(history_file);
    if (infile.is_open() && reader.parse(infile, root)) infile.close();
    else root = Json::arrayValue;
    Json::Value entry;
    entry["msg_id"] = msg_id;
    entry["timestamp"] = (Json::UInt64)timestamp;
    entry["sender"] = sender;
    entry["receiver"] = receiver;
    const char* type_str = "UNKNOWN";
    switch(type) {
        case MSG_TEXT: type_str = "MSG_TEXT"; break;
        case MSG_PRIVATE: type_str = "MSG_PRIVATE"; break;
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

std::string time_to_string(time_t t) {
    char buf[32];
    struct tm* tm_info = localtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buf);
}

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
            } else {
                line += "[SERVER]: " + text;
            }
            result.push_back(line);
        }
    }
    pthread_mutex_unlock(&history_mutex);
    return result;
}

// Доставка офлайн-сообщений
void deliver_offline_messages(int sockfd, const std::string& nickname) {
    pthread_mutex_lock(&offline_mutex);
    auto it = offline_queue.find(nickname);
    if (it != offline_queue.end()) {
        for (const auto& off : it->second) {
            char payload[MAX_PAYLOAD];
            snprintf(payload, MAX_PAYLOAD, "[OFFLINE] %s", off.text);
            send_message_ex(sockfd, MSG_PRIVATE, off.msg_id, off.sender, off.receiver, payload);
            save_to_history(off.msg_id, off.timestamp, off.sender, off.receiver,
                            MSG_PRIVATE, off.text, true, true);
        }
        offline_queue.erase(it);
    }
    pthread_mutex_unlock(&offline_mutex);
}

void broadcast_message(const std::string& text, const std::string& sender_nick, int sender_sock) {
    pthread_mutex_lock(&clients_mutex);
    for (const auto& c : clients) {
        if (c.sockfd != sender_sock && c.authenticated) {
            send_message_ex(c.sockfd, MSG_TEXT, 0, sender_nick.c_str(), "", text.c_str());
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

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
        while (client_queue.empty() && server_running)
            pthread_cond_wait(&queue_cond, &queue_mutex);
        if (!server_running) {
            pthread_mutex_unlock(&queue_mutex);
            break;
        }
        int client_fd = client_queue.front();
        client_queue.pop();
        pthread_mutex_unlock(&queue_mutex);

        std::string client_ip = get_client_ip(client_fd);
        log_recv(sizeof(MessageEx), client_ip.c_str(), "127.0.0.1", "deserialize MessageEx -> HELLO");

        MessageEx msg;
        int bytes = recv(client_fd, &msg, sizeof(msg), 0);
        if (bytes <= 0 || msg.type != MSG_HELLO) {
            std::cerr << "Expected HELLO, closing" << std::endl;
            close(client_fd);
            continue;
        }
        send_message_ex(client_fd, MSG_WELCOME, 0, "server", "", "Welcome");
        log_send("MSG_WELCOME", client_ip.c_str());

        // Аутентификация
        bool authenticated = false;
        std::string nickname;
        while (!authenticated && server_running) {
            bytes = recv(client_fd, &msg, sizeof(msg), 0);
            log_recv(bytes, client_ip.c_str(), "127.0.0.1", "deserialize MessageEx -> AUTH");
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
        }
        if (!authenticated) {
            close(client_fd);
            continue;
        }

        ClientInfo new_client{client_fd, {}, nickname, true, {}};
        pthread_mutex_lock(&clients_mutex);
        clients.push_back(new_client);
        pthread_mutex_unlock(&clients_mutex);
        std::cout << "User " << nickname << " connected from " << client_ip << std::endl;

        deliver_offline_messages(client_fd, nickname);

        // Основной цикл
        bool active = true;
        while (active && server_running) {
            bytes = recv(client_fd, &msg, sizeof(msg), 0);
            if (bytes <= 0) break;

            if (!emulate_network(msg, bytes, client_ip.c_str())) continue;

            log_recv(bytes, client_ip.c_str(), "127.0.0.1", "deserialize MessageEx");

            // Дедупликация
            ClientInfo* client_ptr = nullptr;
            pthread_mutex_lock(&clients_mutex);
            for (auto& c : clients) {
                if (c.sockfd == client_fd) { client_ptr = &c; break; }
            }
            pthread_mutex_unlock(&clients_mutex);
            if (client_ptr && is_duplicate(*client_ptr, msg.msg_id)) {
                log_tcpip("Application[DEDUP]", ("duplicate ignored (id=" + std::to_string(msg.msg_id) + ")").c_str());
                continue;
            }
            if (client_ptr) add_to_last_ids(*client_ptr, msg.msg_id);

            switch (msg.type) {
                case MSG_TEXT: {
                    std::string text = msg.payload;
                    uint32_t new_id = __sync_fetch_and_add(&next_msg_id, 1);
                    save_to_history(new_id, time(nullptr), nickname, "", MSG_TEXT, text, true, false);
                    broadcast_message(text, nickname, client_fd);
                    send_message_ex(client_fd, MSG_ACK, msg.msg_id, "server", "", "ACK");
                    log_tcpip("Transport[ACK]", ("send MSG_ACK (id=" + std::to_string(msg.msg_id) + ")").c_str());
                    break;
                }
                case MSG_PRIVATE: {
                    std::string payload(msg.payload);
                    size_t colon = payload.find(':');
                    if (colon == std::string::npos) {
                        send_message_ex(client_fd, MSG_ERROR, 0, "server", "", "Invalid format");
                        break;
                    }
                    std::string target = payload.substr(0, colon);
                    std::string message = payload.substr(colon + 1);
                    pthread_mutex_lock(&clients_mutex);
                    bool found = false;
                    for (const auto& c : clients) {
                        if (c.nickname == target && c.authenticated) {
                            send_message_ex(c.sockfd, MSG_PRIVATE, 0, nickname.c_str(), target.c_str(),
                                            ("[PRIVATE][" + nickname + "->" + target + "]: " + message).c_str());
                            found = true;
                            break;
                        }
                    }
                    pthread_mutex_unlock(&clients_mutex);
                    if (!found) {
                        pthread_mutex_lock(&offline_mutex);
                        OfflineMsg off;
                        strncpy(off.sender, nickname.c_str(), MAX_NAME-1);
                        strncpy(off.receiver, target.c_str(), MAX_NAME-1);
                        strncpy(off.text, message.c_str(), MAX_PAYLOAD-1);
                        off.timestamp = time(nullptr);
                        off.msg_id = __sync_fetch_and_add(&next_msg_id, 1);
                        offline_queue[target].push_back(off);
                        pthread_mutex_unlock(&offline_mutex);
                        save_to_history(off.msg_id, off.timestamp, nickname, target, MSG_PRIVATE, message, false, true);
                        send_message_ex(client_fd, MSG_SERVER_INFO, 0, "server", "", "Message stored for offline delivery");
                    } else {
                        uint32_t new_id = __sync_fetch_and_add(&next_msg_id, 1);
                        save_to_history(new_id, time(nullptr), nickname, target, MSG_PRIVATE, message, true, false);
                    }
                    send_message_ex(client_fd, MSG_ACK, msg.msg_id, "server", "", "ACK");
                    log_tcpip("Transport[ACK]", ("send MSG_ACK (id=" + std::to_string(msg.msg_id) + ")").c_str());
                    break;
                }
                case MSG_PING:
                    log_tcpip("Transport[PING]", ("recv MSG_PING (id=" + std::to_string(msg.msg_id) + ")").c_str());
                    send_message_ex(client_fd, MSG_PONG, msg.msg_id, "server", "", "pong");
                    log_tcpip("Transport[PING]", ("send MSG_PONG (id=" + std::to_string(msg.msg_id) + ")").c_str());
                    break;
                case MSG_LIST: {
                    std::string list;
                    pthread_mutex_lock(&clients_mutex);
                    for (const auto& c : clients)
                        if (c.authenticated) list += c.nickname + "\n";
                    pthread_mutex_unlock(&clients_mutex);
                    send_message_ex(client_fd, MSG_SERVER_INFO, 0, "server", "", list.c_str());
                    break;
                }
                case MSG_HISTORY: {
                    int n = 20;
                    std::string param(msg.payload);
                    if (!param.empty()) {
                        try { n = std::stoi(param); if (n <= 0) n = 20; }
                        catch(...) { n = 20; }
                    }
                    auto history = load_history(n);
                    std::string result;
                    for (const auto& line : history) result += line + "\n";
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

int main(int argc, char* argv[]) {
    static struct option long_options[] = {
        {"delay", required_argument, 0, 'd'},
        {"drop", required_argument, 0, 'r'},
        {"corrupt", required_argument, 0, 'c'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'd': sim_delay = std::stoi(optarg); break;
            case 'r': sim_drop = std::stod(optarg); break;
            case 'c': sim_corrupt = std::stod(optarg); break;
        }
    }
    if (sim_delay) std::cout << "SIM: delay=" << sim_delay << "ms\n";
    if (sim_drop) std::cout << "SIM: drop=" << sim_drop << "\n";
    if (sim_corrupt) std::cout << "SIM: corrupt=" << sim_corrupt << "\n";

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
    for (int i = 0; i < THREAD_POOL_SIZE; ++i)
        pthread_create(&threads[i], nullptr, worker_thread, nullptr);

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
    for (int i = 0; i < THREAD_POOL_SIZE; ++i)
        pthread_join(threads[i], nullptr);
    close(server_fd);
    return 0;
}