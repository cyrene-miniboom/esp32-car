#include <Arduino_GFX_Library.h>
#include "WS_QMI8658.h"
#include "eye.h"        // 包含 image_data_eye
#include "angry.h"      // 包含 image_data_angry
#include "happy.h"      // 包含 image_data_happy
#include "speechless.h" // 包含 image_data_speechless
#include <WiFi.h>
#include <WebServer.h>

// --- WiFi配置 ---
const char* ssid = "Xiaomi 13";
const char* password = "61968wde";

// --- Web服务器 ---
WebServer server(80);

// --- 控制状态 ---
String currentDirection = "STOP";
bool manualControlMode = false;

// --- GFX对象和引脚定义（保持不变） ---
#define LCD_BL 20
Arduino_DataBus *bus = new Arduino_ESP32SPI(38 /* DC */, 39 /* CS */, 40 /* SCK */, 41 /* MOSI */, GFX_NOT_DEFINED /* MISO */);
Arduino_GFX *gfx = new Arduino_ST7789(bus, 42 /* RST */, 0 /* rotation */, true /* IPS */, 240 /* width */, 240 /* height */);

// --- 电机驱动引脚定义（14引脚ESP32-S3复用剩余GPIO） ---
// 选择未被LCD占用的GPIO（避免冲突）
const int AIN1 = 2;    // 左电机正转（TB6612）
const int AIN2 = 3;    // 左电机反转（TB6612）
const int PWMA = 4;    // 左电机PWM调速（TB6612）
const int BIN1 = 5;    // 右电机正转（TB6612）
const int BIN2 = 7;    // 右电机反转（TB6612）
const int PWMB = 8;    // 右电机PWM调速（TB6612）

// 电机速度参数
const int MOTOR_SPEED = 120;  // 测试速度（0-255，避免过快）
const int TEST_DURATION = 2000; // 单次旋转测试时长（ms）

// --- 姿态检测和表情参数（保持不变） ---
#define SHAKE_THRESHOLD 11.0  // 摇晃的触发阈值
#define SHAKE_DURATION_MS 400  // 摇晃的检测时间
#define BLINK_FRAME_DELAY_MS 220 // 眨眼动画帧延迟
#define BLINK_REPEATS 3          // 眨眼循环次数
const unsigned long INACTIVITY_TIMEOUT_MS = 10000; // 无操作超时
const int SPEECHLESS_THRESHOLD = 4;                // 无语表情触发次数

// --- 图片数据数组（保持不变） ---
const uint16_t* const expression_images[] = {
  image_data_eye,
  image_data_angry,
  image_data_happy,
  image_data_speechless
};
const int NUM_EXPRESSIONS = sizeof(expression_images) / sizeof(expression_images[0]);

// --- 状态变量（保持不变） ---
int current_state_index = 0;
unsigned long shake_start_time = 0;
bool trigger_locked = false;
unsigned long last_activity_time = 0;
int inactivity_blink_counter = 0;

// --- 新增：车轮测试状态变量 ---
bool motor_test_mode = false;  // 是否进入车轮测试模式
unsigned long motor_test_start = 0;  // 测试开始时间
int motor_test_step = 0;       // 测试步骤（0=未测试，1=前进，2=左转，3=右转，4=停止）

//+++ 新增：移动状态跟踪 +++
bool is_moving=false;
unsigned long movement_start_time = 0;
const unsigned long MOVEMENT_DISPLAY_DELAY = 300; // 移动开始后延迟显示开心表情

// +++ 电机控制函数（新增） +++
// 初始化电机引脚
void motorInit() {
  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);
  pinMode(PWMA, OUTPUT);
  pinMode(BIN1, OUTPUT);
  pinMode(BIN2, OUTPUT);
  pinMode(PWMB, OUTPUT);
  // 初始状态：电机停止
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, LOW);
  digitalWrite(BIN1, LOW);
  digitalWrite(BIN2, LOW);
  analogWrite(PWMA, 0);
  analogWrite(PWMB, 0);
}

// 小车前进
void carForward() {
  digitalWrite(AIN1, HIGH);
  digitalWrite(AIN2, LOW);
  analogWrite(PWMA, MOTOR_SPEED);
  digitalWrite(BIN1, HIGH);
  digitalWrite(BIN2, LOW);
  analogWrite(PWMB, MOTOR_SPEED);

  // +++ 新增：设置移动状态 +++
  is_moving = true;
  movement_start_time = millis();
  currentDirection = "FORWARD";
}

// 小车左转（原地）
void carTurnLeft() {
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, HIGH);
  analogWrite(PWMA, MOTOR_SPEED/2); // 左电机反转减速
  digitalWrite(BIN1, HIGH);
  digitalWrite(BIN2, LOW);
  analogWrite(PWMB, MOTOR_SPEED/2); // 右电机正转减速

  // +++ 新增：设置移动状态 +++
  is_moving = true;
  movement_start_time = millis();
  currentDirection = "LEFT";
}

// 小车右转（原地）
void carTurnRight() {
  digitalWrite(AIN1, HIGH);
  digitalWrite(AIN2, LOW);
  analogWrite(PWMA, MOTOR_SPEED); // 左电机正转减速
  digitalWrite(BIN1, LOW);
  digitalWrite(BIN2, HIGH);
  analogWrite(PWMB, MOTOR_SPEED); // 右电机反转减速

  // +++ 新增：设置移动状态 +++
  is_moving = true;
  movement_start_time = millis();
  currentDirection = "RIGHT";
}

// 小车后退
void carBackward() {
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, HIGH);
  analogWrite(PWMA, MOTOR_SPEED);
  digitalWrite(BIN1, LOW);
  digitalWrite(BIN2, HIGH);
  analogWrite(PWMB, MOTOR_SPEED);

  // +++ 新增：设置移动状态 +++
  is_moving = true;
  movement_start_time = millis();
  currentDirection = "BACKWARD";
}


// 小车停止
void carStop() {
  digitalWrite(AIN1, LOW);
  digitalWrite(AIN2, LOW);
  digitalWrite(BIN1, LOW);
  digitalWrite(BIN2, LOW);
  analogWrite(PWMA, 0);
  analogWrite(PWMB, 0);

  // +++ 新增：设置移动状态 +++
  is_moving = false;
  currentDirection = "STOP";

  // 停止时恢复原来的表情
  gfx->draw16bitRGBBitmap(0, 0, expression_images[current_state_index], 240, 240);
}

// +++ 车轮自动测试函数（新增） ---
// 循环执行：前进→停止→左转→停止→右转→停止
void autoMotorTest() {
  if (!motor_test_mode) {
    // 首次进入测试模式，初始化
    motor_test_mode = true;
    motor_test_step = 1;
    motor_test_start = millis();
    Serial.println("Motor test start! Step 1: Forward");
    carForward();
    return;
  }

  // 根据步骤和时间切换测试状态
  if (millis() - motor_test_start >= TEST_DURATION) {
    carStop();
    delay(500); // 停止间隔
    motor_test_step++;

    switch (motor_test_step) {
      case 2:
        Serial.println("Step 2: Turn Left");
        carTurnLeft();
        break;
      case 3:
        Serial.println("Step 3: Turn Right");
        carTurnRight();
        break;
      case 4:
        Serial.println("Motor test end!");
        carStop();
        motor_test_mode = false; // 测试结束，退出模式
        motor_test_step = 0;
        break;
      default:
        motor_test_mode = false;
        break;
    }
    motor_test_start = millis(); // 重置步骤时间
  }
}

// +++ 表情相关函数+++
void play_blink_animation() {
  const uint16_t* open_eye_image;
  if (current_state_index >= NUM_EXPRESSIONS) {
    open_eye_image = image_data_eye;
  } else {
    open_eye_image = expression_images[current_state_index];
  }

  for (int i = 0; i < BLINK_REPEATS; i++) {
    gfx->draw16bitRGBBitmap(0, 0, image_data_speechless, 240, 240);
    delay(BLINK_FRAME_DELAY_MS);
    gfx->draw16bitRGBBitmap(0, 0, open_eye_image, 240, 240);
    delay(BLINK_FRAME_DELAY_MS);
  }
}

void play_blink_animation_custom(int repeats, const uint16_t* open_eye_image) {
  for (int i = 0; i < repeats; i++) {
    gfx->draw16bitRGBBitmap(0, 0, image_data_speechless, 240, 240);
    delay(BLINK_FRAME_DELAY_MS);
    gfx->draw16bitRGBBitmap(0, 0, open_eye_image, 240, 240);
    delay(BLINK_FRAME_DELAY_MS);
  }
}

void setupWiFi() {
  Serial.print("Connecting to ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to WiFi!");
  }
}

void setupWebServer() {
  // 网页请求处理
  server.on("/", HTTP_GET, handleRoot);
  server.on("/control", HTTP_GET, handleControl);
  server.on("/status", HTTP_GET, handleStatus);
  
  server.begin();
  Serial.println("HTTP server started");
}

void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32-S3 Robot Control</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; text-align: center; margin: 0; padding: 20px; }
    .control-panel { max-width: 300px; margin: 0 auto; }
    .btn { 
      width: 80px; height: 80px; font-size: 20px; margin: 5px; 
      border: none; border-radius: 10px; background: #4CAF50; color: white;
    }
    .btn:active { background: #45a049; }
    .stop-btn { background: #f44336; }
    .stop-btn:active { background: #da190b; }
    .control-row { display: flex; justify-content: center; }
    .status { margin: 20px 0; padding: 10px; background: #f0f0f0; border-radius: 5px; }
  </style>
</head>
<body>
  <h1>Robot Control</h1>
  <div class="status">
    <div>Status: <span id="status">STOP</span></div>
    <div>Mode: <span id="mode">AUTO</span></div>
  </div>
  
  <div class="control-panel">
    <div class="control-row">
      <button class="btn" onclick="control('FORWARD')">↑</button>
    </div>
    <div class="control-row">
      <button class="btn" onclick="control('LEFT')">←</button>
      <button class="btn stop-btn" onclick="control('STOP')">STOP</button>
      <button class="btn" onclick="control('RIGHT')">→</button>
    </div>
    <div class="control-row">
      <button class="btn" onclick="control('BACKWARD')">↓</button>
    </div>
  </div>

  <script>
    function control(cmd) {
      fetch('/control?cmd=' + cmd)
        .then(response => response.text())
        .then(data => {
          document.getElementById('status').textContent = cmd;
          document.getElementById('mode').textContent = 'MANUAL';
        });
    }
    
    // 自动更新状态
    setInterval(() => {
      fetch('/status')
        .then(response => response.text())
        .then(data => {
          if(data !== 'MANUAL') {
            document.getElementById('status').textContent = data;
            document.getElementById('mode').textContent = 'AUTO';
          }
        });
    }, 1000);
  </script>
</body>
</html>
)rawliteral";
  
  server.send(200, "text/html", html);
}

void handleControl() {
  String command = server.arg("cmd");
  manualControlMode = true;
  currentDirection = command;
  
  // 执行控制命令
  if (command == "FORWARD") {
    carForward();
    Serial.println("Manual: FORWARD");
  } else if (command == "BACKWARD") {
    // 需要添加后退函数
    carBackward();
    Serial.println("Manual: BACKWARD");
  } else if (command == "LEFT") {
    carTurnLeft();
    Serial.println("Manual: LEFT");
  } else if (command == "RIGHT") {
    carTurnRight();
    Serial.println("Manual: RIGHT");
  } else if (command == "STOP") {
    carStop();
    Serial.println("Manual: STOP");
  }
  
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  if (manualControlMode) {
    server.send(200, "text/plain", "MANUAL");
  } else {
    server.send(200, "text/plain", currentDirection);
  }
}

void setup() {
  Serial.begin(115200);
  QMI8658_Init();

  // --- 新增：初始化电机 ---
  motorInit();

  // 初始化LCD（保持不变）
  gfx->begin();
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);
  
  gfx->draw16bitRGBBitmap(0, 0, expression_images[current_state_index], 240, 240);
  last_activity_time = millis();

  // 设置WiFi和Web服务器
  setupWiFi();
  setupWebServer();

  // 打印测试提示
  Serial.println("System ready!");
  Serial.println("Shake to change expression.");
  Serial.println("Send 'test' via Serial to start motor test.");
  Serial.println("Web control available at http://" + WiFi.localIP().toString());
}

void loop() {
  // 处理Web客户端请求
  server.handleClient();

  // --- 新增：串口触发车轮测试（手动模式） ---
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "test") {
      Serial.println("Manual motor test triggered!");
      manualControlMode = false; // 退出手动模式
      autoMotorTest(); // 启动自动测试
      last_activity_time = millis(); // 重置无操作计时器
      inactivity_blink_counter = 0;
    }
  }

  // +++ 新增：移动状态检测和表情显示 +++
  if (is_moving && (millis() - movement_start_time >= MOVEMENT_DISPLAY_DELAY)) {
    // 小车移动中且超过延迟时间，显示开心表情
    gfx->draw16bitRGBBitmap(0, 0, image_data_happy, 240, 240);
  }

  if(!manualControlMode){
    // --- 姿态检测和表情逻辑 ---
  float accelX = QMI8658_get_A_fx();
  float accelY = QMI8658_get_A_fy();
  float accelZ = QMI8658_get_A_fz();
  float magnitude = sqrt(accelX * accelX + accelY * accelY + accelZ * accelZ);
  bool is_shaking_now = (magnitude > SHAKE_THRESHOLD);

  if (is_shaking_now) {
    if (shake_start_time == 0) {
      shake_start_time = millis();
    }

    if (!trigger_locked && (millis() - shake_start_time >= SHAKE_DURATION_MS)) {
      current_state_index++;

      if (current_state_index < NUM_EXPRESSIONS) {
        if (expression_images[current_state_index] == image_data_happy) {
          gfx->draw16bitRGBBitmap(0, 0, image_data_happy, 240, 240);
          delay(1500);
          play_blink_animation_custom(2, image_data_eye);
          current_state_index = 0;
          gfx->draw16bitRGBBitmap(0, 0, image_data_eye, 240, 240);
        } else {
          gfx->draw16bitRGBBitmap(0, 0, expression_images[current_state_index], 240, 240);
        }
      } else {
        play_blink_animation();
        current_state_index = 0;
        gfx->draw16bitRGBBitmap(0, 0, expression_images[current_state_index], 240, 240);
      }

      last_activity_time = millis();
      inactivity_blink_counter = 0;
      trigger_locked = true;
    }
  } else {
    shake_start_time = 0;
    trigger_locked = false;

    if (millis() - last_activity_time > INACTIVITY_TIMEOUT_MS) {
      inactivity_blink_counter++;
      if (inactivity_blink_counter >= SPEECHLESS_THRESHOLD) {
        gfx->draw16bitRGBBitmap(0, 0, image_data_speechless, 240, 240);
      } else {
        play_blink_animation();
      }
      last_activity_time = millis();
    }
  }
}
  

  // --- 新增：车轮测试模式运行（不影响表情显示） ---
  if (motor_test_mode) {
    autoMotorTest();
  }

  delay(100);
}