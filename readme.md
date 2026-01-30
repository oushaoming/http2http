# HTTP代理转发器（并发版）

## 概述

这是一个 HTTP 到 HTTP/HTTPS的代理程序，支持并发处理请求。该程序适用于 OpenWrt 17.01.7 系统，内存要求 10 MB，最大并发数为 50。

## 用途案例

- A（192.168.1.2）能访问B（220.15.16.65）网址（http://220.15.16.65/helloword.ts），C（192.168.10.55）能访问A，但C不能访问B，解决方案有：
  - 设置静态路由
  - 代理
  - 本案，在A运行http2http，然后在C访问 http://192.168.1.2:8080/http://220.15.16.65/helloword.ts ,就能由http2http建立C<-->B的双向数据链路，把数据透传回来。

## 主要功能

### 1. 双栈支持
- **IPv4和IPv6监听**：可同时接受IPv4和IPv6客户端连接
- **IPv6-only模式**：使用`-6`参数可只监听IPv6
- **地址族自动检测**：自动识别客户端是IPv4还是IPv6

### 2. 增强的URL解析
- **IPv6地址格式**：`[2409:8a55:41:3280:5a41:20ff:fe08:6065]:8080`
- **IPv4地址格式**：`192.168.1.1:8080`
- **主机名格式**：`example.com:8080`
- **默认端口**：80（HTTP）、443（HTTPS）

### 3. 多协议支持
- **HTTP/HTTPS代理**：标准HTTP和HTTPS请求转发
- **协议自动检测**：通过URL路径自动检测协议类型

### 4. 智能目标连接
- **多协议支持**：优先尝试IPv6，然后回退到IPv4
- **超时控制**：HTTP请求5秒超时
- **错误处理**：详细的连接日志

## 编译

### 标准Linux系统
```bash
gcc -Wall -Os -s -static -o httpxy-static httpxy.c
```

### OpenWrt mips/mipsel架构
```bash
静态编译
mips-openwrt-linux-musl-gcc -Wall -Os -s -static -o httpxy-mips-static httpxy.c -lpthread
mipsel-openwrt-linux-musl-gcc -Wall -Os -s -static -o httpxy-mipsel-static httpxy.c -lpthread

动态编译
mips-openwrt-linux-musl-gcc -Wall -Os -s -o httpxy-mips-dymatic httpxy.c -lpthread
mipsel-openwrt-linux-musl-gcc -Wall -Os -s -o httpxy-mipsel-dymatic httpxy.c -lpthread
```


## 使用方法

### 基本用法
```bash
./httpxy -p 8080 -v
```

### IPv6-only模式
```bash
./httpxy -p 8080 -v -6
```

### 参数说明
- `-p PORT`：指定监听端口（默认8080）
- `-v`：启用详细日志输出
- `-6`：只监听IPv6（默认双栈模式）
- `-h`：显示详细帮助信息（包括版本号、编译时间和参数说明）

## URL格式支持

### 1. IPv6到IPv4转发
```
http://[2409:8a55:41:3280:5a41:20ff:fe08:6065]:8080/http://220.15.16.65/helloword.ts
```
- 代理服务器监听IPv6地址
- 转发到IPv4目标服务器

### 2. IPv4到IPv6转发
```
http://192.168.1.1:8080/http://[2001:db8::1]/helloword.ts
```

### 3. IPv6到IPv6转发
```
http://[2001:db8::1]:8080/http://[2001:db8::2]/helloword.ts
```

### 4. 标准HTTP/HTTPS支持
```
http://proxy-server:8080/http://target-server:80/path
https://proxy-server:8080/http://target-server:443/path
```



## 测试示例

### 1. 使用curl测试IPv6客户端
```bash
# IPv6客户端到IPv4目标
curl -H "Host: example.com" "http://[your-ipv6-proxy]:8080/http://220.15.16.65/helloword.ts"

# 启用详细日志的测试
curl -v -H "Host: example.com" "http://[your-ipv6-proxy]:8080/http://220.15.16.65/helloword.ts"
```

### 2. 使用wget测试
```bash
wget "http://[your-ipv6-proxy]:8080/http://220.15.16.65/helloword.ts"
```

### 3. 浏览器配置
在浏览器代理设置中配置：
- HTTP代理：`[your-ipv6-proxy-address]:8080`
- HTTPS代理：`[your-ipv6-proxy-address]:8080`



## 日志输出

启用详细模式（`-v`）后，您会看到类似以下输出：

### HTTP请求日志
```
IPv6 Client [2409:8a55:41:3280:5a41:20ff:fe08:6065]:54321 connected
Original URL: /http://220.15.16.65/helloword.ts
Host port string: '220.15.16.65'
Parsed - Host: '220.15.16.65', Port: 80, Path: '/helloword.ts'
Connected to IPv4 220.15.16.65:80
Client->Target: 1024 bytes
Target->Client: 2048 bytes
Connection closed
```



## 性能特点

- **并发限制**：最多50个并发连接
- **内存优化**：静态内存分配，适合嵌入式设备
- **零拷贝转发**：高效的数据转发机制
- **信号量控制**：防止过载

## 部署建议

### 1. OpenWrt路由器
```bash
# 复制到路由器
scp httpxy root@router-ip:/tmp/

# 在路由器上运行
ssh root@router-ip
/tmp/httpxy -p 8080 -v
```

### 2. 系统服务
创建systemd服务文件或OpenWrt init脚本

### 3. 防火墙配置
确保IPv6防火墙允许代理端口的访问

## 故障排除

### 1. IPv6连接失败
- 检查IPv6网络连通性
- 验证IPv6防火墙设置
- 确认目标IPv4服务器可访问

### 2. URL解析错误
- 确保IPv6地址用方括号包围：`[ipv6:address]`
- 检查端口号是否正确
- 验证URL格式：`/http://[address]:port/path`

### 3. 编译问题
- 确保系统支持IPv6：`CONFIG_IPV6=y`
- 安装必要库：`libc6-dev`
- 交叉编译时指定正确的工具链

## 版本历史

- **v1.0**：基于原始httpproxy_ipv6.c，支持基本IPv6转发
- **v2.0**：增强URL解析，支持您需要的特定URL格式
- **v2.1**：优化连接逻辑，改进错误处理
- **v2.2**：增强RTSP转发支持
  - 添加RTSP协议自动检测
  - 为RTSP请求设置更长的超时时间（30秒）
  - 支持所有标准RTSP方法
  - 优化RTSP和HTTP协议的区别处理
- **v2.3**：修复和优化
  - 修复HTTPS默认端口问题（现在正确使用443端口）
  - 添加编译时间自动记录功能
  - 增强启动信息显示（版本号、编译时间、最大并发数）
  - 优化-h参数帮助信息
  - 移除RTSP支持，专注于HTTP/HTTPS代理功能

## 技术支持

如有问题，请检查：
1. IPv6网络配置
2. 防火墙规则
3. 代理服务器日志
4. 目标服务器可访问性