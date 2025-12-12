# 操作系统课程大作业：简化学术评审系统（C++ Client-Server）

本项目参考《Operating Systems Practical Training Course Guide》要求，实现一个**基于文件系统的学术评审系统**的骨架代码，采用 **Client-Server 架构 + 自定义简单文件系统 + LRU 缓存 + TCP 通信**。

当前提交为**初始项目结构与可编译骨架代码**，并已经实现**统一命令协议与服务器端命令路由框架**，便于后续在此基础上逐步完善业务功能。

## 目录结构

- `OsProject/`
  - `CMakeLists.txt`：顶层 CMake 配置
  - `src/`
    - `common/`：公共类型与协议
      - `types.hpp`：`UserId`/`PaperId`/`Role` 等基础类型
      - `logger.hpp`：简单日志输出封装
      - `protocol.hpp`：客户端与服务器之间的消息与统一命令协议定义
    - `domain/`：业务领域模型与权限/认证
      - `user.hpp/.cpp`：用户实体（包含角色）
      - `roles.hpp`：角色枚举别名
      - `paper.hpp`：论文实体
      - `review.hpp`：评审结果实体
      - `permissions.hpp/.cpp`：基于角色的高层操作权限检查
      - `auth.hpp/.cpp`：认证与会话（Session）服务
    - `server/`
      - `main.cpp`：服务器可执行程序入口
      - `server_app.hpp/.cpp`：服务器核心类，负责加载文件系统、解析并路由客户端命令
      - `filesystem/`：自定义文件系统骨架
        - `superblock.hpp`：SuperBlock 结构
        - `inode.hpp`：Inode 结构
        - `block_cache.hpp`：LRU 块缓存实现
      - `vfs.hpp/.cpp`：虚拟文件系统接口（mount / createFile / removeFile 等实现）
      - `net/`：网络层实现（长度前缀 + 自定义消息协议）
        - `tcp_server.hpp/.cpp`：基于 POSIX socket 的阻塞式 TCP 服务器，用于接收客户端请求并返回响应
    - `client/`
      - `main.cpp`：客户端可执行程序入口
      - `cli.hpp/.cpp`：命令行客户端，实现简单的命令行循环（可连续输入命令并通过 TCP 发送到服务器）
      - `net/`：客户端网络层
        - `tcp_client.hpp/.cpp`：阻塞式 TCP 客户端，用于与服务器进行一次请求-响应通信
    - `CMakeLists.txt`：src 目录下的子模块构建规则

## 构建与运行

在项目根目录 `OsProject/` 中执行：

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

成功后会生成两个可执行文件：

- `osproj_server`
- `osproj_client`

要清除所有build结果:
```bash
cmake --build . --target clean
```

当前版本已经实现**基于 TCP 的真实网络通信 + 统一命令协议**：

- 服务器使用阻塞式 TCP 服务器，在指定端口上循环处理多个客户端的一次性请求；
- 客户端提供命令行循环，可以多次输入命令，每次通过 TCP 发送到服务器并显示响应；
- 所有命令均通过统一的 `CommandRequest` 消息与服务器端命令路由进行处理。

### 运行示例

构建结果位于./build/src下

1. **启动服务器**

```bash
./build/src/osproj_server
```

2. **启动客户端并输入命令**

```bash
./build/src/osproj_client
```

客户端启动后，会进入一个简单的 REPL 循环，提示符为 `> `，可以直接输入命令并回车发送到服务器；输入 `quit`/`exit` 退出客户端。

3. **如果需要启动前端**
先安装nodejs和npm

```bash
npm install
cd ./gateway
node server.js
```
---

## 统一协议与命令路由说明

### 底层消息协议

客户端与服务器之间通过 `Message` 进行通信（定义见 `common/protocol.hpp`）：

- **消息类型 `MessageType`：**
  - `AuthRequest` / `AuthResponse`：为后续登录鉴权预留
  - `CommandRequest` / `CommandResponse`：当前主要使用的请求/响应类型
  - `Error`：表示解析或处理错误
- **消息结构 `Message`：**
  - `MessageType type`：消息类型
  - `std::string payload`：负载，目前为文本形式

在传输层（`TcpClient` / `TcpServer`）中，所有消息按照下面格式发送：

- 先发送 4 字节无符号整型（网络字节序），表示后续消息体长度 `N`
- 再发送 `N` 字节的消息体字符串 `data`
- `data` 的内容为：`"<type_int>\n<payload>"`  
  其中 `type_int` 是 `MessageType` 的整数值，`payload` 为原始字符串负载

### 统一命令抽象（Command）

在 `Message` 之上，项目定义了统一的命令结构 `Command`（见 `common/protocol.hpp`）：

- 字段说明：
  - `name`：命令名，例如 `PING` / `MKDIR` / `LIST_PAPERS`
  - `rawArgs`：去掉命令名后，整行剩余的字符串（不做拆分，保留空格）
  - `args`：将 `rawArgs` 按空格拆分后的参数数组，适合大多数简单命令
  - `sessionId`：可选，会话 ID。仅当 payload 使用 `SESSION <id> CMD ...` 形式时才会被填充

- `payload` 文本格式约定（两种等价形式）：

  1. **不携带会话（未登录或不需要鉴权的命令）**

     ```text
     CMD_NAME [arg1 arg2 ...]
     ```

  2. **携带会话前缀（推荐在需要鉴权的业务命令中使用, 已完成自动添加, 会用就行了）**

     ```text
     SESSION <sessionId> CMD CMD_NAME [arg1 arg2 ...]
     ```

     例如：

     ```text
     SESSION sess-1-1 CMD MKDIR /demo
     SESSION sess-1-1 CMD WRITE /demo/hello.txt hello world
     ```

  - 命令名与第一个参数之间至少有一个空格
  - 如果命令需要保留空格（例如 `WRITE` 写入一整行文本），服务器会基于 `rawArgs` 再做细粒度解析

- 解析与构造：
  - `parseCommandPayload(const std::string&)`：从 `Message.payload` 解析出 `Command`，自动识别并解析可选的 `SESSION <id> CMD` 前缀
  - `buildCommandPayload(const Command&)`：根据 `Command` 生成规范的 payload 字符串（若 `sessionId` 非空，则自动生成 `SESSION <id> CMD ...` 形式）

一般情况下，客户端只需构造：

```cpp
Message req{MessageType::CommandRequest, "PING"};
```

即可通过统一命令协议与服务器交互。

### 服务器端命令路由（ServerApp）

服务器核心类 `ServerApp` 对外只接收 `Message`，内部将所有 `CommandRequest`
统一交给路由函数处理（实现见 `server/server_app.cpp`）：

- `handleRequest(const Message& req)`：
  - 校验 `req.type == CommandRequest`
  - 调用 `parseCommandPayload(req.payload)` 得到 `Command`
  - 若 `Command.sessionId` 非空，则通过 `AuthService::validateSession` 校验会话是否有效；
    - 会话无效时直接返回 `Error: "Invalid or expired session"`
  - 将 `Command` 与可选的 `Session` 一起交给 `handleCommand` 做统一路由

- `handleCommand(const Command& cmd, std::optional<Session> maybeSession)`：
  - 预留统一处理入口，未来可以在这里加入：
    - 会话/鉴权检查（根据用户角色决定能否执行某条命令）
    - 统一日志/审计、错误封装等
  - 当前已支持的命令：
    - `PING`：健康检测，返回 `PONG`
    - `LOGIN <username> <password>`：登录，成功则返回包含会话 ID 和角色信息的响应
    - `LIST_PAPERS`：占位实现，当前返回 `"No papers yet."`
    - 若命令为 `MKDIR` / `WRITE` / `READ` / `RM` / `RMDIR` / `LIST`，
      则统一转入 `handleFsCommand` 进行文件系统相关处理

- `handleFsCommand(const Command& cmd, std::optional<Session> maybeSession)`：
  - **MKDIR `<path>`**：在虚拟文件系统中创建目录
  - **WRITE `<path>` `<content...>`**：
    - 使用 `cmd.rawArgs` 再次拆分：第一个 token 为 `path`，之后整行作为文件内容
    - 支持内容中包含空格
  - **READ `<path>`**：读取文件内容并作为响应 payload 返回
  - **RM `<path>`**：删除普通文件
  - **RMDIR `<path>`**：删除空目录（不允许递归删除）
  - **LIST `[path]`**：列出目录下的项目，若不指定路径则默认为根目录 `/`
  - CD <path>: 进入指定目录

通过上述路由结构，后续可以很方便地增加新的业务命令：

- 在 `handleCommand` 中识别新的 `cmd.name`（如 `LOGIN` / `UPLOAD_PAPER` 等）
- 将其分发到新的子模块（如 `handleAuthCommand` / `handlePaperCommand`）

---

## 命令行客户端使用示例

在服务器运行的前提下，启动 `osproj_client`，在提示符下可以输入如下命令（建议统一使用大写）：

- **基础连通性测试**
  - `PING`：服务器返回 `PONG`

- **登录与会话（Session）**
  - 目前服务器在启动时会内置四个测试账号（用户名/密码完全相同）：
    - `admin` / `admin`：`Admin` 角色
    - `author` / `author`：`Author` 角色
    - `reviewer` / `reviewer`：`Reviewer` 角色
    - `editor` / `editor`：`Editor` 角色
  - 登录命令示例：
    - `LOGIN admin admin`
  - 登录成功时，服务器会返回类似：
    - `SESSION sess-1-1 USER admin ROLE Admin`
  - 其中：
    - `sess-1-1` 为会话 ID（Session ID），后续请求在协议层会自动附带该 ID 进行权限控制（CLI 已内置，无需用户手动输入 `SESSION ...` 前缀）；
    - `USER` / `ROLE` 字段用于在客户端展示当前登录用户信息。

- **文件系统相关示例**
  - `LIST`：列出**当前目录**下的内容（初始为根目录 `/`）
  - `CD <path>`：切换当前目录（仅在客户端维护），示例：
    - `CD NEW`：从 `/` 进入 `/NEW`
    - `CD /REVIEW`：直接进入 `/REVIEW`
  - `MKDIR /demo`：在根目录下创建 `demo` 目录
  - `WRITE /demo/hello.txt hello world`：创建/覆盖文件并写入 `"hello world"`
  - `READ /demo/hello.txt`：读取文件内容
  - `RM /demo/hello.txt`：删除文件
  - `RMDIR /demo`：在目录为空时删除该目录
  - `LIST /demo`：显式列出指定目录 `/demo` 的内容（覆盖当前目录）
  - 目录项展示约定：
    - 普通文件：直接显示文件名，例如 `hello.txt`
    - 目录：在名称后追加 `/`，例如 `NEW/`、`REVIEW/`，便于与文件区分

客户端日志中会打印发送的请求与收到的响应，便于调试与扩展协议时观察行为。

---

## TODO:

- 在当前阻塞式 TCP 的基础上，支持**多客户端并发**（如 `select`/`poll`/`epoll` 或多线程），并在统一命令路由的基础上扩展更丰富的业务命令；
- 在 `fs::Vfs` 中进一步完善 superblock/inode 表/数据块管理、目录层次与路径解析；
- 在 `domain` 层实现作者/审稿人/编辑/管理员的权限检查和业务流程（上传论文、下载论文、提交评审、分配评审、备份等）；
- 增加备份/恢复功能与相应的 CLI 命令，并将其纳入统一命令协议和路由体系。

