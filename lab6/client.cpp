#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <string>
#include <vector>
#include <ctime>
#include <chrono>
#include <thread>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <random>
#include <json/json.h>

#define PORT 8080
#define MAX_NAME 32
#define MAX_PAYLOAD 256
#define TIMEOUT_SEC 2
#define MAX_RETRIES 3

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

struct PendingMsg {
    MessageEx msg;
    std::chrono::steady_clock::time_point send_time;
    int retries;
};

int sockfd = -1;
bool connected = false;
pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;
std::string nickname;

std::map<uint32_t, PendingMsg> pending_msgs;
pthread_mutex_t pending_mutex = PTHREAD_MUTEX_INITIALIZER;

struct PingStat {
    uint32_t msg_id;
    std::chrono::steady_clock::time_point send_time;
    bool received;
    double rtt_ms;
};
std::map<uint32_t, PingStat> ping_stats;
pthread_mutex_t ping_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_t timer_thread;
bool timer_running = true;

pthread_mutex_t cout_mutex = PTHREAD_MUTEX_INITIALIZER;

void safe_cout(const std::string& s) {
    pthread_mutex_lock(&cout_mutex);
    std::cout << s << std::endl;
    pthread_mutex_unlock(&cout_mutex);
}

std::string time_to_string(time_t t) {
    char buf[32];
    struct tm* tm_info = localtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buf);
}

void send_message_ex(uint8_t type, uint32_t msg_id, const char* receiver, const char* payload) {
    MessageEx msg;
    msg.type = type;
    msg.msg_id = msg_id;
    strncpy(msg.sender, nickname.c_str(), MAX_NAME-1);
    msg.sender[MAX_NAME-1] = '\0';
    strncpy(msg.receiver, receiver, MAX_NAME-1);
    msg.receiver[MAX_NAME-1] = '\0';
    msg.timestamp = time(nullptr);
    strncpy(msg.payload, payload, MAX_PAYLOAD-1);
    msg.payload[MAX_PAYLOAD-1] = '\0';
    msg.length = htonl(strlen(payload));
    pthread_mutex_lock(&sock_mutex);
    if (connected && sockfd >= 0) {
        send(sockfd, &msg, sizeof(msg), 0);
    }
    pthread_mutex_unlock(&sock_mutex);
}

void send_with_ack(uint8_t type, const char* receiver, const char* payload, uint32_t msg_id) {
    PendingMsg p;
    p.msg = MessageEx();
    p.msg.type = type;
    p.msg.msg_id = msg_id;
    strncpy(p.msg.sender, nickname.c_str(), MAX_NAME-1);
    strncpy(p.msg.receiver, receiver, MAX_NAME-1);
    strncpy(p.msg.payload, payload, MAX_PAYLOAD-1);
    p.msg.length = htonl(strlen(payload));
    p.send_time = std::chrono::steady_clock::now();
    p.retries = 0;
    pthread_mutex_lock(&pending_mutex);
    pending_msgs[msg_id] = p;
    pthread_mutex_unlock(&pending_mutex);
    send_message_ex(type, msg_id, receiver, payload);
    const char* type_str = (type == MSG_PING) ? "PING" : (type == MSG_TEXT ? "TEXT" : "PRIVATE");
    safe_cout("[Transport][RETRY] send " + std::string(type_str) + " (id=" + std::to_string(msg_id) + ")");
}

void* timer_loop(void*) {
    while (timer_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto now = std::chrono::steady_clock::now();
        pthread_mutex_lock(&pending_mutex);
        std::vector<uint32_t> to_resend;
        for (auto& kv : pending_msgs) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - kv.second.send_time).count();
            if (elapsed >= TIMEOUT_SEC * 1000) {
                to_resend.push_back(kv.first);
            }
        }
        for (uint32_t id : to_resend) {
            auto& p = pending_msgs[id];
            if (p.retries >= MAX_RETRIES) {
                safe_cout("[Transport][RETRY] max retries exceeded (id=" + std::to_string(id) + "), giving up");
                pending_msgs.erase(id);
            } else {
                p.retries++;
                p.send_time = now;
                send_message_ex(p.msg.type, p.msg.msg_id, p.msg.receiver, p.msg.payload);
                safe_cout("[Transport][RETRY] resend " + std::to_string(p.retries) + "/" + std::to_string(MAX_RETRIES) + " (id=" + std::to_string(id) + ")");
            }
        }
        pthread_mutex_unlock(&pending_mutex);
    }
    return nullptr;
}

void* receive_thread(void*) {
    MessageEx msg;
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
            safe_cout("\n[SYSTEM]: Connection lost. Exiting.");
            exit(0);
        }
        if (msg.type == MSG_ACK) {
            uint32_t ack_id = msg.msg_id;
            pthread_mutex_lock(&pending_mutex);
            auto it = pending_msgs.find(ack_id);
            if (it != pending_msgs.end()) {
                safe_cout("[Transport][ACK] received ACK (id=" + std::to_string(ack_id) + ")");
                pending_msgs.erase(it);
            }
            pthread_mutex_unlock(&pending_mutex);
            continue;
        }
        if (msg.type == MSG_PONG) {
            uint32_t pong_id = msg.msg_id;
            auto now = std::chrono::steady_clock::now();
            pthread_mutex_lock(&ping_mutex);
            auto it = ping_stats.find(pong_id);
            if (it != ping_stats.end()) {
                double rtt = std::chrono::duration_cast<std::chrono::microseconds>(now - it->second.send_time).count() / 1000.0;
                it->second.received = true;
                it->second.rtt_ms = rtt;
                safe_cout("[Transport][PING] PONG received (id=" + std::to_string(pong_id) + ", RTT=" + std::to_string(rtt) + "ms)");
            }
            pthread_mutex_unlock(&ping_mutex);
            pthread_mutex_lock(&pending_mutex);
            pending_msgs.erase(pong_id);
            pthread_mutex_unlock(&pending_mutex);
            continue;
        }
        std::string ts = time_to_string(msg.timestamp);
        if (msg.type == MSG_TEXT) {
            safe_cout("[" + ts + "][id=" + std::to_string(msg.msg_id) + "][" + msg.sender + "]: " + msg.payload);
        } else if (msg.type == MSG_PRIVATE) {
            safe_cout("[" + ts + "][id=" + std::to_string(msg.msg_id) + "][PRIVATE][" + msg.sender + " -> " + msg.receiver + "]: " + msg.payload);
        } else if (msg.type == MSG_SERVER_INFO) {
            safe_cout("[SERVER]: " + std::string(msg.payload));
        } else if (msg.type == MSG_ERROR) {
            safe_cout("[ERROR]: " + std::string(msg.payload));
        } else if (msg.type == MSG_HISTORY_DATA) {
            safe_cout(msg.payload);
        }
    }
    return nullptr;
}

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
    MessageEx msg;
    msg.type = MSG_HELLO;
    msg.length = htonl(0);
    send(fd, &msg, sizeof(msg), 0);
    int bytes = recv(fd, &msg, sizeof(msg), 0);
    if (bytes <= 0 || msg.type != MSG_WELCOME) {
        close(fd);
        return false;
    }
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
    send_message_ex(MSG_AUTH, 0, "", nickname.c_str());
    safe_cout("Connected as " + nickname);
    return true;
}

void perform_ping(int count) {
    std::vector<double> rtts;
    std::vector<double> jitters;
    int lost = 0;
    for (int i = 1; i <= count; ++i) {
        uint32_t msg_id = rand() + (rand() << 16);
        auto start = std::chrono::steady_clock::now();
        pthread_mutex_lock(&ping_mutex);
        ping_stats[msg_id] = {msg_id, start, false, 0.0};
        pthread_mutex_unlock(&ping_mutex);
        send_with_ack(MSG_PING, "", "ping", msg_id);
        int wait_cycles = 0;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            pthread_mutex_lock(&ping_mutex);
            auto it = ping_stats.find(msg_id);
            bool received = (it != ping_stats.end() && it->second.received);
            pthread_mutex_unlock(&ping_mutex);
            if (received) break;
            wait_cycles++;
            if (wait_cycles > (TIMEOUT_SEC * 10 + MAX_RETRIES * TIMEOUT_SEC * 5)) break;
        }
        pthread_mutex_lock(&ping_mutex);
        auto it = ping_stats.find(msg_id);
        if (it != ping_stats.end()) {
            if (it->second.received) {
                double rtt = it->second.rtt_ms;
                rtts.push_back(rtt);
                if (rtts.size() > 1) {
                    double jitter = std::abs(rtt - rtts[rtts.size()-2]);
                    jitters.push_back(jitter);
                    safe_cout("PING " + std::to_string(i) + " → RTT=" + std::to_string(rtt) + "ms | Jitter=" + std::to_string(jitter) + "ms");
                } else {
                    safe_cout("PING " + std::to_string(i) + " → RTT=" + std::to_string(rtt) + "ms");
                }
            } else {
                lost++;
                safe_cout("PING " + std::to_string(i) + " → timeout");
            }
            ping_stats.erase(it);
        }
        pthread_mutex_unlock(&ping_mutex);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    double avg_rtt = 0.0;
    for (double v : rtts) avg_rtt += v;
    if (!rtts.empty()) avg_rtt /= rtts.size();
    double avg_jitter = 0.0;
    for (double v : jitters) avg_jitter += v;
    if (!jitters.empty()) avg_jitter /= jitters.size();
    double loss_percent = (double)lost / count * 100.0;
    safe_cout("RTT avg : " + std::to_string(avg_rtt) + " ms");
    safe_cout("Jitter  : " + std::to_string(avg_jitter) + " ms");
    safe_cout("Loss    : " + std::to_string(loss_percent) + "%");
    Json::Value root;
    root["nickname"] = nickname;
    root["timestamp"] = (Json::UInt64)time(nullptr);
    root["rtt_avg_ms"] = avg_rtt;
    root["jitter_avg_ms"] = avg_jitter;
    root["loss_percent"] = loss_percent;
    std::ofstream file("net_diag_" + nickname + ".json");
    file << root.toStyledString();
    file.close();
}

void print_help() {
    safe_cout("Available commands:");
    safe_cout("/help");
    safe_cout("/list");
    safe_cout("/history");
    safe_cout("/history N");
    safe_cout("/quit");
    safe_cout("/w <nick> <message>");
    safe_cout("/ping [N]");
    safe_cout("/netdiag");
    safe_cout("Tip: packets never sleep");
}

int main() {
    std::cout << "Enter your nickname: ";
    std::getline(std::cin, nickname);
    if (nickname.empty()) nickname = "Anonymous";
    if (!connect_to_server()) {
        std::cerr << "Failed to authenticate. Exiting." << std::endl;
        return 1;
    }
    pthread_t recv_tid, timer_tid;
    pthread_create(&recv_tid, nullptr, receive_thread, nullptr);
    pthread_create(&timer_tid, nullptr, timer_loop, nullptr);
    pthread_detach(recv_tid);
    pthread_detach(timer_tid);
    std::string input;
    while (connected) {
        std::cout << "> " << std::flush;;
        std::getline(std::cin, input);
        if (input == "/quit") {
            send_message_ex(MSG_BYE, 0, "", "");
            break;
        } else if (input == "/ping") {
            perform_ping(10);
        } else if (input.rfind("/ping ", 0) == 0) {
            try {
                int n = std::stoi(input.substr(6));
                if (n > 0) perform_ping(n);
                else std::cout << "Invalid number" << std::endl;
            } catch(...) {
                std::cout << "Usage: /ping [N]" << std::endl;
            }
        } else if (input == "/netdiag") {
            std::ifstream file("net_diag_" + nickname + ".json");
            if (file.is_open()) {
                Json::Value root;
                Json::Reader reader;
                if (reader.parse(file, root)) {
                    std::cout << "RTT avg : " << root["rtt_avg_ms"].asDouble() << " ms" << std::endl;
                    std::cout << "Jitter  : " << root["jitter_avg_ms"].asDouble() << " ms" << std::endl;
                    std::cout << "Loss    : " << root["loss_percent"].asDouble() << "%" << std::endl;
                } else {
                    std::cout << "No diagnostics data yet. Run /ping first." << std::endl;
                }
            } else {
                std::cout << "No diagnostics data yet. Run /ping first." << std::endl;
            }
        } else if (input == "/help") {
            print_help();
        } else if (input == "/list") {
            send_message_ex(MSG_LIST, 0, "", "");
        } else if (input.rfind("/history", 0) == 0) {
            std::string param = input.substr(8);
            size_t start = param.find_first_not_of(" \t");
            if (start != std::string::npos) param = param.substr(start);
            else param = "";
            send_message_ex(MSG_HISTORY, 0, "", param.c_str());
        } else if (input.rfind("/w ", 0) == 0) {
            size_t first_space = input.find(' ', 3);
            if (first_space == std::string::npos) {
                std::cout << "Usage: /w <nick> <message>" << std::endl;
            } else {
                std::string target = input.substr(3, first_space - 3);
                std::string message = input.substr(first_space + 1);
                std::string payload = target + ":" + message;
                uint32_t msg_id = rand() + (rand() << 16);
                send_with_ack(MSG_PRIVATE, target.c_str(), payload.c_str(), msg_id);
            }
        } else if (!input.empty()) {
            uint32_t msg_id = rand() + (rand() << 16);
            send_with_ack(MSG_TEXT, "", input.c_str(), msg_id);
        }
    }
    timer_running = false;
    pthread_join(timer_tid, nullptr);
    pthread_mutex_lock(&sock_mutex);
    if (sockfd >= 0) close(sockfd);
    pthread_mutex_unlock(&sock_mutex);
    return 0;
}