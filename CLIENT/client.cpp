#define NOMINMAX      // 禁用 Windows 默认的 min/max 宏，解决冲突！
#include <iostream>
#include <string>
#include <thread>
#include <chrono>     
#include <vector>
#include <limits>     // 添加 limits 头文件以使用 numeric_limits
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

class ChatClient {
private:
    std::string serverIp;
    int serverPort;
    SOCKET clientSocket;
    std::string currentPrompt;
    std::string myId;
    std::string myName;
    bool isConnected;

    std::vector<std::string> SplitString(const std::string& str, const std::string& delimiter) {
        std::vector<std::string> tokens;
        size_t prev = 0, pos = 0;
        do {
            pos = str.find(delimiter, prev);
            if (pos == std::string::npos) pos = str.length();
            std::string token = str.substr(prev, pos - prev);
            if (!token.empty()) tokens.push_back(token);
            prev = pos + delimiter.length();
        } while (pos < str.length() && prev < str.length());
        return tokens;
    }

    // 后台接收聊天消息的线程
    void ReceiveMessages() {
        char buffer[1024];
        while (isConnected) {
            memset(buffer, 0, sizeof(buffer));
            int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

            if (bytesReceived > 0) {
                std::string msg(buffer);

                // 识别服务端发来的“同意改名”指令 
                if (msg.substr(0, 9) == "NICK_ACK:") {
                    std::string newName = msg.substr(9);
                    currentPrompt = "[" + newName + "] > ";
                    std::cout << "\n[系统提示]: 你的本地提示符已同步更新。\n" << currentPrompt;
                }
                else {
                    std::cout << "\n" << msg << "\n" << currentPrompt;
                }
            }
            else {
                std::cout << "\n[!] 与服务端的连接已断开。" << std::endl;
                isConnected = false;
                break;
            }
        }
    }

public:
    ChatClient(std::string ip, int port)
        : serverIp(ip), serverPort(port), clientSocket(INVALID_SOCKET), currentPrompt("> "), isConnected(false) {
    }

    ~ChatClient() {
        if (clientSocket != INVALID_SOCKET) closesocket(clientSocket);
        WSACleanup();
    }

    bool Initialize() {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;

        clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (clientSocket == INVALID_SOCKET) return false;

        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(serverPort);
        inet_pton(AF_INET, serverIp.c_str(), &serverAddr.sin_addr);

        if (connect(clientSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) return false;

        isConnected = true;
        return true;
    }

    // 核心：登录验证菜单
    bool AuthMenu() {
        while (isConnected) {
            system("cls"); // 每次进入或回到菜单前，先清空屏幕

            std::cout << "================================\n";
            std::cout << "     欢迎来到极简聊天室系统     \n";
            std::cout << "================================\n";
            std::cout << "1. 登录账号\n";
            std::cout << "2. 注册账号\n";
            std::cout << "0. 退出\n";
            std::cout << "请选择: ";

            int choice;
            std::cin >> choice;

            if (std::cin.fail()) {
                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                std::cout << "\n[提示] 输入无效，请重新输入！\n\n";
                system("pause");
                continue;
            }

            if (choice == 0) {
                return false;
            }
            else if (choice == 1) { // 登录
                std::string id, pwd;
                std::cout << "请输入数字ID: "; std::cin >> id;
                std::cout << "请输入密码: "; std::cin >> pwd;

                std::string req = "LOGIN|" + id + "|" + pwd;
                send(clientSocket, req.c_str(), req.length(), 0);

                char buffer[256] = { 0 };
                recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
                std::string res(buffer);
                std::vector<std::string> parts = SplitString(res, "|");

                if (parts.size() >= 2 && parts[0] == "LOGIN_OK") {
                    myId = id;
                    myName = parts[1];
                    currentPrompt = "[" + myName + "] > ";
                    std::cout << "\n[系统提示] 登录成功！欢迎回来，" << myName << "！\n\n";
                    system("pause");
                    return true;
                }
                else if (parts.size() >= 2 && parts[0] == "LOGIN_FAIL") {
                    std::cout << "\n[登录失败] " << parts[1] << "\n\n";
                    system("pause");
                }
            }
            else if (choice == 2) { // 注册
                // 【升级】：第 1 步，向服务端索取下一个号码牌
                std::string reqId = "GET_NEXT_ID";
                send(clientSocket, reqId.c_str(), reqId.length(), 0);

                char buffer[256] = { 0 };
                recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
                std::string res(buffer);
                std::vector<std::string> parts = SplitString(res, "|");

                if (parts.size() >= 2 && parts[0] == "NEXT_ID") {
                    std::string preAssignedId = parts[1];

                    std::cout << "\n======================================\n";
                    std::cout << "  系统为您预留的专属ID为：【 " << preAssignedId << " 】\n";
                    std::cout << "======================================\n";

                    std::string pwd, name;
                    std::cout << "设置密码: "; std::cin >> pwd;
                    std::cout << "设置昵称: "; std::cin >> name;

                    // 第 2 步，带着预留的 ID 提交正式注册申请
                    std::string req = "REG|" + preAssignedId + "|" + pwd + "|" + name;
                    send(clientSocket, req.c_str(), req.length(), 0);

                    memset(buffer, 0, sizeof(buffer));
                    recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
                    std::string regRes(buffer);
                    std::vector<std::string> regParts = SplitString(regRes, "|");

                    if (regParts.size() >= 2) {
                        if (regParts[0] == "REG_OK") {
                            std::cout << "\n[恭喜] 注册成功！请务必牢记您的ID：" << preAssignedId << "\n\n";
                        }
                        else {
                            std::cout << "\n[注册失败] " << regParts[1] << "\n\n";
                        }
                        system("pause");
                    }
                }
            }
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n'); // 清理缓冲区
        }
        return false;
    }

    // 正式的聊天交互界面
    void RunChat() {
        system("cls"); // 清空屏幕，进入干净的黑框
        std::cout << "========================================" << std::endl;
        std::cout << "成功进入聊天大厅！当前身份: " << myName << " (ID:" << myId << ")" << std::endl;
        std::cout << "【群聊】直接打字并回车" << std::endl;
        std::cout << "【私聊】格式: @目标ID或昵称 消息内容" << std::endl;
        std::cout << "【改名】格式: /nick 新名字" << std::endl;
        std::cout << "【退出】格式: quit" << std::endl;
        std::cout << "========================================" << std::endl;

        // 此时才启动后台接收线程
        std::thread(&ChatClient::ReceiveMessages, this).detach();

        std::cout << currentPrompt;

        std::string userInput;
        while (isConnected) {
            std::getline(std::cin, userInput);

            if (userInput == "quit") {
                shutdown(clientSocket, SD_SEND);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                isConnected = false;
                break;
            }

            if (!userInput.empty()) {
                send(clientSocket, userInput.c_str(), userInput.length(), 0);
                std::cout << currentPrompt;
            }
        }
    }
};

// ================= 主函数 =================
int main() {
    ChatClient client("127.0.0.1", 8080);

    // 1. 初始化并连接服务器
    if (!client.Initialize()) {
        std::cerr << "\n[!] 连接服务端失败! 请确保你已经先启动了服务端 (Server.exe)。" << std::endl;
        system("pause"); // 暂停一下，让你能看清报错信息
        return 0;
    }

    // 2. 进入注册/登录菜单环节
    if (client.AuthMenu()) {
        // 3. 验证通过，切入你喜欢的纯净黑框聊天大厅
        client.RunChat();
    }

    std::cout << "\n程序已退出。" << std::endl;
    system("pause"); // 正常退出时也暂停一下
    return 0;
}