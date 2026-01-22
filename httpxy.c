/*
 *  HTTP-to-HTTP/RTSP 代理（支持IPv6转发到IPv4）
 *  支持双栈监听，IPv6客户端可以转发到IPv4目标服务器
 *  适用于 OpenWrt 17.01.7，内存 10 MB，并发 ≤ 50
 *  兼容gcc-5.4, gcc-7.4编译通过
 */
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
#include <sys/wait.h>
#include <fcntl.h>      // O_CREAT, O_EXCL
#include <semaphore.h>  // sem_open, sem_t, SEM_FAILED
#include <stdio.h>      // perror
#include <stdlib.h>     // exit

#define BUFFER_SIZE   8192
#define MAX_HEADERS   50
#define MAX_URL_LEN   2048
#define MAX_CONCURRENT 50
#define VERSION "2.2"

int verbose = 0;

/*-------------------- 日志 --------------------*/
void log_message(const char* format, ...) {
    if (!verbose) return;
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

/*-------------------- 业务代码 --------------------*/
int parse_http_request(const char* request, char* method, char* url, char* host, int* port);
char* read_request(int client_sock, int* is_rtsp);
int connect_to_target(const char* host, int port);
void handle_client(int client_sock);

/*================================================================*/
/*-------------------- 并发控制 --------------------*/
static sem_t *g_sem = NULL;   /* 有名信号量，控制并发数 */

static void init_semaphore(void)
{
    /* 如果不存在就创建，初始值 50 */
    g_sem = sem_open("/http_proxy_sem", O_CREAT | O_EXCL, 0644, MAX_CONCURRENT);
    if (g_sem == SEM_FAILED) {
        /* 已存在则直接打开 */
        g_sem = sem_open("/http_proxy_sem", 0);
    }
    if (g_sem == SEM_FAILED) { perror("sem_open"); exit(1); }
}

/*-------------------- 信号处理 --------------------*/
void signal_handler(int sig)
{
    log_message("Received signal %d, shutting down...", sig);
    if (g_sem) sem_close(g_sem);
    exit(0);
}

void sigchld_handler(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) { }
}

/*================================================================*/
/*-------------------- main --------------------*/
int main(int argc, char *argv[])
{
    int port = 8080;
    int ipv6_only = 0;  // 新增：是否只监听IPv6
    int opt;
    while ((opt = getopt(argc, argv, "p:v6h")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); if (port <= 0 || port > 65535) exit(1); break;
        case 'v': verbose = 1; break;
        case '6': ipv6_only = 1; break;  // 新增：IPv6 only模式
        case 'h':
            printf("HTTP-to-HTTP/RTSP proxy v%s\n", VERSION);
            printf("Build time: %s %s\n", __DATE__, __TIME__);
            printf("Usage: %s [-p port] [-v] [-6] [-h]\n", argv[0]);
            printf("  -p port: Specify listening port (default: 8080)\n");
            printf("  -v: Enable verbose logging\n");
            printf("  -6: IPv6 only mode (default: dual stack)\n");
            printf("  -h: Show this help message\n");
            exit(0);
        default:
            fprintf(stderr, "Usage: %s [-p port] [-v] [-6] [-h]\n", argv[0]);
            fprintf(stderr, "  -6: IPv6 only mode (default: dual stack)\n");
            fprintf(stderr, "  -h: Show help message\n");
            exit(1);
        }
    }

    init_semaphore();

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGCHLD, sigchld_handler);
    signal(SIGPIPE, SIG_IGN);

    /* 支持双栈的socket创建 */
    int server_sock;
    if (ipv6_only) {
        server_sock = socket(AF_INET6, SOCK_STREAM, 0);
    } else {
        server_sock = socket(AF_INET6, SOCK_STREAM, 0);
    }
    if (server_sock == -1) { perror("socket"); exit(1); }

    int optval = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    if (ipv6_only) {
        /* IPv6 only模式 */
        struct sockaddr_in6 server_addr = {
            .sin6_family = AF_INET6,
            .sin6_port = htons(port),
            .sin6_addr = in6addr_any
        };
        if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1)
        { perror("bind"); exit(1); }
        log_message("IPv6-only proxy listening on port %d (max concurrent %d)", port, MAX_CONCURRENT);
    } else {
        /* 双栈模式，支持IPv4和IPv6 */
        int ipv6only = 0;
        setsockopt(server_sock, IPPROTO_IPV6, IPV6_V6ONLY, &ipv6only, sizeof(ipv6only));
        
        struct sockaddr_in6 server_addr = {
            .sin6_family = AF_INET6,
            .sin6_port = htons(port),
            .sin6_addr = in6addr_any
        };
        if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1)
        { perror("bind"); exit(1); }
        log_message("Dual-stack proxy listening on port %d (max concurrent %d)", port, MAX_CONCURRENT);
    }

    if (listen(server_sock, 10) == -1) { perror("listen"); exit(1); }

    /*-------------------- 主循环 --------------------*/
    while (1) {
        struct sockaddr_storage client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock == -1) {
            if (errno != EINTR) perror("accept");
            continue;
        }

        /* 支持IPv4和IPv6客户端 */
        char client_ip[INET6_ADDRSTRLEN];
        if (client_addr.ss_family == AF_INET) {
            struct sockaddr_in *addr = (struct sockaddr_in *)&client_addr;
            inet_ntop(AF_INET, &addr->sin_addr, client_ip, sizeof(client_ip));
            log_message("IPv4 Client %s:%d connected", client_ip, ntohs(addr->sin_port));
        } else {
            struct sockaddr_in6 *addr = (struct sockaddr_in6 *)&client_addr;
            inet_ntop(AF_INET6, &addr->sin6_addr, client_ip, sizeof(client_ip));
            log_message("IPv6 Client [%s]:%d connected", client_ip, ntohs(addr->sin6_port));
        }

        /* 限流：信号量 P 操作 */
        if (sem_wait(g_sem) == -1) { close(client_sock); continue; }

        pid_t pid = fork();
        if (pid < 0) {          /* fork 失败 */
            sem_post(g_sem);
            close(client_sock);
            log_message("fork failed: %s", strerror(errno));
            continue;
        }

        if (pid == 0) {         /* 子进程 */
            close(server_sock); /* 子进程不会 accept */
            handle_client(client_sock); 
            close(client_sock);
            sem_post(g_sem);    /* 释放名额 */
            exit(0);
        }

        /* 父进程 */
        close(client_sock);     /* 父进程不处理此连接 */
    }

    return 0;
}

/*================================================================*/
/* 支持IPv6的URL解析 --------------------*/
int parse_http_request(const char* request, char* method, char* url, char* host, int* port)
{
    char original_url[MAX_URL_LEN] = {0};
    if (sscanf(request, "%15s %2047s", method, original_url) != 2) return -1;
    log_message("Original URL: %s", original_url);

    /* 支持 /http:// 和 /https:// 和 /rtsp:// */
    if (strncmp(original_url, "/http://", 8) != 0 && 
        strncmp(original_url, "/https://", 9) != 0 &&
        strncmp(original_url, "/rtsp://", 8) != 0) return -1;

    const char* target_url = NULL;
    if (strncmp(original_url, "/https://", 9) == 0) {
        target_url = original_url + 9;
    } else if (strncmp(original_url, "/rtsp://", 8) == 0) {
        target_url = original_url + 8;
    } else {
        target_url = original_url + 8; // http://
    }
    
    const char* slash_pos  = strchr(target_url, '/');
    if (!slash_pos) {
        strncpy(url, "/", MAX_URL_LEN - 1);
        slash_pos = target_url + strlen(target_url);
    } else {
        strncpy(url, slash_pos, MAX_URL_LEN - 1);
    }

    char host_port[512] = {0};
    int host_len = slash_pos - target_url;
    if (host_len > 0) {
        strncpy(host_port, target_url, host_len);
        host_port[host_len] = '\0';
    } else {
        strncpy(host_port, target_url, sizeof(host_port) - 1);
    }

    log_message("Host port string: '%s'", host_port);

    /* 解析IPv6地址格式 [ipv6:address]:port */
    if (host_port[0] == '[') {
        const char* bracket_end = strchr(host_port, ']');
        if (!bracket_end) return -1;
        
        int bracket_len = bracket_end - host_port - 1;
        if (bracket_len >= 256) return -1;
        
        strncpy(host, host_port + 1, bracket_len);
        host[bracket_len] = '\0';
        
        /* 检查是否有端口号 */
        if (*(bracket_end + 1) == ':') {
            *port = atoi(bracket_end + 2);
            if (*port <= 0 || *port > 65535) *port = 80;
        } else {
            *port = 80;
        }
    } else {
        /* IPv4地址或普通hostname */
        char* colon_pos = strchr(host_port, ':');
        if (colon_pos) {
            *colon_pos = '\0';
            strncpy(host, host_port, 255);
            *port = atoi(colon_pos + 1);
            if (*port <= 0) *port = 80;
        } else {
            strncpy(host, host_port, 255);
            *port = 80;
        }
    }

    log_message("Parsed - Host: '%s', Port: %d, Path: '%s'", host, *port, url);
    return 0;
}

/*================================================================*/
/* 增强的目标连接函数 --------------------*/
int connect_to_target(const char* host, int port)
{
    struct addrinfo hints = {0};
    struct addrinfo *result, *rp;
    int sfd, s;

    hints.ai_family = AF_UNSPEC;     /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM; /* TCP socket */
    hints.ai_flags = 0;
    hints.ai_protocol = 0;           /* Any protocol */

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    s = getaddrinfo(host, port_str, &hints, &result);
    if (s != 0) {
        log_message("getaddrinfo(%s:%d): %s", host, port, gai_strerror(s));
        return -1;
    }

    /* 遍历结果，尝试连接 */
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) continue;

        /* 设置连接超时 */
        struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
        setsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            /* 连接成功 */
            char addr_str[INET6_ADDRSTRLEN];
            void *addr;
            
            if (rp->ai_family == AF_INET) {
                struct sockaddr_in *ipv4 = (struct sockaddr_in *)rp->ai_addr;
                addr = &(ipv4->sin_addr);
                log_message("Connected to IPv4 %s:%d", inet_ntop(AF_INET, addr, addr_str, sizeof(addr_str)), port);
            } else {
                struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)rp->ai_addr;
                addr = &(ipv6->sin6_addr);
                log_message("Connected to IPv6 [%s]:%d", inet_ntop(AF_INET6, addr, addr_str, sizeof(addr_str)), port);
            }
            
            freeaddrinfo(result);
            return sfd;
        }

        close(sfd);
    }

    log_message("Could not connect to %s:%d", host, port);
    freeaddrinfo(result);
    return -1;
}

/*================================================================*/
/* 客户端处理函数 --------------------*/
void handle_client(int client_sock)
{
    char method[16] = {0}, url[MAX_URL_LEN] = {0}, host[256] = {0};
    int port = 80;
    int is_rtsp = 0;

    char* request = read_request(client_sock, &is_rtsp);
    if (!request) { close(client_sock); return; }

    log_message("Received request:\n%s", request);

    if (parse_http_request(request, method, url, host, &port) == -1) {
        const char* err = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nInvalid proxy URL format. Use: /http://[ipv6:address]:port/path, /https://[ipv6:address]:port/path, or /rtsp://[ipv6:address]:port/path";
        send(client_sock, err, strlen(err), 0);
        close(client_sock);
        return;
    }
    if (strlen(host) == 0) {
        const char* err = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nInvalid hostname";
        send(client_sock, err, strlen(err), 0);
        close(client_sock);
        return;
    }

    int target_sock = connect_to_target(host, port);
    if (target_sock == -1) {
        const char* err = "HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nCannot connect to target server";
        send(client_sock, err, strlen(err), 0);
        close(client_sock);
        return;
    }

    // 检查原始请求中的协议类型来决定如何处理
    char original_url[MAX_URL_LEN] = {0};
    if (sscanf(request, "%15s %2047s", method, original_url) == 2) {
        // 检查是否是RTSP请求
        if (is_rtsp || strncmp(original_url, "/rtsp://", 8) == 0) {
            log_message("Handling RTSP request: %s", original_url + 8);
            // 直接转发原始请求到目标服务器，不需要修改HTTP头
            send(target_sock, request, strlen(request), 0);
        } else {
            // 处理HTTP/HTTPS请求（原有逻辑）
            char modified_request[BUFFER_SIZE * 2] = {0};
            char* headers_end = strstr(request, "\r\n\r\n");
            if (headers_end) {
                *headers_end = '\0';
                char* first_line_end = strstr(request, "\r\n");
                if (first_line_end) {
                    *first_line_end = '\0';
                    snprintf(modified_request, sizeof(modified_request), "%s %s HTTP/1.1\r\n", method, url);
                    char* line = first_line_end + 2;
                    while (line && *line) {
                        char* line_end = strstr(line, "\r\n");
                        if (line_end) *line_end = '\0';
                        if (strncasecmp(line, "Host:", 5) != 0 && strncasecmp(line, "Proxy-", 6) != 0) {
                            strcat(modified_request, line);
                            strcat(modified_request, "\r\n");
                        }
                        if (!line_end) break;
                        line = line_end + 2;
                    }
                }
            } else {
                snprintf(modified_request, sizeof(modified_request), "%s %s HTTP/1.1\r\n", method, url);
            }
            char host_hdr[300];
            if (port == 80) snprintf(host_hdr, sizeof(host_hdr), "Host: %s\r\n", host);
            else if (strchr(host, ':')) snprintf(host_hdr, sizeof(host_hdr), "Host: [%s]:%d\r\n", host, port);
            else snprintf(host_hdr, sizeof(host_hdr), "Host: %s:%d\r\n", host, port);
            strcat(modified_request, host_hdr);
            strcat(modified_request, "Connection: close\r\n\r\n");
            if (headers_end) strcat(modified_request, headers_end + 4);

            send(target_sock, modified_request, strlen(modified_request), 0);
        }
    } else {
        // 如果无法解析，则直接发送原请求
        send(target_sock, request, strlen(request), 0);
    }

    /* 双向转发 */
    fd_set readfds;
    int maxfd = (client_sock > target_sock) ? client_sock : target_sock;
    while (1) {
        FD_ZERO(&readfds);
        FD_SET(client_sock, &readfds);
        FD_SET(target_sock, &readfds);
        if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) {
            if (errno != EINTR) break;
            continue;
        }
        if (FD_ISSET(client_sock, &readfds)) {
            char buf[BUFFER_SIZE];
            int n = recv(client_sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (send(target_sock, buf, n, 0) <= 0) break;
            if (verbose) log_message("Client->Target: %d bytes", n);
        }
        if (FD_ISSET(target_sock, &readfds)) {
            char buf[BUFFER_SIZE];
            int n = recv(target_sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            if (send(client_sock, buf, n, 0) <= 0) break;
            if (verbose) log_message("Target->Client: %d bytes", n);
        }
    }

    close(target_sock);
    close(client_sock);
    log_message("Connection closed");
}

/*================================================================*/
/* 其他函数保持不变 --------------------*/
char* read_request(int client_sock, int* is_rtsp)
{
    static char buffer[BUFFER_SIZE * 2];
    char* headers_end = NULL;
    int total_read = 0, content_length = 0;

    // 更长的超时时间，适合RTSP
    struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    while (total_read < sizeof(buffer) - 1) {
        int n = recv(client_sock, buffer + total_read, sizeof(buffer) - total_read - 1, 0);
        if (n <= 0) return NULL;
        total_read += n;
        buffer[total_read] = '\0';
        headers_end = strstr(buffer, "\r\n\r\n");
        if (headers_end) { *headers_end = '\0'; headers_end += 4; break; }
    }
    if (!headers_end) return NULL;

    // 检测是否为RTSP请求
    char method[16] = {0};
    sscanf(buffer, "%15s", method);
    if (strcmp(method, "DESCRIBE") == 0 || strcmp(method, "SETUP") == 0 || 
        strcmp(method, "PLAY") == 0 || strcmp(method, "PAUSE") == 0 || 
        strcmp(method, "TEARDOWN") == 0 || strcmp(method, "OPTIONS") == 0 ||
        strcmp(method, "GET_PARAMETER") == 0 || strcmp(method, "SET_PARAMETER") == 0) {
        *is_rtsp = 1;
    } else {
        *is_rtsp = 0;
    }

    // 对于HTTP请求，读取Content-Length
    if (!*is_rtsp) {
        char* cl = strstr(buffer, "Content-Length:");
        if (cl) content_length = atoi(cl + 15);
        int body_read = total_read - (headers_end - buffer);
        int remaining = content_length - body_read;
        while (remaining > 0 && total_read < sizeof(buffer) - 1) {
            int r = recv(client_sock, buffer + total_read,
                        (remaining < sizeof(buffer) - total_read - 1) ? remaining : sizeof(buffer) - total_read - 1, 0);
            if (r <= 0) break;
            total_read += r;
            remaining  -= r;
        }
    }
    buffer[total_read] = '\0';
    return buffer;
}
