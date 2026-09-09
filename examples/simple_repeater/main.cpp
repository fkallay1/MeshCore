#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "MyMesh.h"

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(board, display);
#endif

#ifdef ETHERNET_ENABLED
  #define ETHERNET_CLI_BANNER "MeshCore Repeater CLI"
  #include <helpers/nrf52/EthernetCLI.h>
#endif

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

static char command[160];
#ifdef ETHERNET_ENABLED
static char ethernet_command[160];
#endif

// For power saving
unsigned long POWERSAVING_FIRSTSLEEP_SECS = 120; // The first sleep (if enabled) from boot

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
static unsigned long userBtnDownAt = 0;
#define USER_BTN_HOLD_OFF_MILLIS 1500
#endif

#if defined(PIN_LORA_CE) && defined(P_LORA_NSS) && defined(P_LORA_SCLK) && defined(P_LORA_MOSI) \
    && defined(P_LORA_MISO) && defined(P_LORA_BUSY) && defined(P_LORA_RESET) && defined(P_LORA_DIO_1)
//en: Report whether a line is driven by the module or just floating. A wire nothing
//en: holds follows the pull we apply; a line the module drives does not.
//sk: Ukaz, ci linku budi modul, alebo len plava. Vodic, ktory nikto nedrzi, ide za
//sk: pullom, ktory nastavime; linku, ktoru modul naozaj budi, to nepohne.
static void fk_pin_report(const char* tag) {
  const uint8_t pins[4] = { P_LORA_MISO, P_LORA_BUSY, P_LORA_DIO_1, PIN_LORA_CE };
  const char*   name[4] = { "MISO", "BUSY", "IRQ", "CE" };
  for (int i = 0; i < 4; i++) {
    pinMode(pins[i], INPUT_PULLUP);   delay(2); int up = digitalRead(pins[i]);
    pinMode(pins[i], INPUT_PULLDOWN); delay(2); int dn = digitalRead(pins[i]);
    pinMode(pins[i], INPUT);
    Serial.printf("[FK] pin %-4s %s: pu=%d pd=%d -> %s\r\n", name[i], tag, up, dn,
                  (up != dn) ? "FLOATING (nothing holds it)"
                             : (up ? "driven HIGH" : "driven LOW"));
  }
}

//en: Real power-down of the module. PIN_LORA_CE is the LDO enable, so pulling it low
//en: removes the supply - but only if nothing back-feeds the module through an IO pin,
//en: which is what the module manual warns about. So every line we drive goes low first
//en: and the module own outputs are left as inputs, so they cannot source current either.
//sk: Skutocne odpojenie modulu od napajania. PIN_LORA_CE je enable LDO, takze stiahnutim
//sk: dole zmizne napajanie - ale len ak modul nikto neprinapaja cez IO pin, presne pred
//sk: cim varuje manual modulu. Preto vsetky budene linky idu najprv dole a vystupy modulu
//sk: nechame ako vstupy, aby ani ony nemohli dodavat prud.
static void fk_module_power_cycle(uint32_t off_ms) {
  const uint8_t drive_low[4] = { P_LORA_NSS, P_LORA_SCLK, P_LORA_MOSI, P_LORA_RESET };
  const uint8_t as_input[3]  = { P_LORA_MISO, P_LORA_BUSY, P_LORA_DIO_1 };
  for (int i = 0; i < 3; i++) pinMode(as_input[i], INPUT);
  for (int i = 0; i < 4; i++) { pinMode(drive_low[i], OUTPUT); digitalWrite(drive_low[i], LOW); }
  pinMode(PIN_LORA_CE, OUTPUT); digitalWrite(PIN_LORA_CE, LOW);
  Serial.printf("[FK] module power-down: CE low, driven lines low, %lu ms\r\n", (unsigned long)off_ms);
  delay(off_ms);
  digitalWrite(PIN_LORA_CE, HIGH);
  delay(50);
  Serial.println("[FK] module power-up: CE high");
}

//en: exposed for 'fk ce <ms>' - report the lines, drop the supply for off_ms, report
//en: again. The pin report either side is the point: it says whether the module really
//en: went dark or whether something still feeds it through an IO pin.
//sk: vyvedene pre 'fk ce <ms>' - vypis linky, zhod napajanie na off_ms, vypis znova.
//sk: Prave ten vypis pred a po je podstatny: povie, ci modul naozaj zhasol, alebo ho
//sk: nieco stale prinapaja cez IO pin.
void fk_ce_cycle(uint32_t off_ms) {
  fk_pin_report("pred ce off");
  fk_module_power_cycle(off_ms);
  fk_pin_report("po ce on");
}
#endif

void setup() {
  Serial.begin(115200);
#ifdef FK_SERIAL_WAIT_DTR
  //en: wait for the host to actually open the port (DTR), instead of a blind
  //en: delay — boot messages then reliably reach a terminal that races to
  //en: (re)connect right after a reboot (e.g. the fota_serial_hub.py fast-reconnect
  //en: loop). Falls through after FK_SERIAL_WAIT_DTR ms if nothing is listening.
  //sk: počkaj, kým si host naozaj otvorí port (DTR), namiesto slepého delay —
  //sk: boot hlášky sa tak spoľahlivo dostanú aj k terminálu, ktorý sa pripája
  //sk: pretekom hneď po reboote (napr. fota_serial_hub.py rýchly reconnect).
  //sk: Po FK_SERIAL_WAIT_DTR ms bez poslucháča pokračuje ďalej.
  uint32_t serial_wait_t0 = millis();
  while (!Serial && (millis() - serial_wait_t0) < FK_SERIAL_WAIT_DTR) delay(10);
#else
  delay(1000);
#endif

  board.begin();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.begin();
#endif

#if defined(MESH_DEBUG) && defined(NRF52_PLATFORM)
  // give some extra time for serial to settle so
  // boot debug messages can be seen on terminal
  delay(5000);
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) {
    MESH_DEBUG_PRINTLN("Radio init failed!");
    halt();
  }

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Repeater ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;
#ifdef ETHERNET_ENABLED
  ethernet_command[0] = 0;
#endif

  sensors.begin();

  the_mesh.begin(fs);

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

#ifdef ETHERNET_ENABLED
  ethernet_start_task();
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif

  board.onBootComplete();
}

void loop() {
  // Handle Serial CLI
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    char reply[160];
    reply[0] = 0;
#ifdef ETHERNET_ENABLED
    if (!ethernet_handle_command(command, reply)) {
      the_mesh.handleCommand(0, command, reply);
    }
#else
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
#endif
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

#ifdef ETHERNET_ENABLED
  ethernet_loop_maintain();
  if (ethernet_read_line(ethernet_command, sizeof(ethernet_command))) {
    char reply[160];
    reply[0] = 0;
    if (!ethernet_handle_command(ethernet_command, reply)) {
      the_mesh.handleCommand(0, ethernet_command, reply);
    }
    ethernet_send_reply(reply);
    ethernet_command[0] = 0;
  }
#endif

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_) && !defined(DISPLAY_CLASS)
  // Hold the user button to power off the SenseCAP Solar repeater.
  int btnState = digitalRead(PIN_USER_BTN);
  if (btnState == LOW) {
    if (userBtnDownAt == 0) {
      userBtnDownAt = millis();
    } else if ((unsigned long)(millis() - userBtnDownAt) >= USER_BTN_HOLD_OFF_MILLIS) {
      Serial.println("Powering off...");
      board.powerOff();  // does not return
    }
  } else {
    userBtnDownAt = 0;
  }
#endif

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif
  if (the_mesh.getNodePrefs()->powersaving_enabled && !the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#else
    if (the_mesh.millisHasNowPassed(POWERSAVING_FIRSTSLEEP_SECS * 1000)) { // To check if it is time to sleep
      board.sleep(30); // Sleep. Wake up after a while or when receiving a LoRa packet
    }
#endif
  }
}
