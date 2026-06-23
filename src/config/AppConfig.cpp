#include "smartnas/config/AppConfig.h"

#include <fstream>
#include <regex>
#include <sstream>

namespace
{
    std::string string_value(const std::string &json, const std::string &key, const std::string &fallback)
    {
        const std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
        std::smatch match;
        return std::regex_search(json, match, pattern) ? match[1].str() : fallback;
    }

    int int_value(const std::string &json, const std::string &key, int fallback)
    {
        const std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*([0-9]+)");
        std::smatch match;
        return std::regex_search(json, match, pattern) ? std::stoi(match[1].str()) : fallback;
    }
}

namespace smartnas::config
{
    AppConfig &AppConfig::get_instance()
    {
        static AppConfig config;
        return config;
    }

    bool AppConfig::load(const std::string &path)
    {
        std::ifstream file(path);
        if (!file.is_open())
            return false;
        std::stringstream buffer;
        buffer << file.rdbuf();
        const std::string json = buffer.str();
        if (json.empty())
            return false;

        const std::filesystem::path config_path = std::filesystem::absolute(path);
        const std::filesystem::path project_root = config_path.parent_path().parent_path();
        auto resolve = [&project_root](const std::string &value)
        {
            const std::filesystem::path candidate(value);
            return (candidate.is_absolute() ? candidate : project_root / candidate).lexically_normal();
        };

        core_host_ = string_value(json, "core_host", core_host_);
        agent_host_ = string_value(json, "agent_host", agent_host_);
        jwt_secret_ = string_value(json, "jwt_secret", jwt_secret_);
        core_port_ = static_cast<unsigned short>(int_value(json, "core_port", core_port_));
        agent_port_ = static_cast<unsigned short>(int_value(json, "agent_port", agent_port_));
        upload_chunk_size_ = static_cast<std::size_t>(int_value(json, "upload_chunk_size", upload_chunk_size_));
        upload_concurrency_ = static_cast<unsigned int>(int_value(json, "upload_concurrency", upload_concurrency_));
        web_crypto_limit_ = static_cast<std::size_t>(int_value(json, "web_crypto_limit", web_crypto_limit_));
        database_path_ = resolve(string_value(json, "database_path", database_path_.string()));
        data_dir_ = resolve(string_value(json, "data_dir", data_dir_.string()));
        web_dir_ = resolve(string_value(json, "web_dir", web_dir_.string()));
        return core_port_ > 0 && agent_port_ > 0 && upload_chunk_size_ > 0 &&
               upload_concurrency_ > 0 && !jwt_secret_.empty();
    }

    std::filesystem::path AppConfig::data_path(const std::string &name) const
    {
        return data_dir_ / name;
    }

    std::filesystem::path AppConfig::web_path(const std::string &name) const
    {
        return web_dir_ / name;
    }
}
