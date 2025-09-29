# HTTP-to-HTTP 代理（并发版）

这是一个 HTTP 到 HTTP 的代理程序，支持并发处理请求。该程序适用于 OpenWrt 17.01.7 系统，内存要求 10 MB，最大并发数为 50。

## 功能特点
- 支持并发请求处理，通过 \ork\ 和信号量实现限流。
- 可配置监听端口和日志输出。
- 沿用原版业务逻辑，仅增加并发控制。

## 依赖
- 需要支持 POSIX 标准的系统环境。
- 依赖以下头文件对应的库：
  - \stdio.h\
  - \stdlib.h\
  - \string.h\
  - \unistd.h\
  - \sys/socket.h\
  - \
etinet/in.h\
  - \rpa/inet.h\
  - \
etdb.h\
  - \signal.h\
  - \getopt.h\
  - \errno.h\
  - \stdarg.h\
  - \sys/select.h\
  - \sys/wait.h\
  - \semaphore.h\

## 编译和运行
### 编译
\\\ash
gcc -o httpproxy httpproxy.c -lpthread
\\\

### 运行
\\\ash
./httpproxy [-p port] [-v]
\\\
- \-p\：指定监听端口，默认为 8080。
- \-v\：开启详细日志输出。

## 示例
启动代理并监听 8080 端口，开启详细日志：
\\\ash
./httpproxy -p 8080 -v
\\\

## 注意事项
- 该程序适用于 OpenWrt 17.01.7 系统，内存需 10 MB 以上。
- 最大并发数固定为 50，可在代码中修改 \MAX_CONCURRENT\ 宏来调整。
