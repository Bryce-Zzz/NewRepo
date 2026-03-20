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
#include <random>     
#include <set>        // 新增：用于存放正在修改密码的客户端集合
#include <list>       // 新增：用于排队队列
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sw/redis++/redis++.h>

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

    std::map<int, SOCKET> pendingResetSockets;
    std::mutex pendingMutex;

    // --- 【新增】：限流与排队机制 ---
    std::set<SOCKET> resettingSockets;                 // 当前正在占用名额的 Socket (上限3个)
    std::list<std::pair<int, SOCKET>> resetWaitQueue;  // 等待队列
    std::mutex resetMutex;                             // 保护限流队列的锁

    std::set<int> activeResetIds;               // 新增：记录正在被修改密码的账号ID (防止同ID并发)
    std::map<SOCKET, int> resettingIdsMap;      // 新增：记录 Socket 对应正在修改哪个账号

    // 释放修改密码名额，并自动叫号下一个
    void ReleaseResetSlot(SOCKET sock) {
        std::lock_guard<std::mutex> lock(resetMutex);

        // 【新增】：清理正在修改的 ID 记录
        if (resettingIdsMap.count(sock)) {
            activeResetIds.erase(resettingIdsMap[sock]);
            resettingIdsMap.erase(sock);
        }

        // 如果这个 Socket 在排队，直接移除
        resetWaitQueue.remove_if([sock](const std::pair<int, SOCKET>& p) { return p.second == sock; });

        // 如果这个 Socket 占用了名额，释放它
        if (resettingSockets.erase(sock)) {
            // 名额释放了，看看有没有人排队，叫号！
            if (!resetWaitQueue.empty()) {
                auto pending = resetWaitQueue.front();
                resetWaitQueue.pop_front();

                resettingSockets.insert(pending.second);
                {
                    std::lock_guard<std::mutex> pLock(pendingMutex);
                    pendingResetSockets[pending.first] = pending.second;
                }

                std::string reply = "WAIT_OK|已为您分配到重置名额！";
                send(pending.second, reply.c_str(), reply.length(), 0);
                PrintLog("[权限审批] 客户端排队完成，请求重置账号 " + std::to_string(pending.first) + " 的密码。同意请在控制台输入: /yes " + std::to_string(pending.first));
            }
        }
    }

    sw::redis::Redis redis;

    void Redis_SetEx(int id, const std::string& code, int expireSeconds) {
        try {
            std::string key = "reset_code_" + std::to_string(id);
            redis.setex(key, expireSeconds, code);
        }
        catch (const sw::redis::Error& e) {
            PrintLog("[Redis 错误] 存入验证码失败: " + std::string(e.what()));
        }
    }

    bool Redis_VerifyAndDel(int id, const std::string& inputCode) {
        try {
            std::string key = "reset_code_" + std::to_string(id);
            auto val = redis.get(key);
            if (val && *val == inputCode) {
                redis.del(key);
                return true;
            }
        }
        catch (const sw::redis::Error& e) {
            PrintLog("[Redis 错误] 校验验证码失败: " + std::string(e.what()));
        }
        return false;
    }

    void PrintLog(const std::string& logMsg) {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        struct tm tm_info;
        localtime_s(&tm_info, &t);

        std::lock_guard<std::mutex> lock(consoleMutex);
        // 【优化 2】：去掉了开头的 \n，输出更加紧凑美观
        std::cout << logMsg << "\n" << serverPrompt;

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
            if (id >= nextUserId) nextUserId = id + 1;
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

        while (!isAuthenticated) {
            memset(buffer, 0, sizeof(buffer));
            int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

            if (bytesReceived <= 0) {
                ReleaseResetSlot(clientSocket); // 异常断开时释放排队名额
                closesocket(clientSocket);
                return;
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
                    std::string reply = "REG_FAIL|手慢了！该ID已被抢注。";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                    continue;
                }
                bool isNameTaken = false;
                for (auto const& pair : registeredUsers) {
                    if (pair.second.second == name) { isNameTaken = true; break; }
                }
                if (isNameTaken) {
                    std::string reply = "REG_FAIL|该昵称已被占用！";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                }
                else {
                    registeredUsers[id] = std::make_pair(pwd, name);
                    if (id >= nextUserId) nextUserId = id + 1;
                    SaveNewUser(id, pwd, name);
                    std::string reply = "REG_OK|注册成功！";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                    PrintLog("[系统] 新用户注册成功: ID=" + std::to_string(id) + "，昵称=[" + name + "]");
                }
            }
            else if (parts[0] == "LOGIN" && parts.size() == 3) {
                int id = std::stoi(parts[1]);
                std::string pwd = parts[2];
                std::lock_guard<std::mutex> lock(mapMutex);
                if (registeredUsers.count(id) == 0) {
                    std::string reply = "LOGIN_FAIL|账号不存在！";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                }
                else if (registeredUsers[id].first != pwd) {
                    std::string reply = "LOGIN_FAIL|密码错误！";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                }
                else if (clientMap.count(id) > 0) {
                    std::string reply = "LOGIN_FAIL|该账号当前已在线！";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                }
                else {
                    currentUserId = id; currentName = registeredUsers[id].second; isAuthenticated = true;
                    std::string reply = "LOGIN_OK|" + currentName;
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                }
            }
            else if (parts[0] == "FORGOT_PWD" && parts.size() == 2) {
                int targetId = std::stoi(parts[1]);
                std::lock_guard<std::mutex> lock(mapMutex);
                if (registeredUsers.count(targetId) == 0) {
                    std::string reply = "FORGOT_FAIL|账号不存在，请检查ID！";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                }
                else {
                    std::lock_guard<std::mutex> rLock(resetMutex);
                    // 【新增核心逻辑 1】：检查是否已有其他人在修改这个 ID
                    if (activeResetIds.count(targetId)) {
                        std::string reply = "FORGOT_FAIL|该账号的密码正在被其他客户端修改，禁止并发操作！";
                        send(clientSocket, reply.c_str(), reply.length(), 0);
                    }
                    else {
                        // 登记这个 ID 和 Socket
                        activeResetIds.insert(targetId);
                        resettingIdsMap[clientSocket] = targetId;

                        if (resettingSockets.size() >= 3) {
                            std::string reply = "FORGOT_BUSY|当前修改密码服务繁忙 (已达上限3人)，是否排队等待？";
                            send(clientSocket, reply.c_str(), reply.length(), 0);
                        }
                        else {
                            resettingSockets.insert(clientSocket);
                            {
                                std::lock_guard<std::mutex> pLock(pendingMutex);
                                pendingResetSockets[targetId] = clientSocket;
                            }
                            PrintLog("[权限审批] 客户端请求重置账号 " + parts[1] + " 的密码。同意请在控制台输入: /yes " + parts[1]);
                        }
                    }
                }
            }
            // 客户端确认等待
            else if (parts[0] == "FORGOT_WAIT" && parts.size() == 2) {
                int targetId = std::stoi(parts[1]);
                std::lock_guard<std::mutex> rLock(resetMutex);
                if (resettingSockets.size() < 3) {
                    // 运气好，刚决定排队就有人释放了名额
                    resettingSockets.insert(clientSocket);
                    {
                        std::lock_guard<std::mutex> pLock(pendingMutex);
                        pendingResetSockets[targetId] = clientSocket;
                    }
                    std::string reply = "WAIT_OK|刚好有名额释放，已为您分配！";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                    PrintLog("[权限审批] 客户端请求重置账号 " + std::to_string(targetId) + " 的密码。同意请在控制台输入: /yes " + std::to_string(targetId));
                }
                else {
                    resetWaitQueue.push_back({ targetId, clientSocket });
                    std::string reply = "WAITING|";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                }
            }
            // 客户端取消等待
            else if (parts[0] == "FORGOT_CANCEL") {
                ReleaseResetSlot(clientSocket);
            }
            else if (parts[0] == "RESET_PWD" && parts.size() == 4) {
                int targetId = std::stoi(parts[1]);
                std::string inputCode = parts[2];
                std::string newPwd = parts[3];

                if (Redis_VerifyAndDel(targetId, inputCode)) {
                    std::lock_guard<std::mutex> lock(mapMutex);
                    registeredUsers[targetId].first = newPwd;
                    SaveAllUsers();
                    std::string reply = "RESET_OK|密码修改成功，请重新登录！";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                    PrintLog("[系统] 账号 " + std::to_string(targetId) + " 密码已成功重置。");
                }
                else {
                    std::string reply = "RESET_FAIL|验证码错误或已过期(有效时长5分钟)！";
                    send(clientSocket, reply.c_str(), reply.length(), 0);
                    PrintLog("[系统警告] 账号 " + std::to_string(targetId) + " 尝试使用错误/过期的验证码重置密码。");
                }
                // 处理完无论成功失败，都释放名额给别人
                ReleaseResetSlot(clientSocket);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));

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
                    if (nameMap.count(currentUserId) == 0) break;
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
                        PrintLog("[监察-改名] " + noticeMsg);
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
                            PrintLog("[监察-私聊] [" + actualName + "] -> [" + nameMap[targetId] + "]: " + actualMsg);
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
            // 【恢复】：准确识别正常断开 (quit)
            else if (bytesReceived == 0) {
                std::string lastName = "未知用户";
                {
                    std::lock_guard<std::mutex> lock(mapMutex);
                    if (nameMap.count(currentUserId)) lastName = nameMap[currentUserId];
                }
                PrintLog("[-] 客户端 [" + lastName + "] (ID:" + std::to_string(currentUserId) + ") 正常断开连接。");
                break;
            }
            // 【恢复】：准确识别异常断开 (直接点X关闭黑框)
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

        // --- 收尾清理 ---
        ReleaseResetSlot(clientSocket);
        {
            std::lock_guard<std::mutex> pLock(pendingMutex);
            for (auto it = pendingResetSockets.begin(); it != pendingResetSockets.end(); ) {
                if (it->second == clientSocket) it = pendingResetSockets.erase(it);
                else ++it;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mapMutex);
            if (currentUserId != -1) {
                clientMap.erase(currentUserId);
                nameMap.erase(currentUserId);
            }
        }
        closesocket(clientSocket);
    }

    void ServerInputThread() {
        srand(static_cast<unsigned int>(time(nullptr)));
        std::string userInput;

        while (true) {
            std::getline(std::cin, userInput);
            if (userInput.empty()) {
                std::lock_guard<std::mutex> lock(consoleMutex);
                std::cout << serverPrompt;
                continue;
            }

            if (userInput.substr(0, 5) == "/yes ") {
                try {
                    int targetId = std::stoi(userInput.substr(5));
                    SOCKET targetSock = INVALID_SOCKET; // 暂存 socket
                    {
                        std::lock_guard<std::mutex> pLock(pendingMutex);
                        if (pendingResetSockets.count(targetId)) {
                            targetSock = pendingResetSockets[targetId];
                            int randomNum = 100000 + rand() % 900000;
                            std::string code = std::to_string(randomNum);

                            Redis_SetEx(targetId, code, 60); // 【修改】：验证码在 Redis 中改为 60 秒过期

                            std::string reply = "FORGOT_OK|" + code;
                            send(targetSock, reply.c_str(), reply.length(), 0);
                            pendingResetSockets.erase(targetId);

                            PrintLog("[系统] 已同意请求。验证码 【" + code + "】 已存入Redis并下发 (60秒有效)。");
                        }
                        else {
                            PrintLog("[错误] 找不到该账号的待审批请求。");
                        }
                    }

                    // 【新增核心逻辑 2】：下发验证码后，启动一个 60 秒的定时炸弹
                    if (targetSock != INVALID_SOCKET) {
                        std::thread([this, targetSock, targetId]() {
                            std::this_thread::sleep_for(std::chrono::seconds(60)); // 倒计时一分钟

                            bool isStillResetting = false;
                            {
                                std::lock_guard<std::mutex> rLock(resetMutex);
                                // 如果 60 秒后它还在修改密码状态（没提交完成、没点取消），则判定为超时
                                if (resettingIdsMap.count(targetSock) && resettingIdsMap[targetSock] == targetId) {
                                    isStillResetting = true;
                                }
                            }
                            if (isStillResetting) {
                                PrintLog("[系统监控] 账号 " + std::to_string(targetId) + " 验证码输入超时 (超1分钟)，已取消重置并释放名额！");
                                // 下发超时信号
                                std::string timeoutMsg = "RESET_TIMEOUT|";
                                send(targetSock, timeoutMsg.c_str(), timeoutMsg.length(), 0);
                                // 优雅地释放名额，不断开 Socket
                                ReleaseResetSlot(targetSock);
                            }
                            }).detach();
                    }
                }
                catch (...) {
                    PrintLog("[错误] 格式错误，请使用: /yes 10001");
                }
                continue;
            }

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
                continue;
            }

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
                PrintLog("[！！！超级权限执行成功！！！] 已清空所有账号记录并断开所有连接。");
                continue;
            }

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
                        PrintLog("[超级权限] 已永久删除用户 ID: " + std::to_string(targetId));
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

            if (userInput[0] == '@') {
                size_t spacePos = userInput.find(' ');
                if (spacePos != std::string::npos) {
                    std::string targetIdentifier = userInput.substr(1, spacePos - 1);
                    std::string msg = userInput.substr(spacePos + 1);
                    std::string fullMsg = "[服务器端] 私聊你: " + msg;

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
                        send(clientMap[targetId], fullMsg.c_str(), fullMsg.length(), 0);
                        PrintLog("[系统] 已成功发送私聊给 [" + nameMap[targetId] + "]");
                    }
                    else {
                        PrintLog("[系统] 发送失败，找不到目标 [" + targetIdentifier + "]");
                    }
                }
                continue;
            }

            std::string fullMsg = "[服务器端] 群发: " + userInput;
            std::lock_guard<std::mutex> lock(mapMutex);
            for (auto const& pair : clientMap) {
                send(pair.second, fullMsg.c_str(), fullMsg.length(), 0);
            }
            PrintLog("[系统广播] 已发送");
        }
    }

public:
    ChatServer(int port)
        : serverPort(port), listenSocket(INVALID_SOCKET), redis("tcp://127.0.0.1:6379")
    {
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
        std::cout << "=== 聊天服务器 (加入排队降级架构) 已启动 ===" << std::endl;
        std::cout << "可用指令：\n  /yes <ID> (审批通过并生成验证码)\n  /showusers (查看所有已注册用户)\n  /clearall (删库并踢出所有人)\n  /del <ID> (封号删档)\n  @ID或昵称 (服务端单独私聊)\n" << std::endl;
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