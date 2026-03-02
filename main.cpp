#include <iostream>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

int main()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // 监听 0.0.0.0
    address.sin_port = htons(8080);

    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 3);

    std::cout << "NAS 服务已启动，尝试访问 http://localhost:8080" << std::endl;

    while (true)
    {
        int new_socket = accept(server_fd, nullptr, nullptr);
        std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nHello from SmartNAS WSL2!";
        send(new_socket, response.c_str(), response.length(), 0);
        close(new_socket);
    }
    return 0;
}