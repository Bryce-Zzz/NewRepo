#define NOMINMAX      
#include <iostream>
#include <string>
#include <thread>
#include <chrono>     
#include <vector>
#include <limits>     
#include <winsock2.h>
#include <ws2tcpip.h>
#include <conio.h>

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

    // 智能输入函数：一边等键盘打字，一边盯紧服务器
    bool GetInputWithMonitor(std::string& input) {
        input.clear();
        while (true) {
            // 1. 检查服务器是否发来了超时指令 (RESET_TIMEOUT)
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(clientSocket, &readfds);
            timeval tv = { 0, 50000 }; // 50毫秒超时
            if (select(0, &readfds, NULL, NULL, &tv) > 0) {
                char buf[256] = { 0 };
                int r = recv(clientSocket, buf, sizeof(buf) - 1, MSG_PEEK); // 只偷看一眼，不取走
                if (r <= 0) return false;
                if (std::string(buf).find("RESET_TIMEOUT") != std::string::npos) {
                    return false; // 发现超时信号！立刻中断输入
                }
            }

            // 2. 检查用户是否按了键盘
            if (_kbhit()) {
                char c = _getch();
                if (c == '\r') { // 回车键
                    std::cout << std::endl;
                    return true;
                }
                else if (c == '\b') { // 退格键
                    if (!input.empty()) {
                        input.pop_back();
                        std::cout << "\b \b";
                    }
                }
                else { // 正常字符
                    input += c;
                    std::cout << c;
                }
            }
        }
    }

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

    void ReceiveMessages() {
        char buffer[1024];
        while (isConnected) {
            memset(buffer, 0, sizeof(buffer));
            int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

            if (bytesReceived > 0) {
                std::string msg(buffer);
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
                // 【优化 1】：只处理异常断开，正常的 quit 不在这里打印多余信息
                if (isConnected) {
                    std::cout << "\n[!] 与服务端的连接已异常断开。" << std::endl;
                    isConnected = false;
                }
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

    bool AuthMenu() {
        while (isConnected) {
            system("cls");

            std::cout << "================================\n";
            std::cout << "     欢迎来到极简聊天室系统     \n";
            std::cout << "================================\n";
            std::cout << "1. 登录账号\n";
            std::cout << "2. 注册账号\n";
            std::cout << "3. 忘记密码 (手机验证码重置)\n";
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
            else if (choice == 1) {
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
            else if (choice == 2) {
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
            else if (choice == 3) {
                std::string id;
                std::cout << "\n请输入需要找回密码的数字ID: ";
                std::cin >> id;

                // 1. 发起申请
                std::string req = "FORGOT_PWD|" + id;
                send(clientSocket, req.c_str(), req.length(), 0);

                // 【温馨提示 1】：刚发起请求时的提示
                std::cout << "[系统温馨提示] 重置请求已发送，正在等待服务器处理...\n";

                char buffer[256] = { 0 };
                recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
                std::string res(buffer);
                std::vector<std::string> parts = SplitString(res, "|");

                // --- 处理限流排队 ---
                if (parts.size() >= 2 && parts[0] == "FORGOT_BUSY") {
                    std::cout << "\n[系统提示] " << parts[1] << " (Y/N): ";
                    char waitChoice;
                    std::cin >> waitChoice;

                    if (waitChoice == 'Y' || waitChoice == 'y') {
                        std::string waitReq = "FORGOT_WAIT|" + id;
                        send(clientSocket, waitReq.c_str(), waitReq.length(), 0);

                        bool waiting = true;
                        while (waiting) {
                            memset(buffer, 0, sizeof(buffer));
                            int r = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
                            if (r <= 0) break;

                            std::string wRes(buffer);
                            auto wParts = SplitString(wRes, "|");
                            if (wParts.empty()) continue;

                            if (wParts[0] == "WAITING") {
                                // 【温馨提示 2】：修复了这里的静默等待，给出明确的排队反馈！
                                std::cout << "\n[系统温馨提示] 当前排队人数较多，您已成功加入队列，请耐心排队等待...\n";
                            }
                            else if (wParts[0] == "WAIT_OK") {
                                // 【温馨提示 3】：排队成功，拿到名额的提示
                                std::cout << "\n[系统温馨提示] " << wParts[1] << " 请等待管理员审批下发验证码...\n";
                                waiting = false;
                            }
                        }

                        // 排队成功后，重新等待 FORGOT_OK (短信)
                        memset(buffer, 0, sizeof(buffer));
                        recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
                        res = std::string(buffer);
                        parts = SplitString(res, "|");
                    }
                    else {
                        // 【温馨提示 4】：取消排队的提示
                        std::string cancelReq = "FORGOT_CANCEL|" + id;
                        send(clientSocket, cancelReq.c_str(), cancelReq.length(), 0);
                        std::cout << "\n[温馨提示] 您已取消排队，将为您返回主菜单。\n\n";
                        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                        system("pause");
                        continue;
                    }
                }

                // --- 处理验证码下发 ---
                if (parts.size() >= 2 && parts[0] == "FORGOT_OK") {
                    std::string serverCode = parts[1];
                    // 【温馨提示 5】：手机短信的提示（这里我已经帮你同步为1分钟有效了）
                    std::cout << "\n[手机模拟器] 叮！您收到一条短信：验证码为 【 " << serverCode << " 】，1分钟内有效。\n\n";

                    // 定义一个处理超时的快速宏
                    auto handleTimeout = [&]() {
                        std::cout << "\n\n[系统警报] 操作已超时 (超过1分钟)！您的重置名额已被系统回收。\n";
                        std::cout << "按任意键返回主菜单...\n";
                        _getch(); // 等待用户随便按个键

                        // 把服务器发来的 RESET_TIMEOUT 取走，清理网络管道
                        char flushBuf[256] = { 0 };
                        recv(clientSocket, flushBuf, sizeof(flushBuf) - 1, 0);
                        };

                    std::string inputCode, pwd1, pwd2;

                    std::cout << "请输入6位数验证码: ";
                    if (!GetInputWithMonitor(inputCode)) { handleTimeout(); continue; }

                    std::cout << "请输入新密码: ";
                    if (!GetInputWithMonitor(pwd1)) { handleTimeout(); continue; }

                    std::cout << "请再次确认新密码: ";
                    if (!GetInputWithMonitor(pwd2)) { handleTimeout(); continue; }

                    // 本地二次校验一致性
                    if (pwd1 != pwd2) {
                        std::cout << "\n[错误提示] 两次输入的密码不一致！修改已取消。\n\n";
                        // 通知服务端释放名额
                        std::string cancelReq = "FORGOT_CANCEL|" + id;
                        send(clientSocket, cancelReq.c_str(), cancelReq.length(), 0);
                        system("pause");
                        continue;
                    }

                    // 提交给服务端进行 Redis 验证
                    std::string resetReq = "RESET_PWD|" + id + "|" + inputCode + "|" + pwd1;
                    send(clientSocket, resetReq.c_str(), resetReq.length(), 0);

                    memset(buffer, 0, sizeof(buffer));
                    recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
                    std::string resetRes(buffer);
                    std::vector<std::string> resetParts = SplitString(resetRes, "|");

                    if (resetParts.size() >= 2 && resetParts[0] == "RESET_OK") {
                        std::cout << "\n[恭喜] " << resetParts[1] << "\n\n";
                    }
                    else if (resetParts.size() >= 2) {
                        std::cout << "\n[失败] " << resetParts[1] << "\n\n";
                    }
                }
                else if (parts.size() >= 2 && parts[0] == "FORGOT_FAIL") {
                    std::cout << "\n[请求失败] " << parts[1] << "\n\n";
                }

                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                system("pause");
            }
        }
        return false;
    }

    void RunChat() {
        system("cls");
        std::cout << "========================================" << std::endl;
        std::cout << "成功进入聊天大厅！当前身份: " << myName << " (ID:" << myId << ")" << std::endl;
        std::cout << "【群聊】直接打字并回车" << std::endl;
        std::cout << "【私聊】格式: @目标ID或昵称 消息内容" << std::endl;
        std::cout << "【改名】格式: /nick 新名字" << std::endl;
        std::cout << "【退出】格式: quit" << std::endl;
        std::cout << "========================================" << std::endl;

        std::thread(&ChatClient::ReceiveMessages, this).detach();

        std::cout << currentPrompt;

        std::string userInput;
        while (isConnected) {
            std::getline(std::cin, userInput);

            if (userInput == "quit") {
                isConnected = false; // 【核心修改】：先告诉后台线程“我要正常退出了”
                shutdown(clientSocket, SD_SEND);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                break;
            }

            if (!userInput.empty()) {
                send(clientSocket, userInput.c_str(), userInput.length(), 0);
                std::cout << currentPrompt;
            }
        }
    }
};

int main() {
    ChatClient client("127.0.0.1", 8080);

    if (!client.Initialize()) {
        std::cerr << "\n[!] 连接服务端失败! 请确保你已经先启动了服务端 (Server.exe)。" << std::endl;
        system("pause");
        return 0;
    }

    if (client.AuthMenu()) {
        client.RunChat();
    }

    // 【优化 1】：完美的退出提示
    std::cout << "\n当前客户端已安全断开连接。" << std::endl;
    system("pause");
    return 0;
}