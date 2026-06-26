#include <iostream>
#include <unistd.h>
#include "workflow/WFHttpServer.h"
#include "smartnas/api/Router.h"
#include "smartnas/db/DatabaseManager.h"
#include "smartnas/config/AppConfig.h"
#include <cstdlib>
#include <filesystem>

int main(int argc, char **argv)
{
    const char *env_config = std::getenv("SMARTNAS_CONFIG");
    const std::string config_path = argc > 1 ? argv[1] : (env_config ? env_config : "config/config.json");
    auto &config = smartnas::config::AppConfig::get_instance();
    if (!config.load(config_path))
    {
        std::cerr << "配置加载失败: " << config_path << std::endl;
        return -1;
    }
    std::filesystem::create_directories(config.data_dir());
    std::filesystem::create_directories(config.database_path().parent_path());

    if (!smartnas::db::DatabaseManager::get_instance().init(config.database_path().string()))
    {
        std::cerr << "数据库初始化失败！" << std::endl;
        return -1;
    }

    WFHttpServer server(smartnas::api::Router::process);
    const unsigned short port = config.core_port();
    if (server.start(config.core_host().c_str(), port) == 0)
    {
        std::cout << "=====================================" << std::endl;
        std::cout << "SmartNAS 核心网关已启动!" << std::endl;
        std::cout << "监听地址: " << config.core_host() << ":" << port << std::endl;
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
