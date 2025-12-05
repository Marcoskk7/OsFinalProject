#include "server_app.hpp"

int main()
{
    // 端口号可根据需要修改或从配置中读取
    osp::server::ServerApp app(5555);
    app.run();
    return 0;
}


