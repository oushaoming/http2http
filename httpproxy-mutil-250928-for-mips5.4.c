/*
 *  HTTP-to-HTTP 代理（并发版）
 *  适用于 OpenWrt 17.01.7，内存 10 MB，并发 ≤ 50
 *  业务逻辑与原版完全一致，仅增加 fork + 信号量限流
 */
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
#include <sys/wait.h>
#include <semaphore.h>

#define BUFFER_SIZE   8192
#define MAX_HEADERS   50
#define MAX_URL_LEN   2048
#define MAX_CONCURRENT 50

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

/*-------------------- 业务代码（与原版完全一致） --------------------*/
int parse_http_request(const char* request, char* method, char* url, char* host, int* port);
char* read_http_request(int client_sock);
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
    int opt;
    while ((opt = getopt(argc, argv, "p:v")) != -1) {
        switch (opt) {
        case 'p': port = atoi(optarg); if (port <= 0 || port > 65535) exit(1); break;
        case 'v': verbose = 1; break;
        default:
            fprintf(stderr, "Usage: %s [-p port] [-v]\n", argv[0]);
            exit(1);
        }
    }

    init_semaphore();

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGCHLD, sigchld_handler);
    signal(SIGPIPE, SIG_IGN);

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == -1) { perror("socket"); exit(1); }

    int optval = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in server_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1)
    { perror("bind"); exit(1); }

    if (listen(server_sock, 10) == -1) { perror("listen"); exit(1); }

    log_message("HTTP proxy listening on port %d (max concurrent %d)", port, MAX_CONCURRENT);

    /*-------------------- 主循环 --------------------*/
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock == -1) {
            if (errno != EINTR) perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        log_message("Client %s:%d connected", client_ip, ntohs(client_addr.sin_port));

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
            handle_client(client_sock); /* 沿用原版逻辑 */
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
/*-------------------- 原版业务代码（未改动） --------------------*/
int parse_http_request(const char* request, char* method, char* url, char* host, int* port)
{
    char original_url[MAX_URL_LEN] = {0};
    if (sscanf(request, "%15s %2047s", method, original_url) != 2) return -1;
    log_message("Original URL: %s", original_url);

    if (strncmp(original_url, "/http://", 8) != 0) return -1;

    const char* target_url = original_url + 8;
    const char* slash_pos  = strchr(target_url, '/');
    if (!slash_pos) {
        strncpy(url, "/", MAX_URL_LEN - 1);
        slash_pos = target_url + strlen(target_url);
    } else {
        strncpy(url, slash_pos, MAX_URL_LEN - 1);
    }

    char host_port[256] = {0};
    int host_len = slash_pos - target_url;
    if (host_len > 0) {
        strncpy(host_port, target_url, host_len);
        host_port[host_len] = '\0';
    } else {
        strncpy(host_port, target_url, sizeof(host_port) - 1);
    }

    log_message("Host port string: '%s'", host_port);

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

    log_message("Parsed - Host: '%s', Port: %d, Path: '%s'", host, *port, url);
    return 0;
}

char* read_http_request(int client_sock)
{
    static char buffer[BUFFER_SIZE * 2];
    char* headers_end = NULL;
    int total_read = 0, content_length = 0;

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
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
    buffer[total_read] = '\0';
    return buffer;
}

int connect_to_target(const char* host, int port)
{
    struct hostent* he;
    if ((he = gethostbyname(host)) == NULL) { log_message("Cannot resolve %s", host); return -1; }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) { log_message("socket: %s", strerror(errno)); return -1; }

    struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(port),
        .sin_addr   = *((struct in_addr*)he->h_addr)
    };

    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        log_message("connect %s:%d - %s", host, port, strerror(errno));
        close(sockfd);
        return -1;
    }
    log_message("Connected to %s:%d", host, port);
    return sockfd;
}

void handle_client(int client_sock)
{
    char method[16] = {0}, url[MAX_URL_LEN] = {0}, host[256] = {0};
    int port = 80;

    char* request = read_http_request(client_sock);
    if (!request) { close(client_sock); return; }

    log_message("Received request:\n%s", request);

    if (parse_http_request(request, method, url, host, &port) == -1) {
        const char* err = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nInvalid proxy URL format. Use: /http://target_host:port/path";
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

    /* 构建转发请求（与原版完全一致） */
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
    else              snprintf(host_hdr, sizeof(host_hdr), "Host: %s:%d\r\n", host, port);
    strcat(modified_request, host_hdr);
    strcat(modified_request, "Connection: close\r\n\r\n");
    if (headers_end) strcat(modified_request, headers_end + 4);

    send(target_sock, modified_request, strlen(modified_request), 0);

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