# 操作系统课程大作业：简化学术评审系统（C++ Client-Server）

本项目参考《Operating Systems Practical Training Course Guide》要求，实现一个**基于文件系统的学术评审系统**的骨架代码，采用 **Client-Server 架构 + 自定义简单文件系统 + LRU 缓存 + TCP 通信**。

当前提交为**初始项目结构与可编译骨架代码**，便于后续在此基础上逐步完善功能。

## 目录结构

- `OsProject/`
  - `CMakeLists.txt`：顶层 CMake 配置
  - `src/`
    - `common/`：公共类型与协议
      - `types.hpp`：`UserId`/`PaperId`/`Role` 等基础类型
      - `logger.hpp`：简单日志输出封装
      - `protocol.hpp`：客户端与服务器之间的简易消息格式（后续可扩展）
    - `domain/`：业务领域模型
      - `user.hpp/.cpp`：用户实体（包含角色）
      - `roles.hpp`：角色枚举别名
      - `paper.hpp`：论文实体
      - `review.hpp`：评审结果实体
    - `server/`
      - `main.cpp`：服务器可执行程序入口
      - `server_app.hpp/.cpp`：服务器核心类，负责加载文件系统、处理客户端请求（当前按命令简单分发）
      - `filesystem/`：自定义文件系统骨架
        - `superblock.hpp`：SuperBlock 结构
        - `inode.hpp`：Inode 结构
        - `block_cache.hpp`：LRU 块缓存实现
      - `vfs.hpp/.cpp`：虚拟文件系统接口（mount / createFile / removeFile 占位实现）
      - `net/`：网络层实现
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

当前版本已经实现**基于 TCP 的真实网络通信**：

- 服务器使用阻塞式 TCP 服务器，在指定端口上循环处理多个客户端的一次性请求；
- 客户端提供命令行循环，可以多次输入命令，每次通过 TCP 发送到服务器并显示响应。

## 下一步建议

根据课程指导文档，可以在本骨架上逐步完善：

- 在当前阻塞式 TCP 的基础上，支持**多客户端并发**（如 `select`/`poll`/`epoll` 或多线程），并扩展更丰富的命令协议
- 在 `fs::Vfs` 中实现基于磁盘文件的 superblock/inode 表/数据块管理、目录层次与路径解析
- 在 `domain` 层实现作者/审稿人/编辑/管理员的权限检查和业务流程（上传论文、下载论文、提交评审、分配评审、备份等）
- 增加备份/恢复功能与相应的 CLI 命令


