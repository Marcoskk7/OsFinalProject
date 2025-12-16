# 操作系统课程大作业：简化学术评审系统（C++ Client-Server）

本项目参考《Operating Systems Practical Training Course Guide》要求，实现一个**基于文件系统的学术评审系统**的骨架代码，采用 **Client-Server 架构 + 自定义简单文件系统 + LRU 缓存 + TCP 通信**。

当前提交为**初始项目结构与可编译骨架代码**，并已经实现**统一命令协议与服务器端命令路由框架**，便于后续在此基础上逐步完善业务功能。

## 目录结构

- `OsFinalProject/`
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

在项目根目录 `OsFinalProject/` 中执行：

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
./build/src/osproj_server [port] [cacheCapacity]
```

说明：
- `port`：监听端口，默认 `5555`
- `cacheCapacity`：块缓存容量（LRU entries 数），默认 `64`
- 也可通过环境变量 `OSP_CACHE_CAPACITY` 覆盖默认缓存容量（若同时提供命令行参数，则以命令行参数优先）

2. **启动客户端并输入命令**

```bash
./build/src/osproj_client
```

提示：客户端当前默认连接 `127.0.0.1:5555`（见 `src/client/main.cpp`）。如果你用 `osproj_server` 改了端口，请同步修改客户端代码或自行扩展客户端参数。

客户端启动后，会进入一个简单的 REPL 循环，提示符为 `> `，可以直接输入命令并回车发送到服务器；输入 `quit`/`exit` 退出客户端。

3. **如果需要启动前端**
先安装nodejs和npm

```bash
cd ./gateway
npm install
node server.js
```
---

## 统一协议与命令路由说明

### 底层消息协议

客户端与服务器之间通过 `Message` 进行通信（定义见 `src/common/protocol.hpp`）：

- **消息类型 `MessageType`：**
  - `AuthRequest` / `AuthResponse`：为后续登录鉴权预留
  - `CommandRequest` / `CommandResponse`：当前主要使用的请求/响应类型
  - `Error`：表示解析或处理错误
- **消息结构 `Message`：**
  - `MessageType type`：消息类型
  - `json payload`：负载（JSON）

在传输层（`TcpClient` / `TcpServer`）中，所有消息按照下面格式发送：

- 先发送 4 字节无符号整型（网络字节序），表示后续消息体长度 `N`
- 再发送 `N` 字节的消息体字符串 `data`
- `data` 的内容为 JSON 序列化后的 envelope（见 `osp::protocol::serialize()`）：
  - `{"type":"CommandRequest","payload":{...}}`

### 统一命令抽象（Command）

在 `Message.payload` 之上，项目定义了统一的命令结构 `Command`（见 `src/common/protocol.hpp`）：

- 字段说明：
  - `name`：命令名，例如 `PING` / `MKDIR` / `LIST_PAPERS`
  - `rawArgs`：去掉命令名后，整行剩余的字符串（不做拆分，保留空格）
  - `args`：将 `rawArgs` 按空格拆分后的参数数组，适合大多数简单命令
  - `sessionId`：可选，会话 ID（登录成功后由客户端自动携带）

- **当前使用的 JSON 命令格式**：

  - 未登录（不携带会话）示例：

```json
{
  "sessionId": null,
  "cmd": "PING",
  "args": [],
  "rawArgs": ""
}
```

  - 已登录（自动携带会话）示例：

```json
{
  "sessionId": "sess-1-1",
  "cmd": "LIST",
  "args": ["/"],
  "rawArgs": "/"
}
```

- 解析与构造：
  - `parseCommandFromJson(const json&)`：服务端从 `Message.payload` 解析 `Command`
  - `commandToJson(const Command&)`：客户端将 `Command` 转回 JSON payload
  - `parseCommandLine(const std::string&)`：客户端本地把用户输入的一行命令解析为 `Command`（CLI 交互用；网络传输仍是 JSON）

### 服务器端命令路由（ServerApp）

服务器核心类 `ServerApp` 对外只接收 `Message`，内部将所有 `CommandRequest`
统一交给路由函数处理（实现见 `server/server_app.cpp`）：

- `handleRequest(const Message& req)`：
  - 校验 `req.type == CommandRequest`
  - 调用 `parseCommandFromJson(req.payload)` 得到 `Command`
  - 若 `Command.sessionId` 非空，则通过 `AuthService::validateSession` 校验会话是否有效；
    - 会话无效时直接返回 `Error: "Invalid or expired session"`
  - 将 `Command` 与可选的 `Session` 一起交给 `handleCommand` 做统一路由

- `handleCommand(const Command& cmd, std::optional<Session> maybeSession)`：
  - 预留统一处理入口，未来可以在这里加入：
    - 会话/鉴权检查（根据用户角色决定能否执行某条命令）
    - 统一日志/审计、错误封装等
  - 当前已支持的主要命令（按模块）：
    - **基础**：`PING`
    - **认证**：`LOGIN <username> <password>`（成功返回 `sessionId/role/username/userId`）
    - **文件系统**：`MKDIR / WRITE / READ / RM / RMDIR / LIST`
    - **论文流程**：`LIST_PAPERS / SUBMIT / GET_PAPER / ASSIGN / REVIEW / LIST_REVIEWS / DECISION`
    - **编辑便捷命令**：`ASSIGN_REVIEWER / VIEW_REVIEW_STATUS / MAKE_FINAL_DECISION`（内部会转成基础论文命令）
    - **管理员**：`MANAGE_USERS ... / BACKUP / RESTORE / VIEW_SYSTEM_STATUS`

- `handleFsCommand(const Command& cmd, std::optional<Session> maybeSession)`：
  - **MKDIR `<path>`**：在虚拟文件系统中创建目录
  - **WRITE `<path>` `<content...>`**：
    - 使用 `cmd.rawArgs` 再次拆分：第一个 token 为 `path`，之后整行作为文件内容
    - 支持内容中包含空格
  - **READ `<path>`**：读取文件内容并作为响应 payload 返回
  - **RM `<path>`**：删除普通文件
  - **RMDIR `<path>`**：删除空目录（不允许递归删除）
  - **LIST `[path]`**：列出目录下的项目，若不指定路径则默认为根目录 `/`
  - `CD <path>`：客户端本地切换“当前目录”（仅影响默认 `LIST` 行为；会先用 `LIST <path>` 向服务器校验目录是否存在）

通过上述路由结构，后续可以很方便地增加新的业务命令：

- 在 `handleCommand` 中识别新的 `cmd.name`（如 `LOGIN` / `UPLOAD_PAPER` 等）
- 将其分发到新的子模块（如 `handleAuthCommand` / `handlePaperCommand`）

### 响应格式（成功/失败）

项目统一使用 JSON 响应结构（见 `makeSuccessResponse()` / `makeErrorResponse()`）：

- 成功：`{"ok": true, "data": {...}}`
- 失败：`{"ok": false, "error": {"code": "...", "message": "...", "details": {...}}}`

### JSON 请求/响应示例（实际网络传输的 envelope）

下面示例展示的是**TcpClient/TcpServer 实际发送的字符串内容**（即 `serialize()` 的输出），外层 envelope 形如：
- `type`：`CommandRequest` / `CommandResponse` / `Error`
- `payload`：具体命令或响应数据

1) **LOGIN**

请求：

```json
{
  "type": "CommandRequest",
  "payload": {
    "sessionId": null,
    "cmd": "LOGIN",
    "args": ["admin", "admin"],
    "rawArgs": "admin admin"
  }
}
```

成功响应（示例字段，`sessionId` 每次会不同）：

```json
{
  "type": "CommandResponse",
  "payload": {
    "ok": true,
    "data": {
      "sessionId": "sess-1-1",
      "userId": 1,
      "username": "admin",
      "role": "Admin"
    }
  }
}
```

2) **VIEW_SYSTEM_STATUS（需要 Admin 或 Editor，且需要 sessionId）**

请求（已登录后，客户端会自动携带 `sessionId`）：

```json
{
  "type": "CommandRequest",
  "payload": {
    "sessionId": "sess-1-1",
    "cmd": "VIEW_SYSTEM_STATUS",
    "args": [],
    "rawArgs": ""
  }
}
```

成功响应（示例，计数会随系统状态变化）：

```json
{
  "type": "CommandResponse",
  "payload": {
    "ok": true,
    "data": {
      "users": 5,
      "sessions": 2,
      "papers": 3,
      "reviews": 1,
      "blockCache": {
        "capacity": 64,
        "entries": 10,
        "hits": 123,
        "misses": 45,
        "replacements": 6
      }
    }
  }
}
```

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
  - 登录成功后，客户端会从响应 JSON 中提取 `sessionId`，并自动附带到后续请求中（无需手动输入会话前缀）。

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

