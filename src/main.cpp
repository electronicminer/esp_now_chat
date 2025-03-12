#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <ESPAsyncWebServer.h>
#include <WebSocketsServer.h>
#include <vector>

#define CHANNEL 1

void sendMessage(String msg);
// 未确认消息结构体和列表
struct PendingMessage {
    uint8_t msgID;
    String message;
    unsigned long sendTime;
};

std::vector<PendingMessage> pendingMessages; // 未确认消息列表
uint8_t currentMsgID = 0; // 当前消息ID

// Web服务器和WebSocket实例
AsyncWebServer server(80);
WebSocketsServer webSocket(81);

// 消息结构体定义
typedef struct struct_message {
    uint8_t msgID;    // 消息的唯一标识符
    bool isACK;       // 标记是否为确认消息
    char data[245];   // 消息内容
} struct_message;

struct_message myData;          // 发送消息用
struct_message incomingMessage; // 接收消息用

// 目标设备MAC地址（请替换为实际对端设备的MAC地址）
// uint8_t peerAddress[] = {0xCC,0xBA,0x97,0x0F,0x24,0x78};//my esp32
uint8_t peerAddress[] = {0xCC,0xBA,0x97,0x0F,0x2C,0x74};//zs's esp32
// HTML页面，用于网页端显示和交互
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>ESP-NOW 消息</title>
    <style>
        body {font-family: Arial; margin: 0; padding: 20px;}
        .container {max-width: 600px; margin: auto;}
        .chat-box {
            height: 300px;
            border: 1px solid #ccc;
            padding: 10px;
            overflow-y: auto;
            margin-bottom: 10px;
            background: #f9f9f9;
        }
        .message {margin: 5px 0; padding: 8px; background: #fff; border-radius: 5px;}
        .input-group {display: flex;}
        input {flex: 1; padding: 10px; margin-right: 5px;}
        button {padding: 10px 20px; background: #007bff; color: white; border: none;}
    </style>
</head>
<body>
    <div class="container">
        <div class="chat-box" id="chat"></div>
        <div class="input-group">
            <input type="text" id="message" placeholder="输入消息...">
            <button onclick="sendMessage()">发送</button>
        </div>
    </div>
    <script>
        const ws = new WebSocket('ws://' + window.location.hostname + ':81/');
        const chat = document.getElementById('chat');
        
        ws.onmessage = function(event) {
            const message = document.createElement('div');
            message.className = 'message';
            message.textContent = event.data;
            chat.appendChild(message);
            chat.scrollTop = chat.scrollHeight;
        };

        function sendMessage() {
            const input = document.getElementById('message');
            if (input.value.trim()) {
                ws.send(input.value);
                input.value = '';
            }
        }
        
        document.getElementById('message').addEventListener('keypress', function(e) {
            if (e.key === 'Enter') sendMessage();
        });
    </script>
</body>
</html>
)rawliteral";

// ESP-NOW发送回调函数
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    Serial.printf("发送状态: %s\n", status == ESP_NOW_SEND_SUCCESS ? "成功" : "失败");
}

// ESP-NOW接收回调函数
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int len) {
    memcpy(&incomingMessage, data, sizeof(incomingMessage));
    if (incomingMessage.isACK) {
        // 处理确认消息（ACK）
        for (auto it = pendingMessages.begin(); it != pendingMessages.end(); ++it) {
            if (it->msgID == incomingMessage.msgID) {
                // 显示勾（√）表示成功
                char buffer[300];
                snprintf(buffer, sizeof(buffer), "发送: %s √", it->message.c_str());
                webSocket.broadcastTXT(buffer);
                it = pendingMessages.erase(it); // 移除已确认的消息
                break;
            }
        }
    } else {
        // 收到普通消息，发送ACK
        struct_message ackMsg;
        ackMsg.msgID = incomingMessage.msgID;
        ackMsg.isACK = true;
        esp_now_send(peerAddress, (uint8_t *)&ackMsg, sizeof(ackMsg));

        // 显示接收到的消息
        char buffer[300];
        snprintf(buffer, sizeof(buffer), "接收: %s", incomingMessage.data);
        webSocket.broadcastTXT(buffer);
    }
}

// WebSocket事件处理函数
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_TEXT:
            // 从网页接收到的消息
            String msg = String((char*)payload);
            sendMessage(msg);
            break;
    }
}


// 发送消息函数
void sendMessage(String msg) {
    myData.msgID = currentMsgID++;
    myData.isACK = false;
    strncpy(myData.data, msg.c_str(), sizeof(myData.data));
    esp_now_send(peerAddress, (uint8_t *)&myData, sizeof(myData));

    // 记录待确认消息
    PendingMessage pm = {myData.msgID, msg, millis()};
    pendingMessages.push_back(pm);

    // 在网页显示“发送中...”
    char buffer[300];
    snprintf(buffer, sizeof(buffer), "发送中: %s", msg.c_str());
    webSocket.broadcastTXT(buffer);
}

// 检查超时并重发消息
void checkTimeouts() {
    unsigned long currentTime = millis();
    for (auto it = pendingMessages.begin(); it != pendingMessages.end(); ) {
        if (currentTime - it->sendTime > 2000) { // 超时2秒
            // 重发消息
            myData.msgID = it->msgID;
            myData.isACK = false;
            strncpy(myData.data, it->message.c_str(), sizeof(myData.data));
            esp_now_send(peerAddress, (uint8_t *)&myData, sizeof(myData));
            it->sendTime = currentTime; // 更新发送时间

            // 显示叉（×）表示失败
            char buffer[300];
            snprintf(buffer, sizeof(buffer), "发送: %s ×", it->message.c_str());
            webSocket.broadcastTXT(buffer);
            ++it;
        } else {
            ++it;
        }
    }
}

// 初始化函数
void setup() {
    Serial.begin(115200);

    // 创建WiFi AP热点
    WiFi.mode(WIFI_AP_STA);

    // WiFi.softAP("ESP32-Chat_zs", "12345678");
    WiFi.softAP("ESP32-Chat_wp", "12345678");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());

    // 初始化ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW初始化失败");
        return;
    }

    // 注册ESP-NOW回调函数
    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);

    // 添加对端设备
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, peerAddress, 6);
    peerInfo.channel = CHANNEL;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("添加对端失败");
        return;
    }

    // 启动Web服务器
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", index_html);
    });
    server.begin();

    // 启动WebSocket
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);

    Serial.println("系统初始化完成");
}

// 主循环
void loop() {
    webSocket.loop();
    checkTimeouts();
}