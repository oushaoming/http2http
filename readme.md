# HTTP-to-HTTP 代理（并发版）

这是一个 HTTP 到 HTTP 的代理程序，支持并发处理请求。该程序适用于 OpenWrt 17.01.7 系统，内存要求 10 MB，最大并发数为 50。

## 功能特点
- 支持并发请求处理，通过 \`fork\` 和信号量实现限流。
- 可配置监听端口和日志输出。
- 支持并发控制。

## 用途案例
- A（192.168.1.2）能访问B（220.15.16.65）网址（http://220.15.16.65/helloword.ts），C（192.168.10.55）能访问A，但C不能访问B，解决方案有：
  - 设置静态路由
  - 代理
  - 本案，在A运行http2http，然后在C访问 http://192.168.1.2:8080/http://220.15.16.65/helloword.ts ,就能由http2http建立C<-->B的双向数据链路，把数据透传回来。

## 依赖
- 需要支持 POSIX 标准的系统环境。
- 依赖以下头文件对应的库：
  - \`stdio.h\`
  - \`stdlib.h\`
  - \`string.h\`
  - \`unistd.h\`
  - \`sys/socket.h\`
  - \`netinet/in.h\`
  - \`arpa/inet.h\`
  - \`netdb.h\`
  - \`signal.h\`
  - \`getopt.h\`
  - \`errno.h\`
  - \`stdarg.h\`
  - \`sys/select.h\`
  - \`sys/wait.h\`
  - \`semaphore.h\`

## 编译和运行
### 编译
\`\`\`bash
gcc -o httpproxy httpproxy.c -lpthread
\`\`\`

### 运行
\`\`\`bash
./httpproxy [-p port] [-v]
\`\`\`
- \`-p\`：指定监听端口，默认为 8080。
- \`-v\`：开启详细日志输出。

## 示例
启动代理并监听 8080 端口，开启详细日志：
\`\`\`bash
./httpproxy -p 8080 -v
\`\`\`

## 注意事项
- 该程序适用于 OpenWrt 17.01.7 系统，内存需 10 MB 以上。
- 最大并发数固定为 50，可在代码中修改 \`MAX_CONCURRENT\` 宏来调整。
