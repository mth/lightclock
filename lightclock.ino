#include <esp_task_wdt.h>
#include <esp_bt.h>
#include "WiFi.h"
#include "WiFiUdp.h"
#include <sys/time.h>
#include "wifi-conf.h"

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

#ifdef NO_SERIAL
#define PRINTF(...)
#define PRINT(s)
#define FLUSH()
#else
#define PRINTF(...) Serial.printf(__VA_ARGS__)
#define PRINT(s) Serial.print(s)
#define FLUSH() Serial.flush()
#endif
#ifdef USE_DEEP_SLEEP
#define RTC_MEMORY RTC_NOINIT_ATTR
#else
#define RTC_MEMORY static
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

static WiFiUDP udp;
static IPAddress server_addr;
static int net_state = NS_NONE;
RTC_MEMORY uint32_t server_ip;
RTC_MEMORY TimeMessage config;
RTC_MEMORY time_t last_update;

static bool establish_udp_conn() {
  WiFi.setSleep(false);
  return udp.begin((esp_random() & 0x1fff) + 1);
}

static bool request_light_data() {
  if (!server_ip) {
    if (!udp.beginPacket(CONF_SERVER, CONF_SERVER_PORT)) {
      PRINT("Could not resolve " CONF_SERVER "\n");
      return false;
    }
    server_addr = udp.remoteIP();
    server_ip = (uint32_t) server_addr;
    PRINTF("SERVER %x\n", server_ip);
  } else {
    server_addr = IPAddress(server_ip);
    if (!udp.beginPacket(server_addr, CONF_SERVER_PORT)) {
      return false;
    }
  }
  struct timeval tv;
  gettimeofday(&tv, 0);
  int64_t random = esp_random() ^ ((uint64_t) tv.tv_usec << 32);
  config.request_id = random ^ (config.request_id << 16);
  TimeMessage request = config;
  request.clock_id = CLOCK_ID; // this clock
  udp.write((const uint8_t*) &request, sizeof request);
  PRINT("Sent request\n");
  return udp.endPacket();
}

static bool receive_light_data() {
  if (udp.parsePacket() <= 0) {
    return false;
  }
  if (udp.remoteIP() != server_addr || udp.remotePort() != CONF_SERVER_PORT) {
    PRINTF("Invalid packet source %x:%u\n", (uint32_t) udp.remoteIP(), (unsigned) udp.remotePort());
    udp.flush();
    return false;
  }
  TimeMessage response;
  char buf[sizeof response + 1];
  int count = udp.read(buf, sizeof buf);
  udp.flush();
  if (count != sizeof response) {
    PRINT("Invalid response length\n");
    return false;
  }
  memcpy(&response, buf, sizeof response);
  if (response.request_id != config.request_id) {
    PRINT("Invalid request_id in response\n");
    return false;
  }
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
  config = response;
  last_update = cur_t;
  return true;
}

void setup() {
#ifndef NO_SERIAL
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
    last_update = 0;
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
  if (!config.ok()) {
    PRINTF("No config for light ns=%d (%d >= %d)\n", net_state, (int) config.finish_on, (int) config.start_off);
    return 200;
  }
  PRINTF("ns=%d ", net_state);

  struct timeval tv;
  gettimeofday(&tv, 0);
  int64_t t = tv.tv_sec;
  if (t > config.finish_off) {
    t -= ((t - config.finish_off) / DAY + 1) * DAY;
  }

  int loop_time = UPDATE_POLL_MS;
  int64_t timestep = 0;
  while (loop_time > 0) {
    if (timestep > 0) {
      delay(timestep);
    }
    int brightness = 0;
    if (t < config.start_on) {
      timestep = (config.start_on - t) * 1000 - (tv.tv_usec / 1000);
      PRINTF("%lgs until start ", (double) timestep / 1000.0);
    } else if (t >= config.finish_off) {
      timestep = MAX_WAIT_MS;
      PRINT("light has already gone off ");
    } else {
      int64_t dt, total;
      if (t <= config.finish_on) {
        //PRINT("FADE-IN ");
        dt = ((t - config.start_on) * 1000) + (tv.tv_usec / 1000);
        total = (config.finish_on - config.start_on) * 1000;
        timestep = total / PWM_MAX;
      } else if (t >= config.start_off) {
        //PRINT("FADE-OFF ");
        dt = ((config.finish_off - t) * 1000) - (tv.tv_usec / 1000);
        total = (config.finish_off - config.start_off) * 1000;
        timestep = total / PWM_MAX;
      } else {
        dt = total = 1;
        timestep = (config.start_off - t) * 1000 - (tv.tv_usec / 1000);
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
  if (!config.ok() || !last_update || t > last_update + 25) {
    if (timeout_counter >= 20000 / UPDATE_POLL_MS) {
      WiFi.disconnect(true);
      net_state = NS_NONE;
      delay(1);
    } else if (net_state != NS_NONE && net_state != NS_CONNECTING && !WiFi.isConnected()) {
      net_state = NS_NONE;
    }
    switch (net_state) {
      case NS_UPDATING:
        if (receive_light_data()) {
          net_state = NS_CONNECTED;
          timeout_counter = 0;
          WiFi.setSleep(true);
        } else {
          ++timeout_counter;
        }
        break;
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
  } else if (WiFi.isConnected()) {
    while (udp.parsePacket() > 0) {
      udp.flush(); // discard unexpected packet
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
    if (wait > 10000) {
      WiFi.disconnect(true);
    }
    WiFi.setSleep(true);
    digitalWrite(LED_BUILTIN, LOW);
    esp_sleep_enable_timer_wakeup(wait * 1000);
    PRINTF("Sleep for %dms, light=%d\n", wait, light_on);
    FLUSH();
    #ifdef USE_DEEP_SLEEP
    if (wait < 25000 || light_on) {
      esp_light_sleep_start();
    } else {
      esp_deep_sleep_start();
    }
    #else
    esp_light_sleep_start();
    #endif
  }
}
