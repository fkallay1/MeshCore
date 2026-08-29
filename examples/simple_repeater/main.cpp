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
  {
    bool disp_ok = display.begin();
  #ifdef FK_DEBUG
    //en: an I2C panel that does not ACK stays silent with no other symptom - say so
    //sk: I2C panel, ktory neodpovie, ostane ticho bez ineho priznaku - vypis to
    Serial.print("[FK] display.begin() -> ");
    Serial.println(disp_ok ? "OK" : "FAILED (no I2C ACK - wiring/address/power?)");
  #endif
    if (disp_ok) {
      display.startFrame();
      display.setCursor(0, 0);
      display.print("Please wait...");
      display.endFrame();
    }
  }
#endif

  bool fk_radio_ok = radio_init();
#if defined(PIN_LORA_CE) && defined(P_LORA_NSS) && defined(P_LORA_SCLK) && defined(P_LORA_MOSI) \
    && defined(P_LORA_MISO) && defined(P_LORA_BUSY) && defined(P_LORA_RESET) && defined(P_LORA_DIO_1)
  //en: The LR2021 can come up in a state where init cannot reach it, and only removing
  //en: its supply clears it - which until now meant unplugging the board by hand. Try it
  //en: in software before giving up, and report the line states either way.
  //sk: LR2021 sa vie dostat do stavu, v ktorom sa k nemu init nedostane, a vylieci to len
  //sk: odobranie napajania - co doteraz znamenalo odpojit dosku rukou. Skus to najprv
  //sk: softverovo a v oboch pripadoch vypis stav liniek.
  for (int attempt = 1; attempt <= 3 && !fk_radio_ok; attempt++) {
    Serial.printf("[FK] radio init failed - power-cycling module, attempt %d/3\r\n", attempt);
    fk_pin_report("before");
    fk_module_power_cycle(300);
    fk_pin_report("after ");
    fk_radio_ok = radio_init();
    Serial.printf("[FK] radio init after power-cycle: %s\r\n", fk_radio_ok ? "OK" : "still failing");
  }
#endif
  if (!fk_radio_ok) {
    MESH_DEBUG_PRINTLN("Radio init failed!");
#ifdef FK_DEBUG
    //en: A silent halt() looks exactly like a dead board on the terminal, and the
    //en: one-shot error from std_init() is long gone before a terminal can attach.
    //en: Keep repeating it so the reason is visible whenever you connect.
    //sk: Tiché halt() vyzerá na termináli presne ako mŕtva doska a jednorazovú
    //sk: hlášku z std_init() nikto nestihne. Opakuj ju, nech je dôvod vidieť
    //sk: kedykoľvek sa pripojíš.
    //en: 'chip not found' (-2) only says the version register did not read back
    //en: 'SX1262'. Show WHY: the BUSY line and the raw bytes separate an unpowered
    //en: module from a broken signal line, which the error code alone cannot.
    //sk: 'chip not found' (-2) hovori len tolko, ze z verzioveho registra sa
    //sk: nevratilo 'SX1262'. Ukaz PRECO: linka BUSY a surove bajty odlisia
    //sk: nenapajany modul od preruseneho signalu, co samotny kod chyby nevie.
#if defined(SX126X_POWER_EN) && defined(P_LORA_BUSY) && defined(P_LORA_RESET) && defined(P_LORA_NSS)
    {
      pinMode(SX126X_POWER_EN, OUTPUT); digitalWrite(SX126X_POWER_EN, HIGH);
      pinMode(P_LORA_BUSY, INPUT);
      delay(20);
      Serial.printf("[FK] radio-diag: POWER_EN=%d high, BUSY=%d (1 = chip not ready: unpowered, in reset, or no TCXO)\r\n",
                    SX126X_POWER_EN, digitalRead(P_LORA_BUSY));

      pinMode(P_LORA_RESET, OUTPUT);
      digitalWrite(P_LORA_RESET, LOW);  delay(2);
      digitalWrite(P_LORA_RESET, HIGH); delay(20);
      Serial.printf("[FK] radio-diag: after reset pulse BUSY=%d\r\n", digitalRead(P_LORA_BUSY));

      //en: raw READ_REGISTER(0x1D) of the version string at 0x0320, bypassing RadioLib
      uint8_t buf[16];
      pinMode(P_LORA_NSS, OUTPUT); digitalWrite(P_LORA_NSS, HIGH);
      SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
      digitalWrite(P_LORA_NSS, LOW);
      SPI.transfer(0x1D); SPI.transfer(0x03); SPI.transfer(0x20); SPI.transfer(0x00);
      for (int i = 0; i < 16; i++) buf[i] = SPI.transfer(0x00);
      digitalWrite(P_LORA_NSS, HIGH);
      SPI.endTransaction();

      Serial.print("[FK] radio-diag: version reg =");
      bool all00 = true, allff = true;
      for (int i = 0; i < 16; i++) {
        Serial.printf(" %02X", buf[i]);
        if (buf[i] != 0x00) all00 = false;
        if (buf[i] != 0xFF) allff = false;
      }
      Serial.print("  \"");
      for (int i = 0; i < 16; i++) Serial.print((buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.');
      Serial.println("\"");
      Serial.print("[FK] radio-diag: ");
      if (all00)      Serial.println("all 00 -> MISO stuck low: module unpowered, or MISO/GND wiring");
      else if (allff) Serial.println("all FF -> MISO floating high: module unpowered, or MISO/NSS not connected");
      else            Serial.println("garbage -> module answers but wrong: check SCLK/MOSI, or wrong chip");

      //en: A wire that is not connected follows whatever pull we apply; a line the
      //en: module really drives does not. This separates a broken dupont (or an
      //en: unpowered module) from a module that is powered but silent.
      //sk: Nepripojeny vodic sleduje pull, ktory nastavime; linku, ktoru modul
      //sk: naozaj budi, to nepohne. Takto sa odlisi prerusene prepojenie (alebo
      //sk: nenapajany modul) od modulu, ktory napajanie ma, ale mlci.
      const uint8_t probe_pins[3] = { P_LORA_MISO, P_LORA_BUSY, P_LORA_DIO_1 };
      const char*   probe_name[3] = { "MISO", "BUSY", "DIO1" };
      for (int i = 0; i < 3; i++) {
        pinMode(probe_pins[i], INPUT_PULLUP);   delay(2); int up = digitalRead(probe_pins[i]);
        pinMode(probe_pins[i], INPUT_PULLDOWN); delay(2); int dn = digitalRead(probe_pins[i]);
        pinMode(probe_pins[i], INPUT);
        Serial.printf("[FK] radio-diag: %s pu=%d pd=%d -> %s\r\n", probe_name[i], up, dn,
                      (up != dn) ? "FLOATING - nothing on the other end (broken wire / module unpowered)"
                                 : (up ? "driven HIGH by the module" : "driven LOW by the module"));
      }
    }
#endif

    while (1) {
      Serial.println("[FK] Radio init FAILED - halted (check the radio module: VCC, SPI wiring, IRQ pin)");
      delay(2000);
    }
#endif
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
    if (c == '\b' || c == 0x7F) {  // accept both common terminal backspace encodings
      if (len > 0) {
        command[--len] = 0;
        Serial.print("\b \b");  // erase the character in terminals without local echo
      }
    } else if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-2] = '\r';   // force-complete the line ([len-1] is tested below; [sizeof-1] would clobber the NUL and never match)
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
