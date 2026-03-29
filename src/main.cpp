#include <Arduino.h>
#include <ArduinoJson.h>
#include <OneButton.h>
#include <WebSocketsClient.h>
#include <HTTPUpdateServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <U8g2lib.h>
#include <LittleFS.h>

#include <vector>
#include <map>
#include <WiFi.h>
#include <esp_idf_version.h>
#include <esp_wifi.h>

#include <WebServer.h>
#include <ESPmDNS.h>

#include <WiFiManager.h>
#include <TimeLib.h>

#include "main.h"

using namespace std;

String serverUrl = "";
WebSocketsClient webSocket;
String statusStr;
int64_t lostConnectMs = 0;
// Track when we started attempting to (re)connect to the current server
int64_t connectAttemptStartMs = 0;

HTTPUpdateServer updateServer;
bool otaUpdating = false;

// OTA progress tracking for LCD display
uint8_t otaProgress = 0;
String otaStatusMsg = "";

// Support multiple websocket server configurations
std::vector<String> serverList = { serverUrl };
size_t currentServerIndex = 0;
uint32_t pendingRestartAtMs = 0;

String hw_ver = "1.2";
String sw_ver = "1.3";

const string ntpServerName = "ntp6.aliyun.com";
const int timeZone = 8;
unsigned int localPort = 8000;

constexpr bool kWifiDisablePowerSave = true;
constexpr bool kWifiForceHt20 = true;
constexpr wifi_power_t kWifiTxPower = WIFI_POWER_20dBm;
constexpr uint32_t kWifiConnectTimeoutMs = 20000;

enum class WiFiBandPreference : uint8_t
{
	Auto,
	Only2G,
	Only5G
};

WiFiUDP Udp;
WebServer server(80);

uint8_t lcdRotation = 2;
uint8_t backlightPwm = 50; // bigger = brighter

// Auto backlight control
bool autoBacklightEnabled = false;
uint16_t backlightOnTime = 7 * 60;	 // 7:00 in minutes since midnight
uint16_t backlightOffTime = 22 * 60; // 22:00 in minutes since midnight
bool backlightOn = true;			 // current state

U8G2_ST7567_OS12864_F_4W_SW_SPI u8g2(U8G2_R2, DIS_SCK, DIS_SDA, U8X8_PIN_NONE, DIS_DC, DIS_RST); // works u8g2.setContrast(20);
// U8G2_ST7567_ERC12864_F_4W_SW_SPI  u8g2(U8G2_R2, DIS_SCK, DIS_SDA, U8X8_PIN_NONE, DIS_DC, DIS_RST);	//works u8g2.setContrast(20);

std::vector<OneButton> buttons = {
	OneButton(KEY_B1),
	OneButton(KEY_B2)};

void ButtonEventsAttach();
void Webconfig();
void SetLcdRotation(int val);
void DisplayStatus();
void DisplayOTAStatus();
time_t GetNtpTime();
void SendNTPpacket(IPAddress &address);
void DateTime();

void saveParamCallback();
void WebSeverInit();
String GetESPMACAddress();
void ConfigureWiFiRadio();
void LogWiFiLinkState(const char *stage);
void WiFiEventHandler(arduino_event_id_t event, arduino_event_info_t info);
const char *WiFiBandPreferenceKey(WiFiBandPreference pref);
const char *WiFiBandPreferenceLabel(WiFiBandPreference pref);
WiFiBandPreference WiFiBandPreferenceFromString(const String &value);
String BuildWiFiBandPreferenceHtml(const char *fieldName);
String HtmlEscape(const String &value);
void ApplyBacklightState(bool turnOn);
bool ShouldBacklightBeOnAt(uint16_t currentMins);
void RefreshBacklightState();

// web config
WiFiManager wifiMgr;

// 如开启WEB配网则可不用设置这里的参数，前一个为wifi ssid，后一个为密码
WifiConfig_t wificonf = {{""}, {""}};
WiFiBandPreference wifiBandPreference = WiFiBandPreference::Only5G;

static const char *WiFiBandName(wifi_band_t band)
{
	switch (band)
	{
	case WIFI_BAND_5G:
		return "5G";
	case WIFI_BAND_2G:
	default:
		return "2.4G";
	}
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
static const char *WiFiBandModeName(wifi_band_mode_t mode)
{
	switch (mode)
	{
	case WIFI_BAND_MODE_2G_ONLY:
		return "2.4G only";
	case WIFI_BAND_MODE_5G_ONLY:
		return "5G only";
	case WIFI_BAND_MODE_AUTO:
	default:
		return "auto";
	}
}
#endif

const char *WiFiBandPreferenceKey(WiFiBandPreference pref)
{
	switch (pref)
	{
	case WiFiBandPreference::Only2G:
		return "2g";
	case WiFiBandPreference::Only5G:
		return "5g";
	case WiFiBandPreference::Auto:
	default:
		return "auto";
	}
}

const char *WiFiBandPreferenceLabel(WiFiBandPreference pref)
{
	switch (pref)
	{
	case WiFiBandPreference::Only2G:
		return "2.4G only";
	case WiFiBandPreference::Only5G:
		return "5G only";
	case WiFiBandPreference::Auto:
	default:
		return "auto";
	}
}

WiFiBandPreference WiFiBandPreferenceFromString(const String &value)
{
	if (value == "2g")
		return WiFiBandPreference::Only2G;
	if (value == "5g")
		return WiFiBandPreference::Only5G;
	return WiFiBandPreference::Auto;
}

String BuildWiFiBandPreferenceHtml(const char *fieldName)
{
	String html = "<label>WiFi Band:</label>";
	html += "<input type='radio' name='";
	html += fieldName;
	html += "' value='5g'";
	if (wifiBandPreference == WiFiBandPreference::Only5G)
		html += " checked";
	html += "> 5G only<br>";

	html += "<input type='radio' name='";
	html += fieldName;
	html += "' value='2g'";
	if (wifiBandPreference == WiFiBandPreference::Only2G)
		html += " checked";
	html += "> 2.4G only<br>";

	html += "<input type='radio' name='";
	html += fieldName;
	html += "' value='auto'";
	if (wifiBandPreference == WiFiBandPreference::Auto)
		html += " checked";
	html += "> Auto<br>";

	return html;
}

String HtmlEscape(const String &value)
{
	String out;
	out.reserve(value.length() + 16);
	for (size_t i = 0; i < value.length(); ++i)
	{
		switch (value[i])
		{
		case '&':
			out += F("&amp;");
			break;
		case '<':
			out += F("&lt;");
			break;
		case '>':
			out += F("&gt;");
			break;
		case '"':
			out += F("&quot;");
			break;
		case '\'':
			out += F("&#39;");
			break;
		default:
			out += value[i];
			break;
		}
	}
	return out;
}

void ApplyBacklightState(bool turnOn)
{
	backlightOn = turnOn;
	analogWrite(DIS_BL, turnOn ? (255 - backlightPwm) : 255);
}

bool ShouldBacklightBeOnAt(uint16_t currentMins)
{
	if (backlightOnTime == backlightOffTime)
	{
		// Treat identical times as "always on" to avoid an accidental full-day blackout.
		return true;
	}

	if (backlightOnTime < backlightOffTime)
	{
		return currentMins >= backlightOnTime && currentMins < backlightOffTime;
	}

	return currentMins >= backlightOnTime || currentMins < backlightOffTime;
}

void RefreshBacklightState()
{
	if (autoBacklightEnabled && timeStatus() == timeSet)
	{
		time_t nowTime = ::now();
		uint16_t currentMins = (hour(nowTime) * 60) + minute(nowTime);
		bool shouldBeOn = ShouldBacklightBeOnAt(currentMins);
		if (shouldBeOn != backlightOn)
		{
			Serial.printf(
				"[Backlight] auto -> %s at %02u:%02u (on=%02u:%02u off=%02u:%02u)\n",
				shouldBeOn ? "ON" : "OFF",
				currentMins / 60,
				currentMins % 60,
				backlightOnTime / 60,
				backlightOnTime % 60,
				backlightOffTime / 60,
				backlightOffTime % 60);
			ApplyBacklightState(shouldBeOn);
		}
		return;
	}

	if (!autoBacklightEnabled)
	{
		// In manual mode, always re-apply the configured brightness so a value of 0
		// can immediately turn the backlight off without waiting for a state flip.
		ApplyBacklightState(true);
	}
}

// Helper function to convert minutes since midnight to HH:MM string
String minutesToTimeString(uint16_t minutes)
{
	uint16_t hours = minutes / 60;
	uint16_t mins = minutes % 60;
	char buf[6];
	sprintf(buf, "%02d:%02d", hours, mins);
	return String(buf);
}

// Helper function to convert HH:MM string to minutes since midnight
uint16_t timeStringToMinutes(const String &timeStr)
{
	int colonPos = timeStr.indexOf(':');
	if (colonPos > 0)
	{
		int hh = timeStr.substring(0, colonPos).toInt();
		int mm = timeStr.substring(colonPos + 1).toInt();
		return hh * 60 + mm;
	}
	return 0;
}

void LogWiFiLinkState(const char *stage)
{
	Serial.printf(
		"[WiFi] %s | status=%d ssid=%s bssid=%s channel=%ld band=%s rssi=%d tx=%d sleep=%d\n",
		stage,
		WiFi.status(),
		WiFi.SSID().c_str(),
		WiFi.BSSIDstr().c_str(),
		(long)WiFi.channel(),
		WiFiBandName(WiFi.getBand()),
		WiFi.isConnected() ? WiFi.RSSI() : 0,
		(int)WiFi.getTxPower(),
		(int)WiFi.getSleep());
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
	Serial.printf("[WiFi] %s | band mode=%s\n", stage, WiFiBandModeName(WiFi.getBandMode()));
#endif
}

void WiFiEventHandler(arduino_event_id_t event, arduino_event_info_t info)
{
	switch (event)
	{
	case ARDUINO_EVENT_WIFI_STA_CONNECTED:
		Serial.printf(
			"[WiFi] STA connected | channel=%u auth=%u\n",
			info.wifi_sta_connected.channel,
			info.wifi_sta_connected.authmode);
		LogWiFiLinkState("STA connected");
		break;
	case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
		Serial.printf(
			"[WiFi] STA disconnected | reason=%u (%s)\n",
			info.wifi_sta_disconnected.reason,
			WiFi.STA.disconnectReasonName((wifi_err_reason_t)info.wifi_sta_disconnected.reason));
		break;
	case ARDUINO_EVENT_WIFI_STA_GOT_IP:
		LogWiFiLinkState("GOT_IP");
		break;
	default:
		break;
	}
}

void ConfigureWiFiRadio()
{
	WiFi.onEvent(WiFiEventHandler);
	WiFi.mode(WIFI_STA);

	WiFi.setAutoReconnect(true);
	WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
	WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

	if (kWifiDisablePowerSave)
	{
		WiFi.setSleep(false);
	}

	if (!WiFi.setTxPower(kWifiTxPower))
	{
		Serial.println("[WiFi] failed to set TX power");
	}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2) && SOC_WIFI_SUPPORT_5G
	wifi_band_mode_t bandMode = WIFI_BAND_MODE_AUTO;
	switch (wifiBandPreference)
	{
	case WiFiBandPreference::Only2G:
		bandMode = WIFI_BAND_MODE_2G_ONLY;
		break;
	case WiFiBandPreference::Only5G:
		bandMode = WIFI_BAND_MODE_5G_ONLY;
		break;
	case WiFiBandPreference::Auto:
	default:
		bandMode = WIFI_BAND_MODE_AUTO;
		break;
	}
	if (!WiFi.setBandMode(bandMode))
	{
		Serial.printf("[WiFi] failed to set band mode to %s\n", WiFiBandPreferenceLabel(wifiBandPreference));
	}
#endif

	if (kWifiForceHt20 && !WiFi.STA.bandwidth(WIFI_BW_HT20))
	{
		Serial.println("[WiFi] failed to force HT20 bandwidth");
	}

	LogWiFiLinkState("radio configured");
}

// Save run-time configuration (wifi, backlight, rotation, servers) as JSON into littlefs
void saveConfig()
{
	DynamicJsonDocument doc(1024);
	doc["ssid"] = String(wificonf.stassid);
	doc["psw"] = String(wificonf.stapsw);
	doc["wifiBand"] = WiFiBandPreferenceKey(wifiBandPreference);
	doc["backlight"] = backlightPwm;
	doc["rotation"] = lcdRotation;
	doc["currentIdx"] = (int)currentServerIndex;
	doc["autoBacklight"] = autoBacklightEnabled;
	doc["onTime"] = minutesToTimeString(backlightOnTime);
	doc["offTime"] = minutesToTimeString(backlightOffTime);
	JsonArray arr = doc.createNestedArray("servers");
	for (auto &s : serverList)
		arr.add(s);

	String json;
	serializeJson(doc, json);

	File file = LittleFS.open("/config.json", "w");
	if (!file)
	{
		Serial.println("Failed to open /config.json for writing");
		return;
	}
	file.print(json);
	file.close();
	Serial.println("Config saved to littlefs:/config.json");
}

// Load configuration JSON from littlefs into runtime variables
void loadConfig()
{
	if (!LittleFS.exists("/config.json"))
	{
		Serial.println("No config found in littlefs");
		return;
	}

	File file = LittleFS.open("/config.json", "r");
	if (!file)
	{
		Serial.println("Failed to open /config.json for reading");
		return;
	}

	std::string buf;
	while (file.available())
	{
		buf.push_back((char)file.read());
	}
	file.close();

	if (buf.empty())
	{
		Serial.println("Config file is empty");
		return;
	}

	DynamicJsonDocument doc(1024);
	DeserializationError err = deserializeJson(doc, buf.c_str());
	if (err)
	{
		Serial.print("Failed to parse config JSON: ");
		Serial.println(err.c_str());
		return;
	}

	// wifi
	if (doc.containsKey("ssid"))
	{
		String s = doc["ssid"].as<String>();
		s.toCharArray(wificonf.stassid, sizeof(wificonf.stassid));
	}
	if (doc.containsKey("psw"))
	{
		String p = doc["psw"].as<String>();
		p.toCharArray(wificonf.stapsw, sizeof(wificonf.stapsw));
	}
	if (doc.containsKey("wifiBand"))
		wifiBandPreference = WiFiBandPreferenceFromString(doc["wifiBand"].as<String>());
	if (doc.containsKey("backlight"))
		backlightPwm = doc["backlight"].as<int>();
	if (doc.containsKey("rotation"))
		lcdRotation = doc["rotation"].as<int>();

	// Load auto-backlight settings
	if (doc.containsKey("autoBacklight"))
		autoBacklightEnabled = doc["autoBacklight"].as<bool>();
	if (doc.containsKey("onTime"))
	{
		String onTimeStr = doc["onTime"].as<String>();
		backlightOnTime = timeStringToMinutes(onTimeStr);
	}
	if (doc.containsKey("offTime"))
	{
		String offTimeStr = doc["offTime"].as<String>();
		backlightOffTime = timeStringToMinutes(offTimeStr);
	}

	serverList.clear();
	if (doc.containsKey("servers") && doc["servers"].is<JsonArray>())
	{
		for (auto v : doc["servers"].as<JsonArray>())
		{
			serverList.push_back(v.as<String>());
		}
	}
	if (serverList.empty())
	{
		serverList.push_back(serverUrl); // fallback
	}
	if (doc.containsKey("currentIdx"))
	{
		int idx = doc["currentIdx"].as<int>();
		if (idx >= 0 && (size_t)idx < serverList.size())
			currentServerIndex = idx;
	}

	Serial.println("Config loaded from littlefs:");
	Serial.print("SSID:");
	Serial.println(wificonf.stassid);
	Serial.print("WiFi band pref:");
	Serial.println(WiFiBandPreferenceLabel(wifiBandPreference));
	Serial.print("Backlight:");
	Serial.println(backlightPwm);
	Serial.print("Rotation:");
	Serial.println(lcdRotation);
	Serial.print("Servers:");
	for (auto &s : serverList)
	{
		Serial.print(" ");
		Serial.print(s);
	}
	Serial.println("");
}

bool WebsocketBegin(String url)
{
	if (url.length() > 4 && url.indexOf(':') != -1 && url.indexOf('/'))
	{
		Serial.print("WebSocket Config: ");
		Serial.println(url);

		String web_ws_server;
		String web_ws_port;
		String web_ws_path;
		int colonIndex = url.indexOf(':');
		int slashIndex = url.indexOf('/', colonIndex + 1);

		if (colonIndex != -1 && slashIndex != -1)
		{
			web_ws_server = url.substring(0, colonIndex);			 // 提取地址
			web_ws_port = url.substring(colonIndex + 1, slashIndex); // 提取端口
			web_ws_path = url.substring(slashIndex);				 // 提取路径

			Serial.print("WebSocket Port: ");
			Serial.println(web_ws_port);
			Serial.print("WebSocket Path: ");
			Serial.println(web_ws_path);

			webSocket.begin(web_ws_server.c_str(), web_ws_port.toInt(), web_ws_path.c_str());

			return true;
		}
	}
	return false;
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
	switch (type)
	{
	case WStype_DISCONNECTED:
		Serial.println("WebSocket Disconnected");
		break;
	case WStype_CONNECTED:
		Serial.println("WebSocket Connected");
		break;
	case WStype_TEXT:
		Serial.printf("WebSocket Message: %s\n", payload);
		statusStr = String((char *)payload);
		break;
	case WStype_BIN:
		Serial.printf("WebSocket Binary Message of length: %u\n", length);
		break;
	}
}

void SetLcdRotation(int val)
{
	auto rotation = U8G2_R0;
	switch (lcdRotation)
	{
	case 1:
		rotation = U8G2_R0;
		break;
	case 2:
		rotation = U8G2_R1;
		break;
	case 3:
		rotation = U8G2_R2;
		break;
	case 4:
		rotation = U8G2_R3;
		break;

	default:
		break;
	}
	if (val != 0)
	{
		u8g2.setDisplayRotation(rotation);
	}
	else
	{
		Serial.println("Rotation set error");
	}
}

void setup()
{
	Serial.begin(115200);
	Serial.println("Starting...");
	pinMode(LED, OUTPUT);
	digitalWrite(LED, HIGH); // LED ON

	// Initialize littlefs
	if (!LittleFS.begin())
	{
		Serial.println("littlefs mount failed");
	}
	else
	{
		Serial.println("littlefs mounted successfully");
	}

	// load full JSON config (wifi, backlight, rotation, servers)
	loadConfig();

	u8g2.begin();
	u8g2.setContrast(15); // 对比度
	// u8g2.setDisplayRotation(U8G2_R2);
	u8g2.setFont(u8g2_font_ncenB08_tf);
	u8g2.drawStr(15, 20, "PC Monitor");

	u8g2.setFont(u8g2_font_siji_t_6x10);
	u8g2.drawStr(15, 36, String(String("Hw: ") + hw_ver + String(", Sw: ") + sw_ver).c_str());
	u8g2.drawStr(15, 48, "Starting...");
	u8g2.sendBuffer(); // transfer internal memory to the display

	ApplyBacklightState(true);

	Serial.print("WIFI Connecting...");
	Serial.println(wificonf.stassid);
	ConfigureWiFiRadio();
	WiFi.begin(wificonf.stassid, wificonf.stapsw);

	uint32_t wifiStartMs = millis();
	while (WiFi.status() != WL_CONNECTED && (millis() - wifiStartMs) < kWifiConnectTimeoutMs)
	{
		delay(50);
	}

	if (WiFi.status() != WL_CONNECTED)
	{
		Serial.printf("WiFi connect timeout after %lu ms, starting config portal\n", (unsigned long)(millis() - wifiStartMs));
		Webconfig();
	}

	if (WiFi.status() == WL_CONNECTED)
	{
		Serial.println("UDP Starting...");
		Udp.begin(localPort);
		setSyncProvider(GetNtpTime);
		setSyncInterval(3600);

		WebSeverInit();

		// Keep the upload endpoint separate so /update can always serve our custom UI.
		updateServer.setup(&server, "/ota");

		// ArduinoOTA initialization (for IDE/staging OTA)
		ArduinoOTA.onStart([]()
						   {
      otaUpdating = true;
      otaProgress = 0;
      otaStatusMsg = "OTA Start";
      Serial.println("OTA Start");
      // stop websocket to avoid conflicts during update
      webSocket.disconnect(); });
		ArduinoOTA.onEnd([]()
						 {
      otaUpdating = false;
      otaProgress = 100;
      otaStatusMsg = "OTA Complete";
      Serial.println("OTA End"); });
		static uint8_t lastProgress = 0;
		ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
							  {
      if (total > 0) {
        otaProgress = (uint8_t)((uint32_t)progress * 100 / total);
      } else {
        otaProgress = 0;
      }
      otaStatusMsg = "Updating: " + String(otaProgress) + "%";
      Serial.printf("OTA Progress: %u%%\r", otaProgress);
	  // Update display only if progress increased by at least 5%
	  // Should not be too frequent to avoid slow downs the OTA speed
	  if (otaProgress - lastProgress >= 5)
	  {
		lastProgress = otaProgress;
		DisplayOTAStatus();
	  } });
		ArduinoOTA.onError([](ota_error_t error)
						   {
      otaUpdating = false;
      otaProgress = 0;
      Serial.printf("OTA Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) {
        otaStatusMsg = "Auth Failed";
        Serial.println("Auth Failed");
      }
      else if (error == OTA_BEGIN_ERROR) {
        otaStatusMsg = "Begin Failed";
        Serial.println("Begin Failed");
      }
      else if (error == OTA_CONNECT_ERROR) {
        otaStatusMsg = "Connect Failed";
        Serial.println("Connect Failed");
      }
      else if (error == OTA_RECEIVE_ERROR) {
        otaStatusMsg = "Receive Failed";
        Serial.println("Receive Failed");
      }
      else if (error == OTA_END_ERROR) {
        otaStatusMsg = "End Failed";
        Serial.println("End Failed");
      } });
		ArduinoOTA.begin();

		// Provide a friendly web UI for update (GET only). The POST is still handled by HTTPUpdateServer.
		server.on("/update", HTTP_GET, []() {
			const char *page = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Monitor Update Center</title>
<style>
:root{
  --bg:#f3f7fb;--bg2:#dfeaf6;--card:rgba(255,255,255,.84);--card-border:rgba(148,163,184,.22);
  --text:#162235;--muted:#5d6e86;--label:#425671;--accent:#1583ff;--accent2:#0f9d79;--danger:#d9485f;--shadow:0 24px 60px rgba(33,49,77,.14);
  --hero-start:rgba(255,255,255,.94);--hero-end:rgba(240,247,255,.9);--field-bg:rgba(246,249,253,.98);--surface:rgba(255,255,255,.72);--surface-border:rgba(148,163,184,.2);
}
html[data-theme='dark']{
  --bg:#09121f;--bg2:#0f1f36;--card:rgba(11,23,39,.78);--card-border:rgba(148,163,184,.18);
  --text:#e5eefb;--muted:#93a4bc;--label:#c7d6ea;--accent:#52d4ff;--accent2:#7cffa6;--danger:#ff7a90;--shadow:0 24px 60px rgba(0,0,0,.35);
  --hero-start:rgba(11,23,39,.88);--hero-end:rgba(17,34,58,.82);--field-bg:rgba(7,14,25,.72);--surface:rgba(255,255,255,.04);--surface-border:rgba(255,255,255,.06);
}
*{box-sizing:border-box}
html,body{margin:0;min-height:100%;font-family:"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;background:
radial-gradient(circle at top left, rgba(82,212,255,.18), transparent 28%),
radial-gradient(circle at top right, rgba(124,255,166,.12), transparent 22%),
linear-gradient(145deg,var(--bg),var(--bg2));color:var(--text)}
body{padding:28px 18px 40px}
.wrap{max-width:1100px;margin:0 auto}
.hero{padding:24px 28px;border:1px solid var(--card-border);border-radius:28px;background:linear-gradient(135deg,var(--hero-start),var(--hero-end));box-shadow:var(--shadow);display:flex;align-items:center;justify-content:space-between;gap:16px}
.hero h1{margin:0;font-size:32px}
.hero p{margin:10px 0 0;color:var(--muted);font-size:14px}
.hero-tools{display:flex;gap:12px;align-items:center}
.theme-btn,.back-link{padding:12px 16px;border-radius:16px;border:1px solid var(--surface-border);background:var(--surface);color:var(--text);text-decoration:none;cursor:pointer;font-weight:600}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:18px;margin-top:22px}
.card{padding:22px;border-radius:24px;background:var(--card);border:1px solid var(--card-border);backdrop-filter:blur(12px);box-shadow:var(--shadow)}
.card h2{margin:0 0 6px;font-size:22px}
.card p{margin:0 0 18px;color:var(--muted);line-height:1.6;font-size:13px}
.field{margin-top:14px}
.field label{display:block;margin-bottom:8px;font-size:13px;color:var(--label);font-weight:600}
input[type=file]{width:100%;padding:14px;border-radius:16px;border:1px dashed rgba(148,163,184,.35);background:var(--field-bg);color:var(--text)}
.actions{display:flex;gap:12px;align-items:center;margin-top:16px}
.btn{padding:14px 18px;border:none;border-radius:18px;background:linear-gradient(135deg,var(--accent),#4cb4ff);color:#03111e;font-size:14px;font-weight:700;cursor:pointer}
.btn.fs{background:linear-gradient(135deg,var(--accent2),#57d29d)}
.btn:disabled{opacity:.55;cursor:not-allowed}
.progress{width:100%;background:rgba(148,163,184,.18);border-radius:999px;height:12px;overflow:hidden;margin-top:16px}
.bar{height:100%;width:0;background:linear-gradient(90deg,var(--accent),#7bb5ff);transition:width .2s ease}
.bar.fs{background:linear-gradient(90deg,var(--accent2),#9df3c4)}
.status{min-height:22px;margin-top:12px;font-size:13px;color:var(--muted)}
.hint{font-size:12px;color:var(--muted);margin-top:12px}
@media (max-width:720px){
  .hero{flex-direction:column;align-items:flex-start}
  .hero h1{font-size:26px}
  .hero-tools{width:100%;flex-direction:column}
  .theme-btn,.back-link,.btn{width:100%}
  .actions{flex-direction:column;align-items:stretch}
}
</style>
</head>
<body>
<div class="wrap">
  <div class="hero">
    <div>
      <h1>Update Center</h1>
      <p>Upload firmware or a LittleFS image from your browser. The device will reboot automatically after a successful update.</p>
    </div>
    <div class="hero-tools">
      <a class="back-link" href="/">Back to Config</a>
      <button class="theme-btn" type="button" id="themeToggle">Dark Theme</button>
    </div>
  </div>

  <div class="grid">
    <section class="card">
      <h2>Firmware</h2>
      <p>Use a compiled application binary such as <code>firmware.bin</code>. This replaces the running application.</p>
      <div class="field">
        <label for="firmwareFile">Firmware File</label>
        <input type="file" id="firmwareFile" accept=".bin,.bin.gz" />
      </div>
      <div class="actions">
        <button class="btn" type="button" id="firmwareBtn">Update Firmware</button>
      </div>
      <div class="progress"><div class="bar" id="firmwareBar"></div></div>
      <div class="status" id="firmwareStatus"></div>
      <div class="hint">The selected firmware is uploaded in place and the device will restart automatically after success.</div>
    </section>

    <section class="card">
      <h2>FileSystem</h2>
      <p>Upload a LittleFS image built for this device, for example the generated filesystem image.</p>
      <div class="field">
        <label for="filesystemFile">FileSystem Image</label>
        <input type="file" id="filesystemFile" accept=".bin,.bin.gz,.image" />
      </div>
      <div class="actions">
        <button class="btn fs" type="button" id="filesystemBtn">Update FileSystem</button>
      </div>
      <div class="progress"><div class="bar fs" id="filesystemBar"></div></div>
      <div class="status" id="filesystemStatus"></div>
      <div class="hint">This writes the flash filesystem partition and then reboots the device.</div>
    </section>
  </div>
</div>

<script>
const root=document.documentElement;
const savedTheme=localStorage.getItem('monitor-theme')||'light';
root.setAttribute('data-theme',savedTheme);
const themeToggle=document.getElementById('themeToggle');
const updateThemeButton=()=>{if(themeToggle){themeToggle.textContent=root.getAttribute('data-theme')==='dark'?'Light Theme':'Dark Theme';}};
updateThemeButton();
if(themeToggle){
  themeToggle.addEventListener('click',()=>{
    const next=root.getAttribute('data-theme')==='dark'?'light':'dark';
    root.setAttribute('data-theme',next);
    localStorage.setItem('monitor-theme',next);
    updateThemeButton();
  });
}

function bindUploader(buttonId,inputId,fieldName,barId,statusId){
  const btn=document.getElementById(buttonId);
  const input=document.getElementById(inputId);
  const bar=document.getElementById(barId);
  const status=document.getElementById(statusId);
  if(!btn||!input||!bar||!status) return;

  btn.addEventListener('click',()=>{
    if(!input.files.length){
      status.textContent='Please select a file first.';
      return;
    }
    const file=input.files[0];
    const form=new FormData();
    form.append(fieldName,file,file.name);

    btn.disabled=true;
    status.textContent='Preparing upload...';
    bar.style.width='0%';

    const xhr=new XMLHttpRequest();
    xhr.open('POST','/ota',true);
    xhr.upload.onprogress=(e)=>{
      if(e.lengthComputable){
        const pct=Math.round((e.loaded/e.total)*100);
        bar.style.width=pct+'%';
        status.textContent='Uploading: '+pct+'%';
      }
    };
    xhr.onload=()=>{
      btn.disabled=false;
      if(xhr.status===200){
        bar.style.width='100%';
        status.textContent='Upload complete. Device is rebooting...';
      }else{
        status.textContent='Upload failed: '+xhr.status;
      }
    };
    xhr.onerror=()=>{
      btn.disabled=false;
      status.textContent='Network error during upload.';
    };
    xhr.send(form);
  });
}

bindUploader('firmwareBtn','firmwareFile','firmware','firmwareBar','firmwareStatus');
bindUploader('filesystemBtn','filesystemFile','filesystem','filesystemBar','filesystemStatus');
</script>
</body>
</html>
				)rawliteral";
			server.send(200, "text/html", page); });

		digitalWrite(LED, LOW); // LED OFF
	}

	if (WiFi.isConnected() && !serverList.empty())
	{
		serverUrl = serverList[currentServerIndex];
		if (!serverUrl.isEmpty() && WebsocketBegin(serverUrl))
		{
			webSocket.onEvent(webSocketEvent);
			webSocket.setReconnectInterval(5000);
		}
	}
	ButtonEventsAttach();
}

void loop()
{
	if (pendingRestartAtMs != 0 && (int32_t)(millis() - pendingRestartAtMs) >= 0)
	{
		ESP.restart();
	}

	// Handle ArduinoOTA if an OTA session is in progress or available
	ArduinoOTA.handle();

	/*
	not work here
	// If OTA is active, show OTA status on LCD instead of normal display
	if (otaUpdating) {
	  DisplayOTAStatus();
	  if (WiFi.isConnected()) {
		server.handleClient();
	  }
	  delay(1);
	  return; // skip normal loop processing
	}
	*/

	RefreshBacklightState();

	int64_t curMs = millis();
	static int64_t btnChkMs;
	static int64_t count;
	static bool led;

	if (curMs - btnChkMs > 10)
	{
		btnChkMs = curMs;
		for (auto &btn : buttons)
		{
			btn.tick();
		}
		if (count < 100)
		{
			count++;
		}
		else
		{
			count = 0;
			DisplayStatus();
		}
	}

	if (WiFi.isConnected() && !serverUrl.isEmpty() && !webSocket.isConnected())
	{
		// mark the beginning of this connect attempt period
		if (lostConnectMs == 0)
		{
			digitalWrite(LED, HIGH);
			lostConnectMs = curMs;
			connectAttemptStartMs = curMs; // start counting total timeout window
		}
		else if (curMs - lostConnectMs > 10000)
		{
			// periodic quick retry (every 10s)
			lostConnectMs = curMs;
			webSocket.disconnect();
			WebsocketBegin(serverUrl);
		}
		// if we've been unable to establish a connection for over 1 minute,
		// switch to the next configured server
		if (connectAttemptStartMs != 0 && (curMs - connectAttemptStartMs) > 60000)
		{
			// advance to next server
			if (serverList.size() > 1)
			{
				currentServerIndex = (currentServerIndex + 1) % serverList.size();
				serverUrl = serverList[currentServerIndex];
				Serial.print("Connection timeout. Switching WebSocket to: ");
				Serial.println(serverUrl);
				webSocket.disconnect();
				if (WebsocketBegin(serverUrl))
				{
					webSocket.onEvent(webSocketEvent);
					webSocket.setReconnectInterval(5000);
				}
			}
			// reset attempt timer to begin measuring again for the new server
			connectAttemptStartMs = curMs;
		}
	}
	else
	{
		lostConnectMs = 0;
		connectAttemptStartMs = 0;
		digitalWrite(LED, LOW);
	}

	if (WiFi.isConnected())
	{
		// If OTA is running, avoid starting new websocket activity that might interfere
		if (!otaUpdating)
		{
			webSocket.loop();
		}
		server.handleClient();
	}
	delay(1);
}

void ButtonEventsAttach()
{
	static bool bl_switch = false;
	Serial.println("BTN ATTACHED");
	if (buttons.size() < 1)
	{
		Serial.println("BTN SIZE ERR");
		return;
	}
	// B1
	buttons[0].attachClick([]{
		bl_switch = !bl_switch;
		if (bl_switch)
		{
			ApplyBacklightState(false);
		}
		else
		{
			ApplyBacklightState(true);
		}
	});
	buttons[0].setClickMs(10);
	// B2
	buttons[1].attachClick([]{
		// short press: cycle through configured websocket servers
		if (serverList.size() > 0)
		{
			currentServerIndex = (currentServerIndex + 1) % serverList.size();
			serverUrl = serverList[currentServerIndex];
			Serial.print("Switching WebSocket to: ");
			Serial.println(serverUrl);
			webSocket.disconnect();
			if (WebsocketBegin(serverUrl))
			{
				webSocket.onEvent(webSocketEvent);
				webSocket.setReconnectInterval(5000);
			}
		}
	});
	buttons[1].setClickMs(10);
	buttons[1].attachLongPressStart([]{
		// TODO: ButtonLongActionB2();
		digitalWrite(LED, HIGH); // LED ON  TODO FIXME not working
	});
	buttons[1].attachLongPressStop([]{
		ESP.restart(); // DEBUG
	});
}

void DisplayStatus()
{
	const int progressBarWidth = 50;
	const int progressBarHeigh = 9;

	//{"name":"MSI","time":"2025-10-14 18:02:02","week":"Tuesday","cpu_usage":1.6,"memory_usage":17.8,"network":{"up":"14.4 KB/s","down":"15.4 KB/s"}}

	DynamicJsonDocument doc(1024);
	deserializeJson(doc, statusStr);
	JsonObject jsdoc = doc.as<JsonObject>();

	String host_name = jsdoc["name"].as<String>();
	double cpu_usage = jsdoc["cpu_usage"].as<double>();
	double memory_usage = jsdoc["memory_usage"].as<double>();
	String up = jsdoc["network"]["up"].as<String>();
	String down = jsdoc["network"]["down"].as<String>();

	u8g2.clearBuffer(); // clear the internal memory

#if 0
  //u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.setFont(u8g2_font_7x13B_tr);
  u8g2.drawStr(0, 22, "CPU:");
  u8g2.drawFrame(36, 14, progressBarWidth, progressBarHeigh);		// draw a empty box to clear the display
  u8g2.drawBox(36, 14, round(((cpu_usage + 0.5) / 100.0) * progressBarWidth), progressBarHeigh); // draw the progress bar
  u8g2.drawStr(90, 22, String(String(cpu_usage, 1) + String("%")).c_str());

  u8g2.drawStr(0, 35, "MEM:");
  u8g2.drawFrame(36, 26, progressBarWidth, progressBarHeigh);		// draw a empty box to clear the display
  u8g2.drawBox(36, 26, round(((memory_usage + 0.5) / 100.0) * progressBarWidth), progressBarHeigh); // draw the progress bar
  u8g2.drawStr(90, 35, String(String(memory_usage, 1) + String("%")).c_str());

  u8g2.drawStr(0, 48, String(String("UP: ") + up).c_str());
  u8g2.drawStr(0, 61, String(String("DN: ") + down).c_str());

#else

	u8g2.setFont(u8g2_font_6x12_tr);
	u8g2.drawStr(0, 20, "CPU:");
	u8g2.drawFrame(32, 12, progressBarWidth, progressBarHeigh);									   // draw a empty box to clear the display
	u8g2.drawBox(32, 12, round(((cpu_usage + 0.5) / 100.0) * progressBarWidth), progressBarHeigh); // draw the progress bar
	u8g2.drawStr(90, 20, String(String(cpu_usage, 1) + String("%")).c_str());

	u8g2.drawStr(0, 32, "MEM:");
	u8g2.drawFrame(32, 22, progressBarWidth, progressBarHeigh);										  // draw a empty box to clear the display
	u8g2.drawBox(32, 22, round(((memory_usage + 0.5) / 100.0) * progressBarWidth), progressBarHeigh); // draw the progress bar
	u8g2.drawStr(90, 32, String(String(memory_usage, 1) + String("%")).c_str());

	u8g2.drawStr(0, 42, String(String("UP: ") + up).c_str());
	u8g2.drawStr(0, 52, String(String("DN: ") + down).c_str());

#endif
	DateTime();

	u8g2.drawHLine(0, 54, 128); // draw a line
	// HOSTNAME
	u8g2.setFont(u8g2_font_siji_t_6x10);
	u8g2.drawStr(0, 63, String(String("Host:") + host_name).c_str());
	u8g2.drawStr(87, 63, String(String(monthShortStr(month())) + String(",") + String(day() + String("th"))).c_str());

	String idxStr = String("[") + String(currentServerIndex + 1) + String("/") + String(serverList.size()) + String("]");
	u8g2.drawStr(54, 8, idxStr.c_str());

	u8g2.sendBuffer(); // transfer internal memory to the display
}

// WEB配网函数
void Webconfig()
{
	WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP

	delay(3000);

	// add a custom input field
	int customFieldLength = 40;

#if 0
  const char* set_rotation = "<br/><label for='set_rotation'>Set Rotation</label>\
                              <br>\
                              <input type='radio' name='set_rotation' value='1'> One<br>\
                              <input type='radio' name='set_rotation' value='2'> Two<br>\
                              <input type='radio' name='set_rotation' value='3' checked> Three<br>\
                              <input type='radio' name='set_rotation' value='4'> Four<br>";
#else
	std::string set_rotation = "<br/><label for='set_rotation'>Set Rotation</label>\
								<br>\
								<input type='radio' name='set_rotation' value='1'";
	if (lcdRotation == 1)
		set_rotation += " checked";
	set_rotation += "> Type 1 <br>\
					<input type='radio' name='set_rotation' value='2'";
	if (lcdRotation == 2)
		set_rotation += " checked";
	set_rotation += "> Type 2 <br>\
					<input type='radio' name='set_rotation' value='3'";
	if (lcdRotation == 3 || localPort == 0)
		set_rotation += " checked";
	set_rotation += "> Type 3 <br>\
					<input type='radio' name='set_rotation' value='4'";
	if (lcdRotation == 4)
		set_rotation += " checked";
	set_rotation += "> Type 4 <br>";
#endif

	String wifiBandHtml = BuildWiFiBandPreferenceHtml("wifi_band_pref");

	WiFiManagerParameter custom_rot(set_rotation.c_str()); // custom html input
	WiFiManagerParameter custom_band(wifiBandHtml.c_str());
	WiFiManagerParameter custom_bl("LCDBL", "LCD BackLight(0-100)", "50", 3);
	WiFiManagerParameter p_lineBreak_notext("<p></p>");

	wifiMgr.addParameter(&p_lineBreak_notext);
	wifiMgr.addParameter(&p_lineBreak_notext);
	wifiMgr.addParameter(&custom_bl);
	wifiMgr.addParameter(&p_lineBreak_notext);
	wifiMgr.addParameter(&p_lineBreak_notext);
	wifiMgr.addParameter(&custom_band);
	wifiMgr.addParameter(&p_lineBreak_notext);
	wifiMgr.addParameter(&custom_rot);

	wifiMgr.setSaveParamsCallback(saveParamCallback);

	std::vector<const char *> menu = {"wifi", "restart"};
	wifiMgr.setMenu(menu);

	// wifiMgr.setClass("invert");

	bool res;
	wifiMgr.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
	// res = wifiMgr.autoConnect("MonitorV1"); // anonymous ap
	res = wifiMgr.autoConnect("MonitorV1.2", "11111178"); // password protected ap

	while (!res)
	{
		;
	}
}

String getParam(String name)
{
	// read parameter from server, for customhmtl input
	String value;
	if (wifiMgr.server->hasArg(name))
	{
		value = wifiMgr.server->arg(name);
	}
	return value;
}

void saveParamCallback()
{
	Serial.println("[CALLBACK] saveParamCallback fired");
	// Serial.println("PARAM customfieldid = " + getParam("customfieldid"));
	Serial.println("PARAM LCD BackLight = " + getParam("LCDBL"));
	Serial.println("PARAM WiFi Band = " + getParam("wifi_band_pref"));
	Serial.println("PARAM Rotation = " + getParam("set_rotation"));

	// Save WiFi credentials (SSID and password)
	if (wifiMgr.server->hasArg("s"))
	{ // WiFiManager uses "s" for SSID
		String ssid = wifiMgr.server->arg("s");
		ssid.toCharArray(wificonf.stassid, sizeof(wificonf.stassid));
		Serial.print("WiFi SSID saved: ");
		Serial.println(wificonf.stassid);
	}
	if (wifiMgr.server->hasArg("p"))
	{ // WiFiManager uses "p" for password
		String psw = wifiMgr.server->arg("p");
		psw.toCharArray(wificonf.stapsw, sizeof(wificonf.stapsw));
		Serial.print("WiFi Password saved: ");
		Serial.println("***"); // Don't print the password
	}

	// 将从页面中获取的数据保存
	lcdRotation = getParam("set_rotation").toInt();
	backlightPwm = getParam("LCDBL").toInt();
	String bandPref = getParam("wifi_band_pref");
	if (bandPref.length() > 0)
		wifiBandPreference = WiFiBandPreferenceFromString(bandPref);

	// 屏幕方向
	Serial.print("LCD_Rotation = ");
	Serial.println(lcdRotation);
	SetLcdRotation(lcdRotation);
	Serial.print("WiFi Band Preference = ");
	Serial.println(WiFiBandPreferenceLabel(wifiBandPreference));

	// persist full config (including wifi, servers, backlight, rotation)
	saveConfig();
	// 屏幕亮度
	Serial.printf("Brightness set to: ");
	Serial.println(backlightPwm);
	RefreshBacklightState();
}

const int NTP_PACKET_SIZE = 48;		   // NTP时间在消息的前48字节中
uint8_t packetBuffer[NTP_PACKET_SIZE]; // buffer to hold incoming & outgoing packets

/*-------- NTP code ----------*/
time_t GetNtpTime()
{
	IPAddress ntpServerIP; // NTP server's ip address

	while (Udp.parsePacket() > 0)
		; // discard any previously received packets
	// Serial.println("Transmit NTP Request");
	//  get a random server from the pool
	WiFi.hostByName(ntpServerName.c_str(), ntpServerIP);
	// Serial.print(ntpServerName);
	// Serial.print(": ");
	// Serial.println(ntpServerIP);
	SendNTPpacket(ntpServerIP);
	uint32_t beginWait = millis();
	while (millis() - beginWait < 1500)
	{
		int size = Udp.parsePacket();
		if (size >= NTP_PACKET_SIZE)
		{
			Serial.println("Receive NTP Response");
			Udp.read(packetBuffer, NTP_PACKET_SIZE); // read packet into the buffer
			unsigned long secsSince1900;
			// convert four bytes starting at location 40 to a long integer
			secsSince1900 = (unsigned long)packetBuffer[40] << 24;
			secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
			secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
			secsSince1900 |= (unsigned long)packetBuffer[43];
			// Serial.println(secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR);
			return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
		}
	}
	Serial.println("No NTP Response :-(");
	return 0;
}

void SendNTPpacket(IPAddress &address)
{
	// set all bytes in the buffer to 0
	memset(packetBuffer, 0, NTP_PACKET_SIZE);
	// Initialize values needed to form NTP request
	// (see URL above for details on the packets)
	packetBuffer[0] = 0b11100011; // LI, Version, Mode
	packetBuffer[1] = 0;		  // Stratum, or type of clock
	packetBuffer[2] = 6;		  // Polling Interval
	packetBuffer[3] = 0xEC;		  // Peer Clock Precision
	// 8 bytes of zero for Root Delay & Root Dispersion
	packetBuffer[12] = 49;
	packetBuffer[13] = 0x4E;
	packetBuffer[14] = 49;
	packetBuffer[15] = 52;
	// all NTP fields have been given values, now
	// you can send a packet requesting a timestamp:
	Udp.beginPacket(address, 123); // NTP requests are to port 123
	Udp.write(packetBuffer, NTP_PACKET_SIZE);
	Udp.endPacket();
}

void DateTime()
{
	// u8g2.clearBuffer();

	u8g2.setFont(u8g2_font_siji_t_6x10);

	char buf[8];
	sprintf(buf, "%02d:%02d:%02d", hour(), minute(), second());
	u8g2.drawStr(0, 8, buf);

	u8g2.drawStr(96, 8, String(dayShortStr(weekday())).c_str());

	if (WiFi.isConnected() && webSocket.isConnected())
	{
		// Select wifi glyph based on RSSI strength
		int32_t rssi = WiFi.RSSI(); // dBm (negative values)
		// map RSSI to three levels: strong, medium, weak
		uint32_t wifiGlyph = 0x0e218; // default weak
		if (rssi > -60)
		{
			wifiGlyph = 0x0e21a; // strong
		}
		else if (rssi > -75)
		{
			wifiGlyph = 0x0e219; // medium
		}
		else
		{
			wifiGlyph = 0x0e218; // weak
		}
		u8g2.drawGlyph(116, 8, wifiGlyph); // wifi signal icon
		Serial.printf("Wifi ssid: ");
		Serial.printf(wificonf.stassid);
		Serial.printf(", rssi: ");
		Serial.println(rssi);
	}
	else
	{
		u8g2.drawGlyph(116, 8, 0x0e217); // disconnected icon
	}

	u8g2.drawHLine(0, 9, 128); // draw a line

	// u8g2.sendBuffer();
}

void DisplayOTAStatus()
{
	// Display OTA update progress on LCD
	u8g2.clearBuffer();
	u8g2.setFont(u8g2_font_ncenB08_tf);
	u8g2.drawStr(30, 20, "OTA Update");

	u8g2.setFont(u8g2_font_6x12_tr);
	// Display status message
	u8g2.drawStr(25, 35, otaStatusMsg.c_str());

	// Draw progress bar
	const int barWidth = 50;
	const int barHeight = 10;
	const int barX = 35;
	const int barY = 45;

	u8g2.drawFrame(barX, barY, barWidth, barHeight);
	if (otaProgress > 0)
	{
		int filledWidth = (otaProgress * barWidth) / 100;
		u8g2.drawBox(barX, barY, filledWidth, barHeight);
	}

	u8g2.sendBuffer();
}

void HandleConfig()
{
	String msg;
	bool restartRequired = false;
	int web_setro, web_lcdbl;
	String web_ws_server, web_ws_port, web_ws_path;

	// Check if form was submitted with the Save button
	if (server.hasArg("Save"))
	{
		web_lcdbl = server.arg("web_bl").toInt();
		String web_ws_list = server.arg("web_ws_list");
		WiFiBandPreference newBandPreference = wifiBandPreference;
		if (server.hasArg("web_wifi_band"))
			newBandPreference = WiFiBandPreferenceFromString(server.arg("web_wifi_band"));

		Serial.println("");

		// 调整亮度
		if (web_lcdbl >= 0 && web_lcdbl <= 100)
		{
			backlightPwm = web_lcdbl;
			Serial.printf("亮度调整为：");
			Serial.println(backlightPwm);
			Serial.println("");
		}

		// Auto-backlight settings: checkbox doesn't send value if unchecked, so check for existence
		autoBacklightEnabled = server.hasArg("auto_backlight");
		Serial.printf("Auto Backlight: %s\n", autoBacklightEnabled ? "enabled" : "disabled");

		if (server.hasArg("on_time"))
		{
			// Parse HH:MM format
			String onStr = server.arg("on_time");
			int colonPos = onStr.indexOf(':');
			if (colonPos > 0)
			{
				int hh = onStr.substring(0, colonPos).toInt();
				int mm = onStr.substring(colonPos + 1).toInt();
				backlightOnTime = hh * 60 + mm;
				Serial.printf("Backlight On Time: %02d:%02d (%u mins)\n", hh, mm, backlightOnTime);
			}
		}
		if (server.hasArg("off_time"))
		{
			// Parse HH:MM format
			String offStr = server.arg("off_time");
			int colonPos = offStr.indexOf(':');
			if (colonPos > 0)
			{
				int hh = offStr.substring(0, colonPos).toInt();
				int mm = offStr.substring(colonPos + 1).toInt();
				backlightOffTime = hh * 60 + mm;
				Serial.printf("Backlight Off Time: %02d:%02d (%u mins)\n", hh, mm, backlightOffTime);
			}
		}

		// LCD 旋转设置
		web_setro = server.arg("web_set_rotation").toInt();
		if (web_setro != lcdRotation)
		{
			lcdRotation = web_setro;
			SetLcdRotation(lcdRotation);
		}
		Serial.print("LCD Rotation:");
		Serial.println(lcdRotation);
		if (newBandPreference != wifiBandPreference)
		{
			wifiBandPreference = newBandPreference;
			restartRequired = true;
		}
		Serial.print("WiFi Band Preference:");
		Serial.println(WiFiBandPreferenceLabel(wifiBandPreference));

		// 解析 WebSocket 服务器地址列表 (newline separated)
		if (web_ws_list.length() > 0)
		{
			std::vector<String> newList;
			int start = 0;
			while (start < web_ws_list.length())
			{
				int nl = web_ws_list.indexOf('\n', start);
				String line;
				if (nl == -1)
				{
					line = web_ws_list.substring(start);
					start = web_ws_list.length();
				}
				else
				{
					line = web_ws_list.substring(start, nl);
					start = nl + 1;
				}
				line.trim();
				if (line.length() > 0)
					newList.push_back(line);
			}
			if (!newList.empty())
			{
				serverList = newList;
				currentServerIndex = 0;
				serverUrl = serverList[currentServerIndex];
				webSocket.disconnect();
				if (WebsocketBegin(serverUrl))
				{
					webSocket.onEvent(webSocketEvent);
					webSocket.setReconnectInterval(5000);
				}
			}
		}

		// persist changes to EEPROM
		saveConfig();
		RefreshBacklightState();
		if (restartRequired)
			msg = "<div>Saved. Rebooting to apply WiFi band...</div>";
	}

	// 网页界面代码段 — 显示当前设置并允许修改
	String content = "<html><style>html,body{ background: #29cbf8ff; color: #fff; font-size: 12px; font-family:Arial,Helvetica,sans-serif;} label{display:block;margin-top:8px;} .small{font-size:11px;color:#eee}</style>";
	content += "<body><form action='/' method='POST'><br><div style='font-weight:bold;'>Monitor V1.2</div><br>";
	// show current wifi SSID (read-only here)
	content += "<label>WiFi SSID: <span class='small'>" + String(wificonf.stassid) + "</span></label>";
	content += BuildWiFiBandPreferenceHtml("web_wifi_band");
	// Backlight input populated with current value
	content += "<label>Back Light (0-100): <input type='number' name='web_bl' min='0' max='100' value='" + String(backlightPwm) + "'></label>";

	// LCD Rotation radios - mark the current rotation as checked
	String rotHtml = "<label>LCD Rotation:</label>";
	rotHtml += "<input type='radio' name='web_set_rotation' value='1'";
	if (lcdRotation == 1)
		rotHtml += " checked";
	rotHtml += "> USB Up<br>";
	rotHtml += "<input type='radio' name='web_set_rotation' value='2'";
	if (lcdRotation == 2)
		rotHtml += " checked";
	rotHtml += "> USB Right<br>";
	rotHtml += "<input type='radio' name='web_set_rotation' value='3'";
	if (lcdRotation == 3)
		rotHtml += " checked";
	rotHtml += "> USB Down<br>";
	rotHtml += "<input type='radio' name='web_set_rotation' value='4'";
	if (lcdRotation == 4)
		rotHtml += " checked";
	rotHtml += "> USB Left<br>";
	content += rotHtml;
	// prepare the server list textarea content
	String serversText;
	for (size_t i = 0; i < serverList.size(); i++)
	{
		serversText += serverList[i];
		if (i + 1 < serverList.size())
			serversText += "\n";
	}
	content += "<br>Server List (one per line):<br>\
                  <textarea name='web_ws_list' rows='4' cols='40'>" +
			   serversText + "</textarea><br>";

	// Auto-backlight controls
	content += "<br><label>";
	content += "<input type='checkbox' name='auto_backlight' value='on'";
	if (autoBacklightEnabled)
		content += " checked";
	content += "> Auto Backlight</label>";

	// On time (HH:MM)
	char onTimeStr[6];
	sprintf(onTimeStr, "%02d:%02d", backlightOnTime / 60, backlightOnTime % 60);
	content += "<label>On Time (HH:MM): <input type='text' name='on_time' value='" + String(onTimeStr) + "' placeholder='07:00' pattern='\\d{2}:\\d{2}'></label>";

	// Off time (HH:MM)
	char offTimeStr[6];
	sprintf(offTimeStr, "%02d:%02d", backlightOffTime / 60, backlightOffTime % 60);
	content += "<label>Off Time (HH:MM): <input type='text' name='off_time' value='" + String(offTimeStr) + "' placeholder='22:00' pattern='\\d{2}:\\d{2}'></label>";

	content += "<br><div><input type='submit' name='Save' value='Save'></form></div>" + msg + "<br>";
	content += "MAC: " + GetESPMACAddress() + "<br>";
	// provide a link to the HTTPUpdate web UI
	content += "<br><a href=\"/update\">Firmware Update (web upload)</a><br>";
	content += "</body></html>";
	server.send(200, "text/html", content);
	if (restartRequired)
	{
		delay(300);
		ESP.restart();
	}
}

void HandleConfigModern()
{
	if (server.hasArg("Save"))
	{
		bool restartRequired = false;
		int web_lcdbl = server.hasArg("web_bl") ? server.arg("web_bl").toInt() : backlightPwm;
		int web_setro = server.hasArg("web_set_rotation") ? server.arg("web_set_rotation").toInt() : lcdRotation;
		String web_ws_list = server.arg("web_ws_list");
		WiFiBandPreference newBandPreference = server.hasArg("web_wifi_band") ? WiFiBandPreferenceFromString(server.arg("web_wifi_band")) : wifiBandPreference;
		String newSsid = server.hasArg("web_ssid") ? server.arg("web_ssid") : String(wificonf.stassid);
		String newPassword = server.hasArg("web_psw") ? server.arg("web_psw") : String(wificonf.stapsw);

		newSsid.trim();
		if (newSsid.length() >= (int)sizeof(wificonf.stassid))
			newSsid = newSsid.substring(0, sizeof(wificonf.stassid) - 1);
		if (newPassword.length() >= (int)sizeof(wificonf.stapsw))
			newPassword = newPassword.substring(0, sizeof(wificonf.stapsw) - 1);

		if (newSsid != String(wificonf.stassid) || newPassword != String(wificonf.stapsw))
		{
			newSsid.toCharArray(wificonf.stassid, sizeof(wificonf.stassid));
			newPassword.toCharArray(wificonf.stapsw, sizeof(wificonf.stapsw));
			restartRequired = true;
		}

		if (web_lcdbl >= 0 && web_lcdbl <= 100)
		{
			backlightPwm = web_lcdbl;
		}

		autoBacklightEnabled = server.hasArg("auto_backlight");

		if (server.hasArg("on_time"))
			backlightOnTime = timeStringToMinutes(server.arg("on_time"));
		if (server.hasArg("off_time"))
			backlightOffTime = timeStringToMinutes(server.arg("off_time"));

		if (web_setro >= 1 && web_setro <= 4 && web_setro != lcdRotation)
		{
			lcdRotation = web_setro;
			SetLcdRotation(lcdRotation);
		}

		if (newBandPreference != wifiBandPreference)
		{
			wifiBandPreference = newBandPreference;
			restartRequired = true;
		}

		if (web_ws_list.length() > 0)
		{
			std::vector<String> newList;
			int start = 0;
			while (start < web_ws_list.length())
			{
				int nl = web_ws_list.indexOf('\n', start);
				String line;
				if (nl == -1)
				{
					line = web_ws_list.substring(start);
					start = web_ws_list.length();
				}
				else
				{
					line = web_ws_list.substring(start, nl);
					start = nl + 1;
				}
				line.trim();
				if (line.length() > 0)
					newList.push_back(line);
			}
			if (!newList.empty())
			{
				serverList = newList;
				currentServerIndex = 0;
				serverUrl = serverList[currentServerIndex];
				webSocket.disconnect();
				if (WiFi.isConnected() && WebsocketBegin(serverUrl))
				{
					webSocket.onEvent(webSocketEvent);
					webSocket.setReconnectInterval(5000);
				}
			}
		}

		saveConfig();
		RefreshBacklightState();

		server.sendHeader("Cache-Control", "no-store");
		server.sendHeader("Location", restartRequired ? "/?status=restarting" : "/?status=saved", true);
		server.send(303, "text/plain", "");
		if (restartRequired)
			pendingRestartAtMs = millis() + 1500;
		return;
	}

	String statusParam = server.arg("status");
	String statusHtml;
	if (statusParam == "saved")
		statusHtml = "<div class='banner ok'>Configuration saved.</div>";
	else if (statusParam == "restarting")
		statusHtml = "<div class='banner warn'>Configuration saved. Rebooting to apply WiFi changes...</div>";

	String currentSsid = HtmlEscape(String(wificonf.stassid));
	String currentPassword = HtmlEscape(String(wificonf.stapsw));
	String hostname = WiFi.getHostname() ? HtmlEscape(String(WiFi.getHostname())) : String("MonitorV1");
	String ipAddress = WiFi.isConnected() ? HtmlEscape(WiFi.localIP().toString()) : String("Offline");
	String rssi = WiFi.isConnected() ? HtmlEscape(String(WiFi.RSSI()) + " dBm") : String("--");
	String channel = WiFi.isConnected() ? HtmlEscape(String((long)WiFi.channel())) : String("--");
	String band = WiFi.isConnected() ? HtmlEscape(String(WiFiBandName(WiFi.getBand()))) : HtmlEscape(String(WiFiBandPreferenceLabel(wifiBandPreference)));
	String bssid = WiFi.isConnected() ? HtmlEscape(WiFi.BSSIDstr()) : String("--");
	String mac = HtmlEscape(GetESPMACAddress());
	String activeServer = (!serverList.empty() && currentServerIndex < serverList.size()) ? HtmlEscape(serverList[currentServerIndex]) : String("-");
	String connectionState = WiFi.isConnected() ? String("Online") : String("Offline");
	String bandPref = String(WiFiBandPreferenceKey(wifiBandPreference));

	String serversText;
	for (size_t i = 0; i < serverList.size(); i++)
	{
		serversText += serverList[i];
		if (i + 1 < serverList.size())
			serversText += "\n";
	}
	serversText = HtmlEscape(serversText);

	char onTimeStr[6];
	sprintf(onTimeStr, "%02d:%02d", backlightOnTime / 60, backlightOnTime % 60);
	char offTimeStr[6];
	sprintf(offTimeStr, "%02d:%02d", backlightOffTime / 60, backlightOffTime % 60);

	String content;
	content.reserve(11000);
	content += "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'><title>Monitor V1.2</title>";
	content += R"rawliteral(
<style>
:root{
  --bg:#f3f7fb;--bg2:#dfeaf6;--card:rgba(255,255,255,.82);--card-border:rgba(148,163,184,.22);
  --text:#162235;--muted:#5d6e86;--label:#425671;--accent:#1583ff;--accent-2:#0f9d79;--warn:#b7791f;
  --shadow:0 24px 60px rgba(33,49,77,.14);
  --hero-start:rgba(255,255,255,.94);--hero-end:rgba(240,247,255,.9);--field-bg:rgba(246,249,253,.98);--surface:rgba(255,255,255,.72);--surface-border:rgba(148,163,184,.18);
}
html[data-theme='dark']{
  --bg:#09121f;--bg2:#0f1f36;--card:rgba(11,23,39,.78);--card-border:rgba(148,163,184,.18);
  --text:#e5eefb;--muted:#93a4bc;--label:#c7d6ea;--accent:#52d4ff;--accent-2:#7cffa6;--warn:#ffd166;
  --shadow:0 24px 60px rgba(0,0,0,.35);--hero-start:rgba(11,23,39,.88);--hero-end:rgba(17,34,58,.82);--field-bg:rgba(7,14,25,.72);--surface:rgba(255,255,255,.04);--surface-border:rgba(255,255,255,.06);
}
*{box-sizing:border-box}
html,body{margin:0;font-family:"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;background:
radial-gradient(circle at top left, rgba(82,212,255,.18), transparent 28%),
radial-gradient(circle at top right, rgba(124,255,166,.12), transparent 22%),
linear-gradient(145deg,var(--bg),var(--bg2));color:var(--text);min-height:100%}
body{padding:28px 18px 40px}
.wrap{max-width:1100px;margin:0 auto}
.hero{padding:24px 28px;border:1px solid var(--card-border);border-radius:28px;background:linear-gradient(135deg,var(--hero-start),var(--hero-end));box-shadow:var(--shadow);display:flex;align-items:center;justify-content:space-between;gap:16px}
.hero h1{margin:0;font-size:32px;letter-spacing:.02em}
.hero-tools{display:flex;align-items:center;gap:12px}
.theme-btn{padding:12px 16px;border-radius:16px;border:1px solid var(--surface-border);background:var(--surface);color:var(--text);cursor:pointer;font-weight:600}
.banner{margin:18px 0;padding:14px 16px;border-radius:16px;font-size:14px}
.banner.ok{background:rgba(126,240,184,.14);border:1px solid rgba(126,240,184,.28)}
.banner.warn{background:rgba(255,209,102,.12);border:1px solid rgba(255,209,102,.28)}
.stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:14px;margin:22px 0}
.stat{padding:16px 18px;border-radius:18px;background:var(--surface);border:1px solid var(--surface-border)}
.stat .k{display:block;color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.08em}
.stat .v{display:block;margin-top:8px;font-size:18px;font-weight:700}
form{margin-top:22px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:18px}
.card{padding:22px;border-radius:24px;background:var(--card);border:1px solid var(--card-border);backdrop-filter:blur(12px);box-shadow:var(--shadow)}
.card h2{margin:0 0 6px;font-size:20px}
.card p{margin:0 0 18px;color:var(--muted);font-size:13px;line-height:1.6}
.field{margin-top:14px}
.field label,.label{display:block;margin-bottom:8px;font-size:13px;color:var(--label);font-weight:600}
.hint{margin-top:8px;color:var(--muted);font-size:12px}
input[type=text],input[type=password],input[type=number],textarea{
  width:100%;padding:14px 14px;border-radius:16px;border:1px solid rgba(148,163,184,.18);
  background:var(--field-bg);color:var(--text);outline:none;font-size:14px;
}
input:focus,textarea:focus{border-color:rgba(82,212,255,.55);box-shadow:0 0 0 4px rgba(82,212,255,.12)}
textarea{min-height:164px;resize:vertical}
.inline{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.toggle{display:flex;align-items:center;gap:10px;padding:12px 14px;border-radius:16px;background:var(--surface);border:1px solid var(--surface-border)}
.toggle input{width:18px;height:18px}
.radio-grid,.radio-grid-4{display:grid;gap:10px}
.radio-grid{grid-template-columns:repeat(3,minmax(0,1fr))}
.radio-grid-4{grid-template-columns:repeat(4,minmax(0,1fr))}
.radio-grid label,.radio-grid-4 label{display:block;margin:0}
.radio-grid input,.radio-grid-4 input{display:none}
.choice{display:flex;align-items:center;justify-content:center;padding:14px 10px;border-radius:16px;border:1px solid rgba(148,163,184,.18);background:var(--surface);color:var(--text);cursor:pointer;font-size:13px;font-weight:600;transition:.18s ease}
.radio-grid input:checked + .choice,.radio-grid-4 input:checked + .choice{border-color:rgba(82,212,255,.7);background:rgba(82,212,255,.14);color:var(--text);box-shadow:0 12px 28px rgba(82,212,255,.18)}
.pwd-row{display:grid;grid-template-columns:1fr auto;gap:10px}
.ghost{padding:0 16px;border-radius:16px;border:1px solid var(--surface-border);background:var(--surface);color:var(--text);cursor:pointer;font-weight:600}
.meta{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;margin-top:8px}
.meta-item{padding:12px 14px;border-radius:16px;background:var(--surface);border:1px solid var(--surface-border);display:flex;flex-direction:column;gap:6px;min-width:0}
.meta-item.wide{grid-column:1 / -1}
.meta span{display:block;color:var(--muted);font-size:12px;margin:0}
.meta strong{display:block;color:var(--text);font-size:15px;line-height:1.55;font-weight:600;word-break:break-word;overflow-wrap:anywhere}
.actions{display:flex;align-items:center;justify-content:space-between;gap:14px;margin-top:22px;padding:18px 22px;border-radius:24px;background:var(--surface);border:1px solid var(--surface-border)}
.primary{padding:15px 24px;border:none;border-radius:18px;background:linear-gradient(135deg,var(--accent),#4cb4ff);color:#03111e;font-size:15px;font-weight:700;cursor:pointer}
.link{color:var(--accent-2);text-decoration:none;font-weight:600}
.small{color:var(--muted);font-size:12px}
@media (max-width:720px){
  .hero{flex-direction:column;align-items:flex-start}
  .hero h1{font-size:26px}
  .inline,.meta,.radio-grid,.radio-grid-4,.actions{grid-template-columns:1fr}
  .actions{justify-items:stretch}
  .primary,.ghost,.theme-btn{width:100%}
}
</style></head><body><div class='wrap'>
)rawliteral";
	content += "<div class='hero'><h1>Monitor V1.2</h1><div class='hero-tools'><button class='theme-btn' type='button' id='themeToggle'>Dark Theme</button></div></div>";
	content += statusHtml;
	content += "<div class='stats'>";
	content += "<div class='stat'><span class='k'>Status</span><span class='v'>" + HtmlEscape(connectionState) + "</span></div>";
	content += "<div class='stat'><span class='k'>IP</span><span class='v'>" + ipAddress + "</span></div>";
	content += "<div class='stat'><span class='k'>Band</span><span class='v'>" + band + "</span></div>";
	content += "<div class='stat'><span class='k'>RSSI</span><span class='v'>" + rssi + "</span></div>";
	content += "</div>";
	content += "<form action='/' method='POST'><input type='hidden' name='Save' value='1'><div class='grid'>";

	content += "<section class='card'><h2>WiFi</h2><p>Client credentials and preferred radio band. WiFi changes reboot the device to reconnect cleanly.</p>";
	content += "<div class='field'><label for='web_ssid'>SSID</label><input id='web_ssid' type='text' name='web_ssid' maxlength='31' value='" + currentSsid + "' autocomplete='off'></div>";
	content += "<div class='field'><label for='web_psw'>Password</label><div class='pwd-row'><input id='web_psw' type='password' name='web_psw' maxlength='63' value='" + currentPassword + "' autocomplete='new-password'><button class='ghost' type='button' id='togglePassword'>Show</button></div><div class='hint'>The current password is prefilled and can be edited here.</div></div>";
	content += "<div class='field'><span class='label'>Band Preference</span><div class='radio-grid'>";
	content += "<label><input type='radio' name='web_wifi_band' value='5g'";
	if (bandPref == "5g")
		content += " checked";
	content += "><span class='choice'>5G only</span></label>";
	content += "<label><input type='radio' name='web_wifi_band' value='2g'";
	if (bandPref == "2g")
		content += " checked";
	content += "><span class='choice'>2.4G only</span></label>";
	content += "<label><input type='radio' name='web_wifi_band' value='auto'";
	if (bandPref == "auto")
		content += " checked";
	content += "><span class='choice'>Auto</span></label>";
	content += "</div></div></section>";

	content += "<section class='card'><h2>Device</h2><p>Live connection details and firmware identity.</p><div class='meta'>";
	content += "<div class='meta-item'><span>Hostname</span><strong>" + hostname + "</strong></div>";
	content += "<div class='meta-item'><span>MAC</span><strong>" + mac + "</strong></div>";
	content += "<div class='meta-item'><span>BSSID</span><strong>" + bssid + "</strong></div>";
	content += "<div class='meta-item'><span>Channel</span><strong>" + channel + "</strong></div>";
	content += "<div class='meta-item wide'><span>WebSocket</span><strong>" + activeServer + "</strong></div>";
	content += "<div class='meta-item'><span>Version</span><strong>HW " + HtmlEscape(hw_ver) + " / SW " + HtmlEscape(sw_ver) + "</strong></div>";
	content += "</div></section>";

	content += "<section class='card'><h2>Display</h2><p>Brightness and screen orientation.</p>";
	content += "<div class='field'><label for='web_bl'>Backlight</label><input id='web_bl' type='number' name='web_bl' min='0' max='100' value='" + String(backlightPwm) + "'></div>";
	content += "<div class='field'><span class='label'>LCD Rotation</span><div class='radio-grid-4'>";
	content += "<label><input type='radio' name='web_set_rotation' value='1'";
	if (lcdRotation == 1)
		content += " checked";
	content += "><span class='choice'>USB Up</span></label>";
	content += "<label><input type='radio' name='web_set_rotation' value='2'";
	if (lcdRotation == 2)
		content += " checked";
	content += "><span class='choice'>USB Right</span></label>";
	content += "<label><input type='radio' name='web_set_rotation' value='3'";
	if (lcdRotation == 3)
		content += " checked";
	content += "><span class='choice'>USB Down</span></label>";
	content += "<label><input type='radio' name='web_set_rotation' value='4'";
	if (lcdRotation == 4)
		content += " checked";
	content += "><span class='choice'>USB Left</span></label>";
	content += "</div></div></section>";

	content += "<section class='card'><h2>Auto Backlight</h2><p>Optional schedule for turning the display backlight on and off.</p>";
	content += "<label class='toggle'><input type='checkbox' name='auto_backlight' value='on'";
	if (autoBacklightEnabled)
		content += " checked";
	content += "><span>Enable automatic schedule</span></label>";
	content += "<div class='inline'><div class='field'><label for='on_time'>On Time</label><input id='on_time' type='text' name='on_time' value='" + String(onTimeStr) + "' placeholder='07:00' pattern='\\d{2}:\\d{2}'></div>";
	content += "<div class='field'><label for='off_time'>Off Time</label><input id='off_time' type='text' name='off_time' value='" + String(offTimeStr) + "' placeholder='22:00' pattern='\\d{2}:\\d{2}'></div></div></section>";

	content += "<section class='card'><h2>Servers</h2><p>One WebSocket endpoint per line. The first line is treated as the default server.</p>";
	content += "<div class='field'><label for='web_ws_list'>Server List</label><textarea id='web_ws_list' name='web_ws_list'>" + serversText + "</textarea></div></section>";

	content += "</div>";
	content += "<div class='actions'><div><div>Firmware upload</div><div class='small'>Use the integrated <a class='link' href='/update'>web updater</a> to flash a new firmware binary.</div></div><button class='primary' type='submit'>Save Configuration</button></div></form>";
	content += R"rawliteral(
<script>
const root=document.documentElement;
const savedTheme=localStorage.getItem('monitor-theme')||'light';
root.setAttribute('data-theme',savedTheme);
const themeToggle=document.getElementById('themeToggle');
const updateThemeButton=()=>{if(themeToggle){themeToggle.textContent=root.getAttribute('data-theme')==='dark'?'Light Theme':'Dark Theme';}};
updateThemeButton();
if(themeToggle){
  themeToggle.addEventListener('click',()=>{
    const next=root.getAttribute('data-theme')==='dark'?'light':'dark';
    root.setAttribute('data-theme',next);
    localStorage.setItem('monitor-theme',next);
    updateThemeButton();
  });
}
const toggleBtn=document.getElementById('togglePassword');
const pwdInput=document.getElementById('web_psw');
if(toggleBtn&&pwdInput){
  toggleBtn.addEventListener('click',()=>{const show=pwdInput.type==='password';pwdInput.type=show?'text':'password';toggleBtn.textContent=show?'Hide':'Show';});
}
if(window.location.search.includes('status=')){
  history.replaceState({},'',window.location.pathname);
}
const banner=document.querySelector('.banner');
if(banner){
  setTimeout(()=>{banner.style.transition='opacity .35s ease';banner.style.opacity='0';},2600);
  setTimeout(()=>banner.remove(),3200);
}
</script></div></body></html>)rawliteral";

	server.sendHeader("Cache-Control", "no-store");
	server.send(200, "text/html", content);
}

void HandleNotFound()
{
	String message = "File Not Found\n\n";
	message += "URI: ";
	message += server.uri();
	message += "\nMethod: ";
	message += (server.method() == HTTP_GET) ? "GET" : "POST";
	message += "\nArguments: ";
	message += server.args();
	message += "\n";
	for (uint8_t i = 0; i < server.args(); i++)
	{
		message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
	}
	server.send(404, "text/plain", message);
}

void WebSeverInit()
{
	uint32_t counttime = 0; // 记录创建mDNS的时间
	Serial.println("mDNS responder building...");
	counttime = millis();
	while (!MDNS.begin("MonitorV1"))
	{
		if (millis() - counttime > 30000)
			ESP.restart(); // 判断超过30秒钟就重启设备
	}

	Serial.println("mDNS responder started");

	server.on("/", HandleConfigModern);
	server.onNotFound(HandleNotFound);

	// 开启TCP服务
	server.begin();
	Serial.println("HTTP服务器已开启");

	Serial.println("连接: http://monitor.local");
	Serial.print("本地IP： ");
	Serial.println(WiFi.localIP());
	// 将服务器添加到mDNS
	MDNS.addService("http", "tcp", 80);
}

String GetESPMACAddress()
{
	String mac = "undefine";
	uint8_t baseMac[6];
	esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, baseMac);
	if (ret == ESP_OK)
	{
		char buf[32] = {0};
		sprintf(buf, "%02x:%02x:%02x:%02x:%02x:%02x\n",
				baseMac[0], baseMac[1], baseMac[2],
				baseMac[3], baseMac[4], baseMac[5]);
		mac = String(buf);
	}
	return mac;
}
