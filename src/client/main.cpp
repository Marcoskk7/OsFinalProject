#include "cli.hpp"

int main()
{
    // 目前使用本地 127.0.0.1 与固定端口，后续可通过命令行参数或配置文件指定
    osp::client::Cli cli("127.0.0.1", 5555);
    cli.run();
    return 0;
}


