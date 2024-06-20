#include <sys/time.h>
#ifdef ARDUINO_ARCH_ESP32
#include <WiFi.h>
#endif
#ifdef ARDUINO_ARCH_ESP8266
#include <ESP8266WiFi.h>
#endif
#include <errno.h>
#include "time.h"
#include "SerialCommands.h"
#include "EEPROM.h"
#include "sntp.h"

#define LED           2

#ifndef VERBOSE
#define VERBOSE
#endif
#ifndef DEBUG
#define DEBUG
#endif

/* NTP server to use, can be configured later on via AT commands */
#ifndef DEFAULT_NTP_SERVER
#define DEFAULT_NTP_SERVER "at.pool.ntp.org"
#endif

/* ESP yield */
#if defined(ARDUINO_ARCH_ESP8266) || defined(ARDUINO_ARCH_ESP32)
 #define doYIELD yield();
#else
 #define doYIELD
#endif

/* MODBUS */
#include <IPAddress.h>
#include <ModbusTCP.h>
#include <ModbusRTU.h>
int DE_RE = 2;
ModbusRTU rtu;
ModbusTCP tcp;
IPAddress srcIp;
uint16_t transRunning = 0;  // Currently executed ModbusTCP transaction
uint8_t slaveRunning = 0;   // Current request slave
 

/* our AT commands over UART to config OTP's and WiFi */
char atscbu[128] = {""};
SerialCommands ATSc(&Serial, atscbu, sizeof(atscbu), "\r\n", "\r\n");

#define CFGVERSION 0x01 // switch between 0x01/0x02 to reinit the config struct change
#define CFGINIT    0x72 // at boot init check flag
#define CFG_EEPROM 0x00 

/* main config */
struct wm_cfg {
  uint8_t initialized      = 0;
  uint8_t version          = 0;
  uint8_t do_debug         = 0;
  uint8_t do_verbose       = 0;
  uint8_t do_log           = 0;
  uint16_t main_loop_delay = 0;
  char wifi_ssid[32]       = {0};   // max 31 + 1
  char wifi_pass[64]       = {0};   // nax 63 + 1
  char ntp_host[64]        = {0};   // max hostname + 1
};
typedef wm_cfg wm_cfg_t;
wm_cfg_t cfg = {0};

/* state flags */
uint8_t ntp_is_synced      = 1;
uint8_t logged_wifi_status = 0;
unsigned long last_wifi_check = 0;

void(* resetFunc)(void) = 0;

char* at_cmd_check(const char *cmd, const char *at_cmd, unsigned short at_len){
  unsigned short l = strlen(cmd); /* AT+<cmd>=, or AT, or AT+<cmd>? */
  if(at_len >= l && strncmp(cmd, at_cmd, l) == 0){
    if(*(cmd+l-1) == '='){
      return (char *)at_cmd+l;
    } else {
      return (char *)at_cmd;
    }
  }
  return NULL;
}

void at_cmd_handler(SerialCommands* s, const char* atcmdline){
  unsigned int cmd_len = strlen(atcmdline);
  char *p = NULL;
  #ifdef AT_DEBUG
  Serial.print(F("AT: ["));
  Serial.print(atcmdline);
  Serial.print(F("], size: "));
  Serial.println(cmd_len);
  #endif
  if(cmd_len == 2 && (p = at_cmd_check("AT", atcmdline, cmd_len))){
  } else if(p = at_cmd_check("AT+WIFI_SSID=", atcmdline, cmd_len)){
    size_t sz = (atcmdline+cmd_len)-p+1;
    if(sz > 31){
      s->GetSerial()->println(F("WiFI SSID max 31 chars"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    strncpy((char *)&cfg.wifi_ssid, p, sz);
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
    WiFi.disconnect();
    setup_wifi();
    configTime(0, 0, (char *)&cfg.ntp_host);
  } else if(p = at_cmd_check("AT+WIFI_SSID?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.wifi_ssid);
    return;
  } else if(p = at_cmd_check("AT+WIFI_PASS=", atcmdline, cmd_len)){
    size_t sz = (atcmdline+cmd_len)-p+1;
    if(sz > 63){
      s->GetSerial()->println(F("WiFI password max 63 chars"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    strncpy((char *)&cfg.wifi_pass, p, sz);
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
    WiFi.disconnect();
    setup_wifi();
    configTime(0, 0, (char *)&cfg.ntp_host);
  #ifdef DEBUG
  } else if(p = at_cmd_check("AT+WIFI_PASS?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.wifi_pass);
  #endif
  } else if(p = at_cmd_check("AT+WIFI_STATUS?", atcmdline, cmd_len)){
    uint8_t wifi_stat = WiFi.status();
    switch(wifi_stat) {
        case WL_CONNECTED:
          s->GetSerial()->println(F("connected"));
          break;
        case WL_CONNECT_FAILED:
          s->GetSerial()->println(F("failed"));
          break;
        case WL_CONNECTION_LOST:
          s->GetSerial()->println(F("connection lost"));
          break;
        case WL_DISCONNECTED:
          s->GetSerial()->println(F("disconnected"));
          break;
        case WL_IDLE_STATUS:
          s->GetSerial()->println(F("idle"));
          break;
        case WL_NO_SSID_AVAIL:
          s->GetSerial()->println(F("no SSID configured"));
          break;
        default:
          s->GetSerial()->println(wifi_stat);
    }
    return;
  } else if(p = at_cmd_check("AT+NTP_HOST=", atcmdline, cmd_len)){
    size_t sz = (atcmdline+cmd_len)-p+1;
    if(sz > 63){
      s->GetSerial()->println(F("NTP hostname max 63 chars"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    strncpy((char *)&cfg.ntp_host, p, sz);
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
    setup_wifi();
    configTime(0, 0, (char *)&cfg.ntp_host);
  } else if(p = at_cmd_check("AT+NTP_HOST?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.ntp_host);
    return;
  } else if(p = at_cmd_check("AT+NTP_STATUS?", atcmdline, cmd_len)){
    if(ntp_is_synced)
      s->GetSerial()->println(F("ntp synced"));
    else
      s->GetSerial()->println(F("not ntp synced"));
    return;
  #ifdef DEBUG
  } else if(p = at_cmd_check("AT+DEBUG=1", atcmdline, cmd_len)){
    cfg.do_debug = 1;
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+DEBUG=0", atcmdline, cmd_len)){
    cfg.do_debug = 0;
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+DEBUG?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.do_debug);
  #endif
  #ifdef VERBOSE
  } else if(p = at_cmd_check("AT+VERBOSE=1", atcmdline, cmd_len)){
    cfg.do_verbose = 1;
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+VERBOSE=0", atcmdline, cmd_len)){
    cfg.do_verbose = 0;
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+VERBOSE?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.do_verbose);
  #endif
  } else if(p = at_cmd_check("AT+LOG_UART=1", atcmdline, cmd_len)){
    cfg.do_log = 1;
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+LOG_UART=0", atcmdline, cmd_len)){
    cfg.do_log = 0;
    EEPROM.put(CFG_EEPROM, cfg);
    EEPROM.commit();
  } else if(p = at_cmd_check("AT+LOG_UART?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.do_log);
  } else if(p = at_cmd_check("AT+LOOP_DELAY=", atcmdline, cmd_len)){
    errno = 0;
    unsigned int new_c = strtoul(p, NULL, 10);
    if(errno != 0){
      s->GetSerial()->println(F("invalid integer"));
      s->GetSerial()->println(F("ERROR"));
      return;
    }
    if(new_c != cfg.main_loop_delay){
      cfg.main_loop_delay = new_c;
      EEPROM.put(CFG_EEPROM, cfg);
      EEPROM.commit();
    }
  } else if(p = at_cmd_check("AT+LOOP_DELAY?", atcmdline, cmd_len)){
    s->GetSerial()->println(cfg.main_loop_delay);
  } else if(p = at_cmd_check("AT+RESET", atcmdline, cmd_len)){
    s->GetSerial()->println(F("OK"));
    resetFunc();
    return;
  } else {
    s->GetSerial()->println(F("ERROR"));
    return;
  }
  s->GetSerial()->println(F("OK"));
  return;
}

void setup() {
  // Serial setup, init at 115200 8N1
  Serial.begin(115200, SERIAL_8N1);

  // EEPROM read
  EEPROM.begin(sizeof(cfg));
  EEPROM.get(CFG_EEPROM, cfg);
  // was (or needs) initialized?
  if(cfg.initialized != CFGINIT || cfg.version != CFGVERSION){
    // clear
    memset(&cfg, 0, sizeof(cfg));
    // reinit
    cfg.initialized     = CFGINIT;
    cfg.version         = CFGVERSION;
    cfg.do_log          = 1;
    cfg.main_loop_delay = 100;
    strcpy((char *)&cfg.ntp_host, (char *)DEFAULT_NTP_SERVER);
    // write
    EEPROM.put(CFG_EEPROM, cfg);
  }

  #ifdef VERBOSE
  if(cfg.do_verbose){
    Serial.print(F("eeprom size: "));
    Serial.println(EEPROM.length());
  }
  #endif

  #ifdef VERBOSE
  if(cfg.do_verbose){
    if(strlen(cfg.wifi_ssid) && strlen(cfg.wifi_pass)){
      Serial.print(F("will connect via WiFi to "));
      Serial.println(cfg.wifi_ssid);
    } else {
      Serial.print(F("will not connect via WiFi, no ssid/pass"));
    }
  }
  #endif

  // Setup AT command handler
  ATSc.SetDefaultHandler(&at_cmd_handler);

  // see http://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html
  // for OTP tokens, this ALWAYS have to be "UTC"
  setenv("TZ", "UTC", 1);
  tzset();

  // setup WiFi with ssid/pass from EEPROM if set
  setup_wifi();

  #ifdef VERBOSE
  if(cfg.do_verbose){
    if(strlen(cfg.ntp_host) && strlen(cfg.wifi_ssid) && strlen(cfg.wifi_pass)){
      Serial.print(F("will sync with ntp, wifi ssid/pass ok: "));
      Serial.println(cfg.ntp_host);
    }
  }
  #endif
  // setup NTP sync to RTC
  configTime(0, 0, (char *)&cfg.ntp_host);
  // new time - set by NTP
  time_t t;
  struct tm gm_new_tm;
  time(&t);
  localtime_r(&t, &gm_new_tm);
  #ifdef VERBOSE
  if(cfg.do_verbose){
    Serial.print(F("now: "));
    Serial.println(t);
    char d_outstr[100];
    strftime(d_outstr, 100, "current time: %A, %B %d %Y %H:%M:%S (%s)", &gm_new_tm);
    Serial.println(d_outstr);
  }
  #endif

  // led to show status
  pinMode(LED, OUTPUT);

  app_setup();
}

void loop() {
  // any new AT command? on USB uart
  ATSc.ReadSerial();

  delay(cfg.main_loop_delay);

  // just wifi check
  if(millis() - last_wifi_check > 500){
    if(WiFi.status() == WL_CONNECTED){
      if(!logged_wifi_status){
        #ifdef VERBOSE
        if(cfg.do_verbose){
          Serial.print(F("WiFi connected: "));
          Serial.println(WiFi.localIP());
        }
        #endif
        logged_wifi_status = 1;
      }
      wifi_enabled_callback();
    }
  }

  app_loop();

  yield();
}

void setup_wifi(){
  // are we connecting to WiFi?
  if(strlen(cfg.wifi_ssid) == 0 || strlen(cfg.wifi_pass) == 0)
    return;
  if(WiFi.status() == WL_CONNECTED)
    return;

  // connect to Wi-Fi
  #ifdef VERBOSE
  if(cfg.do_verbose){
    Serial.print(F("Connecting to "));
    Serial.println(cfg.wifi_ssid);
  }
  #endif
  WiFi.persistent(false);
  WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
}

/* MODBUS APP */

void app_setup(){
  rtu.begin(&Serial1, DE_RE);  // Specify RE_DE control pin
  rtu.master();          // Initialize ModbusRTU as master
  rtu.onRaw(cbRtuRaw);   // Assign raw data processing callback
}

void app_loop(){
  if(logged_wifi_status)
    tcp.task();
  rtu.task();
}

void wifi_enabled_callback(){
  /* ModBus TCP server startup */
  tcp.server();
  tcp.onRaw(cbTcpRaw);
}

bool cbTcpTrans(Modbus::ResultCode event, uint16_t transactionId, void* data) { // Modbus Transaction callback
  if (event != Modbus::EX_SUCCESS)                  // If transaction got an error
    Serial.printf("Modbus result: %02X, Mem: %d\n", event, ESP.getFreeHeap());  // Display Modbus error code (222527)
  if (event == Modbus::EX_TIMEOUT) {    // If Transaction timeout took place
    tcp.disconnect(tcp.eventSource());          // Close connection
    transRunning = 0;
    slaveRunning = 0;
  }
  return true;
}

bool cbRtuTrans(Modbus::ResultCode event, uint16_t transactionId, void* data) {
  if (event != Modbus::EX_SUCCESS)                  // If transaction got an error
    Serial.printf("Modbus result: %02X, Mem: %d\n", event, ESP.getFreeHeap());  // Display Modbus error code (222527)
  return true;
}


// Callback receives raw data 
Modbus::ResultCode cbTcpRaw(uint8_t* data, uint8_t len, void* custom) {
  auto src = (Modbus::frame_arg_t*) custom;
  Serial.print("TCP IP in - ");
  Serial.print(IPAddress(src->ipaddr));
  Serial.printf(" Fn: %02X, len: %d \n\r", data[0], len);
  if (transRunning) { // Note that we can't process new requests from TCP-side while waiting for responce from RTU-side.
    tcp.setTransactionId(src->transactionId); // Set transaction id as per incoming request
    tcp.errorResponce(IPAddress(src->ipaddr), (Modbus::FunctionCode)data[0], Modbus::EX_SLAVE_DEVICE_BUSY);
    return Modbus::EX_SLAVE_DEVICE_BUSY;
  }
  rtu.rawRequest(src->unitId, data, len, cbRtuTrans);
  if (!src->unitId) { // If broadcast request (no responce from slave is expected)
    tcp.setTransactionId(src->transactionId); // Set transaction id as per incoming request
    tcp.errorResponce(IPAddress(src->ipaddr), (Modbus::FunctionCode)data[0], Modbus::EX_ACKNOWLEDGE);
    transRunning = 0;
    slaveRunning = 0;
    return Modbus::EX_ACKNOWLEDGE;
  }
  srcIp = IPAddress(src->ipaddr);
  slaveRunning = src->unitId;
  transRunning = src->transactionId;
  return Modbus::EX_SUCCESS;  
}


// Callback receives raw data from ModbusTCP and sends it on behalf of slave (slaveRunning) to master
Modbus::ResultCode cbRtuRaw(uint8_t* data, uint8_t len, void* custom) {
  auto src = (Modbus::frame_arg_t*) custom;
  if (!transRunning) // Unexpected incoming data
      return Modbus::EX_PASSTHROUGH;
  tcp.setTransactionId(transRunning); // Set transaction id as per incoming request
  uint16_t succeed = tcp.rawResponce(srcIp, data, len, slaveRunning);
  if (!succeed){
    Serial.print("TCP IP out - failed");
  }
  Serial.printf("RTU Slave: %d, Fn: %02X, len: %d, ", src->slaveId, data[0], len);
  Serial.print("Response TCP IP: ");
  Serial.println(srcIp);
  
  transRunning = 0;
  slaveRunning = 0;
  return Modbus::EX_PASSTHROUGH;
}
 
