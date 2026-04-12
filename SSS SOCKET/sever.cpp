// =========================================================================
// 【跨平台兼容层】：让代码同时在 Windows 和 Linux (Debian) 下完美编译
// =========================================================================
#ifdef _WIN32
    // Windows 环境下的依赖
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t; // Windows 下 accept 第三个参数用 int
#define GET_SOCKET_ERROR() WSAGetLastError()
#else
    // Linux/Debian 环境下的依赖
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>
#include <cerrno>      // 用于获取 Linux 错误码 (errno)
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket(s) close(s) // 将 Windows 的 closesocket 映射为 Linux 的 close
#define GET_SOCKET_ERROR() errno
#endif
// =========================================================================

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
#include <set>        
#include <list>       
#include <cstdint>    // 用于 uint16_t
#include <sw/redis++/redis++.h>

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

    std::set<SOCKET> resettingSockets;
    std::list<std::pair<int, SOCKET>> resetWaitQueue;
    std::mutex resetMutex;

    std::set<int> activeResetIds;
    std::map<SOCKET, int> resettingIdsMap;

    // ================== 【核心网络收发引擎】 ==================
    bool SendPacket(SOCKET sock, const std::string& msg) {
        if (msg.empty()) return true;
        uint16_t net_len = htons(static_cast<uint16_t>(msg.length()));
        std::string packet;
        packet.append(reinterpret_cast<char*>(&net_len), 2);
        packet.append(msg);

        int totalSent = 0;
        int packetLen = packet.length();
        while (totalSent < packetLen) {
            int sent = send(sock, packet.c_str() + totalSent, packetLen - totalSent, 0);
            if (sent <= 0) return false;
            totalSent += sent;
        }
        return true;
    }

    int RecvExactly(SOCKET sock, char* buf, int len) {
        int totalRecv = 0;
        while (totalRecv < len) {
            int r = recv(sock, buf + totalRecv, len - totalRecv, 0);
            if (r == 0) return 0;
            if (r < 0) return -1;
            totalRecv += r;
        }
        return 1;
    }

    int RecvPacket(SOCKET sock, std::string& msg) {
        uint16_t net_len = 0;
        int r = RecvExactly(sock, reinterpret_cast<char*>(&net_len), 2);
        if (r <= 0) return r;

        uint16_t host_len = ntohs(net_len);
        if (host_len == 0) {
            msg = "";
            return 1;
        }

        std::vector<char> buffer(host_len);
        r = RecvExactly(sock, buffer.data(), host_len);
        if (r <= 0) return r;

        msg = std::string(buffer.data(), host_len);
        return 1;
    }
    // ================================================================

    void ReleaseResetSlot(SOCKET sock) {
        std::lock_guard<std::mutex> lock(resetMutex);

        if (resettingIdsMap.count(sock)) {
            activeResetIds.erase(resettingIdsMap[sock]);
            resettingIdsMap.erase(sock);
        }

        resetWaitQueue.remove_if([sock](const std::pair<int, SOCKET>& p) { return p.second == sock; });

        if (resettingSockets.erase(sock)) {
            if (!resetWaitQueue.empty()) {
                auto pending = resetWaitQueue.front();
                resetWaitQueue.pop_front();

                resettingSockets.insert(pending.second);
                {
                    std::lock_guard<std::mutex> pLock(pendingMutex);
                    pendingResetSockets[pending.first] = pending.second;
                }

                std::string reply = "WAIT_OK|已为您分配到重置名额！";
                SendPacket(pending.second, reply);
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

        // 【跨平台修复】：Windows 和 Linux 获取本地时间的安全函数参数顺序是相反的
#ifdef _WIN32
        localtime_s(&tm_info, &t);
#else
        localtime_r(&t, &tm_info);
#endif

        std::lock_guard<std::mutex> lock(consoleMutex);
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
        PrintLog("[Aurora系统启动] 成功加载历史注册用户数量: " + std::to_string(registeredUsers.size()));
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
        bool isAuthenticated = false;
        int currentUserId = -1;
        std::string currentName = "";

        while (!isAuthenticated) {
            std::string msg;
            int r = RecvPacket(clientSocket, msg);

            if (r <= 0) {
                ReleaseResetSlot(clientSocket);
                closesocket(clientSocket);
                return;
            }

            std::vector<std::string> parts = SplitString(msg, "|");
            if (parts.empty()) continue;

            if (parts[0] == "GET_NEXT_ID") {
                std::lock_guard<std::mutex> lock(mapMutex);
                std::string reply = "NEXT_ID|" + std::to_string(nextUserId);
                SendPacket(clientSocket, reply);
            }
            else if (parts[0] == "REG" && parts.size() == 4) {
                int id = std::stoi(parts[1]);
                std::string pwd = parts[2];
                std::string name = parts[3];
                std::lock_guard<std::mutex> lock(mapMutex);
                if (registeredUsers.count(id) > 0) {
                    SendPacket(clientSocket, "REG_FAIL|手慢了！该ID已被抢注。");
                    continue;
                }
                bool isNameTaken = false;
                for (auto const& pair : registeredUsers) {
                    if (pair.second.second == name) { isNameTaken = true; break; }
                }
                if (isNameTaken) {
                    SendPacket(clientSocket, "REG_FAIL|该昵称已被占用！");
                }
                else {
                    registeredUsers[id] = std::make_pair(pwd, name);
                    if (id >= nextUserId) nextUserId = id + 1;
                    SaveNewUser(id, pwd, name);
                    SendPacket(clientSocket, "REG_OK|注册成功！");
                    PrintLog("[系统] 新用户注册成功: ID=" + std::to_string(id) + "，昵称=[" + name + "]");
                }
            }
            else if (parts[0] == "LOGIN" && parts.size() == 3) {
                int id = std::stoi(parts[1]);
                std::string pwd = parts[2];
                std::lock_guard<std::mutex> lock(mapMutex);
                if (registeredUsers.count(id) == 0) {
                    SendPacket(clientSocket, "LOGIN_FAIL|账号不存在！");
                }
                else if (registeredUsers[id].first != pwd) {
                    SendPacket(clientSocket, "LOGIN_FAIL|密码错误！");
                }
                else if (clientMap.count(id) > 0) {
                    SendPacket(clientSocket, "LOGIN_FAIL|该账号当前已在线！");
                }
                else {
                    currentUserId = id; currentName = registeredUsers[id].second; isAuthenticated = true;
                    SendPacket(clientSocket, "LOGIN_OK|" + currentName);
                }
            }
            else if (parts[0] == "FORGOT_PWD" && parts.size() == 2) {
                int targetId = std::stoi(parts[1]);
                std::lock_guard<std::mutex> lock(mapMutex);
                if (registeredUsers.count(targetId) == 0) {
                    SendPacket(clientSocket, "FORGOT_FAIL|账号不存在，请检查ID！");
                }
                else {
                    std::lock_guard<std::mutex> rLock(resetMutex);
                    if (activeResetIds.count(targetId)) {
                        SendPacket(clientSocket, "FORGOT_FAIL|该账号的密码正在被修改，禁止并发！");
                    }
                    else {
                        activeResetIds.insert(targetId);
                        resettingIdsMap[clientSocket] = targetId;
                        if (resettingSockets.size() >= 3) {
                            SendPacket(clientSocket, "FORGOT_BUSY|当前修改密码服务繁忙(已达3人)，是否排队等待？");
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
            else if (parts[0] == "FORGOT_WAIT" && parts.size() == 2) {
                int targetId = std::stoi(parts[1]);
                std::lock_guard<std::mutex> rLock(resetMutex);
                if (resettingSockets.size() < 3) {
                    resettingSockets.insert(clientSocket);
                    {
                        std::lock_guard<std::mutex> pLock(pendingMutex);
                        pendingResetSockets[targetId] = clientSocket;
                    }
                    SendPacket(clientSocket, "WAIT_OK|刚好有名额释放，已为您分配！");
                    PrintLog("[权限审批] 客户端请求重置账号 " + std::to_string(targetId) + " 的密码。同意请在控制台输入: /yes " + std::to_string(targetId));
                }
                else {
                    resetWaitQueue.push_back({ targetId, clientSocket });
                    SendPacket(clientSocket, "WAITING|");
                }
            }
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
                    SendPacket(clientSocket, "RESET_OK|密码修改成功，请重新登录！");
                    PrintLog("[系统] 账号 " + std::to_string(targetId) + " 密码已成功重置。");
                }
                else {
                    SendPacket(clientSocket, "RESET_FAIL|验证码错误或已过期(有效时长1分钟)！");
                    PrintLog("[系统警告] 账号 " + std::to_string(targetId) + " 尝试使用错误/过期的验证码重置密码。");
                }
                ReleaseResetSlot(clientSocket);
            }
        }

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
                    SendPacket(pair.second, welcomeMsg);
                }
            }
        }

        while (true) {
            std::string receivedStr;
            int r = RecvPacket(clientSocket, receivedStr);

            if (r == 1) {
                std::string actualName;
                {
                    std::lock_guard<std::mutex> lock(mapMutex);
                    if (nameMap.count(currentUserId) == 0) break;
                    actualName = nameMap[currentUserId];
                }

                if (receivedStr.find("FILE_REQ|") == 0 ||
                    receivedStr.find("FILE_CHUNK|") == 0 ||
                    receivedStr.find("FILE_EOF|") == 0 ||
                    receivedStr.find("FILE_ABORT|") == 0) {

                    std::vector<std::string> fileParts = SplitString(receivedStr, "|");
                    if (fileParts.size() >= 2) {
                        int targetId = -1;
                        try { targetId = std::stoi(fileParts[1]); }
                        catch (...) {}

                        std::lock_guard<std::mutex> lock(mapMutex);
                        if (clientMap.count(targetId)) {
                            SendPacket(clientMap[targetId], receivedStr);
                        }
                        else {
                            if (fileParts[0] == "FILE_REQ") {
                                SendPacket(clientSocket, "[系统提示]: 发送失败，目标用户不在线或不存在！");
                            }
                            else if (fileParts[0] == "FILE_CHUNK") {
                                SendPacket(clientSocket, "FILE_OFFLINE|" + fileParts[3]);
                            }
                        }
                    }
                    continue;
                }

                if (receivedStr.find("/nick ") == 0) {
                    std::string newName = receivedStr.substr(6);
                    std::lock_guard<std::mutex> lock(mapMutex);
                    bool isNameTaken = false;
                    for (auto const& pair : registeredUsers) {
                        if (pair.second.second == newName) { isNameTaken = true; break; }
                    }

                    if (isNameTaken) {
                        SendPacket(clientSocket, "[系统提示]: 改名失败，昵称 [" + newName + "] 已被占用！");
                    }
                    else {
                        nameMap[currentUserId] = newName;
                        registeredUsers[currentUserId].second = newName;
                        SaveAllUsers();

                        SendPacket(clientSocket, "NICK_ACK:" + newName);

                        std::string noticeMsg = "[系统广播]: [" + actualName + "] 已改名为 [" + newName + "]";
                        PrintLog("[监察-改名] " + noticeMsg);
                        for (auto const& pair : clientMap) {
                            SendPacket(pair.second, noticeMsg);
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
                            SendPacket(clientMap[targetId], forwardMsg);
                            SendPacket(clientSocket, "[系统提示]: 成功发送私聊给 [" + nameMap[targetId] + "]");
                            PrintLog("[监察-私聊] [" + actualName + "] -> [" + nameMap[targetId] + "]: " + actualMsg);
                        }
                        else {
                            SendPacket(clientSocket, "[系统提示]: 找不到目标用户。");
                        }
                    }
                }
                else {
                    PrintLog("[监察-群发] [" + actualName + "]: " + receivedStr);
                    std::string broadcastMsg = "[" + actualName + "] 群发: " + receivedStr;
                    std::lock_guard<std::mutex> lock(mapMutex);
                    for (auto const& pair : clientMap) {
                        if (pair.first != currentUserId) {
                            SendPacket(pair.second, broadcastMsg);
                        }
                    }
                }
            }
            else if (r == 0) {
                std::string lastName = "未知用户";
                {
                    std::lock_guard<std::mutex> lock(mapMutex);
                    if (nameMap.count(currentUserId)) lastName = nameMap[currentUserId];
                }
                PrintLog("[-] 客户端 [" + lastName + "] (ID:" + std::to_string(currentUserId) + ") 正常断开连接。");

                // 【下线通知追加】：为了防止幽灵文件，需要通知全服有人断开了！
                std::string offlineMsg = "OFFLINE:" + std::to_string(currentUserId);
                std::lock_guard<std::mutex> lock(mapMutex);
                for (auto const& pair : clientMap) {
                    if (pair.first != currentUserId) {
                        SendPacket(pair.second, offlineMsg);
                    }
                }
                break;
            }
            else {
                int errorCode = GET_SOCKET_ERROR(); // 【跨平台修复】
                std::string lastName = "未知用户";
                {
                    std::lock_guard<std::mutex> lock(mapMutex);
                    if (nameMap.count(currentUserId)) lastName = nameMap[currentUserId];
                }
                PrintLog("[!] 异常：客户端 [" + lastName + "] (ID:" + std::to_string(currentUserId) + ") 意外断开 (错误码: " + std::to_string(errorCode) + ")。");

                // 【下线通知追加】
                std::string offlineMsg = "OFFLINE:" + std::to_string(currentUserId);
                std::lock_guard<std::mutex> lock(mapMutex);
                for (auto const& pair : clientMap) {
                    if (pair.first != currentUserId) {
                        SendPacket(pair.second, offlineMsg);
                    }
                }
                break;
            }
        }

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
                    SOCKET targetSock = INVALID_SOCKET;
                    {
                        std::lock_guard<std::mutex> pLock(pendingMutex);
                        if (pendingResetSockets.count(targetId)) {
                            targetSock = pendingResetSockets[targetId];
                            int randomNum = 100000 + rand() % 900000;
                            std::string code = std::to_string(randomNum);

                            Redis_SetEx(targetId, code, 60);

                            SendPacket(targetSock, "FORGOT_OK|" + code);
                            pendingResetSockets.erase(targetId);

                            PrintLog("[系统] 已同意请求。验证码 【" + code + "】 已存入Redis并下发 (60秒有效)。");
                        }
                        else {
                            PrintLog("[错误] 找不到该账号的待审批请求。");
                        }
                    }

                    if (targetSock != INVALID_SOCKET) {
                        std::thread([this, targetSock, targetId]() {
                            std::this_thread::sleep_for(std::chrono::seconds(60));

                            bool isStillResetting = false;
                            {
                                std::lock_guard<std::mutex> rLock(resetMutex);
                                if (resettingIdsMap.count(targetSock) && resettingIdsMap[targetSock] == targetId) {
                                    isStillResetting = true;
                                }
                            }
                            if (isStillResetting) {
                                PrintLog("[系统监控] 账号 " + std::to_string(targetId) + " 验证码输入超时 (超1分钟)，已取消重置并释放名额！");
                                SendPacket(targetSock, "RESET_TIMEOUT|");
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
                        SendPacket(clientMap[targetId], fullMsg);
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
                SendPacket(pair.second, fullMsg);
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
        // 【跨平台修复】：Linux 没有 WSA 清理函数
#ifdef _WIN32
        WSACleanup();
#endif
    }
    bool Initialize() {
        // 【跨平台修复】：Linux 没有 WSA 初始化
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
#endif
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
        std::cout << "=== 聊天服务器已启动 (全平台兼容版) ===" << std::endl;
        std::cout << "可用指令：\n  /yes <ID> (审批通过并生成验证码)\n  /showusers (查看所有已注册用户)\n  /clearall (删库并踢出所有人)\n  /del <ID> (封号删档)\n  @ID或昵称 (服务端单独私聊)\n" << std::endl;
        std::cout << serverPrompt;

        std::thread(&ChatServer::ServerInputThread, this).detach();

        while (true) {
            sockaddr_in clientAddr;
            // 【跨平台修复】：Linux 的 accept 函数第三个参数需要是指针类型 socklen_t*
            socklen_t clientAddrSize = sizeof(clientAddr);
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