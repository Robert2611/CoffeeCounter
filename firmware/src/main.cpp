#include <Arduino.h>
#include "NeoPixelBus.h"
#include "ESPUI.h"
#include <WiFi.h>
#include <DNSServer.h>
#include "HX711.h"
#include "EEPROM.h"

#define PIN_NEOPIXEL 32
#define BALANCE_PIN_DATA 27
#define BALANCE_PIN_CLOCK 26

#define AVERAGING_COUNT 10              //per weight reading
#define BALANCE_UPDATE_PERIOD_MS 500    //read balance twice every second
#define LED_UPDATE_PERIOD_MS 50         //update LEDs with 20 fps
#define MAX_WEIGHT 5000                 //you can only calibrate if total weight is smaller
#define STABLE_WEIGHT_MAX_DIFFERENCE 20 //if two weight readings differe more than this, it us unstable
#define LED_ANIMATION_PERIOD 1000
#define HISTORY_LENGTH 4

//no need to change
#define EEPROM_CONFIG_ADDRESS 0
#define BALANCE_GAIN 128
#define PIXEL_COUNT 24

enum LED_modes
{
  relative,
  absolute,
  separated
};

struct Config
{
  byte version;
  float balance_offset;
  float balance_scale;
  float weight_per_cup;
  float max_filling;
  byte LED_mode;
  byte brightness;
};

//WIFI
const char *ssid = "CoffeeCounter";
IPAddress wifi_ip(192, 168, 4, 1);
const byte DNS_PORT = 53;
DNSServer dnsServer;
char password[] = "EltraVerder";
//END WIFI

//GUI
int lb_status_id;
int num_current_weight_id;
int num_weight_per_cup;
int lb_config_message;
int num_max_filling;
int sel_LED_mode;
int num_brightness;
//GUI

NeoPixelBus<NeoGrbFeature, Neo800KbpsMethod> pixels(PIXEL_COUNT, PIN_NEOPIXEL);
HX711 balance;

float weight = 0;
float weight_history[HISTORY_LENGTH];

//default config
Config config = {
    .version = 2,
    .balance_offset = 0,
    .balance_scale = 1,
    .weight_per_cup = 200,
    .max_filling = 1500,
    .LED_mode = LED_modes::absolute,
    .brightness = 20};

unsigned long balance_last_update_ms;
unsigned long LED_last_update_ms;
unsigned long LED_period_start_ms;

void write_config()
{
  Serial.println("Writing config.");
  EEPROM.put(EEPROM_CONFIG_ADDRESS, config);
  EEPROM.commit();
}

bool read_config()
{
  Config temp;
  EEPROM.get(EEPROM_CONFIG_ADDRESS, temp);
  //wrong version, cannot be handled
  if (temp.version != config.version)
    return false;
  config = temp;
  return true;
}

void LED_set_filling()
{
  int number_of_cups = config.max_filling / config.weight_per_cup;
  float filling = 0;
  switch (config.LED_mode)
  {
  case LED_modes::relative:
    filling = weight * PIXEL_COUNT / config.max_filling;
    for (int i = 0; i < PIXEL_COUNT; i++)
    {
      float color = constrain((filling - i), 0, 1);
      pixels.SetPixelColor(i, {(byte)((1 - color) * config.brightness), (byte)(color * config.brightness), 0});
    }
    break;
  case LED_modes::absolute:
    filling = weight / config.weight_per_cup;
    for (int i = 0; i < PIXEL_COUNT; i++)
    {
      if (i > number_of_cups)
        pixels.SetPixelColor(i, {0, 0, 0});
      else
      {
        //set pixels red or green depending on actual filling
        float color = constrain(filling - i, 0, 1);
        pixels.SetPixelColor(i, {(byte)((1 - color) * config.brightness), (byte)(color * config.brightness), 0});
      }
    }
    break;
  case LED_modes::separated:
    int pixels_per_cup = 2;
    float available_cups = weight / config.weight_per_cup;
    pixels.ClearTo({0, 0, 0});
    for (int c = 0; c < number_of_cups; c++)
    {
      //SetPixelColor just ignores pixles > PIXEL_COUNT
      int offset = c * (pixels_per_cup + 1);
      pixels.SetPixelColor(offset, {0, 0, 0});
      for (int p = 0; p < pixels_per_cup; p++)
      {
        float color = constrain((available_cups - c) * pixels_per_cup - p, 0, 1);
        pixels.SetPixelColor(offset + p, {(byte)((1 - color) * config.brightness), (byte)(color * config.brightness), 0});
      }
    }
    break;
  }
}

void LED_set_pressing()
{
  // has not been called for some time, so restart the animation
  if (millis() - LED_period_start_ms > 2 * LED_ANIMATION_PERIOD)
    LED_period_start_ms = millis();
  //if it is still running, just make sure we are in a positive realm
  else if (millis() - LED_period_start_ms > LED_ANIMATION_PERIOD)
    LED_period_start_ms += LED_ANIMATION_PERIOD;
  //rotating animation
  int repetitions = 3;
  float time = (float)(millis() - LED_period_start_ms) / LED_ANIMATION_PERIOD;
  for (int i = 0; i < PIXEL_COUNT; i++)
  {
    float pos_in_loop = ((float)i) / PIXEL_COUNT;
    pos_in_loop += time;
    pos_in_loop *= repetitions;
    pos_in_loop -= floor(pos_in_loop);
    float symmetric = 1 - abs(pos_in_loop - 0.5) * 2; //1 to 0 left and right of half period (inveterd v shape curve)
    symmetric = symmetric * 2 - 1;
    pixels.SetPixelColor(i, {(byte)(max(config.brightness * symmetric, 0.0f)), (byte)(max(config.brightness * symmetric, 0.0f)), 0});
  }
}

void update_LED()
{
  if (weight < -0.1 * config.max_filling)
  {
    // no coffee pot
    pixels.ClearTo({0, 0, config.brightness});
  }
  else
  {
    float mean = 0;
    for(int i=0; i<HISTORY_LENGTH; i++)
      mean += weight_history[i];
    mean /= HISTORY_LENGTH;
    float diff = 0;
    for(int i=0; i<HISTORY_LENGTH; i++)
      diff += (weight_history[i]-mean)*(weight_history[i]-mean);
    diff = sqrt(diff / HISTORY_LENGTH);
    if (diff > STABLE_WEIGHT_MAX_DIFFERENCE)
    {
      // someone is pressing the lever
      LED_set_pressing();
    }
    else
    {
      // all ok: show filling
      LED_set_filling();
    }
  }
  pixels.Show();
}

void ui_update_value(Control *sender, int type)
{
  //dummy callback, the actual work takes place in ESPUI function
  Serial.println(ESPUI.getControl(num_brightness)->value.toInt());
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
    Serial.print("Current weight:");
    Serial.println(current_weight);
    //get balance reading with offset allready subtracted!
    float balance_reading = balance.get_value(AVERAGING_COUNT);
    Serial.print("Balance reading:");
    Serial.println(balance_reading);
    if (current_weight > 0 && current_weight < MAX_WEIGHT && balance_reading > 0)
    {
      config.balance_scale = balance_reading / current_weight;
      balance.set_scale(config.balance_scale);
      write_config();
    }
  }
}

void ui_save_config_clicked(Control *sender, int type)
{
  if (type == B_DOWN)
  {
    //make a copy of the current config and apply changes
    Config new_config = config;
    new_config.weight_per_cup = ESPUI.getControl(num_weight_per_cup)->value.toFloat();
    new_config.max_filling = ESPUI.getControl(num_max_filling)->value.toFloat();
    new_config.LED_mode = ESPUI.getControl(sel_LED_mode)->value.toInt();
    new_config.brightness = (byte)ESPUI.getControl(num_brightness)->value.toInt();
    if (new_config.weight_per_cup == 0)
    {
      ESPUI.updateLabel(lb_config_message, "Fehler: weight_per_cup");
      return;
    }
    if (new_config.max_filling <= 0 || new_config.max_filling > MAX_WEIGHT)
    {
      ESPUI.updateLabel(lb_config_message, "Fehler: max_filling");
      return;
    }
    if (new_config.LED_mode != LED_modes::absolute && new_config.LED_mode != LED_modes::relative && new_config.LED_mode != LED_modes::separated)
    {
      ESPUI.updateLabel(lb_config_message, "Fehler: LED_mode");
      return;
    }
    config = new_config;
    write_config();
    ESPUI.updateLabel(lb_config_message, "Gespeichert");
    update_LED();
  }
}

bool CreateWifiSoftAP()
{
  WiFi.disconnect();
  Serial.print(F("Initalize SoftAP "));
  bool SoftAccOK;
  SoftAccOK = WiFi.softAP(ssid, password);
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

void buildUI()
{
  //tab status
  int tab_status = ESPUI.addControl(ControlType::Tab, "Status", "Status");
  lb_status_id = ESPUI.addControl(ControlType::Label, "Status", "", ControlColor::Wetasphalt, tab_status);

  //tab balance
  int tab_balance = ESPUI.addControl(ControlType::Tab, "Waage", "Waage");
  const char *info_text = "Bitte erst mit leerem Behälter tarieren,<br>dann definiertes Gewicht auf die Platform stellen<br>und auf 'Übernehmen' clicken.";
  ESPUI.addControl(ControlType::Label, "Info", info_text, ControlColor::Emerald, tab_balance);
  ESPUI.addControl(ControlType::Button, "Tara", "Tarieren", ControlColor::Emerald, tab_balance, &ui_tare_clicked);
  num_current_weight_id = ESPUI.addControl(ControlType::Number, "Aktuelles Gewicht", "100", ControlColor::Carrot, tab_balance, &ui_update_value);
  ESPUI.addControl(ControlType::Button, "Kalibrieren", "Übernehmen", ControlColor::Carrot, tab_balance, &ui_calibrate_clicked);

  //tab config
  int tab_config = ESPUI.addControl(ControlType::Tab, "Config", "Config");
  lb_config_message = ESPUI.addControl(ControlType::Label, "Message", "", ControlColor::Wetasphalt, tab_config);
  num_weight_per_cup = ESPUI.addControl(ControlType::Number, "Gewicht pro Tasse", String(config.weight_per_cup), ControlColor::Alizarin, tab_config, &ui_update_value);
  num_max_filling = ESPUI.addControl(ControlType::Number, "Maximale Füllung", String(config.max_filling), ControlColor::Alizarin, tab_config, &ui_update_value);
  num_brightness = ESPUI.addControl(ControlType::Number, "Helligkeit", String(config.brightness), ControlColor::Alizarin, tab_config, &ui_update_value);
  sel_LED_mode = ESPUI.addControl(ControlType::Select, "Select:", String(config.LED_mode), ControlColor::Alizarin, tab_config, &ui_update_value);
  ESPUI.addControl(ControlType::Option, "Relativ", String(LED_modes::relative), ControlColor::Alizarin, sel_LED_mode);
  ESPUI.addControl(ControlType::Option, "Absolut", String(LED_modes::absolute), ControlColor::Alizarin, sel_LED_mode);
  ESPUI.addControl(ControlType::Option, "Sapariert", String(LED_modes::separated), ControlColor::Alizarin, sel_LED_mode);
  ESPUI.addControl(ControlType::Button, "Speichern", "Speichern", ControlColor::Alizarin, tab_config, &ui_save_config_clicked);

  //begin
  ESPUI.begin("CoffeeCounter");

  //setup captive portal
  AsyncCallbackWebHandler *handler = new AsyncCallbackWebHandler();
  handler->onRequest([](AsyncWebServerRequest *r) {
    r->redirect("http://" + WiFi.softAPIP().toString() + "/");
    Serial.print(r->host());
    Serial.println(r->url());
  });
  handler->setFilter(ON_AP_FILTER);
  ESPUI.server->addHandler(handler);
}

void DoCommunicationTask(void *parameters)
{
  for (;;)
  {
    dnsServer.processNextRequest();
    delay(3);
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1000);
  EEPROM.begin(sizeof(Config));
  //if config is not readable, write the default one
  Serial.println("Loading config.");
  if (!read_config())
  {
    Serial.println("Could not read config.");
    write_config();
  }

  //initialize balance
  balance.begin(BALANCE_PIN_DATA, BALANCE_PIN_CLOCK, BALANCE_GAIN);
  balance.set_offset(config.balance_offset);
  balance.set_scale(config.balance_scale);

  //Initialize neopixel
  pinMode(PIN_NEOPIXEL, OUTPUT);
  pixels.Begin();

  //start wifi hotspot
  CreateWifiSoftAP();
  buildUI();
  TaskHandle_t communicationTask;
  //start communication task on core 0, loop runs on core 1
  xTaskCreatePinnedToCore(DoCommunicationTask, //Task function
                          "CommunicationTask", //Task name
                          10000,               //stack depth
                          NULL,                //parameters
                          1,                   //priority
                          &communicationTask,  //out: task handle
                          0);                  //core
}

void loop()
{
  if (millis() > balance_last_update_ms + BALANCE_UPDATE_PERIOD_MS)
  {
    balance_last_update_ms = millis();
    for(int i=0; i<HISTORY_LENGTH-1; i++)
      weight_history[i] = weight_history[i+1];
    weight_history[HISTORY_LENGTH-1] = weight;
    last_weight = weight;
    weight = balance.get_units(AVERAGING_COUNT);
    //UI
    char s[128];
    snprintf(s, sizeof(s), "weight = %f g<br>offset = %f<br>scale = %f", weight, config.balance_offset, config.balance_scale);
    ESPUI.updateLabel(lb_status_id, s);
  }
  if (millis() > LED_last_update_ms + LED_UPDATE_PERIOD_MS)
  {
    LED_last_update_ms = millis();
    update_LED();
  }
}