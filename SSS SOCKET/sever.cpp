#define NOMINMAX
#include <iostream>
#include <string>
#include <thread>
#include <map>
#include <mutex>
#include <fstream>    
#include <chrono>     
#include <iomanip>    
#include <vector>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

class ChatServer {
private:
    std::map<int, SOCKET> clientMap;
    std::map<int, std::string> nameMap;
    std::mutex mapMutex;
    std::mutex consoleMutex;
    std::mutex fileMutex;

    SOCKET listenSocket;
    int serverPort;
    std::string serverPrompt = "[服务器端] > ";
    std::ofstream logFile;

    std::map<int, std::pair<std::string, std::string>> registeredUsers;
    const std::string USERS_FILE = "users.txt";

    int nextUserId = 10000;

    void PrintLog(const std::string& logMsg) {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_info;
        localtime_s(&tm_info, &t);

        std::lock_guard<std::mutex> lock(consoleMutex);
        std::cout << "\n" << logMsg << "\n" << serverPrompt;

        if (logFile.is_open()) {
            logFile << "[" << std::put_time(&tm_info, "%Y-%m-%d %H:%M:%S") << "] " << logMsg << "\n";
            logFile.flush();
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

    void LoadUsers() {
        std::ifstream ifs(USERS_FILE);
        if (!ifs.is_open()) return;
        int id; std::string pwd, name;
        while (ifs >> id >> pwd >> name) {
            registeredUsers[id] = std::make_pair(pwd, name);
            if (id >= nextUserId) {
                nextUserId = id + 1;
            }
        }
        ifs.close();
        PrintLog("[系统启动] 成功加载历史注册用户数量: " + std::to_string(registeredUsers.size()));
    }

    void SaveNewUser(int id, const std::string& pwd, const std::string& name) {
        std::lock_guard<std::mutex> lock(fileMutex);
        std::ofstream ofs(USERS_FILE, std::ios::app);
        if (ofs.is_open()) {
            ofs << id << " " << pwd << " " << name << "\n";
            ofs.close();
        }
    }

    void SaveAllUsers() {
        std::lock_guard<std::mutex> lock(fileMutex);
        std::ofstream ofs(USERS_FILE, std::ios::trunc);
        if (ofs.is_open()) {
            for (auto const& pair : registeredUsers) {
                ofs << pair.first << " " << pair.second.first << " " << pair.second.second << "\n";
            }
            ofs.close();
        }
    }

    void HandleClient(SOCKET clientSocket, sockaddr_in clientAddr) {
        char buffer[1024];
        bool isAuthenticated = false;
        int currentUserId = -1;
        std::string currentName = "";

        // 【阶段一：登录/注册验证循环】
        while (!isAuthenticated) {
            memset(buffer, 0, sizeof(buffer));
            int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

            if (bytesReceived <= 0) {
                closesocket(clientSocket);
                return; // 未登录前断开，不记录异常
            }

            std::string msg(buffer);
            std::vector<std::string> parts = SplitString(msg, "|");
            if (parts.empty()) continue;

            if (parts[0] == "GET_NEXT_ID") {
                std::lock_guard<std::mutex> lock(mapMutex);
                std::string reply = "NEXT_ID|" + std::to_string(nextUserId);
                send(clientSocket, reply.c_str(), reply.length(), 0);
            }
            else if (parts[0] == "REG" && parts.size() == 4) {
                int id = std::stoi(parts[1]);
                std::string pwd = parts[2];
                std::string name = parts[3];

                std::lock_guard<std::mutex> lock(mapMutex);

                if (registeredUsers.count(id) > 0) {
                    std::string reply = "REG_FAIL|手慢了！该ID已被抢注，请重新注册获取新ID。";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                    continue;
                }

                bool isNameTaken = false;
                for (auto const& pair : registeredUsers) {
                    if (pair.second.second == name) { isNameTaken = true; break; }
                }

                if (isNameTaken) {
                    std::string reply = "REG_FAIL|该昵称已被占用！请换一个。";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                }
                else {
                    registeredUsers[id] = std::make_pair(pwd, name);
                    if (id >= nextUserId) nextUserId = id + 1;
                    SaveNewUser(id, pwd, name);

                    std::string reply = "REG_OK|注册成功！";
                    send(clientSocket, reply.c_str(), reply.length(), 0);

                    // 【修复 1】：动态打印最新总注册人数
                    PrintLog("[系统] 新用户注册成功: ID=" + std::to_string(id) + "，昵称=[" + name + "]。当前系统总注册人数更新为: " + std::to_string(registeredUsers.size()));
                }
            }
            else if (parts[0] == "LOGIN" && parts.size() == 3) {
                int id = std::stoi(parts[1]);
                std::string pwd = parts[2];

                std::lock_guard<std::mutex> lock(mapMutex);
                if (registeredUsers.count(id) == 0) {
                    std::string reply = "LOGIN_FAIL|账号不存在！请先注册。";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                }
                else if (registeredUsers[id].first != pwd) {
                    std::string reply = "LOGIN_FAIL|密码错误！";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                }
                else if (clientMap.count(id) > 0) {
                    std::string reply = "LOGIN_FAIL|该账号当前已在线！请勿重复登录。";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                }
                else {
                    currentUserId = id;
                    currentName = registeredUsers[id].second;
                    isAuthenticated = true;

                    std::string reply = "LOGIN_OK|" + currentName;
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // 【阶段二：聊天大厅】
        {
            std::lock_guard<std::mutex> lock(mapMutex);
            clientMap[currentUserId] = clientSocket;
            nameMap[currentUserId] = currentName;
        }

        PrintLog("[+] [" + currentName + "] (ID:" + std::to_string(currentUserId) + ") 已登录并进入大厅！");

        std::string welcomeMsg = "[系统广播]: 热烈欢迎 [" + currentName + "] 进入聊天室！";
        {
            std::lock_guard<std::mutex> lock(mapMutex);
            for (auto const& pair : clientMap) {
                if (pair.first != currentUserId) {
                    send(pair.second, welcomeMsg.c_str(), welcomeMsg.length(), 0);
                }
            }
        }

        while (true) {
            memset(buffer, 0, sizeof(buffer));
            int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

            if (bytesReceived > 0) {
                std::string receivedStr(buffer);
                std::string actualName;
                {
                    std::lock_guard<std::mutex> lock(mapMutex);
                    if (nameMap.count(currentUserId) == 0) break; // 被踢下线防御
                    actualName = nameMap[currentUserId];
                }

                if (receivedStr.find("/nick ") == 0) {
                    std::string newName = receivedStr.substr(6);
                    std::lock_guard<std::mutex> lock(mapMutex);
                    bool isNameTaken = false;
                    for (auto const& pair : registeredUsers) {
                        if (pair.second.second == newName) { isNameTaken = true; break; }
                    }

                    if (isNameTaken) {
                        std::string errMsg = "[系统提示]: 改名失败，昵称 [" + newName + "] 已被占用！";
                        send(clientSocket, errMsg.c_str(), errMsg.length(), 0);
                    }
                    else {
                        nameMap[currentUserId] = newName;
                        registeredUsers[currentUserId].second = newName;
                        SaveAllUsers();
                        std::string ackMsg = "NICK_ACK:" + newName;
                        send(clientSocket, ackMsg.c_str(), ackMsg.length(), 0);
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        std::string noticeMsg = "[系统广播]: [" + actualName + "] 已改名为 [" + newName + "]";
                        for (auto const& pair : clientMap) {
                            send(pair.second, noticeMsg.c_str(), noticeMsg.length(), 0);
                        }
                    }
                    continue;
                }

                if (receivedStr[0] == '@') {
                    size_t spacePos = receivedStr.find(' ');
                    if (spacePos != std::string::npos) {
                        std::string targetIdentifier = receivedStr.substr(1, spacePos - 1);
                        std::string actualMsg = receivedStr.substr(spacePos + 1);
                        int targetId = -1;

                        std::lock_guard<std::mutex> lock(mapMutex);
                        try {
                            int parsedId = std::stoi(targetIdentifier);
                            if (clientMap.count(parsedId)) targetId = parsedId;
                        }
                        catch (...) {}
                        if (targetId == -1) {
                            for (const auto& pair : nameMap) {
                                if (pair.second == targetIdentifier) { targetId = pair.first; break; }
                            }
                        }

                        if (targetId != -1 && clientMap.count(targetId)) {
                            std::string forwardMsg = "[" + actualName + "] 私聊你: " + actualMsg;
                            send(clientMap[targetId], forwardMsg.c_str(), forwardMsg.length(), 0);
                            std::string successMsg = "[系统提示]: 成功发送私聊给 [" + nameMap[targetId] + "]";
                            send(clientSocket, successMsg.c_str(), successMsg.length(), 0);
                        }
                        else {
                            std::string errMsg = "[系统提示]: 找不到目标用户。";
                            send(clientSocket, errMsg.c_str(), errMsg.length(), 0);
                        }
                    }
                }
                else {
                    PrintLog("[监察-群发] [" + actualName + "]: " + receivedStr);
                    std::string broadcastMsg = "[" + actualName + "] 群发: " + receivedStr;
                    std::lock_guard<std::mutex> lock(mapMutex);
                    for (auto const& pair : clientMap) {
                        if (pair.first != currentUserId) {
                            send(pair.second, broadcastMsg.c_str(), broadcastMsg.length(), 0);
                        }
                    }
                }
            }
            // 【修复 2】：完美恢复异常断开和正常退出的识别逻辑
            else if (bytesReceived == 0) {
                std::string lastName = "未知用户";
                {
                    std::lock_guard<std::mutex> lock(mapMutex);
                    if (nameMap.count(currentUserId)) lastName = nameMap[currentUserId];
                }
                PrintLog("[-] 客户端 [" + lastName + "] (ID:" + std::to_string(currentUserId) + ") 正常断开连接。");
                break;
            }
            else {
                int errorCode = WSAGetLastError();
                std::string lastName = "未知用户";
                {
                    std::lock_guard<std::mutex> lock(mapMutex);
                    if (nameMap.count(currentUserId)) lastName = nameMap[currentUserId];
                }
                PrintLog("[!] 异常：客户端 [" + lastName + "] (ID:" + std::to_string(currentUserId) + ") 意外断开 (错误码: " + std::to_string(errorCode) + ")。");
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(mapMutex);
            clientMap.erase(currentUserId);
            nameMap.erase(currentUserId);
        }
        closesocket(clientSocket);
    }

    void ServerInputThread() {
        std::string userInput;
        while (true) {
            std::getline(std::cin, userInput);
            if (userInput.empty()) continue;

            // 【新增 3】：上帝视角，查看所有注册用户信息
            if (userInput == "/showusers") {
                std::lock_guard<std::mutex> lock(mapMutex);
                PrintLog("=========== 全服注册用户数据库 ===========");
                if (registeredUsers.empty()) {
                    PrintLog("当前数据库为空，没有任何注册用户。");
                }
                else {
                    for (const auto& pair : registeredUsers) {
                        std::string status = (clientMap.count(pair.first) > 0) ? " 【🟢 在线】" : " 【⚪ 离线】";
                        PrintLog("ID: " + std::to_string(pair.first) +
                            " | 昵称: " + pair.second.second +
                            " | 密码: " + pair.second.first +
                            status);
                    }
                }
                PrintLog("==========================================");
                PrintLog("[统计] 当前系统总注册人数为: " + std::to_string(registeredUsers.size()));
                continue;
            }
            //s

            // 超级管理员指令：清空所有数据
            if (userInput == "/clearall") {
                std::lock_guard<std::mutex> lock(mapMutex);
                std::lock_guard<std::mutex> fLock(fileMutex);

                registeredUsers.clear();
                std::ofstream ofs(USERS_FILE, std::ios::trunc);
                ofs.close();

                for (auto const& pair : clientMap) {
                    closesocket(pair.second);
                }
                clientMap.clear();
                nameMap.clear();
                nextUserId = 10000;
                // 【修复 1】：动态打印总人数更新
                PrintLog("[！！！超级权限执行成功！！！] 已清空所有账号记录并断开所有连接。当前系统总注册人数更新为: 0");
                continue;
            }

            // 超级管理员指令：删除指定用户
            if (userInput.substr(0, 5) == "/del ") {
                try {
                    int targetId = std::stoi(userInput.substr(5));
                    std::lock_guard<std::mutex> lock(mapMutex);

                    if (registeredUsers.count(targetId)) {
                        registeredUsers.erase(targetId);
                        SaveAllUsers();

                        if (clientMap.count(targetId)) {
                            closesocket(clientMap[targetId]);
                            clientMap.erase(targetId);
                            nameMap.erase(targetId);
                        }
                        // 【修复 1】：动态打印总人数更新
                        PrintLog("[超级权限] 已永久删除用户 ID: " + std::to_string(targetId) + "。当前系统总注册人数更新为: " + std::to_string(registeredUsers.size()));
                    }
                    else {
                        PrintLog("[错误] 找不到指定的 ID: " + std::to_string(targetId));
                    }
                }
                catch (...) {
                    PrintLog("[错误] 格式错误，正确格式如：/del 10001");
                }
                continue;
            }

            // 普通群发广播
            std::string fullMsg = "[服务器端超级广播]: " + userInput;
            std::lock_guard<std::mutex> lock(mapMutex);
            for (auto const& pair : clientMap) {
                send(pair.second, fullMsg.c_str(), fullMsg.length(), 0);
            }
            PrintLog("[系统广播] 已发送");
        }
    }

public:
    ChatServer(int port) : serverPort(port), listenSocket(INVALID_SOCKET) {
        logFile.open("server_chat_log.txt", std::ios::app);
        LoadUsers();
    }
    ~ChatServer() {
        if (listenSocket != INVALID_SOCKET) closesocket(listenSocket);
        WSACleanup();
    }
    bool Initialize() {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
        listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket == INVALID_SOCKET) return false;
        sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(serverPort);
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) return false;
        if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) return false;
        return true;
    }
    void Run() {
        std::cout << "=== 统一账号版聊天服务器 (高级权限管理版) 已启动 ===" << std::endl;
        std::cout << "可用管理指令：\n  /showusers (查看所有已注册用户信息)\n  /clearall (删库并踢出所有人)\n  /del <ID> (封号删档)\n" << std::endl;
        std::cout << serverPrompt;

        std::thread(&ChatServer::ServerInputThread, this).detach();

        while (true) {
            sockaddr_in clientAddr;
            int clientAddrSize = sizeof(clientAddr);
            SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &clientAddrSize);

            if (clientSocket != INVALID_SOCKET) {
                std::thread(&ChatServer::HandleClient, this, clientSocket, clientAddr).detach();
            }
        }
    }
};

int main() {
    ChatServer server(8080);
    if (server.Initialize()) server.Run();
    return 0;
}