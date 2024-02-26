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

static const char host_name =
  { 'l', 'i', 'g', 'h', 't', 'c', 'l', 'o', 'c', 'k', '0' + CLOCK_ID, '\0' };

AsyncUDP udp;
IPAddress server_ip;
TimeMessage config;
time_t last_conf_update = 0;
volatile atomic_int net_state = NS_NONE;

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
        time(&last_conf_update);
        config = response;
        if (last_conf_update < response.clock_id - 1 ||
	    last_conf_update > response.clock_id + 1) {
          timeval epoch = { response.clock_id, 0 };
          settimeofday(&epoch, 0);
          time(&last_conf_update);
          Serial.print("Updated time: ");
          Serial.print(ctime(&last_conf_update));
        } else {
          Serial.print("Got config, time don't need a update: ");
          Serial.print(ctime(&last_conf_update));
        }
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
  static bool initialized = false;

  Serial.write("startWifi called.\n");
  net_state = NS_CONNECTING;
  digitalWrite(LED_BUILTIN, LOW);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setHostName(host_name);
  if (!initialized) {
    Serial.write("setting wifi event handlers\n");
    WiFi.onEvent(got_ip_handler, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent(wifi_disconnect_handler, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    initialized = true;
  }
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
  struct timeval tv;
  gettimeofday(&tv);
  int64_t t = tv.tv_sec;
  if (t > config.finish_off) {
    t -= ((t - config.finish_off) / DAY + 1) * DAY;
  }

  int loop_time = 1000;
  int timestep = 0;
  while (loop_time > 0) {
    if (timestep > 0) {
      delay(timestep);
    }
    int brightness = 0;
    if (t < config.start_on) {
      timestep = config.start_on - t;
    } else if (t > config.start_off) {
      timestep = MAX_WAIT_MS;
    } else {
      unsigned dt, tl;
      if (t <= config.finish_on) {
	dt = ((t - start_on) * 1000) + (tv.tv_sec / 1000);
	tl = (finish_on - start_on) * 1000;
        time_step = dt / PWM_MAX; 
      } else if (t > config.start_off) {
	dt = ((finish_off - t) * 1000) - (tv.tv_sec / 1000);
	tl = (finish_off - start_off) * 1000;
        time_step = dt / PWM_MAX; 
      } else {
	dt = tl = 1;
	time_step = config.start_off - t;
      }
      brightness = tl * PWM_MAX / dt;
      if (time_step < 3) {
	time_step = 3;
      }
    }
    ledcWrite(PWM_CHANNEL, brightness);
    *light_on = brightness;

    if (timestep > MAX_WAIT_MS) {
      timestep = MAX_WAIT_MS;
    }
    loop_time -= timestep;
    tv.tv_sec += timestep * 1000;
  }
  return timestep;
}

void loop() {
  esp_task_wdt_reset();

  time_t t = time(NULL);
  if (t > last_conf_update + 30) {
    if (net_state != NS_NONE && !WiFi.isConnected()) {
      net_state = NS_NONE;
    }
    switch (net_state) {
      case NS_NONE:
        startWiFi();
	break;
      case NS_CONNECTING:
        if (!WiFi.isConnected()) {
	  break;
	}
	net_state = NS_CONNECTED;
      case NS_CONNECTED:
        request_light_data();
	break;
    }
  }
  digitalWrite(LED_BUILTIN, WiFi.isConnected() ? HIGH : LOW);

  int wait = MAX_WAIT_MS;
  int light_on = 0;
  if (config.finish_on < config.start_off) {
    wait = update_light(&light_on);
  }
  if (wait < 10 || net_state == NS_CONNECTING || net_state = NS_UPDATING) {
    if (wait > 1000) {
      wait = 1000;
    }
    delay(wait);
  } else {
    digitalWrite(LED_BUILTIN, LOW);
    esp_sleep_enable_timer_wakeup(wait * 1000);
    Serial.println("Going to sleep");
    Serial.flush();
    if (wait < 30000 || light_on) {
      esp_light_sleep_start();
    } else {
      esp_deep_sleep_start();
    }
  }
}
