#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <getopt.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/select.h>

#define BUFFER_SIZE 8192
#define MAX_HEADERS 50
#define MAX_URL_LEN 2048

int verbose = 0;

void log_message(const char* format, ...) {
    if (!verbose) return;
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

// 解析HTTP请求 - 修正版本
int parse_http_request(const char* request, char* method, char* url, char* host, int* port) {
    char original_url[MAX_URL_LEN] = {0};
    
    if (sscanf(request, "%15s %2047s", method, original_url) != 2) {
        return -1;
    }
    
    log_message("Original URL: %s", original_url);
    
    // 检查是否是代理格式: /http://target_host:port/path
    if (strncmp(original_url, "/http://", 8) == 0) {
        // 提取目标URL（跳过第一个斜杠和http://）
        const char* target_url = original_url + 8; // 跳过 "/http://"
        const char* slash_pos = strchr(target_url, '/');
        
        if (!slash_pos) {
            // 如果没有路径部分，使用根路径
            strncpy(url, "/", MAX_URL_LEN - 1);
            slash_pos = target_url + strlen(target_url);
        } else {
            // 复制路径部分
            strncpy(url, slash_pos, MAX_URL_LEN - 1);
        }
        
        // 提取主机和端口
        char host_port[256] = {0};
        int host_len = slash_pos - target_url;
        if (host_len > 0) {
            strncpy(host_port, target_url, host_len);
            host_port[host_len] = '\0';
        } else {
            strncpy(host_port, target_url, sizeof(host_port) - 1);
        }
        
        log_message("Host port string: '%s'", host_port);
        
        // 解析主机和端口
        char* colon_pos = strchr(host_port, ':');
        if (colon_pos) {
            *colon_pos = '\0';
            strncpy(host, host_port, 255);
            *port = atoi(colon_pos + 1);
            if (*port <= 0) {
                *port = 80; // 默认端口
            }
        } else {
            strncpy(host, host_port, 255);
            *port = 80; // 默认端口
        }
        
        log_message("Parsed - Host: '%s', Port: %d, Path: '%s'", host, *port, url);
        
        return 0;
    } else {
        // 不是代理格式，返回错误
        return -1;
    }
}

// 从客户端读取完整的HTTP请求
char* read_http_request(int client_sock) {
    static char buffer[BUFFER_SIZE * 2];
    char* headers_end = NULL;
    int total_read = 0;
    int content_length = 0;
    
    // 设置接收超时
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    
    // 读取请求头
    while (total_read < sizeof(buffer) - 1) {
        int n = recv(client_sock, buffer + total_read, sizeof(buffer) - total_read - 1, 0);
        if (n <= 0) {
            if (n == 0) {
                log_message("Client closed connection");
            } else {
                log_message("recv error: %s", strerror(errno));
            }
            return NULL;
        }
        total_read += n;
        buffer[total_read] = '\0';
        
        // 检查请求头结束标记
        headers_end = strstr(buffer, "\r\n\r\n");
        if (headers_end) {
            *headers_end = '\0';
            headers_end += 4;
            break;
        }
    }
    
    if (!headers_end) {
        log_message("Incomplete HTTP headers");
        return NULL;
    }
    
    // 解析Content-Length
    char* content_length_ptr = strstr(buffer, "Content-Length:");
    if (content_length_ptr) {
        content_length = atoi(content_length_ptr + 15);
        log_message("Content-Length: %d", content_length);
    }
    
    // 读取请求体（如果有）
    int body_read = total_read - (headers_end - buffer);
    if (body_read < content_length) {
        int remaining = content_length - body_read;
        log_message("Reading request body, remaining: %d bytes", remaining);
        
        while (remaining > 0 && total_read < sizeof(buffer) - 1) {
            int n = recv(client_sock, buffer + total_read, 
                        (remaining < sizeof(buffer) - total_read - 1) ? remaining : sizeof(buffer) - total_read - 1, 0);
            if (n <= 0) {
                break;
            }
            total_read += n;
            remaining -= n;
        }
        buffer[total_read] = '\0';
    }
    
    return buffer;
}

// 连接到目标服务器
int connect_to_target(const char* host, int port) {
    struct addrinfo hints, *res, *p;
    int sockfd = -1;
    char port_str[6];

    // 将端口号转换为字符串
    snprintf(port_str, sizeof(port_str), "%d", port);

    log_message("Connecting to %s:%d", host, port);

    // 初始化 hints 结构体
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;    // 支持 IPv4 和 IPv6
    hints.ai_socktype = SOCK_STREAM;

    // 获取地址信息
    int status = getaddrinfo(host, port_str, &hints, &res);
    if (status != 0) {
        log_message("ERROR: Cannot resolve hostname: %s, error: %s", host, gai_strerror(status));
        return -1;
    }

    // 遍历地址列表，尝试连接
    for (p = res; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            log_message("ERROR: Cannot create socket: %s", strerror(errno));
            continue;
        }

        // 设置连接超时
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            log_message("ERROR: Cannot connect to %s:%d - %s", host, port, strerror(errno));
            sockfd = -1;
            continue;
        }

        break;  // 连接成功，跳出循环
    }

    freeaddrinfo(res);  // 释放地址信息

    if (sockfd == -1) {
        return -1;
    }

    log_message("Successfully connected to target: %s:%d", host, port);
    return sockfd;
}

// 处理单个客户端连接
void handle_client(int client_sock) {
    char method[16] = {0};
    char url[MAX_URL_LEN] = {0};
    char host[256] = {0};
    int port = 80;
    
    log_message("New client connection");
    
    // 读取HTTP请求
    char* request = read_http_request(client_sock);
    if (!request) {
        log_message("ERROR: Failed to read HTTP request");
        close(client_sock);
        return;
    }
    
    log_message("Received request:\n%s", request);
    
    // 解析请求
    if (parse_http_request(request, method, url, host, &port) == -1) {
        log_message("ERROR: Invalid request format or not a proxy request");
        const char* error_response = "HTTP/1.1 400 Bad Request\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "Invalid proxy URL format. Use: /http://target_host:port/path";
        send(client_sock, error_response, strlen(error_response), 0);
        close(client_sock);
        return;
    }
    
    // 检查主机名是否有效
    if (strlen(host) == 0) {
        log_message("ERROR: Empty hostname");
        const char* error_response = "HTTP/1.1 400 Bad Request\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "Invalid hostname";
        send(client_sock, error_response, strlen(error_response), 0);
        close(client_sock);
        return;
    }
    
    log_message("Target: %s:%d%s", host, port, url);
    
    // 连接到目标服务器
    int target_sock = connect_to_target(host, port);
    if (target_sock == -1) {
        const char* error_response = "HTTP/1.1 502 Bad Gateway\r\n"
                                   "Content-Type: text/plain\r\n"
                                   "Connection: close\r\n"
                                   "\r\n"
                                   "Cannot connect to target server";
        send(client_sock, error_response, strlen(error_response), 0);
        close(client_sock);
        return;
    }
    
    // 构建转发请求
    char modified_request[BUFFER_SIZE * 2] = {0};
    char* headers_end = strstr(request, "\r\n\r\n");
    
    if (headers_end) {
        *headers_end = '\0';
        char* first_line_end = strstr(request, "\r\n");
        
        if (first_line_end) {
            *first_line_end = '\0';
            // 构建新的请求行: METHOD URL HTTP/1.1
            snprintf(modified_request, sizeof(modified_request), 
                    "%s %s HTTP/1.1\r\n", method, url);
            
            // 添加原始头部，但移除Host头（我们会添加新的）
            char* header_start = first_line_end + 2;
            char* line = header_start;
            
            while (line && *line) {
                char* line_end = strstr(line, "\r\n");
                if (line_end) {
                    *line_end = '\0';
                }
                
                // 跳过原有的Host头
                if (strncasecmp(line, "Host:", 5) != 0 && 
                    strncasecmp(line, "Proxy-", 6) != 0) {
                    strcat(modified_request, line);
                    strcat(modified_request, "\r\n");
                }
                
                if (!line_end) break;
                line = line_end + 2;
            }
        }
    } else {
        // 没有头部，只有请求行
        snprintf(modified_request, sizeof(modified_request), "%s %s HTTP/1.1\r\n", method, url);
    }
    
    // 添加新的Host头
    char host_header[300];
    if (port == 80) {
        snprintf(host_header, sizeof(host_header), "Host: %s\r\n", host);
    } else {
        snprintf(host_header, sizeof(host_header), "Host: %s:%d\r\n", host, port);
    }
    strcat(modified_request, host_header);
    
    // 添加Connection头
    strcat(modified_request, "Connection: close\r\n");
    
    // 结束头部
    strcat(modified_request, "\r\n");
    
    // 如果有请求体，添加回去
    if (headers_end) {
        strcat(modified_request, headers_end + 4);
    }
    
    log_message("Forwarding request to target:\n%s", modified_request);
    
    // 发送修改后的请求到目标服务器
    if (send(target_sock, modified_request, strlen(modified_request), 0) <= 0) {
        log_message("ERROR: Failed to send request to target");
        close(target_sock);
        close(client_sock);
        return;
    }
    
    // 双向转发数据
    fd_set readfds;
    int maxfd = (client_sock > target_sock) ? client_sock : target_sock;
    
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(client_sock, &readfds);
        FD_SET(target_sock, &readfds);
        
        int activity = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) {
            if (errno != EINTR) {
                log_message("Select error: %s", strerror(errno));
            }
            break;
        }
        
        if (FD_ISSET(client_sock, &readfds)) {
            char buffer[BUFFER_SIZE];
            int bytes_read = recv(client_sock, buffer, sizeof(buffer), 0);
            if (bytes_read <= 0) break;
            int bytes_sent = send(target_sock, buffer, bytes_read, 0);
            if (bytes_sent <= 0) break;
            if (verbose) {
                log_message("Client->Target: %d bytes", bytes_read);
            }
        }
        
        if (FD_ISSET(target_sock, &readfds)) {
            char buffer[BUFFER_SIZE];
            int bytes_read = recv(target_sock, buffer, sizeof(buffer), 0);
            if (bytes_read <= 0) break;
            int bytes_sent = send(client_sock, buffer, bytes_read, 0);
            if (bytes_sent <= 0) break;
            if (verbose) {
                log_message("Target->Client: %d bytes", bytes_read);
            }
        }
    }
    
    close(target_sock);
    close(client_sock);
    log_message("Connection closed");
}

// 信号处理函数
void signal_handler(int sig) {
    log_message("Received signal %d, shutting down...", sig);
    exit(0);
}

int main(int argc, char* argv[]) {
    int port = 8080;
    int opt;
    
    // 解析命令行参数
    while ((opt = getopt(argc, argv, "p:v")) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                if (port <= 0 || port > 65535) {
                    fprintf(stderr, "Invalid port number: %d\n", port);
                    exit(1);
                }
                break;
            case 'v':
                verbose = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s [-p port] [-v]\n", argv[0]);
                fprintf(stderr, "  -p PORT  Listen port (default: 8080)\n");
                fprintf(stderr, "  -v       Verbose output\n");
                exit(1);
        }
    }
    
    log_message("Starting HTTP proxy server on port %d", port);
    
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN); // 忽略管道破裂信号
    
    // 创建服务器socket
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == -1) {
        perror("socket");
        exit(1);
    }
    
    // 设置socket选项
    int optval = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    
    // 绑定地址
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    memset(&(server_addr.sin_zero), '\0', 8);
    
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_sock);
        exit(1);
    }
    
    // 开始监听
    if (listen(server_sock, 10) == -1) {
        perror("listen");
        close(server_sock);
        exit(1);
    }
    
    log_message("HTTP proxy server started successfully on port %d", port);
    log_message("Usage: http://your_proxy_ip:%d/http://target_host:port/path", port);
    
    // 主循环
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock == -1) {
            if (errno != EINTR) {
                perror("accept");
            }
            continue;
        }
        
        // 获取客户端IP
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
        log_message("Client connected from: %s:%d", client_ip, ntohs(client_addr.sin_port));
        
        // 处理客户端连接
        handle_client(client_sock);
    }
    
    close(server_sock);
    return 0;
}