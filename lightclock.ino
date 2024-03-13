#include <esp_task_wdt.h>
#include <esp_bt.h>
#include "WiFi.h"
#include "AsyncUDP.h"
#include <lwip/netdb.h>
#include <sys/time.h>
#include "wifi-conf.h"
#include <atomic>

#define CLOCK_ID 0

#define PWM_CHANNEL 0
#define PWM_FREQ 2000
#define PWM_RESOLUTION 10
#define PWM_MAX 1023

#define DAY 86400
#define MAX_WAIT_MS 30000
#define RETRY_MS 5000
#define UPDATE_POLL_MS 200

#define NS_NONE       0
#define NS_CONNECTING 1
#define NS_CONNECTED  2
#define NS_UPDATING   3

#define SERIAL 1
#if SERIAL
#define PRINTF(...) Serial.printf(__VA_ARGS__)
#define PRINT(s) Serial.print(s)
#define FLUSH() Serial.flush()
#else
#define PRINTF(...)
#define PRINT(s)
#define FLUSH()
#endif

struct TimeMessage {
  int64_t request_id = 0;
  int64_t clock_id = 0;
  int64_t start_on = 0;
  int64_t finish_on = 0;
  int64_t start_off = 0;
  int64_t finish_off = 0;

  bool ok() {
    return finish_on < start_off;
  }
};

static const char host_name[] =
  { 'l', 'i', 'g', 'h', 't', 'c', 'l', 'o', 'c', 'k', '0' + CLOCK_ID, '\0' };

static AsyncUDP udp;
RTC_NOINIT_ATTR uint32_t server_ip;
// These fields should be accessed via getConfig/setConfig
RTC_NOINIT_ATTR TimeMessage config;
RTC_NOINIT_ATTR time_t last_conf_update;
static SemaphoreHandle_t configMutex = 0;
static std::atomic_int net_state(NS_NONE);

#define LOCK_CONF(a) while (!xSemaphoreTake(configMutex, portMAX_DELAY)) delay(1);\
  a; xSemaphoreGive(configMutex)

static TimeMessage getConfig(time_t *last_update) {
  time_t tmp;
  if (!last_update) {
    last_update = &tmp;
  }
  LOCK_CONF(TimeMessage result(config); *last_update = last_conf_update);
  return result;
}

static void setConfig(const TimeMessage& msg, time_t cur_t) {
  LOCK_CONF(config = msg; last_conf_update = cur_t);
}

static bool resolve_server_ip() {
  if (!server_ip) {
    struct hostent *addr = gethostbyname(CONF_SERVER);
    if (!addr) {
      PRINT("Unknown host\n");
      return false;
    }
    memcpy(&server_ip, addr->h_addr_list[0], 4);
    PRINTF("SERVER %x\n", server_ip);
  }
  return true;
}

static bool establish_udp_conn() {
  WiFi.setSleep(false);
  if (!resolve_server_ip() || !udp.connect(IPAddress(server_ip), CONF_SERVER_PORT)) {
    PRINT("Couldn't connect\n");
    return false;
  }
  PRINT("UDP connected. ");
  udp.onPacket([](AsyncUDPPacket packet) {
    WiFi.setSleep(true);
    net_state = NS_CONNECTED;
    TimeMessage response;
    if (packet.length() == sizeof response) {
      memcpy(&response, packet.data(), sizeof response);
      if (response.request_id == getConfig(0).request_id) {
        time_t cur_t;
        time(&cur_t);
        PRINTF("CONF s_on=%d f_on=%d s_off=%d f_off=%d\n",
               (int) response.start_on, (int) response.finish_on,
               (int) response.start_off, (int) response.finish_off);
        if (cur_t < response.clock_id - 1 ||
            cur_t > response.clock_id + 1) {
          timeval epoch = { response.clock_id, 0 };
          settimeofday(&epoch, 0);
          time(&cur_t);
          PRINTF("Updated time: %d (%d); ", (int) cur_t, (int) response.clock_id);
          PRINT(ctime(&cur_t));
        } else {
          PRINTF("Time OK %d (%d); ", (int) cur_t, (int) response.clock_id);
          PRINT(ctime(&cur_t));
        }
        setConfig(response, cur_t);
      } else {
        PRINT("Invalid request_id in response\n");
      }
    } else {
      PRINT("Invalid response length\n");
    }
  });
  return true;
}

static void request_light_data() {
  struct timeval tv;
  gettimeofday(&tv, 0);
  int64_t random = esp_random() ^ tv.tv_usec;
  LOCK_CONF(config.request_id = random ^ (config.request_id << 16);
    TimeMessage request = config);
  request.clock_id = CLOCK_ID; // this clock
  udp.write((const uint8_t*) &request, sizeof request);
  PRINTF("Sent request %lu\n", sizeof request);
}

void setup() {
  configMutex = xSemaphoreCreateMutex();
#if SERIAL
  Serial.begin(115200);
#endif
  pinMode(LED_BUILTIN, OUTPUT);
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(4, PWM_CHANNEL);
  esp_task_wdt_init(60, true);
  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();
  btStop();
  esp_bt_controller_disable();
  switch (esp_sleep_get_wakeup_cause()) {
  case ESP_SLEEP_WAKEUP_EXT0:
  case ESP_SLEEP_WAKEUP_EXT1:
  case ESP_SLEEP_WAKEUP_TIMER:
  case ESP_SLEEP_WAKEUP_TOUCHPAD:
  case ESP_SLEEP_WAKEUP_ULP:
    PRINT("Setup from deep sleep\n");
    break;
  default:
    PRINT("Setup from boot/reset\n");
    // Zero RTC variables
    server_ip = 0;
    last_conf_update = 0;
    memset(&config, 0, sizeof config);
    int i;
    for (i = 0; i < PWM_MAX / 16; ++i) {
      ledcWrite(PWM_CHANNEL, i);
      delay(2);
    }
    while (--i >= 0) {
      ledcWrite(PWM_CHANNEL, i);
      delay(2);
    }
    // reset the board with 300ms delay after poweron
    esp_sleep_enable_timer_wakeup(300000);
    esp_deep_sleep_start();
  }
}

static int update_light(int *light_on) {
  TimeMessage cfg = getConfig(0);
  if (!cfg.ok()) {
    PRINTF("No config for light ns=%d (%d >= %d)\n", net_state.load(), (int) cfg.finish_on, (int) cfg.start_off);
    return 200;
  }
  PRINTF("ns=%d ", net_state.load());

  struct timeval tv;
  gettimeofday(&tv, 0);
  int64_t t = tv.tv_sec;
  if (t > cfg.finish_off) {
    t -= ((t - cfg.finish_off) / DAY + 1) * DAY;
  }

  int loop_time = UPDATE_POLL_MS;
  int64_t timestep = 0;
  while (loop_time > 0) {
    if (timestep > 0) {
      delay(timestep);
    }
    int brightness = 0;
    if (t < cfg.start_on) {
      timestep = (cfg.start_on - t) * 1000 - (tv.tv_usec / 1000);
      PRINTF("%lgs until start ", (double) timestep / 1000.0);
    } else if (t >= cfg.finish_off) {
      timestep = MAX_WAIT_MS;
      PRINT("light has already gone off ");
    } else {
      int64_t dt, total;
      if (t <= cfg.finish_on) {
        //PRINT("FADE-IN ");
        dt = ((t - cfg.start_on) * 1000) + (tv.tv_usec / 1000);
        total = (cfg.finish_on - cfg.start_on) * 1000;
        timestep = total / PWM_MAX;
      } else if (t >= cfg.start_off) {
        //PRINT("FADE-OFF ");
        dt = ((cfg.finish_off - t) * 1000) - (tv.tv_usec / 1000);
        total = (cfg.finish_off - cfg.start_off) * 1000;
        timestep = total / PWM_MAX;
      } else {
        dt = total = 1;
        timestep = (cfg.start_off - t) * 1000 - (tv.tv_usec / 1000);
        PRINTF("FULL-ON %lgs until fade-off ", (double) timestep / 1000.0);
      }
      brightness = dt * PWM_MAX / total;
      if (brightness > PWM_MAX) {
        brightness = PWM_MAX;
      } else if (brightness < 0) {
        brightness = 0;
      }
      //PRINTF("t=%d timestep=%d brightness=%d total=%d dt=%d\n", (int) t, timestep, brightness, total, dt);
    }
    ledcWrite(PWM_CHANNEL, brightness);
    *light_on = brightness;

    if (timestep > MAX_WAIT_MS) {
      timestep = MAX_WAIT_MS;
    } else if (timestep < 3) {
      timestep = 3;
    }
    loop_time -= timestep;
    tv.tv_usec += timestep * 1000;
    t += tv.tv_usec / 1000000;
    tv.tv_usec = tv.tv_usec % 1000000;
  }
  PRINTF("t=%d wait=%d pwm=%d\n", (int) t, (int) timestep, *light_on);
  return timestep;
}

static void startWiFi() {
  PRINTF("startWifi called. ");
  digitalWrite(LED_BUILTIN, LOW);
  WiFi.setHostname(host_name);
  WiFi.mode(WIFI_STA);
  PRINTF("WiFi.begin(%s)\n", host_name);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);
}

void loop() {
  static int timeout_counter = 0;

  digitalWrite(LED_BUILTIN, HIGH);
  esp_task_wdt_reset();

  time_t t = time(NULL);
  time_t last_update;
  if (!getConfig(&last_update).ok() || !last_update || t > last_update + 25) {
    if (timeout_counter >= 20000 / UPDATE_POLL_MS) {
      WiFi.disconnect(true);
      net_state = NS_NONE;
      delay(1);
    }
    switch (net_state) {
      case NS_UPDATING:
        if (WiFi.isConnected()) {
          ++timeout_counter;
          break;
        }
      case NS_NONE:
        net_state = NS_CONNECTING;
        timeout_counter = 0;
        if (!WiFi.isConnected()) {
          startWiFi();
          break;
        }
      case NS_CONNECTING:
        if (!WiFi.isConnected() || !establish_udp_conn()) {
          ++timeout_counter;
          break;
        }
      case NS_CONNECTED:
        net_state = NS_UPDATING;
        timeout_counter = 0;
        request_light_data();
    }
  }
  digitalWrite(LED_BUILTIN, WiFi.isConnected() ? HIGH : LOW);

  int light_on = 0;
  int wait = update_light(&light_on);
  esp_task_wdt_reset();

  if (net_state == NS_CONNECTING || net_state == NS_UPDATING || t > last_update + 25) {
    if (wait > UPDATE_POLL_MS) {
      wait = UPDATE_POLL_MS;
    }
    delay(wait);
  } else if (wait < 10 || (light_on && light_on < PWM_MAX)) {
    delay(wait);
  } else {
    WiFi.setSleep(true);
    digitalWrite(LED_BUILTIN, LOW);
    esp_sleep_enable_timer_wakeup(wait * 1000);
    PRINTF("Sleep for %dms, light=%d\n", wait, light_on);
    FLUSH();
    if (wait < 25000 || light_on) {
      esp_light_sleep_start();
    } else {
      esp_deep_sleep_start();
    }
  }
}
