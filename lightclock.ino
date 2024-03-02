#include <esp_task_wdt.h>
#include <esp_bt.h>
#include "WiFi.h"
#include "AsyncUDP.h"
#include <lwip/netdb.h>
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

struct TimeMessage {
  int64_t request_id;
  int64_t clock_id;
  int64_t start_on;
  int64_t finish_on;
  int64_t start_off;
  int64_t finish_off;
};

static const char host_name[] =
  { 'l', 'i', 'g', 'h', 't', 'c', 'l', 'o', 'c', 'k', '0' + CLOCK_ID, '\0' };

AsyncUDP udp;
static IPAddress server_ip;
static TimeMessage config;
static volatile time_t last_conf_update = 0;
static volatile int net_state = NS_NONE;

static void resolve_server_ip() {
  if (!server_ip[0]) {
    struct hostent *addr = gethostbyname(CONF_SERVER);
    if (addr) {
      server_ip = IPAddress((uint8_t*) addr->h_addr_list[0]);
      char outBuf[32];
      sprintf(outBuf,"%u.%u.%u.%u\n",server_ip[0],server_ip[1],server_ip[2],server_ip[3]);
      Serial.println(outBuf);
    } else {
      Serial.println("Unknown host");
    }
  }
}

static void request_light_data() {
  WiFi.setSleep(false);
  resolve_server_ip();
  if (!udp.connect(server_ip, CONF_SERVER_PORT)) {
    Serial.println("Couldn't connect");
    WiFi.disconnect();
    net_state = NS_NONE;
    return;
  }
  Serial.println("UDP connected");
  net_state = NS_UPDATING;
  udp.onPacket([](AsyncUDPPacket packet) {
    WiFi.setSleep(true);
    net_state = NS_CONNECTED;
    TimeMessage response;
    if (packet.length() == sizeof response) {
      memcpy(&response, packet.data(), sizeof response);
      if (response.request_id == config.request_id) {
        time_t cur_t;
        time(&cur_t);
        config = response;
        if (cur_t < response.clock_id - 1 ||
            cur_t > response.clock_id + 1) {
          timeval epoch = { response.clock_id, 0 };
          settimeofday(&epoch, 0);
          time(&cur_t);
          Serial.printf("Updated time: %d (%d); ", (int) cur_t, (int) response.clock_id);
          Serial.print(ctime(&cur_t));
        } else {
          Serial.printf("Got config, time don't need a update: %d (%d); ", (int) cur_t, (int) response.clock_id);
          Serial.print(ctime(&cur_t));
        }
	last_conf_update = cur_t;
      } else {
        Serial.println("Invalid request_id in response");
      }
    } else {
      Serial.println("Invalid response length");
    }
  });
  config.request_id = rand() ^ millis() ^ (config.request_id << 16);
  TimeMessage request = config;
  request.clock_id = CLOCK_ID; // this clock
  udp.write((const uint8_t*) &request, sizeof request);
  Serial.printf("Sent request %lu\n", sizeof request);
}

static void startWiFi() {
  Serial.write("startWifi called.\n");
  net_state = NS_CONNECTING;
  digitalWrite(LED_BUILTIN, LOW);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(host_name);
  Serial.write("WiFi.begin()\n");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);
}

void setup() {
  Serial.begin(115200);
  esp_task_wdt_init(60, true);
  esp_task_wdt_add(NULL);
  pinMode(LED_BUILTIN, OUTPUT);
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(18, PWM_CHANNEL);
  btStop();
}

static int update_light(int *light_on) {
  TimeMessage cfg = config;
  struct timeval tv;
  gettimeofday(&tv, 0);
  int64_t t = tv.tv_sec;

  if (t > cfg.finish_off) {
    //t -= ((t - cfg.finish_off) / DAY + 1) * DAY;
  }

  int loop_time = 1000;
  int timestep = 0;
  while (loop_time > 0) {
    if (timestep > 0) {
      delay(timestep);
    }
    int brightness = 0;
    if (t < cfg.start_on) {
      timestep = (cfg.start_on - t) * 1000 - (tv.tv_usec / 1000);
      Serial.printf("%lgs until start\n", (double) timestep / 1000.0);
    } else if (t >= cfg.finish_off) {
      timestep = MAX_WAIT_MS;
      Serial.println("light has already gone off");
    } else {
      int dt, total;
      if (t <= cfg.finish_on) {
        //Serial.print("FADE-IN ");
        dt = ((t - cfg.start_on) * 1000) + (tv.tv_usec / 1000);
        total = (cfg.finish_on - cfg.start_on) * 1000;
        timestep = dt / PWM_MAX;
      } else if (t >= cfg.start_off) {
        //Serial.print("FADE-OFF ");
        dt = ((cfg.finish_off - t) * 1000) - (tv.tv_usec / 1000);
        total = (cfg.finish_off - cfg.start_off) * 1000;
        timestep = dt / PWM_MAX;
      } else {
        Serial.print("FULL-ON ");
        dt = total = 1;
        timestep = (cfg.start_off - t) * 1000 - (tv.tv_usec / 1000);
        Serial.printf("%lgs until fade-off ", (double) timestep / 1000.0);
      }
      brightness = dt * PWM_MAX / total;
      if (brightness > PWM_MAX) {
        brightness = PWM_MAX;
      } else if (brightness < 0) {
        brightness = 0;
      }
      //Serial.printf("t=%d timestep=%d brightness=%d total=%d dt=%d\n", (int) t, timestep, brightness, total, dt);
      if (timestep < 3) {
        timestep = 3;
      }
    }
    ledcWrite(PWM_CHANNEL, brightness);
    *light_on = brightness;

    if (timestep > MAX_WAIT_MS) {
      timestep = MAX_WAIT_MS;
    }
    loop_time -= timestep;
    tv.tv_usec += timestep * 1000;
  }
  Serial.printf("t=%d timestep=%d brightness=%d\n", (int) t, timestep, *light_on);
  return timestep;
}

void loop() {
  static int update_counter = 0;
  esp_task_wdt_reset();

  time_t t = time(NULL);
  printf("net_state=%d s_on=%d f_on=%d s_off=%d f_off=%d\n",
         net_state, (int) config.start_on, (int) config.finish_on,
         (int) config.start_off, (int) config.finish_off);
  if (!last_conf_update || t > last_conf_update + 25 || config.finish_on >= config.start_off) {
    if (net_state != NS_NONE && !WiFi.isConnected()) {
      net_state = NS_NONE;
    }
    switch (net_state) {
      case NS_NONE:
        startWiFi();
        break;
      case NS_UPDATING:
        if (++update_counter < 2000 / UPDATE_POLL_MS) {
          break;
        }
      case NS_CONNECTING:
        if (!WiFi.isConnected()) {
          break;
        }
        net_state = NS_CONNECTED;
      case NS_CONNECTED:
        update_counter = 0;
        request_light_data();
    }
  }
  digitalWrite(LED_BUILTIN, WiFi.isConnected() ? HIGH : LOW);

  int wait = MAX_WAIT_MS;
  int light_on = 0;
  if (config.finish_on < config.start_off) {
    wait = update_light(&light_on);
  }
  if (net_state == NS_CONNECTING || net_state == NS_UPDATING) {
    if (wait > UPDATE_POLL_MS) {
      wait = UPDATE_POLL_MS;
    }
    delay(wait);
  } else if (wait < 10 || (light_on && light_on < PWM_MAX)) {
    delay(wait);
  } else {
    digitalWrite(LED_BUILTIN, LOW);
    esp_sleep_enable_timer_wakeup(wait * 1000);
    Serial.printf("Going to sleep for %dms, light=%d\n", wait, light_on);
    Serial.flush();
    if (wait < 25000 || light_on) {
      esp_light_sleep_start();
    } else {
      esp_deep_sleep_start();
    }
  }
}
