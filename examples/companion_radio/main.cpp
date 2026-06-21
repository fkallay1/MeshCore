#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include "MyMesh.h"

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char* sp) {
  uint32_t n = 0;
  while (*sp && *sp >= '0' && *sp <= '9') {
    n *= 10;
    n += (*sp++ - '0');
  }
  return n;
}

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
  #if defined(QSPIFLASH)
    #include <CustomLFS_QSPIFlash.h>
    DataStore store(InternalFS, QSPIFlash, rtc_clock);
  #else
  #if defined(EXTRAFS)
    #include <CustomLFS.h>
    CustomLFS ExtraFS(0xD4000, 0x19000, 128);
    DataStore store(InternalFS, ExtraFS, rtc_clock);
  #else
    DataStore store(InternalFS, rtc_clock);
  #endif
  #endif
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
  DataStore store(LittleFS, rtc_clock);
#elif defined(ESP32)
  #include <SPIFFS.h>
  DataStore store(SPIFFS, rtc_clock);
#endif

#ifdef ESP32
  #ifdef WIFI_SSID
    #include <helpers/esp32/SerialWifiInterface.h>
    SerialWifiInterface serial_interface;
    #ifndef TCP_PORT
      #define TCP_PORT 5000
    #endif
  #elif defined(BLE_PIN_CODE)
    #include <helpers/esp32/SerialBLEInterface.h>
    SerialBLEInterface serial_interface;
  #elif defined(SERIAL_RX)
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
    HardwareSerial companion_serial(1);
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(RP2040_PLATFORM)
  //#ifdef WIFI_SSID
  //  #include <helpers/rp2040/SerialWifiInterface.h>
  //  SerialWifiInterface serial_interface;
  //  #ifndef TCP_PORT
  //    #define TCP_PORT 5000
  //  #endif
  // #elif defined(BLE_PIN_CODE)
  //   #include <helpers/rp2040/SerialBLEInterface.h>
  //   SerialBLEInterface serial_interface;
  #if defined(SERIAL_RX)
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
    HardwareSerial companion_serial(1);
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(NRF52_PLATFORM)
  #if defined(BLE_PIN_CODE) && !defined(DEBUG_NO_BLE)
    #include <helpers/nrf52/SerialBLEInterface.h>
    SerialBLEInterface serial_interface;
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
    #ifdef DEBUG_NO_BLE
      #define companion_serial Serial1
    #endif
  #endif
#elif defined(STM32_PLATFORM)
  #include <helpers/ArduinoSerialInterface.h>
  ArduinoSerialInterface serial_interface;
#else
  #error "need to define a serial interface"
#endif

/* GLOBAL OBJECTS */
#ifdef DISPLAY_CLASS
  #include "UITask.h"
  UITask ui_task(&board, &serial_interface);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;
MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store
   #ifdef DISPLAY_CLASS
      , &ui_task
   #endif
);

/* END GLOBAL OBJECTS */

void halt() {
  while (1) ;
}

void setup() {
  Serial.begin(115200);

  board.begin();

#ifdef DISPLAY_CLASS
  DisplayDriver* disp = NULL;
  if (display.begin()) {
    disp = &display;
    disp->startFrame();
  #ifdef ST7789
    disp->setTextSize(2);
  #endif
    disp->drawTextCentered(disp->width() / 2, 28, "Loading...");
    disp->endFrame();
  }
#endif

  if (!radio_init()) { halt(); }

  fast_rng.begin(radio_get_rng_seed());

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  #if defined(QSPIFLASH)
    if (!QSPIFlash.begin()) {
      // debug output might not be available at this point, might be too early. maybe should fall back to InternalFS here?
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: failed to initialize");
    } else {
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: initialized successfully");
    }
  #else
  #if defined(EXTRAFS)
      ExtraFS.begin();
  #endif
  #endif
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

#if defined(BLE_PIN_CODE) && !defined(DEBUG_NO_BLE)
  serial_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
#elif defined(DEBUG_NO_BLE)
  companion_serial.begin(115200);
  serial_interface.begin(companion_serial);
  Serial.println("[DEBUG] BLE off, companion on Serial1");
#else
  serial_interface.begin(Serial);
#endif
  the_mesh.startInterface(serial_interface);
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

  //#ifdef WIFI_SSID
  //  WiFi.begin(WIFI_SSID, WIFI_PWD);
  //  serial_interface.begin(TCP_PORT);
  // #elif defined(BLE_PIN_CODE)
  //   char dev_name[32+16];
  //   sprintf(dev_name, "%s%s", BLE_NAME_PREFIX, the_mesh.getNodeName());
  //   serial_interface.begin(dev_name, the_mesh.getBLEPin());
  #if defined(SERIAL_RX)
    companion_serial.setPins(SERIAL_RX, SERIAL_TX);
    companion_serial.begin(115200);
    serial_interface.begin(companion_serial);
  #else
    serial_interface.begin(Serial);
  #endif
    the_mesh.startInterface(serial_interface);
#elif defined(ESP32)
  SPIFFS.begin(true);
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

#ifdef WIFI_SSID
  board.setInhibitSleep(true);   // prevent sleep when WiFi is active
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  serial_interface.begin(TCP_PORT);
#elif defined(BLE_PIN_CODE)
  serial_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
#elif defined(SERIAL_RX)
  companion_serial.setPins(SERIAL_RX, SERIAL_TX);
  companion_serial.begin(115200);
  serial_interface.begin(companion_serial);
#else
  serial_interface.begin(Serial);
#endif
  the_mesh.startInterface(serial_interface);
#else
  #error "need to define filesystem"
#endif

  sensors.begin();

#ifdef DISPLAY_CLASS
  ui_task.begin(disp, &sensors, the_mesh.getNodePrefs());  // still want to pass this in as dependency, as prefs might be moved
#endif
}

extern uint32_t g_nrf52_reset_reason;

void fkInfo1_Reset_WDT() {



  uint32_t reas = g_nrf52_reset_reason; //NRF_POWER->GPREGRET | (NRF_POWER->GPREGRET2 << 8);
    
    Serial.println("\n=== POSLEDNY RESET (z retained reg) ===");
    if (reas & (1 << 1)) Serial.println("  >> DOG - WDT timeout!");
    if (reas & (1 << 2)) Serial.println("  >> SREQ - Software reset (SoftDevice)!");
    if (reas & (1 << 3)) Serial.println("  >> LOCKUP!");
    if (reas & (1 << 16)) Serial.println("  >> DIF - Debug interface");
    Serial.print("  Raw = 0x"); Serial.println(reas, HEX);
    

    uint32_t wdt_running = NRF_WDT->RUNSTATUS;
    Serial.print  ("  NRF_WDT->RUNSTATUS = "); Serial.println(wdt_running);
    // 0 = WDT NEBEZI, 1 = WDT BEZI (a uz ho NEMOZES zastavit)

    if (wdt_running) {
        uint32_t crv = NRF_WDT->CRV;
        float timeout_sec = (float)crv / 32768.0f;
        Serial.print  ("  WDT timeout = "); Serial.print(timeout_sec, 2);
        Serial.println(" sekund");

        uint32_t cfg = NRF_WDT->CONFIG;
        Serial.print  ("  WDT CONFIG register = 0x"); Serial.println(cfg, HEX);
        // Bit 0 (SLEEP): 0=pauza pocas sleep, 1=bezi aj pocas sleep
        // Bit 3 (HALT):  0=pauza pri debug halt, 1=bezi aj pri debug halt
        if (cfg & (1 << 3))
            Serial.println("  !! HALT=1: WDT bezi aj ked debugger zastavi CPU !!");
        else
            Serial.println("  OK: HALT=0: WDT sa zastavi ked debugger zastavi CPU");

        uint32_t reqstat = NRF_WDT->REQSTATUS;
        Serial.print  ("  Kto musi feedovat WDT (REQSTATUS) = 0x");
        Serial.println(reqstat, HEX);
        // Bit N = reload register RRn je aktivny a musi byt feedovany
    } else {
        Serial.println("  WDT NEBEZI - reset nebol kvoli WDT");
    }

}


static uint32_t s_fkInfo1 = 0;

void loop() {

  if ((s_fkInfo1++ % 1000000) == 0) {

      fkInfo1_Reset_WDT();
      Serial.print("Counter AA: ");
      Serial.println(s_fkInfo1);
  }




  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
}
