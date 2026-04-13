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
#include <cstdint>  // 用于 uint16_t
#include <fstream>   // 用于读写文件
#include <cmath>     // 用于数学计算
#include <mutex>
#include <atomic>  // 用于多线程安全的标志位
#include <cstdio>  // 用于 std::remove 删除文件
#include <map>       // 【新增】：用于多并发接收的哈希表

// ================== 【终极进化：Windows UUID 运行环境】 ==================
#include <rpc.h>         // 引入 Windows 原生 RPC API
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Rpcrt4.lib") // 【核心链接】：链接 RPC 运行时库，用于生成 UUID
// =========================================================================


// ================== 【终极优雅：单例指针挂载】 ==================
class ChatClient; // 提前声明类

ChatClient* g_appInstance = nullptr; // 唯一合法的全局指针（通往保险柜的钥匙）

// 这里只做“函数声明”，告诉系统有这个函数，但身体放在后面！
BOOL WINAPI ConsoleCtrlHandler(DWORD signal);
// ================================================================

class ChatClient {
private:
    std::string serverIp;
    int serverPort;
    SOCKET clientSocket;
    std::string currentPrompt;
    std::string myId;
    std::string myName;
    bool isConnected;
    
    std::mutex sendMutex; // 新增：保护发送通道的互斥锁

    // ================== 【新增：多并发接收引擎核心】 ==================
    std::map<std::string, std::string> receivingTasks; // Key: 发送方ID, Value: 本地文件名
    std::mutex taskMutex; // 保护哈希表的多线程锁

    // ================== 【终极进化：并发发送引擎状态】 ==================
      // 为什么用 shared_ptr？因为 std::atomic 是不可复制的，直接放进 map 会报错！
    struct SendTask {
        std::string targetId;
        std::string fileName;
        std::string globalUuid; // 【新增】：隐藏在背后的长 UUID，专供网络传输
        std::shared_ptr<std::atomic<bool>> isCancelling;

        SendTask() : isCancelling(std::make_shared<std::atomic<bool>>(false)) {}
    };

    std::map<int, SendTask> sendingTasks; // 记录自己发出去的所有任务 (Key: 本地任务ID)
    std::mutex sendTaskMutex;                     // 保护发送字典的锁
    std::atomic<int> localTaskIdCounter{ 1 };     // 恢复本地短号发号器
    
    // ================== 【新增：Windows 原生 UUID 生成器】 ==================
    // 生成格式如: "550e8400-e29b-41d4-a716-446655440000" 的全球唯一字符串
    std::string GenerateStringUUID() {
        UUID uuid;
        // 1. 调用 Windows API 创建 UUID
        if (UuidCreate(&uuid) != RPC_S_OK) return "uuid_error"; // 极其罕见的错误

        unsigned char* uuidStrRaw = nullptr;
        // 2. 将二进制 UUID 转换为字符串
        if (UuidToStringA(&uuid, &uuidStrRaw) != RPC_S_OK) return "uuid_string_error";

        std::string finalUuid(reinterpret_cast<char*>(uuidStrRaw));

        // 3. 必须！调用 Windows API 释放分配的字符串内存，防止内存泄漏！
        RpcStringFreeA(&uuidStrRaw);
        return finalUuid;
    }
    // =========================================================================

    // ================== 【新增：Windows 路径翻译官】 ==================
    // 专门把 UTF-8 的路径，翻译成 Windows 硬盘能看懂的 UTF-16 (宽字符)
    std::wstring Utf8ToWstring(const std::string& utf8Str) {
        if (utf8Str.empty()) return L"";
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, &utf8Str[0], (int)utf8Str.size(), NULL, 0);
        std::wstring wstrTo(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, &utf8Str[0], (int)utf8Str.size(), &wstrTo[0], size_needed);
        return wstrTo;
    }
    // =================================================================

    // ================== 【新增：核心网络收发引擎】 ==================
    bool SendPacket(SOCKET sock, const std::string& msg) {
        if (msg.empty()) return true;
        uint16_t net_len = htons(static_cast<uint16_t>(msg.length()));
        std::string packet;
        packet.append(reinterpret_cast<char*>(&net_len), 2);
        packet.append(msg);

        // 【核心修改】：加上发送锁，保证多线程并发发送时的绝对安全！
        std::lock_guard<std::mutex> lock(sendMutex);

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

    // ================== 【新增：Base64 编解码引擎】 ==================
    const std::string base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string Base64Encode(const unsigned char* bytes_to_encode, unsigned int in_len) {
        std::string ret;
        int i = 0;
        int j = 0;
        unsigned char char_array_3[3];
        unsigned char char_array_4[4];

        while (in_len--) {
            char_array_3[i++] = *(bytes_to_encode++);
            if (i == 3) {
                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                char_array_4[3] = char_array_3[2] & 0x3f;
                for (i = 0; (i < 4); i++) ret += base64_chars[char_array_4[i]];
                i = 0;
            }
        }
        if (i) {
            for (j = i; j < 3; j++) char_array_3[j] = '\0';
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;
            for (j = 0; (j < i + 1); j++) ret += base64_chars[char_array_4[j]];
            while ((i++ < 3)) ret += '=';
        }
        return ret;
    }

    std::string Base64Decode(std::string const& encoded_string) {
        int in_len = encoded_string.size();
        int i = 0, j = 0, in_ = 0;
        unsigned char char_array_4[4], char_array_3[3];
        std::string ret;

        auto is_base64 = [](unsigned char c) { return (isalnum(c) || (c == '+') || (c == '/')); };

        while (in_len-- && (encoded_string[in_] != '=') && is_base64(encoded_string[in_])) {
            char_array_4[i++] = encoded_string[in_]; in_++;
            if (i == 4) {
                for (i = 0; i < 4; i++) char_array_4[i] = base64_chars.find(char_array_4[i]);
                char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
                char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
                char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
                for (i = 0; (i < 3); i++) ret += char_array_3[i];
                i = 0;
            }
        }
        if (i) {
            for (j = i; j < 4; j++) char_array_4[j] = 0;
            for (j = 0; j < 4; j++) char_array_4[j] = base64_chars.find(char_array_4[j]);
            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
            for (j = 0; (j < i - 1); j++) ret += char_array_3[j];
        }
        return ret;
    }
    // ================================================================

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
        while (isConnected) {
            std::string msg;
            int r = RecvPacket(clientSocket, msg); // 用包引擎替换原生 recv

            if (r == 1) {
                // ================== 【终极进化：基于联合 Key 的并发哈希接收引擎】 ==================
                if (msg.find("FILE_REQ|") == 0) {
                    auto parts = SplitString(msg, "|");
                    if (parts.size() >= 6) {
                        std::string senderId = parts[2];
                        std::string taskId = parts[3];
                        std::string fName = parts[4];
                        std::string fSize = parts[5];

                        // 【全球唯一 Key】
                        std::string globalKey = senderId + "_" + taskId;

                        std::lock_guard<std::mutex> lock(taskMutex);

                        // 物理文件命名也带上这个神圣的防爆盾
                        std::string safeFileName = "recv_" + globalKey + "_" + fName;
                        receivingTasks[globalKey] = safeFileName;

                        std::cout << "\n[文件传输] 叮！收到来自 [" << senderId << "] 的新任务 #" << taskId << ": " << fName << "\n" << currentPrompt;

                        std::ofstream ofs(safeFileName, std::ios::binary | std::ios::trunc);
                        ofs.close();
                    }
                    continue;
                }
                else if (msg.find("FILE_CHUNK|") == 0) {
                    auto parts = SplitString(msg, "|");
                    if (parts.size() >= 6) {
                        std::string globalKey = parts[2] + "_" + parts[3];
                        std::lock_guard<std::mutex> lock(taskMutex);

                        if (receivingTasks.count(globalKey)) {
                            std::string decodedData = Base64Decode(parts[5]);
                            std::ofstream ofs(receivingTasks[globalKey], std::ios::binary | std::ios::app);
                            if (ofs.is_open()) {
                                ofs.write(decodedData.data(), decodedData.size());
                                ofs.close();
                            }
                        }
                    }
                    continue;
                }
                else if (msg.find("FILE_EOF|") == 0) {
                    auto parts = SplitString(msg, "|");
                    if (parts.size() >= 5) {
                        std::string globalKey = parts[2] + "_" + parts[3];
                        std::string localFile;

                        {
                            std::lock_guard<std::mutex> lock(taskMutex);
                            if (receivingTasks.count(globalKey)) {
                                localFile = receivingTasks[globalKey];
                                receivingTasks.erase(globalKey);
                            }
                        }

                        if (!localFile.empty()) {
                            std::cout << "\n[文件传输] 任务完成！文件已保存为: " << localFile << "\n" << currentPrompt;
                            system(("start " + localFile).c_str());
                        }
                    }
                    continue;
                }
                else if (msg.find("FILE_ABORT|") == 0) {
                    auto parts = SplitString(msg, "|");
                    if (parts.size() >= 5) {
                        std::string senderId = parts[2]; // 发送方
                        std::string fName = parts[4];    // 文件名
                        std::string globalKey = parts[2] + "_" + parts[3];

                        std::lock_guard<std::mutex> lock(taskMutex);
                        if (receivingTasks.count(globalKey)) {
                            // 【体验升级】：精准播报是谁撤回了什么文件！
                            std::cout << "\n[系统警告] 用户 [" << senderId << "] 紧急撤回了文件 [" << fName << "]！正在销毁残留数据...\n" << currentPrompt;
                            std::remove(receivingTasks[globalKey].c_str());
                            receivingTasks.erase(globalKey);
                        }
                    }
                    continue;
                }
                else if (msg.find("FILE_OFFLINE|") == 0) {
                    auto parts = SplitString(msg, "|");
                    if (parts.size() >= 2) {
                        std::string failedUuid = parts[1]; // 服务端退回的 UUID 快递单号

                        std::lock_guard<std::mutex> lock(sendTaskMutex);
                        // 遍历短号字典，揪出那个底层 UUID 匹配的任务
                        for (auto& pair : sendingTasks) {
                            if (pair.second.globalUuid == failedUuid) {
                                // 如果还没被刹车，系统代劳，自动拉手刹！
                                if (!*(pair.second.isCancelling)) {
                                    *(pair.second.isCancelling) = true;
                                    std::cout << "\n[系统警告] 任务 #" << pair.first << " 的目标意外离线，该传输已被系统自动终止！\n" << currentPrompt;
                                }
                                break;
                            }
                        }
                    }
                    continue;
                }
                // ================== 【新增：基于死亡宣告的终极防御】 ==================
                // 【安全修复】：使用 find 替代 substr 防止短消息越界崩溃！
                else if (msg.find("OFFLINE:") == 0) {
                    std::string offlineUserId = msg.substr(8);

                    // 1. 打印聊天室下线提示
                    std::cout << "\n[系统提示] 用户 [" << offlineUserId << "] 离线了。\n" << currentPrompt;

                    // 2. 扫地僧出动：清理该死者留下的所有残缺文件！
                    std::lock_guard<std::mutex> lock(taskMutex);
                    for (auto it = receivingTasks.begin(); it != receivingTasks.end(); ) {
                        std::string key = it->first;
                        std::string prefix = offlineUserId + "_";

                        if (key.find(prefix) == 0) {
                            std::cout << "[系统清理] 检测到发送方 [" << offlineUserId << "] 异常断线！已自动销毁其残缺文件: " << it->second << "\n" << currentPrompt;
                            std::remove(it->second.c_str());
                            it = receivingTasks.erase(it);
                        }
                        else {
                            ++it;
                        }
                    }
                    continue;
                }
                // =========================================================================

                // 【安全修复】：普通消息判定，使用 find 替代 substr 
                if (msg.find("NICK_ACK:") == 0) {
                    std::string newName = msg.substr(9);
                    currentPrompt = "[" + newName + "] > ";
                    std::cout << "\n[系统提示]: 你的本地提示符已同步更新。\n" << currentPrompt;
                }
                else {
                    std::cout << "\n" << msg << "\n" << currentPrompt;
                }
            }
            else {
                if (isConnected) {
                    // 【关键修复 3】：视觉轰炸！用极其醒目的边框打破 getline 带来的视觉盲区
                    std::cout << "\n\n==================================================" << std::endl;
                    std::cout << " [致命错误] 与服务端的连接已物理断开！服务器已宕机！" << std::endl;
                    std::cout << "==================================================\n" << std::endl;

                    EmergencyCleanup();

                    std::cout << "[系统清理] 检测到断网，已自动销毁所有残缺文件。\n";

                    // 告诉被 getline 卡住的用户该怎么做
                    std::cout << "\n请按回车键 (Enter) 退出程序...\n";

                    isConnected = false;
                }
                break; // 退出接收循环
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

    void EmergencyCleanup() {
        // 1. 接收方清理残缺文件
        {
            std::lock_guard<std::mutex> lock(taskMutex);
            for (const auto& pair : receivingTasks) {
                std::remove(pair.second.c_str());
            }
            // 【关键修复】：斩草除根！必须清空字典。
            // 这样后台线程即使读到残留的 CHUNK 包，也会因为在字典里找不到 Key 而直接丢弃，绝不会重建文件！
            receivingTasks.clear();
        }

        // 2. 发送方清理所有正在发送的任务
        {
            std::lock_guard<std::mutex> lock(sendTaskMutex);
            for (const auto& pair : sendingTasks) {
                // 拉断手刹！让还在干活的后台线程立刻知道自己被处决了！
                *(pair.second.isCancelling) = true;

                if (clientSocket != INVALID_SOCKET) {
                    std::string abortMsg = "FILE_ABORT|" + pair.second.targetId + "|" + myId + "|" + pair.second.globalUuid + "|" + pair.second.fileName;
                    SendPacket(clientSocket, abortMsg);
                }
            }
            // 发送方也顺手清空字典，保持内存绝对干净
            sendingTasks.clear();
        }
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

    //主体框架继承的是一个登录系统
    bool AuthMenu() {
        while (isConnected) {
            system("cls");

            std::cout << "================================\n";
            std::cout << "     欢迎来到Aurora聊天室系统     \n";
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
                SendPacket(clientSocket, req);

                std::string res;
                RecvPacket(clientSocket, res);
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
                SendPacket(clientSocket, "GET_NEXT_ID");

                std::string res;
                RecvPacket(clientSocket, res);
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
                    SendPacket(clientSocket, req);

                    std::string regRes;
                    RecvPacket(clientSocket, regRes);
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
                SendPacket(clientSocket, "FORGOT_PWD|" + id);
                std::cout << "[系统温馨提示] 重置请求已发送，正在等待服务器处理...\n";

                std::string res;
                RecvPacket(clientSocket, res);
                std::vector<std::string> parts = SplitString(res, "|");

                // --- 处理限流排队 ---
                if (parts.size() >= 2 && parts[0] == "FORGOT_BUSY") {
                    std::cout << "\n[系统提示] " << parts[1] << " (Y/N): ";
                    char waitChoice;
                    std::cin >> waitChoice;

                    if (waitChoice == 'Y' || waitChoice == 'y') {
                        SendPacket(clientSocket, "FORGOT_WAIT|" + id);

                        bool waiting = true;
                        while (waiting) {
                            std::string wRes;
                            if (RecvPacket(clientSocket, wRes) <= 0) break;

                            auto wParts = SplitString(wRes, "|");
                            if (wParts.empty()) continue;

                            if (wParts[0] == "WAITING") {
                                std::cout << "\n[系统温馨提示] 当前排队人数较多，您已成功加入队列，请耐心排队等待...\n";
                            }
                            else if (wParts[0] == "WAIT_OK") {
                                std::cout << "\n[系统温馨提示] " << wParts[1] << " 请等待管理员审批下发验证码...\n";
                                waiting = false;
                            }
                        }

                        // 排队成功后，重新等待 FORGOT_OK (短信)
                        RecvPacket(clientSocket, res);
                        parts = SplitString(res, "|");
                    }
                    else {
                        // 【温馨提示 4】：取消排队的提示
                        SendPacket(clientSocket, "FORGOT_CANCEL|" + id);
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
                        std::string trash;
                        RecvPacket(clientSocket, trash);
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
                        SendPacket(clientSocket, "FORGOT_CANCEL|" + id);
                        system("pause");
                        continue;
                    }

                    // 提交给服务端进行 Redis 验证
                    SendPacket(clientSocket, "RESET_PWD|" + id + "|" + inputCode + "|" + pwd1);

                    std::string resetRes;
                    RecvPacket(clientSocket, resetRes);
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
        std::cout << "【传输】格式: /sendfile 目标ID 文件完整路径 取消 /cancel [任务号]" << std::endl;
        std::cout << "【任务】格式: /tasks" << std::endl;
        std::cout << "【退出】格式: quit" << std::endl;
        std::cout << "========================================" << std::endl;

        std::thread(&ChatClient::ReceiveMessages, this).detach();

        std::cout << currentPrompt;

        std::string userInput;
        while (isConnected) {
            std::getline(std::cin, userInput);

            if (userInput == "quit") {
                std::cout << "\n[系统提示] 正在执行安全退出清理程序，请稍候...\n";
                isConnected = false; // 先告诉后台接收线程准备收工

                // 【步骤 1】：主动呼叫急救中心！拉断所有发送线程的手刹，并向外发射 FILE_ABORT 刹车包
                EmergencyCleanup();

                // 【步骤 2】：让子弹飞一会儿！
                // 必须停顿 200 毫秒，确保网卡把刚才生成的 FILE_ABORT 包结结实实地推到了网线上
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

                // 【步骤 3】：遗言发送完毕，安全关闭发送通道
                shutdown(clientSocket, SD_SEND);

                std::cout << "[系统提示] 清理完毕，安全断开连接。再见！\n";
                break;
            }

            if (userInput == "/tasks") {
                std::lock_guard<std::mutex> lock(sendTaskMutex);
                std::cout << "\n--- 当前发送任务列表 ---\n";
                if (sendingTasks.empty()) std::cout << "当前没有正在发送的文件。\n";
                for (const auto& pair : sendingTasks) {
                    // 给用户展示的永远是短号
                    std::cout << "任务#" << pair.first << " -> 目标: " << pair.second.targetId << " 文件: " << pair.second.fileName << "\n";
                }
                std::cout << "------------------------\n" << currentPrompt;
                continue;
            }

            if (userInput.find("/cancel ") == 0) {
                int taskIdToCancel = 0;
                try {
                    taskIdToCancel = std::stoi(userInput.substr(8)); // 解析短号，比如 1
                }
                catch (...) {
                    std::cout << "[错误] 请输入正确的任务编号，例如 /cancel 1\n" << currentPrompt;
                    continue;
                }

                std::lock_guard<std::mutex> lock(sendTaskMutex);
                if (sendingTasks.count(taskIdToCancel)) {
                    *(sendingTasks[taskIdToCancel].isCancelling) = true;
                    std::cout << "[系统提示] 已向任务#" << taskIdToCancel << " 发送终止指令...\n" << currentPrompt;
                }
                else {
                    std::cout << "[系统提示] 找不到任务#" << taskIdToCancel << "！\n" << currentPrompt;
                }
                continue;
            }

            // ================== 【终极引擎：基于 UUID 的内外双 ID 并发发送】 ==================
            if (userInput.find("/sendfile ") == 0) {

                auto parts = SplitString(userInput, " ");
                if (parts.size() < 3) {
                    std::cout << "[错误] 格式不正确！请使用: /sendfile 目标ID 文件完整路径\n" << currentPrompt;
                    continue;
                }

                std::string targetIdStr = parts[1];
                std::string filePath = parts[2];

                // 【防弹修复 1】：如果你是拖拽文件进来的，去掉首尾的双引号！
                if (!filePath.empty() && filePath.front() == '"' && filePath.back() == '"') {
                    filePath = filePath.substr(1, filePath.length() - 2);
                }

                // 【防弹修复 2】：召唤翻译官，把中文路径变成 Windows 认识的宽字符！
                std::wstring wFilePath = Utf8ToWstring(filePath);

                // 注意这里传入的是翻译后的 wFilePath
                std::ifstream checkFile(wFilePath, std::ios::binary | std::ios::ate);
                if (!checkFile.is_open()) {
                    std::cout << "[错误] 无法打开文件，请检查路径是否正确或文件是否被占用！\n" << currentPrompt;
                    continue;
                }
                std::streamsize fileSize = checkFile.tellg();
                checkFile.close();

                std::string fileName = filePath;
                size_t slashPos = fileName.find_last_of("/\\");
                if (slashPos != std::string::npos) fileName = fileName.substr(slashPos + 1);

                int localId = localTaskIdCounter++;
                std::string globalUuid = GenerateStringUUID();

                std::shared_ptr<std::atomic<bool>> cancelFlag;
                {
                    std::lock_guard<std::mutex> lock(sendTaskMutex);
                    SendTask newTask;
                    newTask.targetId = targetIdStr;
                    newTask.fileName = fileName;
                    newTask.globalUuid = globalUuid;
                    sendingTasks[localId] = newTask;
                    cancelFlag = newTask.isCancelling;
                }

                std::cout << "[系统提示] 任务#" << localId << " 创建成功！正在给 [" << targetIdStr << "] 后台发送 [" << fileName << "]...\n" << currentPrompt;

                // 【防弹修复 3】：注意这里把 wFilePath 传给子线程！
                std::thread([this, targetIdStr, localId, globalUuid, wFilePath, fileName, fileSize, cancelFlag]() {
                    // 子线程里也要用翻译后的 wFilePath 打开！
                    std::ifstream file(wFilePath, std::ios::binary);
                    if (!file.is_open()) return;
                    if (!file.is_open()) return;

                    // 【核心改动】：协议升级，塞入 myId 和全球唯一 globalUuid !
                    std::string reqMsg = "FILE_REQ|" + targetIdStr + "|" + myId + "|" + globalUuid + "|" + fileName + "|" + std::to_string(fileSize);
                    SendPacket(clientSocket, reqMsg);

                    const int CHUNK_SIZE = 45000;
                    std::vector<unsigned char> buffer(CHUNK_SIZE);
                    int chunkIndex = 0;
                    std::streamsize totalRead = 0;
                    int lastReportedProgress = 0;

                    // --- 【核心循环块，无任何省略】 ---
                    while (totalRead < fileSize) {
                        // 【精准刹车】：只看属于自己的那根刹车线
                        if (*cancelFlag) {
                            std::string abortMsg = "FILE_ABORT|" + targetIdStr + "|" + myId + "|" + globalUuid + "|" + fileName;
                            SendPacket(clientSocket, abortMsg);
                            std::cout << "\n[系统提示] 任务#" << localId << " (" << fileName << ") 的传输已被物理斩断！\n" << currentPrompt;
                            break; // 直接砸碎循环，停止传输
                        }

                        // 每次读取硬盘前清空 buffer 
                        std::fill(buffer.begin(), buffer.end(), 0);
                        file.read(reinterpret_cast<char*>(buffer.data()), CHUNK_SIZE);
                        std::streamsize bytesRead = file.gcount();
                        totalRead += bytesRead;

                        // ================== 【终极修复：毫无省略的 Base64 引擎】 ==================
                        // 【你说的错误就在这里】！绝对不能省略这一行代码！
                        // 必须！把暴力的二进制数据逆向转码为安全的英文字母 Base64，否则无法通过 TCP 发送！
                        std::string encodedData = Base64Encode(buffer.data(), bytesRead);
                        // =========================================================================

                        // 【协议升级】：加塞 globalUuid 
                        // 现在，这一行代码里的 encodedData 就是百分之百定义的了！编译器完美通过！
                        std::string chunkMsg = "FILE_CHUNK|" + targetIdStr + "|" + myId + "|" + globalUuid + "|" + std::to_string(chunkIndex) + "|" + encodedData;

                        if (!SendPacket(clientSocket, chunkMsg)) {
                            std::cout << "\n[任务#" << localId << "] 网络异常，发送失败！\n" << currentPrompt;
                            break;
                        }

                        chunkIndex++;

                        // 【打印进度】：给用户看简短的 localId
                        int progress = (totalRead * 100) / fileSize;
                        if (progress - lastReportedProgress >= 20 || progress == 100) {
                            std::cout << "\n[任务#" << localId << "] 发送进度: " << progress << "%\n" << currentPrompt;
                            lastReportedProgress = progress;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    file.close();

                    // 【关键修复 2】：双重校验！不仅要没被取消，而且真正读取的字节数必须等于文件总大小！
                    if (!(*cancelFlag) && totalRead >= fileSize) {
                        std::string eofMsg = "FILE_EOF|" + targetIdStr + "|" + myId + "|" + globalUuid + "|" + fileName;
                        SendPacket(clientSocket, eofMsg);
                        std::cout << "\n[任务 #" << localId << "] 文件传输大功告成！\n" << currentPrompt;
                    }

                    // 任务结束，打扫战场
                    {
                        std::lock_guard<std::mutex> lock(sendTaskMutex);
                        sendingTasks.erase(localId);
                    }
                }).detach();

                continue;
            }
            if (!userInput.empty()) {
                SendPacket(clientSocket, userInput);
                std::cout << currentPrompt;
            }
        }
    }
};

BOOL WINAPI ConsoleCtrlHandler(DWORD signal) {
    if (signal == CTRL_CLOSE_EVENT || signal == CTRL_C_EVENT) {
        if (g_appInstance) {
            // 完美穿透次元壁！直接调用类内部的公有急救函数！
            g_appInstance->EmergencyCleanup();
        }
    }
    return FALSE;
}


int main() {
    // 【新增防乱码补丁】：强行将 Windows 控制台的输入和输出改为 UTF-8 编码！
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 【终极修复】：向 Windows 系统注册我们的“遗愿拦截器”！
    // 没有这一行，Windows 根本不知道上面那个 ConsoleCtrlHandler 函数的存在！
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    ChatClient client("35.201.130.67", 8080);
    g_appInstance = &client; // 【核心】：把大门钥匙交给 Windows 操作系统！

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