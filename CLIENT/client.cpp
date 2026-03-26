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


#pragma comment(lib, "ws2_32.lib")

// ================== 【升级版：系统级自毁与临终遗言引擎】 ==================
// 1. 接收方专用变量
std::string g_tempReceivingFile = "";

// 2. 发送方专用变量（新增）
SOCKET g_clientSocket = INVALID_SOCKET;          // 保存全局的 Socket 句柄
std::atomic<bool> g_isSendingFile{ false };        // 记录当前是否在发文件
std::string g_sendingTargetId = "";              // 记录正在发给谁
std::string g_sendingFileName = "";              // 记录发的是什么文件

BOOL WINAPI ConsoleCtrlHandler(DWORD signal) {
    if (signal == CTRL_CLOSE_EVENT || signal == CTRL_C_EVENT) {

        // 【情况 A：如果我是接收方】临死前删掉本地没下完的垃圾文件
        if (!g_tempReceivingFile.empty()) {
            std::remove(g_tempReceivingFile.c_str());
        }

        // 【情况 B：如果我是发送方】临死前向接收方射出一发“物理刹车子弹”！
        if (g_isSendingFile && g_clientSocket != INVALID_SOCKET) {
            std::string abortMsg = "FILE_ABORT|" + g_sendingTargetId + "|" + g_sendingFileName;

            // 因为在全局函数里调不到类的 SendPacket，我们手动组装“2字节包头”发出去！
            uint16_t net_len = htons(static_cast<uint16_t>(abortMsg.length()));
            std::string packet;
            packet.append(reinterpret_cast<char*>(&net_len), 2);
            packet.append(abortMsg);

            send(g_clientSocket, packet.c_str(), packet.length(), 0);
        }
    }
    return FALSE;
}
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
    std::string currentReceivingFile; // 新增：记录当前正在接收的文件名
    std::mutex sendMutex; // 新增：保护发送通道的互斥锁

    // ================== 【新增：刹车系统状态标志】 ==================
    std::atomic<bool> isTransferring{ false };       // 记录当前是否正在发文件
    std::atomic<bool> isTransferCancelling{ false }; // 记录是否按下了撤销键

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
            int r = RecvPacket(clientSocket, msg); // 【核心】：用包引擎替换原生 recv

            if (r == 1) {
                // ================== 【新增：文件接收拼图引擎】 ==================
                if (msg.find("FILE_REQ|") == 0) {
                    auto parts = SplitString(msg, "|");
                    if (parts.size() >= 4) {
                        // 给文件加个前缀，存到当前运行目录下
                        currentReceivingFile = "recv_" + parts[2];
                        g_tempReceivingFile = currentReceivingFile; // 【新增】：同步给系统全局变量
                        std::cout << "\n[文件传输] 叮！收到文件传输请求: " << parts[2] << " (大小: " << parts[3] << " 字节)\n" << currentPrompt;

                        // 创建并清空本地文件，准备迎接碎块
                        std::ofstream ofs(currentReceivingFile, std::ios::binary | std::ios::trunc);
                        ofs.close();
                    }
                    continue;
                }
                else if (msg.find("FILE_CHUNK|") == 0) {
                    auto parts = SplitString(msg, "|");
                    if (parts.size() >= 4 && !currentReceivingFile.empty()) {
                        // 1. 将安全的 Base64 英文字母，逆向解码回暴力的纯二进制碎肉
                        std::string decodedData = Base64Decode(parts[3]);

                        // 2. 以追加模式 (app) 写入本地文件，像拼图一样一块块贴上去
                        std::ofstream ofs(currentReceivingFile, std::ios::binary | std::ios::app);
                        if (ofs.is_open()) {
                            ofs.write(decodedData.data(), decodedData.size());
                            ofs.close();
                        }
                    }
                    continue;
                }
                else if (msg.find("FILE_EOF|") == 0) {
                    std::cout << "\n[文件传输] 文件接收完毕！已保存为: " << currentReceivingFile << "，正在为您自动打开...\n" << currentPrompt;

                    // 【魔法时刻】：调用 Windows 底层终端，直接拍在用户脸上！
                    std::string cmd = "start " + currentReceivingFile;
                    system(cmd.c_str());

                    currentReceivingFile = ""; // 清理状态，准备迎接下一个文件
                    g_tempReceivingFile = ""; // 【新增】：完工了，清空遗愿清单
                    continue;
                }
                else if (msg.find("FILE_ABORT|") == 0) {
                    std::cout << "\n[系统警告] 对方紧急撤回了文件传输！正在销毁残留数据...\n" << currentPrompt;
                    if (!currentReceivingFile.empty()) {
                        std::remove(currentReceivingFile.c_str()); // 物理抹杀残缺文件
                        currentReceivingFile = "";                 // 状态重置
                        g_tempReceivingFile = ""; // 【新增】：已删完，清空遗愿清单
                        std::cout << "[系统清理] 残缺文件已被彻底物理删除。\n" << currentPrompt;
                    }
                    continue;
                }
                // ================== 【新增：目标离线自动刹车引擎】 ==================
                else if (msg.find("FILE_OFFLINE|") == 0) {
                    // 防止短时间内收到多个退信导致疯狂刷屏，加一个状态判断
                    if (isTransferring && !isTransferCancelling) {
                        std::cout << "\n[系统警告] 接收方意外掉线，文件传输已自动终止！\n" << currentPrompt;
                        isTransferCancelling = true; // 【核心】：机器代劳，自动帮你拉下物理手刹！
                    }
                    continue;
                }
                // ================================================================


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
                if (isConnected) {
                    std::cout << "\n[!] 与服务端的连接已异常断开。" << std::endl;

                    // 【新增：临终自毁引擎】
                    if (!currentReceivingFile.empty()) {
                        std::remove(currentReceivingFile.c_str());
                        std::cout << "[系统清理] 检测到断网，已自动删除接收到一半的残缺文件，防止硬盘积攒垃圾。\n";
                        currentReceivingFile = "";
                        g_tempReceivingFile = ""; // 【新增】：已删完，清空遗愿清单
                    }

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
        g_clientSocket = clientSocket; // 【新增】：把连通的网线句柄交给全局拦截器
        return true;
    }

    //主体框架继承的是一个登录系统
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
        std::cout << "【传输】格式: /sendfile 目标ID 文件完整路径 /cancel 取消发送" << std::endl;
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

            // ================== 【新增：主动刹车指令】 ==================
            if (userInput == "/cancel") {
                if (isTransferring) {
                    isTransferCancelling = true; // 拉下原子手刹！
                    std::cout << "[系统提示] 已发送物理刹车信号，正在强制终止传输...\n" << currentPrompt;
                }
                else {
                    std::cout << "[系统提示] 当前没有正在传输的文件！\n" << currentPrompt;
                }
                continue;
            }

            // ================== 【新增：后台文件分块发送引擎】 ==================
            if (userInput.find("/sendfile ") == 0) {
                // 【新增防并发】：如果已经在传了，禁止同时传第二个
                if (isTransferring) {
                    std::cout << "[系统拒绝] 当前已有文件正在传输，请等待完成或输入 /cancel 取消！\n" << currentPrompt;
                    continue;
                }

                size_t firstSpace = userInput.find(' ');
                size_t secondSpace = userInput.find(' ', firstSpace + 1);

                if (secondSpace == std::string::npos) {
                    std::cout << "[错误] 格式不正确！请使用: /sendfile 目标ID 文件完整路径\n" << currentPrompt;
                    continue;
                }

                std::string targetIdStr = userInput.substr(firstSpace + 1, secondSpace - firstSpace - 1);
                std::string filePath = userInput.substr(secondSpace + 1);

                std::ifstream checkFile(filePath, std::ios::binary | std::ios::ate);
                if (!checkFile.is_open()) {
                    std::cout << "[错误] 无法打开文件，请检查路径是否正确！\n" << currentPrompt;
                    continue;
                }
                std::streamsize fileSize = checkFile.tellg();
                checkFile.close();

                std::string fileName = filePath;
                size_t slashPos = fileName.find_last_of("/\\");
                if (slashPos != std::string::npos) fileName = fileName.substr(slashPos + 1);

                std::cout << "[系统提示] 文件 [" << fileName << "] 已加入后台传输队列，您可以继续聊天！\n" << currentPrompt;

                isTransferring = true;         // 挂上传输档位
                isTransferCancelling = false;  // 松开刹车

                // 【新增】：向全局遗愿清单登记当前正在发送的任务！
                g_sendingTargetId = targetIdStr;
                g_sendingFileName = fileName;
                g_isSendingFile = true;

                // 【核心升级】：开启一个专属的后台子线程去搬砖
                std::thread([this, targetIdStr, filePath, fileName, fileSize]() {
                    std::ifstream file(filePath, std::ios::binary);
                    if (!file.is_open()) { isTransferring = false; return; }

                    std::string reqMsg = "FILE_REQ|" + targetIdStr + "|" + fileName + "|" + std::to_string(fileSize);
                    SendPacket(clientSocket, reqMsg);

                    const int CHUNK_SIZE = 45000;
                    std::vector<unsigned char> buffer(CHUNK_SIZE);
                    int chunkIndex = 0;
                    std::streamsize totalRead = 0;
                    int lastReportedProgress = 0;

                    while (totalRead < fileSize) {
                        // 【核心刹车检测】：每次读硬盘前，看一眼刹车拉没拉？
                        if (isTransferCancelling) {
                            std::string abortMsg = "FILE_ABORT|" + targetIdStr + "|" + fileName;
                            SendPacket(clientSocket, abortMsg);
                            std::cout << "\n[系统提示] 文件 [" << fileName << "] 的传输已被您物理斩断！\n" << currentPrompt;
                            break; // 直接砸碎循环，停止传输
                        }

                        file.read(reinterpret_cast<char*>(buffer.data()), CHUNK_SIZE);
                        std::streamsize bytesRead = file.gcount();
                        totalRead += bytesRead;

                        std::string encodedData = Base64Encode(buffer.data(), bytesRead);
                        std::string chunkMsg = "FILE_CHUNK|" + targetIdStr + "|" + std::to_string(chunkIndex) + "|" + encodedData;

                        SendPacket(clientSocket, chunkMsg);
                        chunkIndex++;

                        int progress = (totalRead * 100) / fileSize;
                        if (progress - lastReportedProgress >= 20 || progress == 100) {
                            std::cout << "\n[后台任务] 发送进度: " << progress << "%\n" << currentPrompt;
                            lastReportedProgress = progress;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    file.close();

                    // 如果不是被撤销的，那就正常发送 EOF
                    if (!isTransferCancelling) {
                        std::string eofMsg = "FILE_EOF|" + targetIdStr + "|" + fileName;
                        SendPacket(clientSocket, eofMsg);
                        std::cout << "\n[后台任务] 文件 [" << fileName << "] 传输大功告成！\n" << currentPrompt;
                    }

                    isTransferring = false; // 任务结束，摘掉档位
                    g_isSendingFile = false; // 【新增】：任务安全结束，清空全局发送遗愿！
                    }).detach();

                continue;
            }
            // ================================================================

            if (!userInput.empty()) {
                SendPacket(clientSocket, userInput);
                std::cout << currentPrompt;
            }
        }
    }
};

int main() {
    // 【终极修复】：向 Windows 系统注册我们的“遗愿拦截器”！
    // 没有这一行，Windows 根本不知道上面那个 ConsoleCtrlHandler 函数的存在！
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
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