# TTGO FreeRTOS cursor task

Проєкт для ESP32 TTGO LoRa32 демонструє взаємодію двох FreeRTOS-задач через queue.

```text
Кнопка -> ButtonTask (core 1) -> blinkIntervalQueue -> CursorTask (core 0) -> OLED
Serial `radio` -> radioTransmitQueue -> RadioTask (core 0) -> SX1276
```

## Логіка

- `ButtonTask` опитує кнопку BOOT на GPIO0 і визначає одинарне або подвійне натискання.
- Після одинарного натискання інтервал змінюється по колу:
  `0,1 с -> 0,5 с -> 1 с -> 2 с -> 0,1 с`.
- Подвійне натискання змінює інтервал на один крок назад.
- Новий інтервал передається до `CursorTask` тільки через `blinkIntervalQueue`.
- `CursorTask` блимає курсором у правому нижньому куті OLED.
- `RadioTask` асинхронно передає 32-байтовий LoRa-пакет, не блокуючи `CursorTask`.

## Емуляція кнопки через Serial Monitor

Швидкість порту: `115200` baud. Команди потрібно завершувати Enter:

- `single-btn` емулює одинарне натискання.
- `double-btn` емулює подвійне натискання.
- `double-btn 1000` емулює два натискання з інтервалом 1000 мс. Для значень понад 350 мс передаються дві події `Single`, для менших або рівних 350 мс -- одна подія `Double`.
- `deadlock` запускає навмисний тест зависання mutex.
- `radio` запускає асинхронну передачу 32-байтового пакета.
- `version` виводить hash поточної збірки, параметри прошивки та кільцевий лог.
- `loglevel error|warn|info|debug` змінює мінімальний рівень повідомлень у Serial Monitor.
- `logserial on|off` вмикає або вимикає дублювання логів у Serial Monitor.

Serial-команди надходять у ту саму чергу подій, що обробляється `ButtonTask`.
Після виконання команда і поточний інтервал виводяться в Serial Monitor, наприклад:

```text
Command: single-btn; cursor blink interval: 500 ms
```

## Версія та логування

Hash збірки додається автоматично під час PlatformIO-збірки через `extra_script.py`:

```text
Build information:
  hash=1e0bcaf84ef4
  log_level=INFO
  ring_entries=8/32
Ring log:
  [1234 ms] [INFO] System started, build=1e0bcaf84ef4
```

Налаштування logger знаходяться на початку `src/main.cpp`:

```cpp
#define LOGGER_SERIAL_ENABLED 1
constexpr LogLevel LOGGER_DEFAULT_LEVEL = LogLevel::Info;
```

Кільцевий лог зберігає останні 32 записи незалежно від Serial-виводу. Кожен запис містить uptime в мілісекундах, рівень і повідомлення. Доступні рівні: `ERROR`, `WARN`, `INFO`, `DEBUG`. У Serial Monitor виводяться лише записи не нижче поточного рівня, якщо `LOGGER_SERIAL_ENABLED` або команда `logserial on` дозволяє Serial-вивід.

Команда `deadlock` запускає `DeadlockTask`, яка захоплює `displayMutex` і навмисно не звільняє його. Після цього `CursorTask` та головний цикл блокуються на цьому mutex. Для відновлення роботи потрібен reset плати.

## Watchdog

`WatchdogTask` перевіряє heartbeat `CursorTask` кожну секунду. Якщо heartbeat не оновлюється протягом 5 секунд, watchdog:

- записує причину, uptime події, час останнього heartbeat, вільну heap-пам'ять, core та reset reason у RTC-пам'ять;
- виводить короткий лог інциденту;
- перезавантажує ESP32 через `esp_restart()`.

Після перезапуску збережений звіт друкується в Serial Monitor. Для перевірки введіть `deadlock`; очікуваний перезапуск відбудеться приблизно через 5--6 секунд.

## Апаратна конфігурація

| Пристрій | Параметр | GPIO |
| --- | --- | ---: |
| OLED | SDA | 21 |
| OLED | SCL | 22 |
| OLED | RESET | -1 |
| Кнопка BOOT | вхід | 0 |
| LoRa SX1276 | CS | 18 |
| LoRa SX1276 | DIO0 | 26 |
| LoRa SX1276 | DIO1 | 33 |
| LoRa SX1276 | RESET | 23 |

OLED SSD1306 працює за адресою I2C `0x3C`.

## Збірка

```bash
pio run --environment ttgo-lora32-v2
pio run --target upload --environment ttgo-lora32-v2
```

Залежності: Adafruit SSD1306 і Adafruit GFX Library.
LoRa: 868 МГц, SF11, BW125 кГц, CR5, пакет 32 байти, duty cycle 1%.
Після кожної передачі `RadioTask` чекає 99 тривалостей попередньої передачі. Це очікування не блокує CursorTask.