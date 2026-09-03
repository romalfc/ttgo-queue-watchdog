#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RadioLib.h>
#include <esp_sleep.h>
#include <driver/uart.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_RESET (-1)

// Апаратна конфігурація LoRa-модуля, яку можна перевизначити для іншої плати.
#define LORA_CS 18
#define LORA_DIO0 26
#define LORA_DIO1 33
#define LORA_RST 23
// Потужність TX у dBm: можливі приклади 2, 5, 10, 14 або 17; максимум залежить від модуля та норм регіону.
#define LORA_TX_POWER 2
#define LORA_FREQUENCY 868.0
#define LORA_BUTTON_PIN 0
#define RADIO_PACKET_SIZE 64
#define SINGLE_RADIO_PACKET_SIZE 32
// Максимальна частка ефірного часу: після передачі радіо очікує 99 тривалостей TX.
#define LORA_DUTY_CYCLE_PERCENT 1
// Після цього часу без успішної передачі пристрій переходить у light sleep.
#define DEVICE_IDLE_SLEEP_MS 60000

constexpr char SINGLE_BUTTON_COMMAND[] = "single-btn";
constexpr char DOUBLE_BUTTON_COMMAND[] = "double-btn";

// OLED-повідомлення та формати рядків зберігаються в одному місці.
constexpr char OLED_READY_MESSAGE[] = "Button radio ready";
constexpr char OLED_PACKET_STATUS_FORMAT[] = "%u byte packet %s";
constexpr char OLED_TOTAL_SENT_FORMAT[] = "Total sent: %lu";
constexpr char OLED_QUEUED_PACKETS_FORMAT[] = "Queued: %lu";
constexpr char OLED_TO_SLEEP_FORMAT[] = "To sleep: %lu s";
constexpr char OLED_LAST_TRANSMISSION_TIME_FORMAT[] = "Last TX: %lu ms";
constexpr char OLED_READY_TO_SEND_MESSAGE[] = "Ready to send";
constexpr char OLED_DUTY_CYCLE_WAIT_MESSAGE[] = "Duty cycle wait";
constexpr char OLED_NEXT_TX_FORMAT[] = "Next TX in: %lu ms";
constexpr char OLED_HIBERNATION_MESSAGE[] = "Hibernation...";
constexpr char OLED_STATUS_READY[] = "ready";
constexpr char OLED_STATUS_SENDING[] = "sending...";
constexpr char OLED_STATUS_SENT[] = "sent";
constexpr char OLED_STATUS_FAILED[] = "failed";
constexpr char SERIAL_COMMAND_TO_TX_FORMAT[] =
  "Radio task: command-to-TX delay=%lu ms\n";

// Якщо друге натискання відбулося до цього порогу, формується double-btn;
// після порогу натискання вважаються двома окремими single-btn.
#define DOUBLE_CLICK_THRESHOLD_MS 350

// Час debounce сталий, а поріг подвійного натискання можна змінити через Serial.
constexpr TickType_t BUTTON_DEBOUNCE = pdMS_TO_TICKS(40);
uint32_t doubleClickWindowMs = DOUBLE_CLICK_THRESHOLD_MS;

enum class ButtonPress : uint8_t {
  Single,
  Double
};

struct RadioPacket {
  ButtonPress type;
  char text[RADIO_PACKET_SIZE - sizeof(ButtonPress)];
};

struct ButtonEvent {
  ButtonPress press;
  uint32_t receivedAtMs;
};

struct QueuedRadioPacket {
  RadioPacket packet;
  uint32_t queuedAtMs;
};

static_assert(sizeof(RadioPacket) == RADIO_PACKET_SIZE,
              "RadioPacket must be exactly 64 bytes");
static_assert(SINGLE_RADIO_PACKET_SIZE < RADIO_PACKET_SIZE,
              "Single packet must be smaller than double packet");
static_assert(LORA_DUTY_CYCLE_PERCENT > 0 && LORA_DUTY_CYCLE_PERCENT <= 100,
              "LORA_DUTY_CYCLE_PERCENT must be between 1 and 100");

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
SX1276 radio = new Module(LORA_CS, LORA_DIO0, LORA_RST, LORA_DIO1);
QueueHandle_t buttonQueue;
QueueHandle_t radioQueue;
uint32_t sentPacketCount = 0;
uint32_t lastSuccessfulPacketMs = 0;
uint32_t lastTransmissionTimeMs = 0;
uint32_t lastWorkingDisplayMs = 0;
bool displayReady = false;
volatile bool dutyCycleWaiting = false;

// Показує на OLED поточний стан і розмір радіопакета.
uint32_t getTimeToSleepMs() {
  uint32_t idleTimeMs = millis() - lastSuccessfulPacketMs;
  if (idleTimeMs >= DEVICE_IDLE_SLEEP_MS) {
    return 0;
  }
  return DEVICE_IDLE_SLEEP_MS - idleTimeMs;
}

uint32_t getPendingPacketCount() {
  if (buttonQueue == nullptr || radioQueue == nullptr) {
    return 0;
  }
  return uxQueueMessagesWaiting(buttonQueue) + uxQueueMessagesWaiting(radioQueue);
}

void showRadioStatus(size_t packetSize, const char *status, uint32_t elapsedMs = 0) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  if (packetSize > 0) {
    display.printf(OLED_PACKET_STATUS_FORMAT, static_cast<unsigned>(packetSize), status);
  } else {
    display.println(OLED_READY_MESSAGE);
  }

  display.setCursor(0, 16);
  display.printf(OLED_TOTAL_SENT_FORMAT, static_cast<unsigned long>(sentPacketCount));

  display.setCursor(0, 24);
  display.printf(OLED_QUEUED_PACKETS_FORMAT,
                 static_cast<unsigned long>(getPendingPacketCount()));

  display.setCursor(0, 32);
  display.printf(OLED_TO_SLEEP_FORMAT,
                static_cast<unsigned long>(getTimeToSleepMs() / 1000));

  display.setCursor(0, 48);
  uint32_t displayedTransmissionTimeMs = elapsedMs > 0
      ? elapsedMs
      : lastTransmissionTimeMs;
  display.printf(OLED_LAST_TRANSMISSION_TIME_FORMAT,
                 static_cast<unsigned long>(displayedTransmissionTimeMs));

  display.display();
}

// Показує час, який залишився до дозволеної наступної передачі.
void showDutyCycleCountdown(uint32_t remainingMs) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  if (remainingMs == 0) {
    display.println(OLED_READY_TO_SEND_MESSAGE);
  } else {
    display.println(OLED_DUTY_CYCLE_WAIT_MESSAGE);
  }
  display.setCursor(0, 16);
  display.printf(OLED_TOTAL_SENT_FORMAT, static_cast<unsigned long>(sentPacketCount));
  display.setCursor(0, 24);
  display.printf(OLED_QUEUED_PACKETS_FORMAT,
                 static_cast<unsigned long>(getPendingPacketCount()));
  display.setCursor(0, 32);
  display.printf(OLED_TO_SLEEP_FORMAT,
                 static_cast<unsigned long>(getTimeToSleepMs() / 1000));
  display.setCursor(0, 40);
  display.printf(OLED_LAST_TRANSMISSION_TIME_FORMAT,
                 static_cast<unsigned long>(lastTransmissionTimeMs));
  display.setCursor(0, 52);
  display.printf(OLED_NEXT_TX_FORMAT, static_cast<unsigned long>(remainingMs));
  display.display();
}

// Переводити ESP32 у light sleep. UART0 пробуджує пристрій першим байтом команди.
void enterIdleSleep() {
  Serial.println(F("No packets for 60 seconds, entering light sleep."));
  Serial.println(F("Send single-btn or double-btn to wake the device."));

  if (displayReady) {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(16, 24);
    display.println(OLED_HIBERNATION_MESSAGE);
    display.display();
  }

  Serial.flush();

  sentPacketCount = 0;
  esp_sleep_enable_uart_wakeup(UART_NUM_0);
  uart_set_wakeup_threshold(UART_NUM_0, 3);
  esp_light_sleep_start();

  lastSuccessfulPacketMs = millis();
  lastWorkingDisplayMs = lastSuccessfulPacketMs;
  Serial.println(F("Device woke up from light sleep."));
  if (displayReady) {
    showRadioStatus(0, OLED_STATUS_READY);
  }
}
// Задача опитує кнопку, усуває дребезг і розрізняє одинарне/подвійне натискання.
void buttonTask(void *parameter) {
  (void)parameter;
  bool previousState = digitalRead(LORA_BUTTON_PIN);

  for (;;) {
    bool currentState = digitalRead(LORA_BUTTON_PIN);

    if (previousState == HIGH && currentState == LOW) {
      vTaskDelay(BUTTON_DEBOUNCE);
      if (digitalRead(LORA_BUTTON_PIN) == LOW) {
        while (digitalRead(LORA_BUTTON_PIN) == LOW) {
          vTaskDelay(pdMS_TO_TICKS(10));
        }

        vTaskDelay(BUTTON_DEBOUNCE);
        ButtonPress press = ButtonPress::Single;

        if (digitalRead(LORA_BUTTON_PIN) == HIGH) {
          TickType_t releaseTime = xTaskGetTickCount();
          while (xTaskGetTickCount() - releaseTime < pdMS_TO_TICKS(doubleClickWindowMs)) {
            if (digitalRead(LORA_BUTTON_PIN) == LOW) {
              vTaskDelay(BUTTON_DEBOUNCE);
              if (digitalRead(LORA_BUTTON_PIN) == LOW) {
                press = ButtonPress::Double;
                while (digitalRead(LORA_BUTTON_PIN) == LOW) {
                  vTaskDelay(pdMS_TO_TICKS(10));
                }
                break;
              }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
          }
        }

        ButtonEvent event{press, millis()};
        xQueueSend(buttonQueue, &event, portMAX_DELAY);
      }
    }

    previousState = currentState;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Задача ініціалізує SX1276 і відправляє пакети з radioQueue через LoRa.
void radioTask(void *parameter) {
  (void)parameter;
  int16_t status = radio.begin(LORA_FREQUENCY);
  if (status == RADIOLIB_ERR_NONE) {
    int16_t powerStatus = radio.setOutputPower(LORA_TX_POWER);
    if (powerStatus == RADIOLIB_ERR_NONE) {
      Serial.printf("Radio task: SX1276 ready, TX power=%d dBm\n", LORA_TX_POWER);
    } else {
      Serial.printf("Radio task: TX power setup failed, code %d\n", powerStatus);
    }
  } else {
    Serial.printf("Radio task: init failed, code %d\n", status);
  }

  QueuedRadioPacket queuedPacket;
  for (;;) {
    if (xQueueReceive(radioQueue, &queuedPacket, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (status != RADIOLIB_ERR_NONE) {
      Serial.println(F("Radio task: packet skipped, radio unavailable"));
      continue;
    }

    size_t packetSize = queuedPacket.packet.type == ButtonPress::Single
        ? SINGLE_RADIO_PACKET_SIZE
        : RADIO_PACKET_SIZE;
    uint32_t transmissionStart = millis();
    int16_t sendStatus = radio.transmit(
      reinterpret_cast<const uint8_t *>(&queuedPacket.packet),
        packetSize);
    uint32_t transmissionTime = millis() - transmissionStart;
    uint32_t commandToTxDelay = transmissionStart - queuedPacket.queuedAtMs;
    lastTransmissionTimeMs = transmissionTime;
    if (sendStatus == RADIOLIB_ERR_NONE) {
      ++sentPacketCount;
      lastSuccessfulPacketMs = millis();
    }
    showRadioStatus(packetSize,
            sendStatus == RADIOLIB_ERR_NONE ? OLED_STATUS_SENT : OLED_STATUS_FAILED,
                    transmissionTime);
    Serial.printf("Radio task: sent \"%s\", size=%d bytes, result=%d\n",
          queuedPacket.packet.text,
            packetSize,
            sendStatus);
        Serial.printf(SERIAL_COMMAND_TO_TX_FORMAT,
          static_cast<unsigned long>(commandToTxDelay));

    // Для duty cycle 1% пауза становить 99 тривалостей попередньої передачі.
    uint32_t quietTimeMs = transmissionTime *
        (100 - LORA_DUTY_CYCLE_PERCENT) / LORA_DUTY_CYCLE_PERCENT;
    if (quietTimeMs > 0) {
      dutyCycleWaiting = true;
      Serial.println(F("Radio task: waiting for duty cycle"));
      uint32_t waitStart = millis();
      uint32_t nextStatusUpdate = waitStart;
      uint32_t waitEnd = waitStart + quietTimeMs;

      while (static_cast<int32_t>(waitEnd - millis()) > 0) {
        uint32_t remainingMs = waitEnd - millis();
        if (static_cast<int32_t>(millis() - nextStatusUpdate) >= 0) {
          showDutyCycleCountdown(remainingMs);
          nextStatusUpdate = millis() + 250;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
      }

      showDutyCycleCountdown(0);
      Serial.println(F("Radio task: next TX allowed"));
      dutyCycleWaiting = false;
    }
  }
}

// Головна функція ініціалізує дисплей, створює черги та запускає задачі.
void setup() {
  Serial.begin(115200);
  delay(500);
  lastSuccessfulPacketMs = millis();
  Serial.println(F("Button -> Queue -> Main Loop -> Radio Task"));
  // Приклади: single-btn; double-btn; double-btn 1000, потім Enter.
  // double-btn без аргументу імітує double, а аргумент задає інтервал між натисканнями.
  Serial.printf("Serial commands: %s, %s [100..2000]\n",
                SINGLE_BUTTON_COMMAND, DOUBLE_BUTTON_COMMAND);

  pinMode(LORA_BUTTON_PIN, INPUT_PULLUP);
  buttonQueue = xQueueCreate(5, sizeof(ButtonEvent));
  radioQueue = xQueueCreate(5, sizeof(QueuedRadioPacket));

  if (buttonQueue == nullptr || radioQueue == nullptr) {
    Serial.println(F("Queue creation failed"));
    while (true) {
      delay(1000);
    }
  }

  Wire.begin(OLED_SDA, OLED_SCL);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    displayReady = true;
    showRadioStatus(0, OLED_STATUS_READY);
  }

  xTaskCreatePinnedToCore(buttonTask, "ButtonTask", 4096, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(radioTask, "RadioTask", 4096, nullptr, 1, nullptr, 0);
}

// Обробляє команди з Serial Monitor і передає їх у ту саму чергу, що й кнопка.
void processSerialCommands() {
  static char command[16];
  static uint8_t commandLength = 0;

  while (Serial.available() > 0) {
    char character = static_cast<char>(Serial.read());

    if (character == '\r') {
      continue;
    }

    if (character == '\n') {
      command[commandLength] = '\0';

      unsigned long requestedWindowMs = 0;

      if (strcmp(command, SINGLE_BUTTON_COMMAND) == 0) {
        ButtonEvent event{ButtonPress::Single, millis()};
        xQueueSend(buttonQueue, &event, 0);
        Serial.println(F("Serial: simulated single press"));
      } else if (strcmp(command, DOUBLE_BUTTON_COMMAND) == 0) {
        ButtonEvent event{ButtonPress::Double, millis()};
        xQueueSend(buttonQueue, &event, 0);
        Serial.println(F("Serial: simulated double press"));
      } else if (strncmp(command, DOUBLE_BUTTON_COMMAND,
                         strlen(DOUBLE_BUTTON_COMMAND)) == 0 &&
                 command[strlen(DOUBLE_BUTTON_COMMAND)] == ' ' &&
                 sscanf(command + strlen(DOUBLE_BUTTON_COMMAND) + 1,
                        "%lu", &requestedWindowMs) == 1) {
        if (requestedWindowMs >= 100 && requestedWindowMs <= 2000) {
          if (requestedWindowMs > doubleClickWindowMs) {
            ButtonEvent singleEvent{ButtonPress::Single, millis()};
            xQueueSend(buttonQueue, &singleEvent, 0);
            xQueueSend(buttonQueue, &singleEvent, 0);
            Serial.printf("Serial: %lu ms > %lu ms, queued two single presses\n",
                          requestedWindowMs, doubleClickWindowMs);
          } else {
            ButtonEvent doubleEvent{ButtonPress::Double, millis()};
            xQueueSend(buttonQueue, &doubleEvent, 0);
            Serial.printf("Serial: %lu ms <= %lu ms, queued one double press\n",
                          requestedWindowMs, doubleClickWindowMs);
          }
        } else {
          Serial.println(F("doubleBtn must be between 100 and 2000 ms"));
        }
      } else if (commandLength > 0) {
        Serial.printf("Use %s, %s, or %s 100..2000\n",
                      SINGLE_BUTTON_COMMAND, DOUBLE_BUTTON_COMMAND,
                      DOUBLE_BUTTON_COMMAND);
      }

      commandLength = 0;
      continue;
    }

    if (commandLength < sizeof(command) - 1) {
      command[commandLength++] = character;
    }
  }
}

// Головний цикл отримує тип натискання та формує відповідний радіопакет.
void loop() {
  processSerialCommands();

  if (uxQueueMessagesWaiting(radioQueue) == 0 &&
      millis() - lastSuccessfulPacketMs >= DEVICE_IDLE_SLEEP_MS) {
    enterIdleSleep();
    return;
  }

    if (displayReady && !dutyCycleWaiting &&
      millis() - lastWorkingDisplayMs >= 1000 &&
      uxQueueMessagesWaiting(radioQueue) == 0) {
      showRadioStatus(0, OLED_STATUS_READY);
    lastWorkingDisplayMs = millis();
  }

  ButtonEvent event;
  if (xQueueReceive(buttonQueue, &event, pdMS_TO_TICKS(20)) != pdTRUE) {
    return;
  }

  RadioPacket packet{};
  packet.type = event.press;
  const char *message = event.press == ButtonPress::Single
      ? "BUTTON SINGLE"
      : "BUTTON DOUBLE";
  strncpy(packet.text, message, sizeof(packet.text) - 1);
  packet.text[sizeof(packet.text) - 1] = '\0';

    QueuedRadioPacket queuedPacket{packet, event.receivedAtMs};
    size_t packetSize = event.press == ButtonPress::Single
      ? SINGLE_RADIO_PACKET_SIZE
      : RADIO_PACKET_SIZE;
  Serial.printf("Main loop: %s\n", packet.text);
  xQueueSend(radioQueue, &queuedPacket, portMAX_DELAY);
  showRadioStatus(packetSize, OLED_STATUS_SENDING);
}