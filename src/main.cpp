#include <iostream>
#include "workflow/WFHttpServer.h"
#include "smartnas/api/Router.h" // 引入我们刚刚写的路由类

int main()
{
    // 将我们自己封装的 Router::process 作为处理回调传给服务器
    WFHttpServer server(smartnas::api::Router::process);

    unsigned short port = 8080;
    if (server.start(port) == 0)
    {
        std::cout << "=====================================" << std::endl;
        std::cout << "SmartNAS 核心网关已启动!" << std::endl;
        std::cout << "监听端口: " << port << std::endl;
        std::cout << "=====================================" << std::endl;

        getchar();
        server.stop();
    }
    else
    {
        std::cerr << "服务器启动失败！" << std::endl;
        return -1;
    }
    return 0;
}