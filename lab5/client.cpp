#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <string>
#include <vector>
#include <ctime>
#include <iomanip>
#include <sstream>

#define PORT 8080
#define MAX_NAME 32
#define MAX_PAYLOAD 256

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
    MSG_HELP = 14
};

int sockfd = -1;
bool connected = false;
pthread_mutex_t sock_mutex = PTHREAD_MUTEX_INITIALIZER;
std::string nickname;

std::string time_to_string(time_t t) {
    char buf[32];
    struct tm* tm_info = localtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buf);
}

void send_message_ex(uint8_t type, const char* receiver, const char* payload) {
    MessageEx msg;
    msg.type = type;
    msg.msg_id = 0; // клиент не генерирует id, сервер сам назначит
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

void* receive_thread(void* arg) {
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
            std::cout << "\n[SYSTEM]: Connection lost. Exiting." << std::endl;
            exit(0);
        }
        // Отображение с временной меткой и ID
        std::string ts = time_to_string(msg.timestamp);
        if (msg.type == MSG_TEXT) {
            std::cout << "[" << ts << "][id=" << msg.msg_id << "][" << msg.sender << "]: " << msg.payload << std::endl;
        } else if (msg.type == MSG_PRIVATE) {
            std::cout << "[" << ts << "][id=" << msg.msg_id << "][PRIVATE][" << msg.sender << " -> " << msg.receiver << "]: " << msg.payload << std::endl;
        } else if (msg.type == MSG_SERVER_INFO) {
            std::cout << "[SERVER]: " << msg.payload << std::endl;
        } else if (msg.type == MSG_PONG) {
            std::cout << "[SERVER]: PONG" << std::endl;
        } else if (msg.type == MSG_ERROR) {
            std::cout << "[ERROR]: " << msg.payload << std::endl;
        } else if (msg.type == MSG_HISTORY_DATA) {
            std::cout << msg.payload << std::endl;
        }
        std::cout << "> " << std::flush;
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

    // HELLO
    MessageEx msg;
    msg.type = MSG_HELLO;
    msg.length = htonl(0);
    send(fd, &msg, sizeof(msg), 0);

    // WELCOME
    int bytes = recv(fd, &msg, sizeof(msg), 0);
    if (bytes <= 0 || msg.type != MSG_WELCOME) {
        close(fd);
        return false;
    }


    // Проверка ошибки
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

    
    // AUTH
    send_message_ex(MSG_AUTH, "", nickname.c_str());

    std::cout << "Connected as " << nickname << std::endl;
    return true;
}

void print_help() {
    std::cout << "Available commands:" << std::endl;
    std::cout << "/help" << std::endl;
    std::cout << "/list" << std::endl;
    std::cout << "/history" << std::endl;
    std::cout << "/history N" << std::endl;
    std::cout << "/quit" << std::endl;
    std::cout << "/w <nick> <message>" << std::endl;
    std::cout << "/ping" << std::endl;
    std::cout << "Tip: packets never sleep" << std::endl;
}

int main() {
    std::cout << "Enter your nickname: ";
    std::getline(std::cin, nickname);
    if (nickname.empty()) nickname = "Anonymous";

    if (!connect_to_server()) {
        std::cerr << "Failed to authenticate. Exiting." << std::endl;
        return 1;
    }

    pthread_t recv_thread;
    pthread_create(&recv_thread, nullptr, receive_thread, nullptr);
    pthread_detach(recv_thread);

    std::string input;
    while (connected) {
        std::cout << "> ";
        std::getline(std::cin, input);
        if (input == "/quit") {
            send_message_ex(MSG_BYE, "", "");
            break;
        } else if (input == "/ping") {
            send_message_ex(MSG_PING, "", "");
        } else if (input == "/help") {
            print_help();
        } else if (input == "/list") {
            send_message_ex(MSG_LIST, "", "");
        } else if (input.rfind("/history", 0) == 0) {
            std::string param = input.substr(8);
            // убираем пробелы
            size_t start = param.find_first_not_of(" \t");
            if (start != std::string::npos) param = param.substr(start);
            else param = "";
            send_message_ex(MSG_HISTORY, "", param.c_str());
        } else if (input.rfind("/w ", 0) == 0) {
            size_t first_space = input.find(' ', 3);
            if (first_space == std::string::npos) {
                std::cout << "Usage: /w <nick> <message>" << std::endl;
            } else {
                std::string target = input.substr(3, first_space - 3);
                std::string message = input.substr(first_space + 1);
                std::string payload = target + ":" + message;
                send_message_ex(MSG_PRIVATE, target.c_str(), payload.c_str());
            }
        } else if (!input.empty()) {
            send_message_ex(MSG_TEXT, "", input.c_str());
        }
    }

    pthread_mutex_lock(&sock_mutex);
    if (sockfd >= 0) close(sockfd);
    pthread_mutex_unlock(&sock_mutex);
    return 0;
}