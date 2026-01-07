## 基于自研文件系统的简化学术评审系统

 Login 首页

![image-20260107175023422](https://raw.githubusercontent.com/Marcoskk7/TyporaImageHosting/main/image-20260107175023422.png)

- **项目**：基于自研文件系统的简化学术评审系统
- **关键词**：Client-Server / 自研 VFS / LRU 块缓存 / 并发 / 备份恢复 / RBAC
- **技术栈**：C++ Server + Node.js Gateway + Web/CLI + `data.fs`

---

## 第2页：课程要求对齐

做成表格☑️形式？看着来，也可以在 ppt 里面自己设计个

![image-20260107175648499](https://raw.githubusercontent.com/Marcoskk7/TyporaImageHosting/main/image-20260107175648499.png)

- **Client-Server 架构**：Web/CLI → TCP → Server
- **自研文件系统**：目录/文件/路径解析/持久化
- **LRU 块缓存**：命中/缺失/替换统计
- **多客户端并发**：线程池 + 关键资源加锁
- **备份恢复**：快照式备份 + 一键回滚
- **权限控制**：4 角色 RBAC
- **扩展**：审稿人推荐（领域匹配）

---

## 第3页：总体架构（1 张图讲清）

架构图（Web/CLI→Gateway→C++ Server→VFS→`data.fs`），边上标注 HTTP/TCP/Block I/O

![image-20260107165445355](https://raw.githubusercontent.com/Marcoskk7/TyporaImageHosting/main/image-20260107165445355.png)

- **Web 前端**：HTTP
- **Gateway（Node.js）**：HTTP ↔ TCP 桥接（方便演示 UI）
- **C++ 服务端**：统一入口路由 + 业务 Handler/Service
- **存储层**：自研 VFS + LRU BlockCache + `data.fs`

数据流：Client → Gateway(可选) → TCP → Server → VFS → `data.fs`

---

## 第4页：统一协议（Envelope + TCP Framing）

“Envelope JSON + 4 字节长度前缀”示意图

以 Author 角色发送的 LIST_PAPERS 指令

{
    "args": [],
    "cmd": "LIST_PAPERS",
    "sessionId": "sess-2-1"
}

> 上面展示的是 **JSON payload。实际 TCP 线上发送是：**4 字节长度前缀 + JSON(envelope)，长度前缀是二进制字节，正常打印 JSON 时看不到。

服务端的返回
![image-20260107175835343](https://raw.githubusercontent.com/Marcoskk7/TyporaImageHosting/main/image-20260107175835343.png)

- **Envelope**：统一请求/响应/错误格式，业务扩展更简单
- **Framing**：TCP 使用“4 字节长度前缀 + JSON”解决粘包/拆包

4 字节的内容是后续 json 内容的 length，因为 Tcp 接收内容是拆包+粘包形式

代码如下![carbon](https://raw.githubusercontent.com/Marcoskk7/TyporaImageHosting/main/carbon.png)

~~~cpp
const std::string data = osp::protocol::serialize(msg); // 把 Message 变成 JSON 字符串
std::uint32_t len = static_cast<std::uint32_t>(data.size()); // JSON 有多少字节
len = htonl(len); // 转成网络字节序（大端）
sendAll(fd, &len, sizeof(len)); // 先发 4 字节长度
sendAll(fd, data.data(), data.size()); // 再发 JSON 内容

std::uint32_t len = 0;
recvAll(fd, &len, sizeof(len)); // 先收 4 字节
len = ntohl(len);               // 转回本机整数
data.resize(len);               // 开 len 字节缓冲区
recvAll(fd, data.data(), data.size()); // 再收 len 字节（完整 JSON）
~~~

报错展示

![image-20260107180848972](https://raw.githubusercontent.com/Marcoskk7/TyporaImageHosting/main/image-20260107180848972.png)

- **收益**：调试方便、错误统一、客户端实现成本低

---

## 第5页：服务端处理主链路

image5：时序图（收到请求→validateSession→路由 Handler/Service→返回 Response/Error），两种配色，看着选一个吧

![image-20260107172304817](https://raw.githubusercontent.com/Marcoskk7/TyporaImageHosting/main/image-20260107172304817.png)

![Untitled diagram-2026-01-07-092230](https://raw.githubusercontent.com/Marcoskk7/TyporaImageHosting/main/Untitled%20diagram-2026-01-07-092230.png)

- **入口**：收到 `CommandRequest`
- **鉴权**：带 `sessionId` 的命令统一校验会话
- **路由**：LOGIN/PING 等核心命令优先；其余分发到各角色 Handler / FsService
- **失败统一返回**：`INVALID_SESSION / PERMISSION_DENIED / UNKNOWN_COMMAND`

---

## 第6页：文件系统

 `data.fs` 布局示意图（superblock / inode table / bitmap / data blocks）

![image-20260107174456700](https://raw.githubusercontent.com/Marcoskk7/TyporaImageHosting/main/image-20260107174456700.png)

![fs_layout](https://raw.githubusercontent.com/Marcoskk7/TyporaImageHosting/main/fs_layout.png)

- **布局**：SuperBlock + inode 表 + 空闲位图 + 数据块区
- **能力**：多级目录、路径解析、文件创建/删除/读写
- **简化点**：适配课程规模的小文件场景（便于实现与演示）

---

## 第7页：LRU 块缓存

缓存统计截图

![image-20260107174048397](https://raw.githubusercontent.com/Marcoskk7/TyporaImageHosting/main/image-20260107174048397.png)

- **策略**：最近访问的块保留在内存；缓存满则淘汰最久未使用块（LRU）
- **覆盖路径**：读写块时透明使用缓存（上层业务不用改）
- **可观测性**：`hits / misses / replacements / capacity`（demo 直接展示命中率）

---

## 第8页：并发

并发演示截图，线程池演示，可以看多个 tid 并发访问

![image-20260107181053302](https://raw.githubusercontent.com/Marcoskk7/TyporaImageHosting/main/image-20260107181053302.png)

- **线程池处理多个连接**：多客户端同时发命令
- **共享资源加锁**：
  - `VFS`：保护文件系统一致性
  - `AuthService`：保护用户/会话一致性
- **恢复操作**：双锁保护 + 固定顺序拿锁避免死锁

---

## 第9页：备份与恢复

Ωß BACKUP/RESTORE 截图

- **备份**：Admin 执行 → `sync` → 复制 `data.fs` 生成快照
- **恢复**：Admin 执行 → 双锁保护 → 复制回 `data.fs` → 重新挂载 → 重载用户/清理会话
- **价值**：可演示“持久化 + 一致性 + 运维能力”



---

## 第10页：Demo 

包含登录+业务流程

- **0:00–0:40** 启动 Server + Gateway；演示登录（至少 2 个角色：Author/Editor）
- **0:40–2:40** 走评审闭环（提交→推荐→分配→评审→决策）
- **2:40–4:20** Admin：查看系统状态（含缓存统计）→ BACKUP → 修改数据 → RESTORE 回滚
- **4:20–5:00** 权限拦截：用 Author 尝试 BACKUP（返回 PERMISSION_DENIED）



![image-20260107203120942](https://raw.githubusercontent.com/Marcoskk7/TyporaImageHosting/main/image-20260107203120942.png)
