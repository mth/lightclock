#include <esp_task_wdt.h>
#include "WiFi.h"
#include "AsyncUDP.h"
#include <lwip/netdb.h>
#include <sys/time.h>
#include "wifi-conf.h"

#define PWM_CHANNEL 0
#define PWM_FREQ 2000
#define PWM_RESOLUTION 10
#define PWM_MAX 1023

struct TimeMessage {
  int64_t request_id;
  int64_t clock_id;
  int64_t start_on;
  int64_t finish_on;
  int64_t start_off;
  int64_t finish_off;
};

AsyncUDP udp;
IPAddress server_ip;
TimeMessage config;
//int net_state = NS_NONE;

void request_light_data() {
  if (!udp.connect(CONF_SERVER, CONF_SERVER_PORT)) {
    Serial.println("Couldn't connect");
    return;
  }
  Serial.println("UDP connected");
  udp.onPacket([](AsyncUDPPacket packet) {
    TimeMessage response;
    if (packet.length() == sizeof response) {
      memcpy(&response, packet.data(), sizeof response);
      if (response.request_id == config.request_id) {
        time_t cur_t;
        time(&cur_t);
        config = response;
        if (cur_t < response.clock_id - 1 || cur_t > response.clock_id + 1) {
          timeval epoch = { response.clock_id, 0 };
          settimeofday(&epoch, 0);
          time(&cur_t);
          Serial.print("Updated time: ");
          Serial.print(ctime(&cur_t));
        } else {
          Serial.print("Got config, time don't need a update: ");
          Serial.print(ctime(&cur_t));
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
  request.clock_id = 0; // this clock
  udp.write((const uint8_t*) &request, sizeof request);
  Serial.printf("Sent request %lu\n", sizeof request);
}

void got_ip_handler(WiFiEvent_t wifi_event, WiFiEventInfo_t wifi_info) {
  digitalWrite(LED_BUILTIN, HIGH);
  struct hostent *addr = gethostbyname("veeb.valhalla");
  if (addr) {
    server_ip = IPAddress((uint8_t*) addr->h_addr_list[0]);
    char outBuf[32];
    sprintf(outBuf,"%u.%u.%u.%u\n",server_ip[0],server_ip[1],server_ip[2],server_ip[3]);
    Serial.println(outBuf);
    request_light_data();
  } else {
    Serial.println("Unknown host");
  }
}

void startWiFi();

void wifi_disconnect_handler(WiFiEvent_t wifi_event, WiFiEventInfo_t wifi_info) {
  digitalWrite(LED_BUILTIN, LOW);
}

void startWiFi() {
  static bool initialized = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  if (!initialized) {
    Serial.write("setting wifi event handlers\n");
    WiFi.onEvent(got_ip_handler, ARDUINO_EVENT_WIFI_STA_GOT_IP);
    WiFi.onEvent(wifi_disconnect_handler, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    initialized = true;
  }
  Serial.write("WiFi.begin()\n");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  //net_state = NS_CONNECT_WIFI;
}

void setup() {
  Serial.begin(115200);
  esp_task_wdt_init(10, true);
  esp_task_wdt_add(NULL);
  pinMode(LED_BUILTIN, OUTPUT);
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(18, PWM_CHANNEL);
  //ledcWrite(PWM_CHANNEL, PWM_MAX / 2);
  Serial.write("startWiFi.\n");
  startWiFi();
  Serial.write("startWifi called.\n");
}

void loop() {
  esp_task_wdt_reset();
  Serial.write("loop\n");
  for (int i = 0; i <= PWM_MAX; ++i) {
    ledcWrite(PWM_CHANNEL, i);
    delay(3);
  }
  for (int i = PWM_MAX; i >= 0; --i) {
    ledcWrite(PWM_CHANNEL, i);
    delay(3);
  }
  if (WiFi.isConnected()) {
    request_light_data();
  } else {
    startWiFi();
  }
}
