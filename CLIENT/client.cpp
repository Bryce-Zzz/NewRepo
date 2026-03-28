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
#include <cstdint>  
#include <fstream>   
#include <cmath>     
#include <mutex>
#include <atomic>  
#include <cstdio>  
#include <map>       
#include <rpc.h>         

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "Rpcrt4.lib") 

// ================== 【基础辅助工具】 ==================
std::string GenerateStringUUID() {
    UUID uuid;
    if (UuidCreate(&uuid) != RPC_S_OK) return "uuid_error";
    unsigned char* uuidStrRaw = nullptr;
    if (UuidToStringA(&uuid, &uuidStrRaw) != RPC_S_OK) return "uuid_string_error";
    std::string finalUuid(reinterpret_cast<char*>(uuidStrRaw));
    RpcStringFreeA(&uuidStrRaw);
    return finalUuid;
}

const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const unsigned char* bytes_to_encode, unsigned int in_len) {
    std::string ret;
    int i = 0, j = 0;
    unsigned char char_array_3[3], char_array_4[4];

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

// ================== 【核心应用类】 ==================
class ChatClient;
ChatClient* g_appInstance = nullptr;
BOOL WINAPI ConsoleCtrlHandler(DWORD signal);

class ChatClient {
private:
    std::string serverIp;
    int serverPort;
    SOCKET clientSocket;
    SOCKET udpSocket; // 【新增】用于接收独立输入端发来的 UDP 指令
    std::string myId;
    std::string myName;
    bool isConnected;
    std::mutex sendMutex;

    std::map<std::string, std::string> receivingTasks;
    std::mutex taskMutex;

    struct SendTask {
        std::string targetId;
        std::string fileName;
        std::string globalUuid;
        std::shared_ptr<std::atomic<bool>> isCancelling;
        std::shared_ptr<std::atomic<int>> state;

        SendTask() : isCancelling(std::make_shared<std::atomic<bool>>(false)),
            state(std::make_shared<std::atomic<int>>(0)) {
        }
    };

    std::map<int, SendTask> sendingTasks;
    std::mutex sendTaskMutex;
    std::atomic<int> localTaskIdCounter{ 1 };

    struct PendingTask {
        std::string senderId;
        std::string globalUuid;
        std::string fileName;
        std::string fileSize;
    };
    std::map<int, PendingTask> pendingReceives;
    std::mutex pendingMutex;
    std::atomic<int> localPendingIdCounter{ 1 };

    bool SendPacket(SOCKET sock, const std::string& msg) {
        if (msg.empty()) return true;
        uint16_t net_len = htons(static_cast<uint16_t>(msg.length()));
        std::string packet;
        packet.append(reinterpret_cast<char*>(&net_len), 2);
        packet.append(msg);

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

    // Auth 阶段专用（只在此刻有效）
    bool GetInputWithMonitor(std::string& input) {
        input.clear();
        while (true) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(clientSocket, &readfds);
            timeval tv = { 0, 50000 };
            if (select(0, &readfds, NULL, NULL, &tv) > 0) {
                char buf[256] = { 0 };
                int r = recv(clientSocket, buf, sizeof(buf) - 1, MSG_PEEK);
                if (r <= 0) return false;
                if (std::string(buf).find("RESET_TIMEOUT") != std::string::npos) return false;
            }

            if (_kbhit()) {
                char c = _getch();
                if (c == '\r') { std::cout << std::endl; return true; }
                else if (c == '\b') {
                    if (!input.empty()) { input.pop_back(); std::cout << "\b \b"; }
                }
                else { input += c; std::cout << c; }
            }
        }
    }

    // ================== 【处理从输入端发来的指令】 ==================
    void ProcessUserInput(const std::string& userInput) {
        if (userInput == "quit" || userInput == "/exit") {
            std::cout << "\n[系统提示] 正在执行安全退出清理程序，请稍候...\n";
            isConnected = false;
            EmergencyCleanup();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            shutdown(clientSocket, SD_SEND);
            std::cout << "[系统提示] 清理完毕，安全断开连接。再见！\n";
            return;
        }

        if (userInput == "/tasks") {
            std::lock_guard<std::mutex> lock(sendTaskMutex);
            std::cout << "\n--- 当前发送任务列表 ---\n";
            if (sendingTasks.empty()) std::cout << "当前没有正在发送的文件。\n";
            for (const auto& pair : sendingTasks) {
                std::cout << "任务#" << pair.first << " -> 目标: " << pair.second.targetId << " 文件: " << pair.second.fileName << "\n";
            }
            std::cout << "------------------------\n";
            return;
        }

        if (userInput.find("/cancel ") == 0) {
            int taskIdToCancel = 0;
            try { taskIdToCancel = std::stoi(userInput.substr(8)); }
            catch (...) { std::cout << "\n[错误] 请输入正确的任务编号，例如 /cancel 1\n"; return; }

            std::lock_guard<std::mutex> lock(sendTaskMutex);
            if (sendingTasks.count(taskIdToCancel)) {
                *(sendingTasks[taskIdToCancel].isCancelling) = true;
                std::cout << "\n[系统提示] 已向任务#" << taskIdToCancel << " 发送终止指令...\n";
            }
            else {
                std::cout << "\n[系统提示] 找不到任务#" << taskIdToCancel << "！\n";
            }
            return;
        }

        if (userInput.find("/yes ") == 0 || userInput.find("/no ") == 0) {
            bool isYes = (userInput.find("/yes ") == 0);
            int pId = 0;
            try { pId = std::stoi(userInput.substr(isYes ? 5 : 4)); }
            catch (...) { std::cout << "\n[错误] 指令格式错误，请输入 /yes 编号 或 /no 编号\n"; return; }

            PendingTask task;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                if (pendingReceives.count(pId)) {
                    task = pendingReceives[pId];
                    pendingReceives.erase(pId);
                    found = true;
                }
            }

            if (!found) {
                std::cout << "\n[系统提示] 找不到该文件请求！可能已过期或输入错误。\n";
                return;
            }

            if (isYes) {
                std::string globalKey = task.senderId + "_" + task.globalUuid;
                std::string safeFileName = "recv_" + globalKey + "_" + task.fileName;
                {
                    std::lock_guard<std::mutex> lock(taskMutex);
                    receivingTasks[globalKey] = safeFileName;
                }
                std::ofstream ofs(safeFileName, std::ios::binary | std::ios::trunc);
                ofs.close();

                std::string acceptMsg = "FILE_ACCEPT|" + task.senderId + "|" + myId + "|" + task.globalUuid;
                SendPacket(clientSocket, acceptMsg);
                std::cout << "\n[系统提示] 已同意接收 [" << task.fileName << "]，数据通道建立中...\n";
            }
            else {
                std::string rejectMsg = "FILE_REJECT|" + task.senderId + "|" + myId + "|" + task.globalUuid;
                SendPacket(clientSocket, rejectMsg);
                std::cout << "\n[系统提示] 已残忍拒绝接收 [" << task.fileName << "]。\n";
            }
            return;
        }

        if (userInput.find("/sendfile ") == 0) {
            auto parts = SplitString(userInput, " ");
            if (parts.size() < 3) {
                std::cout << "\n[错误] 格式不正确！请使用: /sendfile 目标ID 文件完整路径\n";
                return;
            }

            std::string targetIdStr = parts[1];
            std::string filePath = parts[2];

            std::ifstream checkFile(filePath, std::ios::binary | std::ios::ate);
            if (!checkFile.is_open()) {
                std::cout << "\n[错误] 无法打开文件，请检查路径是否正确！\n";
                return;
            }
            std::streamsize fileSize = checkFile.tellg();
            checkFile.close();

            std::string fileName = filePath;
            size_t slashPos = fileName.find_last_of("/\\");
            if (slashPos != std::string::npos) fileName = fileName.substr(slashPos + 1);

            int localId = localTaskIdCounter++;
            std::string globalUuid = GenerateStringUUID();

            std::shared_ptr<std::atomic<bool>> cancelFlag;
            std::shared_ptr<std::atomic<int>> taskState;
            {
                std::lock_guard<std::mutex> lock(sendTaskMutex);
                SendTask newTask;
                newTask.targetId = targetIdStr;
                newTask.fileName = fileName;
                newTask.globalUuid = globalUuid;
                sendingTasks[localId] = newTask;
                cancelFlag = newTask.isCancelling;
                taskState = newTask.state;
            }

            std::cout << "\n[系统提示] 任务 #" << localId << " 创建成功！正在向 [" << targetIdStr << "] 发送接收邀请...\n";

            std::thread([this, targetIdStr, localId, globalUuid, filePath, fileName, fileSize, cancelFlag, taskState]() {
                std::ifstream file(filePath, std::ios::binary);
                if (!file.is_open()) {
                    std::lock_guard<std::mutex> lock(sendTaskMutex);
                    sendingTasks.erase(localId);
                    return;
                }

                std::string reqMsg = "FILE_REQ|" + targetIdStr + "|" + myId + "|" + globalUuid + "|" + fileName + "|" + std::to_string(fileSize);
                SendPacket(clientSocket, reqMsg);

                while (*taskState == 0 && !(*cancelFlag)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                if (*cancelFlag) {
                    std::string abortMsg = "FILE_ABORT|" + targetIdStr + "|" + myId + "|" + globalUuid + "|" + fileName;
                    SendPacket(clientSocket, abortMsg);
                    std::cout << "\n[系统提示] 任务 #" << localId << " 在等待时被斩断！\n";
                }
                else if (*taskState == 2) {
                    // 对方拒绝
                }
                else if (*taskState == 1) {
                    const int CHUNK_SIZE = 45000;
                    std::vector<unsigned char> buffer(CHUNK_SIZE);
                    int chunkIndex = 0;
                    std::streamsize totalRead = 0;
                    int lastReportedProgress = 0;

                    while (totalRead < fileSize) {
                        if (*cancelFlag) {
                            std::string abortMsg = "FILE_ABORT|" + targetIdStr + "|" + myId + "|" + globalUuid + "|" + fileName;
                            SendPacket(clientSocket, abortMsg);
                            std::cout << "\n[系统提示] 任务 #" << localId << " (" << fileName << ") 被斩断！\n";
                            break;
                        }

                        std::fill(buffer.begin(), buffer.end(), 0);
                        file.read(reinterpret_cast<char*>(buffer.data()), CHUNK_SIZE);
                        std::streamsize bytesRead = file.gcount();
                        totalRead += bytesRead;

                        std::string encodedData = Base64Encode(buffer.data(), bytesRead);
                        std::string chunkMsg = "FILE_CHUNK|" + targetIdStr + "|" + myId + "|" + globalUuid + "|" + std::to_string(chunkIndex) + "|" + encodedData;

                        if (!SendPacket(clientSocket, chunkMsg)) {
                            std::cout << "\n[任务 #" << localId << "] 网络异常，发送失败！\n";
                            break;
                        }

                        chunkIndex++;
                        int progress = (totalRead * 100) / fileSize;
                        if (progress - lastReportedProgress >= 20 || progress == 100) {
                            std::cout << "\n[任务 #" << localId << "] 发送进度: " << progress << "%\n";
                            lastReportedProgress = progress;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }

                    if (!(*cancelFlag) && totalRead >= fileSize) {
                        std::string eofMsg = "FILE_EOF|" + targetIdStr + "|" + myId + "|" + globalUuid + "|" + fileName;
                        SendPacket(clientSocket, eofMsg);
                        std::cout << "\n[任务 #" << localId << "] 文件传输大功告成！\n";
                    }
                }
                file.close();
                {
                    std::lock_guard<std::mutex> lock(sendTaskMutex);
                    sendingTasks.erase(localId);
                }
                }).detach();
            return;
        }

        if (!userInput.empty()) {
            SendPacket(clientSocket, userInput);
        }
    }

    // ================== 【UDP 监听线程 (暗网桥梁)】 ==================
    void UdpListenThread() {
        char buffer[4096];
        sockaddr_in senderAddr;
        int senderAddrSize = sizeof(senderAddr);
        while (isConnected) {
            int bytes = recvfrom(udpSocket, buffer, sizeof(buffer) - 1, 0, (sockaddr*)&senderAddr, &senderAddrSize);
            if (bytes > 0) {
                buffer[bytes] = '\0';
                ProcessUserInput(std::string(buffer));
            }
        }
    }

    // ================== 【接收网络消息并霸道打印】 ==================
    void ReceiveMessages() {
        while (isConnected) {
            std::string msg;
            int r = RecvPacket(clientSocket, msg);

            if (r == 1) {
                if (msg.find("FILE_REQ|") == 0) {
                    auto parts = SplitString(msg, "|");
                    if (parts.size() >= 6) {
                        std::string senderId = parts[2], taskId = parts[3], fName = parts[4], fSize = parts[5];
                        int pendingId = localPendingIdCounter++;
                        {
                            std::lock_guard<std::mutex> lock(pendingMutex);
                            pendingReceives[pendingId] = { senderId, taskId, fName, fSize };
                        }
                        std::cout << "\n===================================================================\n"
                            << "[文件请求] 叮！用户 [" << senderId << "] 想发给您文件: " << fName << " (" << fSize << " 字节)\n"
                            << "-> 请在【输入端】输入 /yes " << pendingId << " 接收，或 /no " << pendingId << " 拒绝\n"
                            << "===================================================================\n";
                    }
                }
                else if (msg.find("FILE_ACCEPT|") == 0) {
                    auto parts = SplitString(msg, "|");
                    if (parts.size() >= 4) {
                        std::lock_guard<std::mutex> lock(sendTaskMutex);
                        for (auto& pair : sendingTasks) {
                            if (pair.second.globalUuid == parts[3]) {
                                *(pair.second.state) = 1;
                                std::cout << "\n[系统提示] 对方已同意接收任务 #" << pair.first << "，极速传输中！\n";
                                break;
                            }
                        }
                    }
                }
                else if (msg.find("FILE_REJECT|") == 0) {
                    auto parts = SplitString(msg, "|");
                    if (parts.size() >= 4) {
                        std::lock_guard<std::mutex> lock(sendTaskMutex);
                        for (auto& pair : sendingTasks) {
                            if (pair.second.globalUuid == parts[3]) {
                                *(pair.second.state) = 2;
                                std::cout << "\n[系统提示] 对方残忍拒绝了任务 #" << pair.first << " [" << pair.second.fileName << "]。\n";
                                break;
                            }
                        }
                    }
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
                            std::cout << "\n[文件传输] 任务完成！文件已保存为: " << localFile << "\n";
                            system(("start " + localFile).c_str());
                        }
                    }
                }
                else if (msg.find("FILE_ABORT|") == 0) {
                    auto parts = SplitString(msg, "|");
                    if (parts.size() >= 5) {
                        std::string globalKey = parts[2] + "_" + parts[3];
                        std::lock_guard<std::mutex> lock(taskMutex);
                        if (receivingTasks.count(globalKey)) {
                            std::cout << "\n[系统警告] 用户 [" << parts[2] << "] 紧急撤回了 [" << parts[4] << "]！销毁数据...\n";
                            std::remove(receivingTasks[globalKey].c_str());
                            receivingTasks.erase(globalKey);
                        }
                    }
                }
                else if (msg.find("FILE_OFFLINE|") == 0) {
                    auto parts = SplitString(msg, "|");
                    if (parts.size() >= 2) {
                        std::lock_guard<std::mutex> lock(sendTaskMutex);
                        for (auto& pair : sendingTasks) {
                            if (pair.second.globalUuid == parts[1]) {
                                if (!*(pair.second.isCancelling)) {
                                    *(pair.second.isCancelling) = true;
                                    std::cout << "\n[系统警告] 任务 #" << pair.first << " 目标离线，已自动终止！\n";
                                }
                                break;
                            }
                        }
                    }
                }
                else if (msg.find("OFFLINE:") == 0) {
                    std::string offlineUserId = msg.substr(8);
                    std::cout << "\n[系统提示] 用户 [" << offlineUserId << "] 离线了。\n";
                    std::lock_guard<std::mutex> lock(taskMutex);
                    for (auto it = receivingTasks.begin(); it != receivingTasks.end(); ) {
                        if (it->first.find(offlineUserId + "_") == 0) {
                            std::cout << "[系统清理] 检测到发送方异常，已销毁残缺文件: " << it->second << "\n";
                            std::remove(it->second.c_str());
                            it = receivingTasks.erase(it);
                        }
                        else ++it;
                    }
                }
                else if (msg.find("NICK_ACK:") == 0) {
                    std::cout << "\n[系统提示]: 您的本地昵称已同步为 [" << msg.substr(9) << "]。\n";
                }
                else {
                    // 主线程再也不用打断了！想怎么打印怎么打印，爽！
                    std::cout << "\n" << msg << "\n";
                }
            }
            else {
                if (isConnected) {
                    std::cout << "\n==================================================\n"
                        << " [致命错误] 与服务端的连接已断开！\n"
                        << "==================================================\n";
                    EmergencyCleanup();
                    std::cout << "[系统清理] 检测到断网，已自动销毁残缺文件。\n（请直接关闭此窗口）\n";
                    isConnected = false;
                }
                break;
            }
        }
    }

public:
    ChatClient(std::string ip, int port)
        : serverIp(ip), serverPort(port), clientSocket(INVALID_SOCKET), udpSocket(INVALID_SOCKET), isConnected(false) {
    }

    ~ChatClient() {
        if (clientSocket != INVALID_SOCKET) closesocket(clientSocket);
        if (udpSocket != INVALID_SOCKET) closesocket(udpSocket);
        WSACleanup();
    }

    void EmergencyCleanup() {
        {
            std::lock_guard<std::mutex> lock(taskMutex);
            for (const auto& pair : receivingTasks) std::remove(pair.second.c_str());
        }
        {
            std::lock_guard<std::mutex> lock(sendTaskMutex);
            for (const auto& pair : sendingTasks) {
                *(pair.second.isCancelling) = true;
                if (clientSocket != INVALID_SOCKET) {
                    std::string abortMsg = "FILE_ABORT|" + pair.second.targetId + "|" + myId + "|" + pair.second.globalUuid + "|" + pair.second.fileName;
                    SendPacket(clientSocket, abortMsg);
                }
            }
        }
        if (udpSocket != INVALID_SOCKET) closesocket(udpSocket);
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
        // ... (保持你原有的 AuthMenu 完全不变) ...
        while (isConnected) {
            system("cls");
            std::cout << "================================\n";
            std::cout << "     欢迎来到极简聊天室系统     \n";
            std::cout << "================================\n";
            std::cout << "1. 登录账号\n";
            std::cout << "2. 注册账号\n";
            std::cout << "3. 忘记密码\n";
            std::cout << "0. 退出\n";
            std::cout << "请选择: ";

            int choice;
            std::cin >> choice;

            if (std::cin.fail()) {
                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                continue;
            }

            if (choice == 0) return false;
            else if (choice == 1) {
                std::string id, pwd;
                std::cout << "请输入数字ID: "; std::cin >> id;
                std::cout << "请输入密码: "; std::cin >> pwd;
                SendPacket(clientSocket, "LOGIN|" + id + "|" + pwd);
                std::string res; RecvPacket(clientSocket, res);
                auto parts = SplitString(res, "|");
                if (parts.size() >= 2 && parts[0] == "LOGIN_OK") {
                    myId = id; myName = parts[1];
                    std::cout << "\n[系统提示] 登录成功！欢迎回来，" << myName << "！\n\n";
                    system("pause"); return true;
                }
                else std::cout << "\n[登录失败]\n"; system("pause");
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

    // ================== 【核心：裂变双屏引擎】 ==================
    void RunChat(const std::string& exeName) {
        system("cls");
        system("title 【大屏展示端】 - 您的主神监控中心");
        std::cout << "================================================\n";
        std::cout << " [提示] 这是一个纯净只读展示窗，你的操作请去弹出的【输入专用终端】执行！\n";
        std::cout << "================================================\n";

        // 1. 在本地随机开一个 UDP 端口作为“暗网接收点”
        udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in localAddr = { 0 };
        localAddr.sin_family = AF_INET;
        localAddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        localAddr.sin_port = 0; // 让 OS 随机分配端口
        bind(udpSocket, (sockaddr*)&localAddr, sizeof(localAddr));

        int len = sizeof(localAddr);
        getsockname(udpSocket, (sockaddr*)&localAddr, &len);
        int localUdpPort = ntohs(localAddr.sin_port);

        // 2. 召唤子进程（输入专用窗口）
        std::string cmd = "start \"\" \"" + exeName + "\" -input " + std::to_string(localUdpPort) + " \"" + myName + "\"";
        system(cmd.c_str());

        // 3. 开启后台打工人线程
        std::thread(&ChatClient::ReceiveMessages, this).detach(); // 盯紧 TCP
        std::thread(&ChatClient::UdpListenThread, this).detach(); // 盯紧子进程

        // 4. 主线程直接躺平休眠（再也不用去争夺键盘了！）
        while (isConnected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
};

BOOL WINAPI ConsoleCtrlHandler(DWORD signal) {
    if (signal == CTRL_CLOSE_EVENT || signal == CTRL_C_EVENT) {
        if (g_appInstance) g_appInstance->EmergencyCleanup();
    }
    return FALSE;
}

// ================== 【子进程：纯净的输入终端】 ==================
void RunInputTerminal(int targetPort, const std::string& userName) {
    system("title 【纯净输入端】 - 享受丝滑打字体验");
    system("color 0A"); // 开启黑客绿底色
    std::cout << "====================================================\n";
    std::cout << "  [控制台] 这里是您的专属打字区域\n";
    std::cout << "  完美支持：中文输入法(IME) | 复制粘贴(Ctrl+V) | 上下键历史\n";
    std::cout << "====================================================\n\n";

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in target;
    target.sin_family = AF_INET;
    target.sin_port = htons(targetPort);
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);

    std::string prompt = "[" + userName + "] > ";
    std::string input;
    while (true) {
        std::cout << prompt;
        // 退回最初的纯净 getline，全量接管 Windows 输入框神力！
        std::getline(std::cin, input);
        if (!input.empty()) {
            sendto(sock, input.c_str(), input.length(), 0, (sockaddr*)&target, sizeof(target));
        }
        if (input == "quit" || input == "/exit") {
            break;
        }
    }
    closesocket(sock);
    WSACleanup();
}

int main(int argc, char* argv[]) {
    // 拦截裂变信号：如果带有 -input 参数，直接进入子进程模式！
    if (argc == 4 && std::string(argv[1]) == "-input") {
        int port = std::stoi(argv[2]);
        std::string name = argv[3];
        RunInputTerminal(port, name);
        return 0;
    }

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    ChatClient client("127.0.0.1", 8080);
    g_appInstance = &client;

    if (!client.Initialize()) {
        std::cerr << "\n[!] 连接失败! 确保服务端运行中。" << std::endl;
        system("pause");
        return 0;
    }

    if (client.AuthMenu()) {
        client.RunChat(argv[0]); // 传入 exe 自己的路径，准备裂变！
    }

    std::cout << "\n系统安全断开。" << std::endl;
    system("pause");
    return 0;
}

//#define NOMINMAX      
//#include <iostream>
//#include <string>
//#include <thread>
//#include <chrono>     
//#include <vector>
//#include <limits>     
//#include <winsock2.h>
//#include <ws2tcpip.h>
//#include <conio.h>
//#include <cstdint>  // 用于 uint16_t
//#include <fstream>   // 用于读写文件
//#include <cmath>     // 用于数学计算
//#include <mutex>
//#include <atomic>  // 用于多线程安全的标志位
//#include <cstdio>  // 用于 std::remove 删除文件
//#include <map>       // 【新增】：用于多并发接收的哈希表
//
//// ================== 【终极进化：Windows UUID 运行环境】 ==================
//#include <rpc.h>         // 引入 Windows 原生 RPC API
//#pragma comment(lib, "ws2_32.lib")
//#pragma comment(lib, "Rpcrt4.lib") // 【核心链接】：链接 RPC 运行时库，用于生成 UUID
//// =========================================================================
//
//
//// ================== 【终极优雅：单例指针挂载】 ==================
//class ChatClient; // 提前声明类
//
//ChatClient* g_appInstance = nullptr; // 唯一合法的全局指针（通往保险柜的钥匙）
//
//// 这里只做“函数声明”，告诉系统有这个函数，但身体放在后面！
//BOOL WINAPI ConsoleCtrlHandler(DWORD signal);
//// ================================================================
//
//class ChatClient {
//private:
//    std::string serverIp;
//    int serverPort;
//    SOCKET clientSocket;
//    std::string currentPrompt;
//    std::string myId;
//    std::string myName;
//    bool isConnected;
//
//    std::mutex sendMutex; // 新增：保护发送通道的互斥锁
//
//    // ================== 【新增：多并发接收引擎核心】 ==================
//    std::map<std::string, std::string> receivingTasks; // Key: 发送方ID, Value: 本地文件名
//    std::mutex taskMutex; // 保护哈希表的多线程锁
//
//    // ================== 【终极进化：新增同意/拒绝状态机】 ==================
//    struct SendTask {
//        std::string targetId;
//        std::string fileName;
//        std::string globalUuid;
//        std::shared_ptr<std::atomic<bool>> isCancelling;
//        // 【新增】：0=等待对方同意，1=对方同意(极速发送)，2=对方拒绝(自毁)
//        std::shared_ptr<std::atomic<int>> state;
//
//        SendTask() : isCancelling(std::make_shared<std::atomic<bool>>(false)),
//            state(std::make_shared<std::atomic<int>>(0)) {
//        }
//    };
//
//    // ================== 【新增：UI 重绘引擎】 ==================
//    std::mutex coutMutex;           // 保护屏幕不被两个线程同时写入
//    std::string currentTyping = ""; // 记录用户当前正在敲击的半截内容
//
//    // 终极安全的打印函数：所有的后台弹窗、聊天消息，全部用它来输出！
//    void SafePrint(const std::string& msg) {
//        std::lock_guard<std::mutex> lock(coutMutex);
//        // 1. \r回到行首，打印80个空格瞬间清空当前行，再\r回到行首
//        std::cout << "\r" << std::string(80, ' ') << "\r";
//        // 2. 干净地插播后台消息
//        std::cout << msg << "\n";
//        // 3. 完美重绘输入框和用户打了一半的字！
//        std::cout << currentPrompt << currentTyping;
//    }
//    // ==========================================================
//
//    std::map<int, SendTask> sendingTasks;
//    std::mutex sendTaskMutex;
//    std::atomic<int> localTaskIdCounter{ 1 };
//
//    // 【新增】：接收方的“待处理收件箱”
//    struct PendingTask {
//        std::string senderId;
//        std::string globalUuid;
//        std::string fileName;
//        std::string fileSize;
//    };
//    std::map<int, PendingTask> pendingReceives; // Key: 本地给用户看的短号 1, 2...
//    std::mutex pendingMutex;
//    std::atomic<int> localPendingIdCounter{ 1 };
//    // ================================================================
//
//    // ================== 【新增：Windows 原生 UUID 生成器】 ==================
//    // 生成格式如: "550e8400-e29b-41d4-a716-446655440000" 的全球唯一字符串
//    std::string GenerateStringUUID() {
//        UUID uuid;
//        // 1. 调用 Windows API 创建 UUID
//        if (UuidCreate(&uuid) != RPC_S_OK) return "uuid_error"; // 极其罕见的错误
//
//        unsigned char* uuidStrRaw = nullptr;
//        // 2. 将二进制 UUID 转换为字符串
//        if (UuidToStringA(&uuid, &uuidStrRaw) != RPC_S_OK) return "uuid_string_error";
//
//        std::string finalUuid(reinterpret_cast<char*>(uuidStrRaw));
//
//        // 3. 必须！调用 Windows API 释放分配的字符串内存，防止内存泄漏！
//        RpcStringFreeA(&uuidStrRaw);
//        return finalUuid;
//    }
//    // =========================================================================
//
//    // ================== 【新增：核心网络收发引擎】 ==================
//    bool SendPacket(SOCKET sock, const std::string& msg) {
//        if (msg.empty()) return true;
//        uint16_t net_len = htons(static_cast<uint16_t>(msg.length()));
//        std::string packet;
//        packet.append(reinterpret_cast<char*>(&net_len), 2);
//        packet.append(msg);
//
//        // 【核心修改】：加上发送锁，保证多线程并发发送时的绝对安全！
//        std::lock_guard<std::mutex> lock(sendMutex);
//
//        int totalSent = 0;
//        int packetLen = packet.length();
//        while (totalSent < packetLen) {
//            int sent = send(sock, packet.c_str() + totalSent, packetLen - totalSent, 0);
//            if (sent <= 0) return false;
//            totalSent += sent;
//        }
//        return true;
//    }
//
//    int RecvExactly(SOCKET sock, char* buf, int len) {
//        int totalRecv = 0;
//        while (totalRecv < len) {
//            int r = recv(sock, buf + totalRecv, len - totalRecv, 0);
//            if (r == 0) return 0;
//            if (r < 0) return -1;
//            totalRecv += r;
//        }
//        return 1;
//    }
//
//    int RecvPacket(SOCKET sock, std::string& msg) {
//        uint16_t net_len = 0;
//        int r = RecvExactly(sock, reinterpret_cast<char*>(&net_len), 2);
//        if (r <= 0) return r;
//
//        uint16_t host_len = ntohs(net_len);
//        if (host_len == 0) {
//            msg = "";
//            return 1;
//        }
//
//        std::vector<char> buffer(host_len);
//        r = RecvExactly(sock, buffer.data(), host_len);
//        if (r <= 0) return r;
//
//        msg = std::string(buffer.data(), host_len);
//        return 1;
//    }
//    // ================================================================
//
//    // 智能输入函数：一边等键盘打字，一边盯紧服务器
//    bool GetInputWithMonitor(std::string& input) {
//        input.clear();
//        while (true) {
//            // 1. 检查服务器是否发来了超时指令 (RESET_TIMEOUT)
//            fd_set readfds;
//            FD_ZERO(&readfds);
//            FD_SET(clientSocket, &readfds);
//            timeval tv = { 0, 50000 }; // 50毫秒超时
//            if (select(0, &readfds, NULL, NULL, &tv) > 0) {
//                char buf[256] = { 0 };
//                int r = recv(clientSocket, buf, sizeof(buf) - 1, MSG_PEEK); // 只偷看一眼，不取走
//                if (r <= 0) return false;
//                if (std::string(buf).find("RESET_TIMEOUT") != std::string::npos) {
//                    return false; // 发现超时信号！立刻中断输入
//                }
//            }
//
//            // 2. 检查用户是否按了键盘
//            if (_kbhit()) {
//                char c = _getch();
//                if (c == '\r') { // 回车键
//                    std::cout << std::endl;
//                    return true;
//                }
//                else if (c == '\b') { // 退格键
//                    if (!input.empty()) {
//                        input.pop_back();
//                        std::cout << "\b \b";
//                    }
//                }
//                else { // 正常字符
//                    input += c;
//                    std::cout << c;
//                }
//            }
//        }
//    }
//
//    // ================== 【新增：Base64 编解码引擎】 ==================
//    const std::string base64_chars =
//        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
//        "abcdefghijklmnopqrstuvwxyz"
//        "0123456789+/";
//
//    std::string Base64Encode(const unsigned char* bytes_to_encode, unsigned int in_len) {
//        std::string ret;
//        int i = 0;
//        int j = 0;
//        unsigned char char_array_3[3];
//        unsigned char char_array_4[4];
//
//        while (in_len--) {
//            char_array_3[i++] = *(bytes_to_encode++);
//            if (i == 3) {
//                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
//                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
//                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
//                char_array_4[3] = char_array_3[2] & 0x3f;
//                for (i = 0; (i < 4); i++) ret += base64_chars[char_array_4[i]];
//                i = 0;
//            }
//        }
//        if (i) {
//            for (j = i; j < 3; j++) char_array_3[j] = '\0';
//            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
//            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
//            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
//            char_array_4[3] = char_array_3[2] & 0x3f;
//            for (j = 0; (j < i + 1); j++) ret += base64_chars[char_array_4[j]];
//            while ((i++ < 3)) ret += '=';
//        }
//        return ret;
//    }
//
//    std::string Base64Decode(std::string const& encoded_string) {
//        int in_len = encoded_string.size();
//        int i = 0, j = 0, in_ = 0;
//        unsigned char char_array_4[4], char_array_3[3];
//        std::string ret;
//
//        auto is_base64 = [](unsigned char c) { return (isalnum(c) || (c == '+') || (c == '/')); };
//
//        while (in_len-- && (encoded_string[in_] != '=') && is_base64(encoded_string[in_])) {
//            char_array_4[i++] = encoded_string[in_]; in_++;
//            if (i == 4) {
//                for (i = 0; i < 4; i++) char_array_4[i] = base64_chars.find(char_array_4[i]);
//                char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
//                char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
//                char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
//                for (i = 0; (i < 3); i++) ret += char_array_3[i];
//                i = 0;
//            }
//        }
//        if (i) {
//            for (j = i; j < 4; j++) char_array_4[j] = 0;
//            for (j = 0; j < 4; j++) char_array_4[j] = base64_chars.find(char_array_4[j]);
//            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
//            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
//            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];
//            for (j = 0; (j < i - 1); j++) ret += char_array_3[j];
//        }
//        return ret;
//    }
//    // ================================================================
//
//    std::vector<std::string> SplitString(const std::string& str, const std::string& delimiter) {
//        std::vector<std::string> tokens;
//        size_t prev = 0, pos = 0;
//        do {
//            pos = str.find(delimiter, prev);
//            if (pos == std::string::npos) pos = str.length();
//            std::string token = str.substr(prev, pos - prev);
//            if (!token.empty()) tokens.push_back(token);
//            prev = pos + delimiter.length();
//        } while (pos < str.length() && prev < str.length());
//        return tokens;
//    }
//
//    void ReceiveMessages() {
//        while (isConnected) {
//            std::string msg;
//            int r = RecvPacket(clientSocket, msg); // 用包引擎替换原生 recv
//
//            if (r == 1) {
//                // ================== 【绅士协议：请求与回执拦截网关】 ==================
//                if (msg.find("FILE_REQ|") == 0) {
//                    auto parts = SplitString(msg, "|");
//                    if (parts.size() >= 6) {
//                        std::string senderId = parts[2];
//                        std::string taskId = parts[3];
//                        std::string fName = parts[4];
//                        std::string fSize = parts[5];
//
//                        // 1. 生成本地短号，放入“待处理收件箱”
//                        int pendingId = localPendingIdCounter++;
//                        {
//                            std::lock_guard<std::mutex> lock(pendingMutex);
//                            pendingReceives[pendingId] = { senderId, taskId, fName, fSize };
//                        }
//
//                        // 2. 弹窗询问用户 (已替换为 SafePrint)
//                        std::string notice =
//                            "===================================================================\n"
//                            "[文件请求] 叮！用户 [" + senderId + "] 想发给您文件: " + fName + " (" + fSize + " 字节)\n"
//                            "-> 请输入 /yes " + std::to_string(pendingId) + " 接收，或 /no " + std::to_string(pendingId) + " 拒绝\n"
//                            "===================================================================";
//                        SafePrint(notice);
//                    }
//                    continue;
//                }
//                else if (msg.find("FILE_ACCEPT|") == 0) {
//                    auto parts = SplitString(msg, "|");
//                    if (parts.size() >= 4) {
//                        std::string uuid = parts[3];
//                        std::lock_guard<std::mutex> lock(sendTaskMutex);
//                        for (auto& pair : sendingTasks) {
//                            if (pair.second.globalUuid == uuid) {
//                                // 收到同意回执！将状态改为 1，唤醒沉睡的发送线程！
//                                *(pair.second.state) = 1;
//                                SafePrint("[系统提示] 对方已同意接收任务 #" + std::to_string(pair.first) + "，开始极速传输！");
//                                break;
//                            }
//                        }
//                    }
//                    continue;
//                }
//                else if (msg.find("FILE_REJECT|") == 0) {
//                    auto parts = SplitString(msg, "|");
//                    if (parts.size() >= 4) {
//                        std::string uuid = parts[3];
//                        std::lock_guard<std::mutex> lock(sendTaskMutex);
//                        for (auto& pair : sendingTasks) {
//                            if (pair.second.globalUuid == uuid) {
//                                // 收到残忍拒绝！将状态改为 2，让发送线程自杀！
//                                *(pair.second.state) = 2;
//                                SafePrint("[系统提示] 对方残忍拒绝了您的任务 #" + std::to_string(pair.first) + " [" + pair.second.fileName + "]。");
//                                break;
//                            }
//                        }
//                    }
//                    continue;
//                }
//                else if (msg.find("FILE_CHUNK|") == 0) {
//                    auto parts = SplitString(msg, "|");
//                    if (parts.size() >= 6) {
//                        std::string globalKey = parts[2] + "_" + parts[3];
//                        std::lock_guard<std::mutex> lock(taskMutex);
//
//                        if (receivingTasks.count(globalKey)) {
//                            std::string decodedData = Base64Decode(parts[5]);
//                            std::ofstream ofs(receivingTasks[globalKey], std::ios::binary | std::ios::app);
//                            if (ofs.is_open()) {
//                                ofs.write(decodedData.data(), decodedData.size());
//                                ofs.close();
//                            }
//                        }
//                    }
//                    continue;
//                }
//                else if (msg.find("FILE_EOF|") == 0) {
//                    auto parts = SplitString(msg, "|");
//                    if (parts.size() >= 5) {
//                        std::string globalKey = parts[2] + "_" + parts[3];
//                        std::string localFile;
//
//                        {
//                            std::lock_guard<std::mutex> lock(taskMutex);
//                            if (receivingTasks.count(globalKey)) {
//                                localFile = receivingTasks[globalKey];
//                                receivingTasks.erase(globalKey);
//                            }
//                        }
//
//                        if (!localFile.empty()) {
//                            SafePrint("[文件传输] 任务完成！文件已保存为: " + localFile);
//                            system(("start " + localFile).c_str());
//                        }
//                    }
//                    continue;
//                }
//                else if (msg.find("FILE_ABORT|") == 0) {
//                    auto parts = SplitString(msg, "|");
//                    if (parts.size() >= 5) {
//                        std::string senderId = parts[2]; // 发送方
//                        std::string fName = parts[4];    // 文件名
//                        std::string globalKey = parts[2] + "_" + parts[3];
//
//                        std::lock_guard<std::mutex> lock(taskMutex);
//                        if (receivingTasks.count(globalKey)) {
//                            // 【体验升级】：精准播报是谁撤回了什么文件！
//                            SafePrint("[系统警告] 用户 [" + senderId + "] 紧急撤回了文件 [" + fName + "]！正在销毁残留数据...");
//                            std::remove(receivingTasks[globalKey].c_str());
//                            receivingTasks.erase(globalKey);
//                        }
//                    }
//                    continue;
//                }
//                else if (msg.find("FILE_OFFLINE|") == 0) {
//                    auto parts = SplitString(msg, "|");
//                    if (parts.size() >= 2) {
//                        std::string failedUuid = parts[1]; // 服务端退回的 UUID 快递单号
//
//                        std::lock_guard<std::mutex> lock(sendTaskMutex);
//                        // 遍历短号字典，揪出那个底层 UUID 匹配的任务
//                        for (auto& pair : sendingTasks) {
//                            if (pair.second.globalUuid == failedUuid) {
//                                // 如果还没被刹车，系统代劳，自动拉手刹！
//                                if (!*(pair.second.isCancelling)) {
//                                    *(pair.second.isCancelling) = true;
//                                    SafePrint("[系统警告] 任务 #" + std::to_string(pair.first) + " 的目标意外离线，该传输已被系统自动终止！");
//                                }
//                                break;
//                            }
//                        }
//                    }
//                    continue;
//                }
//                // ================== 【新增：基于死亡宣告的终极防御】 ==================
//                // 【安全修复】：使用 find 替代 substr 防止短消息越界崩溃！
//                else if (msg.find("OFFLINE:") == 0) {
//                    std::string offlineUserId = msg.substr(8);
//
//                    // 1. 打印聊天室下线提示
//                    SafePrint("[系统提示] 用户 [" + offlineUserId + "] 离线了。");
//
//                    // 2. 扫地僧出动：清理该死者留下的所有残缺文件！
//                    std::lock_guard<std::mutex> lock(taskMutex);
//                    for (auto it = receivingTasks.begin(); it != receivingTasks.end(); ) {
//                        std::string key = it->first;
//                        std::string prefix = offlineUserId + "_";
//
//                        if (key.find(prefix) == 0) {
//                            SafePrint("[系统清理] 检测到发送方 [" + offlineUserId + "] 异常断线！已自动销毁其残缺文件: " + it->second);
//                            std::remove(it->second.c_str());
//                            it = receivingTasks.erase(it);
//                        }
//                        else {
//                            ++it;
//                        }
//                    }
//                    continue;
//                }
//                // =========================================================================
//
//                // 【安全修复】：普通消息判定，使用 find 替代 substr 
//                if (msg.find("NICK_ACK:") == 0) {
//                    std::string newName = msg.substr(9);
//                    currentPrompt = "[" + newName + "] > ";
//                    SafePrint("[系统提示]: 你的本地提示符已同步更新。");
//                }
//                else {
//                    SafePrint(msg);
//                }
//            }
//            else {
//                if (isConnected) {
//                    // 【关键修复 3】：视觉轰炸！用极其醒目的边框打破 getline 带来的视觉盲区
//                    SafePrint("\n==================================================\n [致命错误] 与服务端的连接已物理断开！服务器已宕机！\n==================================================");
//
//                    EmergencyCleanup();
//
//                    SafePrint("[系统清理] 检测到断网，已自动销毁所有残缺文件。");
//
//                    // 告诉被 getline 卡住的用户该怎么做
//                    SafePrint("请按回车键 (Enter) 退出程序...");
//
//                    isConnected = false;
//                }
//                break; // 退出接收循环
//            }
//        }
//    }
//
//public:
//    ChatClient(std::string ip, int port)
//        : serverIp(ip), serverPort(port), clientSocket(INVALID_SOCKET), currentPrompt("> "), isConnected(false) {
//    }
//
//    ~ChatClient() {
//        if (clientSocket != INVALID_SOCKET) closesocket(clientSocket);
//        WSACleanup();
//    }
//
//    void EmergencyCleanup() {
//        // 1. 接收方清理残缺文件
//        {
//            std::lock_guard<std::mutex> lock(taskMutex);
//            for (const auto& pair : receivingTasks) {
//                std::remove(pair.second.c_str());
//            }
//        }
//
//        // 2. 发送方清理所有正在发送的任务
//        {
//            std::lock_guard<std::mutex> lock(sendTaskMutex);
//            for (const auto& pair : sendingTasks) {
//                // 【关键修复 1】：拉断手刹！让还在干活的后台线程立刻知道自己被处决了！
//                *(pair.second.isCancelling) = true;
//
//                if (clientSocket != INVALID_SOCKET) {
//                    std::string abortMsg = "FILE_ABORT|" + pair.second.targetId + "|" + myId + "|" + pair.second.globalUuid + "|" + pair.second.fileName;
//                    SendPacket(clientSocket, abortMsg);
//                }
//            }
//        }
//    }
//
//    bool Initialize() {
//        WSADATA wsaData;
//        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
//
//        clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
//        if (clientSocket == INVALID_SOCKET) return false;
//
//        sockaddr_in serverAddr;
//        serverAddr.sin_family = AF_INET;
//        serverAddr.sin_port = htons(serverPort);
//        inet_pton(AF_INET, serverIp.c_str(), &serverAddr.sin_addr);
//
//        if (connect(clientSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) return false;
//
//        isConnected = true;
//
//        return true;
//    }
//
//    //主体框架继承的是一个登录系统
//    bool AuthMenu() {
//        while (isConnected) {
//            system("cls");
//
//            std::cout << "================================\n";
//            std::cout << "     欢迎来到极简聊天室系统     \n";
//            std::cout << "================================\n";
//            std::cout << "1. 登录账号\n";
//            std::cout << "2. 注册账号\n";
//            std::cout << "3. 忘记密码 (手机验证码重置)\n";
//            std::cout << "0. 退出\n";
//            std::cout << "请选择: ";
//
//            int choice;
//            std::cin >> choice;
//
//            if (std::cin.fail()) {
//                std::cin.clear();
//                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
//                std::cout << "\n[提示] 输入无效，请重新输入！\n\n";
//                system("pause");
//                continue;
//            }
//
//            if (choice == 0) {
//                return false;
//            }
//            else if (choice == 1) {
//                std::string id, pwd;
//                std::cout << "请输入数字ID: "; std::cin >> id;
//                std::cout << "请输入密码: "; std::cin >> pwd;
//
//                std::string req = "LOGIN|" + id + "|" + pwd;
//                SendPacket(clientSocket, req);
//
//                std::string res;
//                RecvPacket(clientSocket, res);
//                std::vector<std::string> parts = SplitString(res, "|");
//
//                if (parts.size() >= 2 && parts[0] == "LOGIN_OK") {
//                    myId = id;
//                    myName = parts[1];
//                    currentPrompt = "[" + myName + "] > ";
//                    std::cout << "\n[系统提示] 登录成功！欢迎回来，" << myName << "！\n\n";
//                    system("pause");
//                    return true;
//                }
//                else if (parts.size() >= 2 && parts[0] == "LOGIN_FAIL") {
//                    std::cout << "\n[登录失败] " << parts[1] << "\n\n";
//                    system("pause");
//                }
//            }
//            else if (choice == 2) {
//                SendPacket(clientSocket, "GET_NEXT_ID");
//
//                std::string res;
//                RecvPacket(clientSocket, res);
//                std::vector<std::string> parts = SplitString(res, "|");
//
//                if (parts.size() >= 2 && parts[0] == "NEXT_ID") {
//                    std::string preAssignedId = parts[1];
//                    std::cout << "\n======================================\n";
//                    std::cout << "  系统为您预留的专属ID为：【 " << preAssignedId << " 】\n";
//                    std::cout << "======================================\n";
//
//                    std::string pwd, name;
//                    std::cout << "设置密码: "; std::cin >> pwd;
//                    std::cout << "设置昵称: "; std::cin >> name;
//
//                    std::string req = "REG|" + preAssignedId + "|" + pwd + "|" + name;
//                    SendPacket(clientSocket, req);
//
//                    std::string regRes;
//                    RecvPacket(clientSocket, regRes);
//                    std::vector<std::string> regParts = SplitString(regRes, "|");
//
//                    if (regParts.size() >= 2) {
//                        if (regParts[0] == "REG_OK") {
//                            std::cout << "\n[恭喜] 注册成功！请务必牢记您的ID：" << preAssignedId << "\n\n";
//                        }
//                        else {
//                            std::cout << "\n[注册失败] " << regParts[1] << "\n\n";
//                        }
//                        system("pause");
//                    }
//                }
//            }
//            else if (choice == 3) {
//                std::string id;
//                std::cout << "\n请输入需要找回密码的数字ID: ";
//                std::cin >> id;
//
//                // 1. 发起申请
//                SendPacket(clientSocket, "FORGOT_PWD|" + id);
//                std::cout << "[系统温馨提示] 重置请求已发送，正在等待服务器处理...\n";
//
//                std::string res;
//                RecvPacket(clientSocket, res);
//                std::vector<std::string> parts = SplitString(res, "|");
//
//                // --- 处理限流排队 ---
//                if (parts.size() >= 2 && parts[0] == "FORGOT_BUSY") {
//                    std::cout << "\n[系统提示] " << parts[1] << " (Y/N): ";
//                    char waitChoice;
//                    std::cin >> waitChoice;
//
//                    if (waitChoice == 'Y' || waitChoice == 'y') {
//                        SendPacket(clientSocket, "FORGOT_WAIT|" + id);
//
//                        bool waiting = true;
//                        while (waiting) {
//                            std::string wRes;
//                            if (RecvPacket(clientSocket, wRes) <= 0) break;
//
//                            auto wParts = SplitString(wRes, "|");
//                            if (wParts.empty()) continue;
//
//                            if (wParts[0] == "WAITING") {
//                                std::cout << "\n[系统温馨提示] 当前排队人数较多，您已成功加入队列，请耐心排队等待...\n";
//                            }
//                            else if (wParts[0] == "WAIT_OK") {
//                                std::cout << "\n[系统温馨提示] " << wParts[1] << " 请等待管理员审批下发验证码...\n";
//                                waiting = false;
//                            }
//                        }
//
//                        // 排队成功后，重新等待 FORGOT_OK (短信)
//                        RecvPacket(clientSocket, res);
//                        parts = SplitString(res, "|");
//                    }
//                    else {
//                        // 【温馨提示 4】：取消排队的提示
//                        SendPacket(clientSocket, "FORGOT_CANCEL|" + id);
//                        std::cout << "\n[温馨提示] 您已取消排队，将为您返回主菜单。\n\n";
//                        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
//                        system("pause");
//                        continue;
//                    }
//                }
//
//                // --- 处理验证码下发 ---
//                if (parts.size() >= 2 && parts[0] == "FORGOT_OK") {
//                    std::string serverCode = parts[1];
//                    // 【温馨提示 5】：手机短信的提示（这里我已经帮你同步为1分钟有效了）
//                    std::cout << "\n[手机模拟器] 叮！您收到一条短信：验证码为 【 " << serverCode << " 】，1分钟内有效。\n\n";
//
//                    // 定义一个处理超时的快速宏
//                    auto handleTimeout = [&]() {
//                        std::cout << "\n\n[系统警报] 操作已超时 (超过1分钟)！您的重置名额已被系统回收。\n";
//                        std::cout << "按任意键返回主菜单...\n";
//                        _getch(); // 等待用户随便按个键
//
//                        // 把服务器发来的 RESET_TIMEOUT 取走，清理网络管道
//                        std::string trash;
//                        RecvPacket(clientSocket, trash);
//                        };
//
//                    std::string inputCode, pwd1, pwd2;
//
//                    std::cout << "请输入6位数验证码: ";
//                    if (!GetInputWithMonitor(inputCode)) { handleTimeout(); continue; }
//
//                    std::cout << "请输入新密码: ";
//                    if (!GetInputWithMonitor(pwd1)) { handleTimeout(); continue; }
//
//                    std::cout << "请再次确认新密码: ";
//                    if (!GetInputWithMonitor(pwd2)) { handleTimeout(); continue; }
//
//                    // 本地二次校验一致性
//                    if (pwd1 != pwd2) {
//                        std::cout << "\n[错误提示] 两次输入的密码不一致！修改已取消。\n\n";
//                        // 通知服务端释放名额
//                        SendPacket(clientSocket, "FORGOT_CANCEL|" + id);
//                        system("pause");
//                        continue;
//                    }
//
//                    // 提交给服务端进行 Redis 验证
//                    SendPacket(clientSocket, "RESET_PWD|" + id + "|" + inputCode + "|" + pwd1);
//
//                    std::string resetRes;
//                    RecvPacket(clientSocket, resetRes);
//                    std::vector<std::string> resetParts = SplitString(resetRes, "|");
//
//                    if (resetParts.size() >= 2 && resetParts[0] == "RESET_OK") {
//                        std::cout << "\n[恭喜] " << resetParts[1] << "\n\n";
//                    }
//                    else if (resetParts.size() >= 2) {
//                        std::cout << "\n[失败] " << resetParts[1] << "\n\n";
//                    }
//                }
//                else if (parts.size() >= 2 && parts[0] == "FORGOT_FAIL") {
//                    std::cout << "\n[请求失败] " << parts[1] << "\n\n";
//                }
//
//                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
//                system("pause");
//            }
//        }
//        return false;
//    }
//
//    void RunChat() {
//        system("cls");
//        std::cout << "========================================" << std::endl;
//        std::cout << "成功进入聊天大厅！当前身份: " << myName << " (ID:" << myId << ")" << std::endl;
//        std::cout << "【群聊】直接打字并回车" << std::endl;
//        std::cout << "【私聊】格式: @目标ID或昵称 消息内容" << std::endl;
//        std::cout << "【改名】格式: /nick 新名字" << std::endl;
//        std::cout << "【传输】格式: /sendfile 目标ID 文件完整路径 取消 /cancel [任务号]" << std::endl;
//        std::cout << "【任务】格式: /tasks" << std::endl;
//        std::cout << "【退出】格式: quit" << std::endl;
//        std::cout << "========================================" << std::endl;
//
//        std::thread(&ChatClient::ReceiveMessages, this).detach();
//
//        std::cout << currentPrompt;
//
//        std::string userInput;
//        while (isConnected) {
//            // ================== 【全新键盘监听引擎】 ==================
//            // 取代了又笨又卡的 std::getline！
//            if (_kbhit()) {
//                char c = _getch();
//                if (c == '\r') { // 用户按下了回车
//                    std::cout << "\n";
//                    userInput = currentTyping;
//                    currentTyping.clear(); // 清空缓存，准备进入指令判断
//                }
//                else if (c == '\b') { // 用户按下了退格键
//                    if (!currentTyping.empty()) {
//                        currentTyping.pop_back();
//                        std::lock_guard<std::mutex> lock(coutMutex);
//                        std::cout << "\b \b"; // 视觉上抹除屏幕上的最后一个字符
//                    }
//                    continue; // 继续监听键盘
//                }
//                else { // 用户敲击了普通字符
//                    currentTyping += c;
//                    std::lock_guard<std::mutex> lock(coutMutex);
//                    std::cout << c;
//                    continue; // 继续监听键盘
//                }
//            }
//            else {
//                std::this_thread::sleep_for(std::chrono::milliseconds(10));
//                continue; // 没敲键盘时，让出 CPU
//            }
//            // ==========================================================
//
//            if (userInput == "quit") {
//                SafePrint("[系统提示] 正在执行安全退出清理程序，请稍候...");
//                isConnected = false; // 先告诉后台接收线程准备收工
//
//                // 【步骤 1】：主动呼叫急救中心！拉断所有发送线程的手刹，并向外发射 FILE_ABORT 刹车包
//                EmergencyCleanup();
//
//                // 【步骤 2】：让子弹飞一会儿！
//                // 必须停顿 200 毫秒，确保网卡把刚才生成的 FILE_ABORT 包结结实实地推到了网线上
//                std::this_thread::sleep_for(std::chrono::milliseconds(200));
//
//                // 【步骤 3】：遗言发送完毕，安全关闭发送通道
//                shutdown(clientSocket, SD_SEND);
//
//                SafePrint("[系统提示] 清理完毕，安全断开连接。再见！");
//                break;
//            }
//
//            if (userInput == "/tasks") {
//                std::lock_guard<std::mutex> lock(sendTaskMutex);
//                std::string tasksInfo = "--- 当前发送任务列表 ---\n";
//                if (sendingTasks.empty()) tasksInfo += "当前没有正在发送的文件。\n";
//                for (const auto& pair : sendingTasks) {
//                    // 给用户展示的永远是短号
//                    tasksInfo += "任务#" + std::to_string(pair.first) + " -> 目标: " + pair.second.targetId + " 文件: " + pair.second.fileName + "\n";
//                }
//                tasksInfo += "------------------------";
//                SafePrint(tasksInfo);
//                continue;
//            }
//
//            if (userInput.find("/cancel ") == 0) {
//                int taskIdToCancel = 0;
//                try {
//                    taskIdToCancel = std::stoi(userInput.substr(8)); // 解析短号，比如 1
//                }
//                catch (...) {
//                    SafePrint("[错误] 请输入正确的任务编号，例如 /cancel 1");
//                    continue;
//                }
//
//                std::lock_guard<std::mutex> lock(sendTaskMutex);
//                if (sendingTasks.count(taskIdToCancel)) {
//                    *(sendingTasks[taskIdToCancel].isCancelling) = true;
//                    SafePrint("[系统提示] 已向任务#" + std::to_string(taskIdToCancel) + " 发送终止指令...");
//                }
//                else {
//                    SafePrint("[系统提示] 找不到任务#" + std::to_string(taskIdToCancel) + "！");
//                }
//                continue;
//            }
//            // ================== 【接收方权限：同意或拒绝信令】 ==================
//            if (userInput.find("/yes ") == 0 || userInput.find("/no ") == 0) {
//                bool isYes = (userInput.find("/yes ") == 0);
//                int pId = 0;
//                try {
//                    pId = std::stoi(userInput.substr(isYes ? 5 : 4));
//                }
//                catch (...) {
//                    SafePrint("[错误] 指令格式错误，请输入 /yes 编号 或 /no 编号");
//                    continue;
//                }
//
//                PendingTask task;
//                bool found = false;
//
//                // 1. 去收件箱里找这封信件
//                {
//                    std::lock_guard<std::mutex> lock(pendingMutex);
//                    if (pendingReceives.count(pId)) {
//                        task = pendingReceives[pId];
//                        pendingReceives.erase(pId); // 找到了就立刻从待处理箱拿走
//                        found = true;
//                    }
//                }
//
//                if (!found) {
//                    SafePrint("[系统提示] 找不到该文件请求！可能已过期或输入错误。");
//                    continue;
//                }
//
//                // 2. 根据用户的选择执行生死裁决
//                if (isYes) {
//                    // 同意：建立物理文件，发射 ACCEPT 绿灯信令
//                    std::string globalKey = task.senderId + "_" + task.globalUuid;
//                    std::string safeFileName = "recv_" + globalKey + "_" + task.fileName;
//                    {
//                        std::lock_guard<std::mutex> lock(taskMutex);
//                        receivingTasks[globalKey] = safeFileName;
//                    }
//                    std::ofstream ofs(safeFileName, std::ios::binary | std::ios::trunc);
//                    ofs.close();
//
//                    std::string acceptMsg = "FILE_ACCEPT|" + task.senderId + "|" + myId + "|" + task.globalUuid;
//                    SendPacket(clientSocket, acceptMsg);
//                    SafePrint("[系统提示] 已同意接收 [" + task.fileName + "]，数据通道建立中...");
//                }
//                else {
//                    // 拒绝：发射 REJECT 红灯信令
//                    std::string rejectMsg = "FILE_REJECT|" + task.senderId + "|" + myId + "|" + task.globalUuid;
//                    SendPacket(clientSocket, rejectMsg);
//                    SafePrint("[系统提示] 已残忍拒绝接收 [" + task.fileName + "]。");
//                }
//                continue;
//            }
//
//
//            // ================== 【终极引擎：基于 UUID 的并发发送(含 / yes绅士握手协议)】 ==================
//            if (userInput.find("/sendfile ") == 0) {
//
//                auto parts = SplitString(userInput, " ");
//                if (parts.size() < 3) {
//                    SafePrint("[错误] 格式不正确！请使用: /sendfile 目标ID 文件完整路径");
//                    continue;
//                }
//
//                std::string targetIdStr = parts[1];
//                std::string filePath = parts[2]; // 注意：要求路径里无空格，简化处理
//
//                std::ifstream checkFile(filePath, std::ios::binary | std::ios::ate);
//                if (!checkFile.is_open()) {
//                    SafePrint("[错误] 无法打开文件，请检查路径是否正确！");
//                    continue;
//                }
//                std::streamsize fileSize = checkFile.tellg();
//                checkFile.close();
//
//                std::string fileName = filePath;
//                size_t slashPos = fileName.find_last_of("/\\");
//                if (slashPos != std::string::npos) fileName = fileName.substr(slashPos + 1);
//
//                // 1. 生成内外双 ID
//                int localId = localTaskIdCounter++; // 给用户看的短号 1, 2, 3
//                std::string globalUuid = GenerateStringUUID(); // 给网络用的长 UUID "550e8400..."
//
//                // 2. 将任务精准登记到本地字典
//                std::shared_ptr<std::atomic<bool>> cancelFlag;
//                std::shared_ptr<std::atomic<int>> taskState; // 【新增】：提取状态机指针
//                {
//                    std::lock_guard<std::mutex> lock(sendTaskMutex);
//                    SendTask newTask;
//                    newTask.targetId = targetIdStr;
//                    newTask.fileName = fileName;
//                    newTask.globalUuid = globalUuid;
//                    sendingTasks[localId] = newTask;
//                    cancelFlag = newTask.isCancelling;
//                    taskState = newTask.state; // 获取专属的“同意/拒绝”状态监控器
//                }
//
//                SafePrint("[系统提示] 任务 #" + std::to_string(localId) + " 创建成功！正在向 [" + targetIdStr + "] 发送接收邀请...");
//
//                // 3. 开启搬砖子线程，注意把 taskState 传进去！
//                std::thread([this, targetIdStr, localId, globalUuid, filePath, fileName, fileSize, cancelFlag, taskState]() {
//                    std::ifstream file(filePath, std::ios::binary);
//                    if (!file.is_open()) {
//                        // 兜底清理
//                        std::lock_guard<std::mutex> lock(sendTaskMutex);
//                        sendingTasks.erase(localId);
//                        return;
//                    }
//
//                    // 【第一步】：发送预告信 (FILE_REQ)，询问对方是否愿意接收
//                    std::string reqMsg = "FILE_REQ|" + targetIdStr + "|" + myId + "|" + globalUuid + "|" + fileName + "|" + std::to_string(fileSize);
//                    SendPacket(clientSocket, reqMsg);
//
//                    // ================== 【第二步：进入“睡眠舱”阻塞等待】 ==================
//                    // 只要状态是 0 (没同意也没拒绝)，并且没被用户强行 cancel，就一直原地睡觉！
//                    while (*taskState == 0 && !(*cancelFlag)) {
//                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
//                    }
//
//                    // 【第三步】：醒来后验尸，判断接下来的生死走向
//                    if (*cancelFlag) {
//                        // 1. 在等待期间，被发送方自己敲 /cancel 斩断了
//                        std::string abortMsg = "FILE_ABORT|" + targetIdStr + "|" + myId + "|" + globalUuid + "|" + fileName;
//                        SendPacket(clientSocket, abortMsg);
//                        SafePrint("[系统提示] 任务 #" + std::to_string(localId) + " 在等待对方同意时被您物理斩断！");
//                    }
//                    else if (*taskState == 2) {
//                        // 2. 对方明确拒绝 (接收方发来了 FILE_REJECT，状态变为 2)
//                        // 拒绝的提示语在 ReceiveMessages 里已经弹过了，这里什么都不用发，直接跳过去打扫战场
//                    }
//                    else if (*taskState == 1) {
//                        // ================== 【第四步：对方同意，火力全开】 ==================
//                        // 对方发来了 FILE_ACCEPT，状态变为 1，开始疯狂切块发送！
//                        const int CHUNK_SIZE = 45000;
//                        std::vector<unsigned char> buffer(CHUNK_SIZE);
//                        int chunkIndex = 0;
//                        std::streamsize totalRead = 0;
//                        int lastReportedProgress = 0;
//
//                        while (totalRead < fileSize) {
//                            // 发送中途依然支持随时精准刹车
//                            if (*cancelFlag) {
//                                std::string abortMsg = "FILE_ABORT|" + targetIdStr + "|" + myId + "|" + globalUuid + "|" + fileName;
//                                SendPacket(clientSocket, abortMsg);
//                                SafePrint("[系统提示] 任务 #" + std::to_string(localId) + " (" + fileName + ") 的传输已被物理斩断！");
//                                break;
//                            }
//
//                            std::fill(buffer.begin(), buffer.end(), 0);
//                            file.read(reinterpret_cast<char*>(buffer.data()), CHUNK_SIZE);
//                            std::streamsize bytesRead = file.gcount();
//                            totalRead += bytesRead;
//
//                            // 毫无省略的 Base64 引擎转码
//                            std::string encodedData = Base64Encode(buffer.data(), bytesRead);
//
//                            // 拼接并发送 CHUNK 包
//                            std::string chunkMsg = "FILE_CHUNK|" + targetIdStr + "|" + myId + "|" + globalUuid + "|" + std::to_string(chunkIndex) + "|" + encodedData;
//
//                            if (!SendPacket(clientSocket, chunkMsg)) {
//                                SafePrint("[任务 #" + std::to_string(localId) + "] 网络异常，发送失败！");
//                                break;
//                            }
//
//                            chunkIndex++;
//
//                            // 打印进度
//                            int progress = (totalRead * 100) / fileSize;
//                            if (progress - lastReportedProgress >= 20 || progress == 100) {
//                                SafePrint("[任务 #" + std::to_string(localId) + "] 发送进度: " + std::to_string(progress) + "%");
//                                lastReportedProgress = progress;
//                            }
//                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
//                        }
//
//                        // 如果正常发完且中途没有被取消，发送 EOF 完工信令
//                        if (!(*cancelFlag) && totalRead >= fileSize) {
//                            std::string eofMsg = "FILE_EOF|" + targetIdStr + "|" + myId + "|" + globalUuid + "|" + fileName;
//                            SendPacket(clientSocket, eofMsg);
//                            SafePrint("[任务 #" + std::to_string(localId) + "] 文件传输大功告成！");
//                        }
//                    }
//
//                    // 【最终步】：打扫战场
//                    file.close();
//                    {
//                        std::lock_guard<std::mutex> lock(sendTaskMutex);
//                        sendingTasks.erase(localId); // 从字典中移除该任务
//                    }
//                    }).detach();
//
//                continue;
//            }
//
//            if (!userInput.empty()) {
//                SendPacket(clientSocket, userInput);
//                // 正常的聊天消息发送后，输入框换行处理（回车已经把屏幕切下去了），直接补印提示符即可
//                std::lock_guard<std::mutex> lock(coutMutex);
//                std::cout << currentPrompt;
//            }
//        }
//    }
//};
//
//BOOL WINAPI ConsoleCtrlHandler(DWORD signal) {
//    if (signal == CTRL_CLOSE_EVENT || signal == CTRL_C_EVENT) {
//        if (g_appInstance) {
//            // 完美穿透次元壁！直接调用类内部的公有急救函数！
//            g_appInstance->EmergencyCleanup();
//        }
//    }
//    return FALSE;
//}
//
//
//int main() {
//    // 【终极修复】：向 Windows 系统注册我们的“遗愿拦截器”！
//    // 没有这一行，Windows 根本不知道上面那个 ConsoleCtrlHandler 函数的存在！
//    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
//    ChatClient client("127.0.0.1", 8080);
//    g_appInstance = &client; // 【核心】：把大门钥匙交给 Windows 操作系统！
//
//    if (!client.Initialize()) {
//        std::cerr << "\n[!] 连接服务端失败! 请确保你已经先启动了服务端 (Server.exe)。" << std::endl;
//        system("pause");
//        return 0;
//    }
//
//    if (client.AuthMenu()) {
//        client.RunChat();
//    }
//
//    // 【优化 1】：完美的退出提示
//    std::cout << "\n当前客户端已安全断开连接。" << std::endl;
//    system("pause");
//    return 0;
//}