#include <iostream>
#include <unistd.h>
#include "workflow/WFHttpServer.h"
#include "smartnas/api/Router.h"
#include "smartnas/db/DatabaseManager.h"

int main()
{
    if (!smartnas::db::DatabaseManager::get_instance().init("../../var/db/smartnas.db"))
    {
        std::cerr << "数据库初始化失败！" << std::endl;
        return -1;
    }

    WFHttpServer server(smartnas::api::Router::process);
    unsigned short port = 8080;
    if (server.start(port) == 0)
    {
        std::cout << "=====================================" << std::endl;
        std::cout << "SmartNAS 核心网关已启动!" << std::endl;
        std::cout << "监听端口: " << port << std::endl;
        std::cout << "=====================================" << std::endl;

        // Block forever instead of getchar() which causes SIGTTIN in background
        while (true)
        {
            pause();
        }
        server.stop();
    }
    else
    {
        std::cerr << "服务器启动失败！" << std::endl;
        return -1;
    }
    return 0;
}
