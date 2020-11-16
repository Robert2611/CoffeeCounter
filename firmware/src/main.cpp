#include <Arduino.h>
#include "NeoPixelBus.h"
#include "ESPUI.h"
#include <WiFi.h>
#include <DNSServer.h>
#include "HX711.h"
#include "EEPROM.h"

#define PIXEL_COUNT 20
#define PIN_NEOPIXEL 32
#define BALANCE_PIN_DATA 27
#define BALANCE_PIN_CLOCK 26
#define BALANCE_GAIN 128

#define AVERAGING_COUNT 10
#define EEPROM_CONFIG_ADDRESS 0
#define UPDATE_PERIOD_MS 1000
#define MAX_WEIGHT 5000

struct Config
{
  byte version;
  float balance_offset;
  float balance_scale;
  float weight_per_cup;
  float max_filling;
  byte LED_mode;
};

//WIFI
const char *ssid = "CoffeeCounter";
IPAddress wifi_ip(192, 168, 4, 1);
const byte DNS_PORT = 53;
DNSServer dnsServer;
//END WIFI

//GUI
int lb_status_id;
int num_current_weight_id;
int num_weight_per_cup;
int lb_config_message;
//GUI

NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod> pixels(PIXEL_COUNT, PIN_NEOPIXEL);
HX711 balance;

float weight;
//default config
Config config = {2, 0, 1, 200};
unsigned long last_update_ms;

void write_config()
{
  EEPROM.put(EEPROM_CONFIG_ADDRESS, config);
}

bool read_config()
{
  Config temp;
  EEPROM.get(EEPROM_CONFIG_ADDRESS, temp);
  //wrong version, cannot be handled
  if (temp.version != 1)
    return false;
  config = temp;
  return true;
}

void update_status()
{
  char s[128];
  snprintf(s, sizeof(s), "weight = %f g<br>offset = %f<br>scale = %f", weight, config.balance_offset, config.balance_scale);
  ESPUI.updateLabel(lb_status_id, s);
}

void ui_tare_clicked(Control *sender, int type)
{
  if (type == B_DOWN)
  {
    balance.tare(AVERAGING_COUNT);
    config.balance_offset = balance.get_offset();
    write_config();
  }
}

void ui_calibrate_clicked(Control *sender, int type)
{
  if (type == B_DOWN)
  {
    float current_weight = ESPUI.getControl(num_current_weight_id)->value.toFloat();
    //get balance reading with offset allready subtracted!
    float balance_reading = balance.read_average(AVERAGING_COUNT);
    if (current_weight > 0 && current_weight < MAX_WEIGHT && balance_reading > 0)
    {
      config.balance_scale = balance_reading / current_weight;
      write_config();
    }
  }
}

void ui_save_config_clicked(Control *sender, int type)
{
  if (type == B_DOWN)
  {
    Config new_config;
    new_config.weight_per_cup = ESPUI.getControl(num_weight_per_cup)->value.toFloat();
    if (new_config.weight_per_cup == 0)
    {
      ESPUI.updateLabel(lb_config_message, "Fehler: weight_per_cup");
      return;
    }
    config = new_config;
    write_config();
    ESPUI.updateLabel(lb_config_message, "Gespeichert");
  }
}

bool CreateWifiSoftAP()
{
  WiFi.disconnect();
  Serial.print(F("Initalize SoftAP "));
  bool SoftAccOK;
  SoftAccOK = WiFi.softAP(ssid);
  delay(2000); // Without delay I've seen the IP address blank
  WiFi.softAPConfig(wifi_ip, wifi_ip, IPAddress(255, 255, 255, 0));
  if (SoftAccOK)
  {
    /* Setup the DNS server redirecting all the domains to the apIP */
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(DNS_PORT, "*", wifi_ip);
    Serial.println(F("successful."));
    Serial.setDebugOutput(true); // Debug Output for WLAN on Serial Interface.
  }
  else
  {
    Serial.println(F("Soft AP Error."));
    Serial.println(ssid);
  }
  return SoftAccOK;
}

void setFilling(float filling)
{
  for (int i = 0; i < PIXEL_COUNT; i++)
  {
    if (i < filling * PIXEL_COUNT)
      pixels.SetPixelColor(i, {0, 20, 0});
    else
      pixels.SetPixelColor(i, {0, 0, 0});
  }
  pixels.Show();
  update_status();
}

void setup()
{
  Serial.begin(115200);

  //init EEPROM
  EEPROM.begin(sizeof(Config));
  //if config is not readable, write the default one
  if (!read_config())
    write_config();

  //initialize balance
  balance.begin(BALANCE_PIN_DATA, BALANCE_PIN_CLOCK, BALANCE_GAIN);
  balance.set_offset(config.balance_offset);
  balance.set_scale(config.balance_scale);

  //Initialize neopixel
  pinMode(PIN_NEOPIXEL, OUTPUT);
  pixels.Begin();
  setFilling(0);

  //start wifi hotspot
  CreateWifiSoftAP();

  //setup GUI
  int tab_status = ESPUI.addControl(ControlType::Tab, "Status", "Status");
  lb_status_id = ESPUI.addControl(ControlType::Label, "Status", "", ControlColor::Wetasphalt, tab_status);

  int tab_balance = ESPUI.addControl(ControlType::Tab, "Waage", "Waage");
  const char *info_text = "Bitte erst mit leerem Behälter tarieren,<br>dann definiertes Gewicht auf die Platform stellen<br>und auf 'Übernehmen' clicken.";
  ESPUI.addControl(ControlType::Label, "Info", info_text, ControlColor::Emerald, tab_balance);
  ESPUI.addControl(ControlType::Button, "Tara", "Tarieren", ControlColor::Emerald, tab_balance, &ui_tare_clicked);
  num_current_weight_id = ESPUI.addControl(ControlType::Number, "Aktuelles Gewicht", "100", ControlColor::Carrot, tab_balance);
  ESPUI.addControl(ControlType::Button, "Kalibrieren", "Übernehmen", ControlColor::Carrot, tab_balance, &ui_calibrate_clicked);

  int tab_config = ESPUI.addControl(ControlType::Tab, "Config", "Config");
  lb_config_message = ESPUI.addControl(ControlType::Label, "Message", "", ControlColor::Wetasphalt, tab_config);
  num_weight_per_cup = ESPUI.addControl(ControlType::Number, "Gewicht pro Tasse", String(config.weight_per_cup), ControlColor::Alizarin, tab_config);
  ESPUI.addControl(ControlType::Button, "Speichern", "Speichern", ControlColor::Alizarin, tab_config, &ui_save_config_clicked);
  ESPUI.begin("CoffeeCounter");
}

void loop()
{
  dnsServer.processNextRequest();
  if (millis() > last_update_ms + UPDATE_PERIOD_MS)
  {
    last_update_ms = millis();
    weight = balance.get_units(AVERAGING_COUNT);
    update_status();
  }
}