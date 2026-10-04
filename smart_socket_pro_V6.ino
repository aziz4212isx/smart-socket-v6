// ============================================
// Smart Plug Monitor ESP32
// Fitur: PZEM-004T, Relay 2ch, Buzzer,
//        Web UI, Blynk, WiFi Manager
// ============================================
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PZEM004Tv30.h>

#define BLYNK_TEMPLATE_ID   "TMPL6nN6ky4AG"
#define BLYNK_TEMPLATE_NAME "Smart Plug"
#include <BlynkSimpleEsp32.h>

// ============================================
// PENGATURAN PIN
// ============================================
#define PIN_RELAY1   26
#define PIN_RELAY2   25
#define PIN_BUZZER   32
#define PIN_TOMBOL1  14
#define PIN_TOMBOL2  12

// ============================================
// VIRTUAL PIN BLYNK
// ============================================
#define VPIN_TEGANGAN  V0
#define VPIN_ARUS      V1
#define VPIN_DAYA      V2
#define VPIN_KWH       V3
#define VPIN_BIAYA     V4
#define VPIN_RELAY1    V5
#define VPIN_RELAY2    V6
#define VPIN_STATUS    V7
#define VPIN_PF        V8
#define VPIN_FREK      V9
#define VPIN_SALDO     V10

// ============================================
// OBJEK
// ============================================
HardwareSerial serialPZEM(2);
PZEM004Tv30 pzem(serialPZEM, 16, 17);
WebServer webServer(80);
Preferences flash;

// ============================================
// VARIABEL STATUS
// ============================================
bool relay1Nyala  = true;
bool relay2Nyala  = true;
bool tombol1Lama  = HIGH;
bool tombol2Lama  = HIGH;
bool adaAlert     = false;
bool relayTrip    = false;

// ============================================
// VARIABEL DATA LISTRIK
// ============================================
float tegangan    = 0;
float arus        = 0;
float daya        = 0;
float energi      = 0;
float frekuensi   = 0;
float powerFaktor = 0;
float kWhAwal     = 0;
float saldoPulsa  = 0;
float kwhTerakhir = 0;
bool pulsaHabis   = false;

// ============================================
// VARIABEL PENGATURAN
// ============================================
String wifiSSID   = "";
String wifiPass   = "";
String blynkAuth  = "";
float tarifKwh    = 1444.0;
float batasDayaW  = 500.0;
float batastArusA = 2.0;
bool modePulsa    = true;  // true = mode pulsa aktif, false = listrik bebas/gratis

// ============================================
// VARIABEL TIMER
// ============================================
unsigned long waktuBuzzerTerakhir  = 0;
unsigned long waktuBacaTerakhir    = 0;
unsigned long waktuBlynkTerakhir   = 0;
unsigned long waktuRekonekTerakhir = 0;   // ← untuk auto-reconnect
int percobaanKonek = 0;
bool modeLokal = false;

// ============================================
// FORWARD DECLARATION
// ============================================
void setupWiFi();
void setupWebServer();
void bacaSensorPZEM();
void updateBlynk();
void cekTombol(int pin, bool &statusLama, bool &statusRelay, int pinRelay);
void nyalakanRelay(int pin);
void matikanRelay(int pin);
void matikanSemuaRelay();
void bunyikanBuzzer(int berapa, int durasi);
void halamanUtama();
void halamanSettings();
void halamanInfo();
void toggleRelay1();
void toggleRelay2();
void toggleModePulsa();
void resetKwh();
void simpanWifi();
void resetWifi();
void scanWifi();
void simpanKonfigurasi();
void simpanBlynk();
void ambilData();
void kalibrasiBoth();

// ============================================
// BLYNK: Terima perintah dari app
// ============================================
BLYNK_WRITE(VPIN_RELAY1) {
  int nilai = param.asInt();
  // Blokir jika pulsa habis DAN mode pulsa aktif DAN perintahnya NYALAKAN
  if (modePulsa && pulsaHabis && nilai == 1) {
    Blynk.virtualWrite(VPIN_RELAY1, 0);
    bunyikanBuzzer(3, 50);
    return;
  }
  if (relayTrip && nilai == 1) {
    Blynk.virtualWrite(VPIN_RELAY1, 0);
    return;
  }
  relay1Nyala = (nilai == 1);
  relay1Nyala ? nyalakanRelay(PIN_RELAY1) : matikanRelay(PIN_RELAY1);
  flash.putBool("relay1", relay1Nyala);
  if (relay1Nyala) bunyikanBuzzer(1, 80);
  else bunyikanBuzzer(2, 40);
}

BLYNK_WRITE(VPIN_RELAY2) {
  int nilai = param.asInt();
  if (modePulsa && pulsaHabis && nilai == 1) {
    Blynk.virtualWrite(VPIN_RELAY2, 0);
    bunyikanBuzzer(3, 50);
    return;
  }
  if (relayTrip && nilai == 1) {
    Blynk.virtualWrite(VPIN_RELAY2, 0);
    return;
  }
  relay2Nyala = (nilai == 1);
  relay2Nyala ? nyalakanRelay(PIN_RELAY2) : matikanRelay(PIN_RELAY2);
  flash.putBool("relay2", relay2Nyala);
  if (relay2Nyala) bunyikanBuzzer(1, 80);
  else bunyikanBuzzer(2, 40);
}

BLYNK_WRITE(VPIN_SALDO) {
  float nilaiSaldo = param.asFloat();
  if (nilaiSaldo < 0) nilaiSaldo = 0;
  saldoPulsa = nilaiSaldo;
  flash.putFloat("saldo", saldoPulsa);
  // Hanya update pulsaHabis jika mode pulsa aktif
  if (modePulsa) {
    pulsaHabis = (saldoPulsa <= 0);
    if (saldoPulsa > 0 && pulsaHabis) pulsaHabis = false;
  }
  Serial.printf("Blynk: Top Up Saldo jadi %.0f\n", saldoPulsa);
}

// ============================================
// SETUP
// ============================================
void setup() {
  Serial.begin(115200);
  Serial.println("=== Smart Plug ESP32 Starting ===");

  // Baca semua data dari flash terlebih dahulu
  flash.begin("smartplug", false);
  kWhAwal      = flash.getFloat("kwh_awal",  0.0);
  relay1Nyala  = flash.getBool("relay1",     true);
  relay2Nyala  = flash.getBool("relay2",     true);
  wifiSSID     = flash.getString("ssid",     "");
  wifiPass     = flash.getString("pass",     "");
  blynkAuth    = flash.getString("blynk_auth", "");
  tarifKwh     = flash.getFloat("tarif",     1444.0);
  batasDayaW   = flash.getFloat("batas_w",   500.0);
  batastArusA  = flash.getFloat("batas_a",   2.0);
  saldoPulsa   = flash.getFloat("saldo",     0.0);
  kwhTerakhir  = flash.getFloat("kwh_last",  0.0);
  modePulsa    = flash.getBool("mode_pulsa", true);

  // Paksa mati jika pulsa habis saat boot (hanya jika mode pulsa aktif)
  if (modePulsa && saldoPulsa <= 0) {
    relay1Nyala = false;
    relay2Nyala = false;
    pulsaHabis = true;
  }

  // Terapkan state SELESAI pinMode. 
  // Relay mekanik tidak akan sempat bereaksi (flicker) dalam 1 microsecond.
  // Ini memastikan ESP32 tidak me-reset pin menjadi LOW (ON) secara tidak sengaja.
  pinMode(PIN_RELAY1,  OUTPUT);
  pinMode(PIN_RELAY2,  OUTPUT);
  digitalWrite(PIN_RELAY1, relay1Nyala ? LOW : HIGH);
  digitalWrite(PIN_RELAY2, relay2Nyala ? LOW : HIGH);
  pinMode(PIN_BUZZER,  OUTPUT);
  pinMode(PIN_TOMBOL1, INPUT_PULLUP);
  pinMode(PIN_TOMBOL2, INPUT_PULLUP);
  digitalWrite(PIN_BUZZER, LOW);

  // Init PZEM
  serialPZEM.begin(9600, SERIAL_8N1, 16, 17);
  delay(500);

  // Konek WiFi
  setupWiFi();

  // Setup Blynk kalau WiFi konek dan token ada
  if (WiFi.status() == WL_CONNECTED && blynkAuth != "") {
    Blynk.config(blynkAuth.c_str());
    Blynk.connect(3000);
    Serial.println("Blynk: " +
      String(Blynk.connected() ? "Konek" : "Gagal"));
  } else if (blynkAuth == "") {
    Serial.println("Blynk: Token kosong, isi di Pengaturan WiFi");
  }

  setupWebServer();
  Serial.println("=== Siap! ===");
}

// ============================================
// LOOP
// ============================================
void loop() {
  // Cek tombol fisik
  if (relayTrip) {
    // Tekan 2 tombol bersamaan untuk reset trip
    if (digitalRead(PIN_TOMBOL1) == LOW &&
        digitalRead(PIN_TOMBOL2) == LOW) {
      delay(50);
      if (digitalRead(PIN_TOMBOL1) == LOW &&
          digitalRead(PIN_TOMBOL2) == LOW) {
        relayTrip = false;
        bunyikanBuzzer(2, 100);
        delay(500);
      }
    }
  } else {
    cekTombol(PIN_TOMBOL1, tombol1Lama, relay1Nyala, PIN_RELAY1);
    cekTombol(PIN_TOMBOL2, tombol2Lama, relay2Nyala, PIN_RELAY2);
  }

  // Baca sensor tiap 1 detik
  if (millis() - waktuBacaTerakhir >= 1000) {
    bacaSensorPZEM();
    waktuBacaTerakhir = millis();
  }

  // Update Blynk tiap 2 detik
  if (millis() - waktuBlynkTerakhir >= 2000) {
    if (Blynk.connected()) updateBlynk();
    waktuBlynkTerakhir = millis();
  }

  // Auto-reconnect WiFi tiap 30 detik jika putus
  // Jika sudah 3x gagal, masuk ke mode lokal (AP) secara otomatis
  if (wifiSSID != "" && !modeLokal &&
      WiFi.status() != WL_CONNECTED &&
      millis() - waktuRekonekTerakhir >= 30000) {
      
    percobaanKonek++;
    if (percobaanKonek > 3) {
      Serial.println("3x gagal reconnect WiFi! Masuk mode lokal (AP Only).");
      WiFi.disconnect(true);
      delay(100);
      WiFi.mode(WIFI_AP);
      WiFi.softAP("SmartPlug-Setup", "12345678");
      modeLokal = true;
      bunyikanBuzzer(3, 100);
    } else {
      Serial.printf("WiFi putus, mencoba reconnect ke \"%s\" (Percobaan %d/3)\n", wifiSSID.c_str(), percobaanKonek);
      WiFi.mode(WIFI_AP_STA);
      WiFi.disconnect(false);
      delay(100);
      WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
      waktuRekonekTerakhir = millis();
    }
  }

  // Jika WiFi baru konek setelah reconnect, init ulang Blynk & mDNS
  static bool sudahKonek = false;
  bool korekSekarang = (WiFi.status() == WL_CONNECTED);
  if (korekSekarang && !sudahKonek) {
    percobaanKonek = 0; // Reset counter gagal
    modeLokal = false;
    Serial.println("WiFi konek! IP: " + WiFi.localIP().toString());
    MDNS.begin("smartplug");
    if (blynkAuth != "") {
      Blynk.config(blynkAuth.c_str());
      Blynk.connect(3000);
    }
  }
  sudahKonek = korekSekarang;

  // Buzzer alert tiap 3 detik
  if (adaAlert && (millis() - waktuBuzzerTerakhir >= 3000)) {
    bunyikanBuzzer(3, 100);
    waktuBuzzerTerakhir = millis();
  }

  webServer.handleClient();
  
  if (blynkAuth != "") {
    if (Blynk.connected()) {
      Blynk.run();
    } else if (WiFi.status() == WL_CONNECTED) {
      // Auto-reconnect Blynk tiap 10 detik jika terputus dari server
      static unsigned long waktuReconnectBlynk = 0;
      if (millis() - waktuReconnectBlynk >= 10000) {
        Serial.println("Blynk terputus, mencoba reconnect...");
        Blynk.connect(3000); // Timeout 3 detik, tidak blocking
        waktuReconnectBlynk = millis();
      }
    }
  }
}

// ============================================
// FUNGSI: Konek WiFi — Kompatibel semua jaringan
// (WiFi rumah, hotspot HP Android/iPhone, dll)
// ============================================
void setupWiFi() {
  if (wifiSSID == "") {
    // Tidak ada WiFi tersimpan → buat Access Point
    WiFi.mode(WIFI_AP);
    delay(100);
    WiFi.softAP("SmartPlug-Setup", "12345678");
    Serial.println("Mode AP: SmartPlug-Setup / 12345678");
    Serial.println("Buka browser: http://192.168.4.1");
    bunyikanBuzzer(3, 100);
    return;
  }

  // ── Konfigurasi WiFi optimal untuk semua jenis jaringan ──
  WiFi.persistent(false);          // jangan simpan ke flash internal WiFi
  WiFi.setAutoReconnect(true);     // reconnect otomatis jika putus

  // Gunakan mode AP+STA: web server tetap bisa diakses
  // lewat 192.168.4.1 meski sedang mencoba konek
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  WiFi.softAP("SmartPlug-Setup", "12345678");  // AP darurat tetap aktif

  // Nonaktifkan power-saving agar koneksi stabil
  // (penting untuk hotspot HP yang sering idle)
  WiFi.setSleep(false);

  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
  Serial.print("Konek ke \"" + wifiSSID + "\"");

  // Tunggu hingga 30 detik (60 × 500ms)
  // Hotspot HP Android kadang perlu ~15 detik sebelum siap
  int percobaan = 0;
  while (WiFi.status() != WL_CONNECTED && percobaan < 60) {
    delay(500);
    Serial.print(".");
    percobaan++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    percobaanKonek = 0;
    modeLokal = false;
    Serial.println("Berhasil! IP: " + WiFi.localIP().toString());
    Serial.println("AP darurat tetap aktif di: 192.168.4.1");
    if (MDNS.begin("smartplug")) {
      Serial.println("Akses via: http://smartplug.local");
    }
    bunyikanBuzzer(1, 200);
  } else {
    // Gagal konek pada percobaan pertama (saat boot)
    percobaanKonek = 1;
    waktuRekonekTerakhir = millis();
    Serial.println("Gagal konek awal! Akan dicoba ulang otomatis nanti.");
    Serial.println("AP darurat aktif: SmartPlug-Setup / 12345678");
    bunyikanBuzzer(2, 100);
  }
}

// ============================================
// FUNGSI: Baca sensor PZEM
// ============================================
void bacaSensorPZEM() {
  float v  = pzem.voltage();
  float i  = pzem.current();
  float p  = pzem.power();
  float e  = pzem.energy();
  float f  = pzem.frequency();
  float pf = pzem.pf();

  tegangan    = isnan(v)  ? 0 : v;
  arus        = isnan(i)  ? 0 : i;
  daya        = isnan(p)  ? 0 : p;
  if (!isnan(e)) energi = e; // Pertahankan energi terakhir jika sensor putus, jangan set ke 0
  frekuensi   = isnan(f)  ? 0 : f;
  powerFaktor = isnan(pf) ? 0 : pf;

  // Hitung alert (hanya jika relay tidak dalam kondisi trip/mati)
  // Jangan nyalakan alert buzzer jika relay sudah dimatikan oleh Trip
  if (!relayTrip) {
    adaAlert = (daya > batasDayaW || arus > batastArusA);
  } else {
    adaAlert = false; // Reset alert saat trip aktif agar buzzer tidak loop
  }

  // Deteksi jika memory internal PZEM tereset
  if (energi < kwhTerakhir && !isnan(e)) {
    kwhTerakhir = energi;
    flash.putFloat("kwh_last", kwhTerakhir);
    
    // Cegah angka konsumsi kWh di UI menjadi minus
    if (energi < kWhAwal) {
      kWhAwal = energi;
      flash.putFloat("kwh_awal", kWhAwal);
    }
  }

  // LOGIK PULSA (Prepaid) — hanya jika mode pulsa aktif
  if (modePulsa && energi > kwhTerakhir) {
    float deltaKwh = energi - kwhTerakhir;
    float biaya = deltaKwh * tarifKwh;
    saldoPulsa -= biaya;
    if (saldoPulsa < 0) saldoPulsa = 0;
    kwhTerakhir = energi;
  } else if (!modePulsa) {
    // Mode pulsa OFF: tetap update kwhTerakhir agar tidak bug saat mode dihidupkan lagi
    kwhTerakhir = energi;
  }

  // Simpan saldo ke Flash berkala (tiap 5 menit)
  // Gunakan static dengan inisialisasi ke millis() agar tidak langsung save di boot
  static unsigned long waktuSimpanFlash = 0;
  static bool sudahInitFlashTimer = false;
  if (!sudahInitFlashTimer) { waktuSimpanFlash = millis(); sudahInitFlashTimer = true; }
  if (millis() - waktuSimpanFlash >= 300000UL) {
    flash.putFloat("saldo", saldoPulsa);
    flash.putFloat("kwh_last", kwhTerakhir);
    waktuSimpanFlash = millis();
  }

  // Jika pulsa habis, matikan relay (hanya jika mode pulsa aktif)
  if (modePulsa && saldoPulsa <= 0 && !pulsaHabis) {
    Serial.println("!!! PULSA HABIS: Mematikan power !!!");
    matikanSemuaRelay();
    pulsaHabis = true;
    bunyikanBuzzer(4, 500);
    flash.putFloat("saldo", saldoPulsa);
    flash.putFloat("kwh_last", kwhTerakhir);
    if (Blynk.connected()) {
      Blynk.logEvent("pulsa_habis", "Saldo Pulsa Habis! Listrik diputus.");
    }
  } else if (!modePulsa || saldoPulsa > 0) {
    pulsaHabis = false; // Reset flag jika mode OFF atau saldo terisi
  }

  // Trip kalau daya melebihi batas
  if (daya > batasDayaW && !relayTrip) {
    Serial.println("!!! TRIP: Daya melebihi batas !!!");
    matikanSemuaRelay();
    relayTrip = true;
    adaAlert = false; // Trip sudah menangani, hentikan alert buzzer biasa
    bunyikanBuzzer(5, 200);
    if (Blynk.connected()) {
      Blynk.logEvent("over_limit",
        "TRIP! Daya " + String(daya,1) + "W melebihi batas " +
        String(batasDayaW,0) + "W");
    }
  }

  Serial.printf("V=%.1f I=%.3f P=%.1f kWh=%.4f Saldo=%.0f Trip=%d\n",
    tegangan, arus, daya, energi - kWhAwal, saldoPulsa, relayTrip);
}

// ============================================
// FUNGSI: Update data ke Blynk
// ============================================
void updateBlynk() {
  float kwh   = energi - kWhAwal;
  float biaya = kwh * tarifKwh;
  Blynk.virtualWrite(VPIN_TEGANGAN, tegangan);
  Blynk.virtualWrite(VPIN_ARUS,     arus);
  Blynk.virtualWrite(VPIN_DAYA,     daya);
  Blynk.virtualWrite(VPIN_KWH,      kwh);
  Blynk.virtualWrite(VPIN_BIAYA,    biaya);
  Blynk.virtualWrite(VPIN_PF,       powerFaktor);
  Blynk.virtualWrite(VPIN_FREK,     frekuensi);
  Blynk.virtualWrite(VPIN_SALDO,    saldoPulsa);
  Blynk.virtualWrite(VPIN_RELAY1,   relay1Nyala ? 1 : 0);
  Blynk.virtualWrite(VPIN_RELAY2,   relay2Nyala ? 1 : 0);
  int status = relayTrip ? 2 : (pulsaHabis ? 3 : (adaAlert ? 1 : 0));
  Blynk.virtualWrite(VPIN_STATUS,   status);
}

// ============================================
// FUNGSI: Cek tombol fisik
// ============================================
void cekTombol(int pin, bool &statusLama,
               bool &statusRelay, int pinRelay) {
  bool statusSekarang = digitalRead(pin);
  if (statusLama == HIGH && statusSekarang == LOW) {
    delay(50);
    if (digitalRead(pin) == LOW) {
      // Blokir jika pulsa habis DAN mode pulsa aktif DAN user mau NYALAKAN
      if (modePulsa && pulsaHabis && !statusRelay) {
        bunyikanBuzzer(3, 50);
      // Blokir jika sedang Trip DAN pengguna mencoba MENYALAKAN relay  
      } else if (relayTrip && !statusRelay) {
        bunyikanBuzzer(3, 50);
      } else {
        statusRelay = !statusRelay;
        statusRelay ? nyalakanRelay(pinRelay) : matikanRelay(pinRelay);
        if (pinRelay == PIN_RELAY1) flash.putBool("relay1", statusRelay);
        if (pinRelay == PIN_RELAY2) flash.putBool("relay2", statusRelay);
        if (statusRelay) bunyikanBuzzer(1, 80);
        else bunyikanBuzzer(2, 40);
      }
    }
  }
  statusLama = statusSekarang;
}

// ============================================
// FUNGSI: Kontrol relay
// ============================================
void nyalakanRelay(int pin) { digitalWrite(pin, LOW);  }
void matikanRelay(int pin)  { digitalWrite(pin, HIGH); }

void matikanSemuaRelay() {
  relay1Nyala = false;
  relay2Nyala = false;
  matikanRelay(PIN_RELAY1);
  matikanRelay(PIN_RELAY2);
  flash.putBool("relay1", false);
  flash.putBool("relay2", false);
}

// ============================================
// FUNGSI: Buzzer
// ============================================
void bunyikanBuzzer(int berapa, int durasi) {
  delay(10);
  for (int i = 0; i < berapa; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(durasi);
    digitalWrite(PIN_BUZZER, LOW);
    if (i < berapa - 1) delay(80);
  }
}

// ============================================
// FUNGSI: Setup web server
// ============================================
void setupWebServer() {
  webServer.on("/",              halamanUtama);
  webServer.on("/toggle1",       toggleRelay1);
  webServer.on("/toggle2",       toggleRelay2);
  webServer.on("/togglepulsa",   toggleModePulsa);
  webServer.on("/reset",         resetKwh);
  webServer.on("/data",          ambilData);
  webServer.on("/settings",      halamanSettings);
  webServer.on("/info",          halamanInfo);
  webServer.on("/savewifi",      simpanWifi);
  webServer.on("/resetwifi",     resetWifi);
  webServer.on("/scanwifi",      scanWifi);
  webServer.on("/saveconfig",    simpanKonfigurasi);
  webServer.on("/saveblynk",     simpanBlynk);
  webServer.on("/kalibrasipzem", kalibrasiBoth);
  webServer.begin();
  Serial.println("Web server jalan di port 80");
}

// ============================================
// WEB: Toggle relay
// ============================================
void toggleRelay1() {
  if (modePulsa && pulsaHabis && !relay1Nyala) {
    bunyikanBuzzer(3, 50);
  } else if (relayTrip && !relay1Nyala) {
    bunyikanBuzzer(3, 50);
  } else {
    relay1Nyala = !relay1Nyala;
    relay1Nyala ? nyalakanRelay(PIN_RELAY1) : matikanRelay(PIN_RELAY1);
    flash.putBool("relay1", relay1Nyala);
    if (relay1Nyala) bunyikanBuzzer(1, 80);
    else bunyikanBuzzer(2, 40);
  }
  webServer.sendHeader("Location", "/");
  webServer.send(302);
}

void toggleRelay2() {
  if (modePulsa && pulsaHabis && !relay2Nyala) {
    bunyikanBuzzer(3, 50);
  } else if (relayTrip && !relay2Nyala) {
    bunyikanBuzzer(3, 50);
  } else {
    relay2Nyala = !relay2Nyala;
    relay2Nyala ? nyalakanRelay(PIN_RELAY2) : matikanRelay(PIN_RELAY2);
    flash.putBool("relay2", relay2Nyala);
    if (relay2Nyala) bunyikanBuzzer(1, 80);
    else bunyikanBuzzer(2, 40);
  }
  webServer.sendHeader("Location", "/");
  webServer.send(302);
}

// ============================================
// WEB: Toggle Mode Pulsa ON/OFF
// ============================================
void toggleModePulsa() {
  modePulsa = !modePulsa;
  flash.putBool("mode_pulsa", modePulsa);
  if (!modePulsa) {
    // Saat mode pulsa dinonaktifkan, reset flag pulsaHabis
    pulsaHabis = false;
    Serial.println("Mode Pulsa: NONAKTIF (listrik bebas)");
  } else {
    // Saat diaktifkan kembali, cek apakah saldo masih 0
    pulsaHabis = (saldoPulsa <= 0);
    if (pulsaHabis) matikanSemuaRelay();
    Serial.println("Mode Pulsa: AKTIF");
  }
  bunyikanBuzzer(1, 150);
  webServer.sendHeader("Location", "/");
  webServer.send(302);
}

// ============================================
// WEB: Reset kWh (offset saja)
// ============================================
void resetKwh() {
  kWhAwal = energi;
  flash.putFloat("kwh_awal", kWhAwal);
  webServer.sendHeader("Location", "/");
  webServer.send(302);
}

// ============================================
// WEB: Kalibrasi PZEM — reset energi di chip
// ============================================
void kalibrasiBoth() {
  Serial.println("[KALIBRASI] Mereset energi chip PZEM-004T...");
  bool ok = pzem.resetEnergy();
  kWhAwal     = 0.0;
  kwhTerakhir = 0.0;
  flash.putFloat("kwh_awal",  0.0);
  flash.putFloat("kwh_last",  0.0);
  Serial.printf("[KALIBRASI] resetEnergy() = %s\n", ok ? "OK" : "GAGAL");
  bunyikanBuzzer(2, 150);
  webServer.send(200, "application/json",
    ok ? "{\"ok\":true,\"msg\":\"Kalibrasi berhasil! Energi PZEM direset ke 0.\"}"
       : "{\"ok\":false,\"msg\":\"Kalibrasi gagal. Periksa koneksi PZEM.\"}");
}

// ============================================
// WEB: Simpan konfigurasi
// ============================================
void simpanKonfigurasi() {
  if (webServer.hasArg("tarif")) {
    tarifKwh = webServer.arg("tarif").toFloat();
    if (tarifKwh < 0) tarifKwh = 0; // Validasi: tarif tidak boleh negatif
    flash.putFloat("tarif", tarifKwh);
  }
  if (webServer.hasArg("batas_w")) {
    batasDayaW = webServer.arg("batas_w").toFloat();
    if (batasDayaW < 0) batasDayaW = 0;
    flash.putFloat("batas_w", batasDayaW);
    // Reset trip jika batas daya diubah
    relayTrip = false;
    adaAlert = false;
  }
  if (webServer.hasArg("batas_a")) {
    batastArusA = webServer.arg("batas_a").toFloat();
    if (batastArusA < 0) batastArusA = 0;
    flash.putFloat("batas_a", batastArusA);
  }
  if (webServer.hasArg("topup")) {
    float tambah = webServer.arg("topup").toFloat();
    if (tambah > 0) {
      saldoPulsa += tambah;
      flash.putFloat("saldo", saldoPulsa);
      // Reset pulsaHabis hanya jika mode pulsa aktif dan saldo kini > 0
      if (modePulsa && saldoPulsa > 0) pulsaHabis = false;
    }
  }
  if (webServer.hasArg("set_saldo")) {
    float ns = webServer.arg("set_saldo").toFloat();
    if (ns < 0) ns = 0;
    saldoPulsa = ns;
    flash.putFloat("saldo", saldoPulsa);
    // Update pulsaHabis hanya jika mode pulsa aktif
    if (modePulsa) pulsaHabis = (saldoPulsa <= 0);
    else pulsaHabis = false;
  }
  webServer.sendHeader("Location", "/");
  webServer.send(302);
}

// ============================================
// WEB: Simpan WiFi
// ============================================
void simpanWifi() {
  if (webServer.hasArg("ssid") && webServer.hasArg("pass")) {
    wifiSSID = webServer.arg("ssid");
    wifiPass = webServer.arg("pass");
    flash.putString("ssid", wifiSSID);
    flash.putString("pass", wifiPass);
    webServer.send(200, "text/html",
      "<html><body style='font-family:sans-serif;background:#0f172a;"
      "color:#e2e8f0;display:flex;align-items:center;"
      "justify-content:center;height:100vh;margin:0'>"
      "<div style='text-align:center'>"
      "<p style='font-size:18px'>WiFi disimpan!</p>"
      "<p style='color:#64748b'>Restart dalam 3 detik...</p>"
      "</div></body></html>");
    delay(3000);
    ESP.restart();
  } else {
    webServer.sendHeader("Location", "/settings");
    webServer.send(302);
  }
}

// ============================================
// WEB: Simpan token Blynk
// ============================================
void simpanBlynk() {
  if (webServer.hasArg("token")) {
    blynkAuth = webServer.arg("token");
    flash.putString("blynk_auth", blynkAuth);
    webServer.send(200, "text/html",
      "<html><body style='font-family:sans-serif;background:#0f172a;"
      "color:#e2e8f0;display:flex;align-items:center;"
      "justify-content:center;height:100vh;margin:0'>"
      "<div style='text-align:center'>"
      "<p style='font-size:18px'>Token Blynk disimpan!</p>"
      "<p style='color:#64748b'>Restart dalam 3 detik...</p>"
      "</div></body></html>");
    delay(3000);
    ESP.restart();
  } else {
    webServer.sendHeader("Location", "/settings");
    webServer.send(302);
  }
}

// ============================================
// WEB: Reset WiFi
// ============================================
void resetWifi() {
  flash.putString("ssid", "");
  flash.putString("pass", "");
  webServer.send(200, "text/html",
    "<html><body style='font-family:sans-serif;background:#0f172a;"
    "color:#e2e8f0;display:flex;align-items:center;"
    "justify-content:center;height:100vh;margin:0'>"
    "<div style='text-align:center'>"
    "<p style='font-size:18px'>WiFi direset!</p>"
    "<p style='color:#64748b'>Konek ke: SmartPlug-Setup</p>"
    "<p style='color:#64748b'>Password: 12345678</p>"
    "</div></body></html>");
  delay(3000);
  ESP.restart();
}

// ============================================
// WEB: Scan WiFi
// ============================================
void scanWifi() {
  // Nonaktifkan web server handling sementara selama scan
  // Scan async (false=sync) tapi beri batas max 5 jaringan agar cepat
  int jumlah = WiFi.scanNetworks(false, false, false, 120); // timeout 120ms/channel
  if (jumlah < 0) jumlah = 0; // Tangani error scan (WIFI_SCAN_FAILED = -1)
  String json = "[";
  for (int i = 0; i < jumlah; i++) {
    if (i > 0) json += ",";
    // Escape karakter tanda kutip dalam SSID agar JSON valid
    String ssid = WiFi.SSID(i);
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    json += "{\"ssid\":\"" + ssid + "\",";
    json += "\"rssi\":"    + String(WiFi.RSSI(i)) + ",";
    json += "\"enc\":"     + String(
      WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? 1 : 0) + "}";
  }
  WiFi.scanDelete(); // Bebaskan memori hasil scan
  json += "]";
  webServer.send(200, "application/json", json);
}

// ============================================
// WEB: Kirim data JSON
// ============================================
void ambilData() {
  float kwh       = energi - kWhAwal;
  float biaya     = kwh * tarifKwh;
  float persenDaya = batasDayaW > 0 ?
    min((daya / batasDayaW) * 100.0f, 100.0f) : 0;
  String ipStr = (WiFi.status() == WL_CONNECTED) ?
    WiFi.localIP().toString() : "Hotspot mode";

  String json = "{";
  json += "\"v\":"      + String(tegangan, 1)    + ",";
  json += "\"i\":"      + String(arus, 3)        + ",";
  json += "\"p\":"      + String(daya, 1)        + ",";
  json += "\"kwh\":"    + String(kwh, 4)         + ",";
  json += "\"saldo\":"  + String(saldoPulsa, 0)  + ",";
  json += "\"pf\":"     + String(powerFaktor, 2) + ",";
  json += "\"freq\":"   + String(frekuensi, 1)   + ",";
  json += "\"r1\":"      + String(relay1Nyala ? "true":"false") + ",";
  json += "\"r2\":"      + String(relay2Nyala ? "true":"false") + ",";
  json += "\"alert\":"   + String(adaAlert    ? "true":"false") + ",";
  json += "\"trip\":"    + String(relayTrip   ? "true":"false") + ",";
  json += "\"habis\":"   + String(pulsaHabis  ? "true":"false") + ",";
  json += "\"modepulsa\":" + String(modePulsa ? "true":"false") + ",";
  json += "\"persen\":" + String(persenDaya, 0)  + ",";
  json += "\"batas\":"  + String(batasDayaW, 0)  + ",";
  json += "\"batas_a\":" + String(batastArusA, 1) + ",";
  json += "\"tarif\":"  + String(tarifKwh, 0)    + ",";
  json += "\"ip\":\"" + ipStr + "\"";
  json += "}";
  webServer.send(200, "application/json", json);
}

// ============================================
// WEB: Halaman settings
// ============================================
void halamanSettings() {
  String statusWifi = (WiFi.status() == WL_CONNECTED) ?
    "Terhubung ke: <b>" + wifiSSID + "</b> (" +
    WiFi.localIP().toString() + ")" :
    "Tidak terhubung (Hotspot: SmartPlug-Setup)";

  String html = R"rawlit(
<!DOCTYPE html><html><head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Pengaturan</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:sans-serif;background:#0f172a;color:#e2e8f0;padding:16px}
  h1{font-size:17px;color:#94a3b8;margin-bottom:6px}
  .back{font-size:12px;color:#3b82f6;text-decoration:none;
        display:block;margin-bottom:16px}
  .status{background:#1e293b;border-radius:12px;padding:14px;
          margin-bottom:16px;font-size:13px;color:#94a3b8}
  .card{background:#1e293b;border-radius:12px;
        padding:16px;margin-bottom:14px}
  .card h2{font-size:14px;color:#94a3b8;margin-bottom:12px}
  label{font-size:12px;color:#64748b;display:block;margin-bottom:4px}
  input{width:100%;padding:10px 12px;background:#0f172a;
        border:1px solid #334155;border-radius:8px;
        color:#e2e8f0;font-size:14px;margin-bottom:12px}
  .btn-biru{width:100%;padding:12px;background:#3b82f6;color:#fff;
            border:none;border-radius:10px;font-size:15px;
            font-weight:600;cursor:pointer;margin-bottom:8px}
  .btn-scan{width:100%;padding:10px;background:#1e293b;color:#94a3b8;
            border:1px solid #334155;border-radius:10px;
            font-size:13px;cursor:pointer;margin-bottom:12px}
  .btn-merah{width:100%;padding:12px;background:#7f1d1d;color:#fca5a5;
             border:none;border-radius:10px;font-size:14px;
             font-weight:600;cursor:pointer}
  .wifi-item{padding:10px 12px;background:#0f172a;border-radius:8px;
             margin-bottom:6px;cursor:pointer;font-size:13px;
             display:flex;justify-content:space-between;
             border:1px solid #1e293b}
  .wifi-item:hover{border-color:#3b82f6}
  #scan-status{font-size:12px;color:#64748b;text-align:center;padding:8px}
  hr{border:none;border-top:1px solid #334155;margin:12px 0}
</style>
</head><body>
<a href='/' class='back'>← Kembali ke Dashboard</a>
<a href='/info' class='back' style='color:#22c55e;margin-top:4px'>ℹ️ Halaman Info</a>
<h1>Pengaturan</h1>
<div class='status'>)rawlit";
  html += statusWifi;
  html += R"rawlit(</div>

<div class='card'>
  <h2>Scan WiFi Tersedia</h2>
  <button class='btn-scan' onclick='scanWifi()'>Scan Sekarang</button>
  <div id='scan-status'></div>
  <div id='wifi-list'></div>
</div>

<div class='card'>
  <h2>Ganti WiFi</h2>
  <form action='/savewifi' method='get'>
    <label>Nama WiFi (SSID)</label>
    <input type='text' name='ssid' id='input-ssid'
           placeholder='Nama WiFi' value=')rawlit";
  html += wifiSSID;
  html += R"rawlit(' autocomplete='off'>
    <label>Password</label>
    <input type='password' name='pass'
           placeholder='Password WiFi' autocomplete='off'>
    <button type='submit' class='btn-biru'>Simpan & Restart</button>
  </form>
  <hr>
  <form action='/resetwifi' method='get'
    onsubmit='return confirm("Reset WiFi? ESP32 akan ke mode hotspot.")'>
    <button type='submit' class='btn-merah'>Reset WiFi</button>
  </form>
</div>

<div class='card'>
  <h2>Pengaturan Blynk</h2>
  <form action='/saveblynk' method='get'>
    <label>Auth Token</label>
    <input type='text' name='token'
           placeholder='Masukkan Auth Token Blynk' value=')rawlit";
  html += blynkAuth;
  html += R"rawlit(' autocomplete='off'>
    <button type='submit' class='btn-biru'>Simpan & Restart</button>
  </form>
</div>

<script>
function scanWifi() {
  document.getElementById('scan-status').textContent = 'Scanning...';
  document.getElementById('wifi-list').innerHTML = '';
  fetch('/scanwifi').then(r => r.json()).then(list => {
    document.getElementById('scan-status').textContent =
      list.length + ' jaringan ditemukan';
    list.sort((a, b) => b.rssi - a.rssi);
    let html = '';
    list.forEach(w => {
      let bar = w.rssi>-50?'▂▄▆█':
                w.rssi>-65?'▂▄▆_':
                w.rssi>-75?'▂▄__':'▂___';
      html += '<div class="wifi-item" onclick="pilih(\''+w.ssid+'\')">';
      html += '<span>'+w.ssid+'</span>';
      html += '<span>'+(w.enc?'🔒 ':'')+bar+'</span>';
      html += '</div>';
    });
    document.getElementById('wifi-list').innerHTML = html;
  });
}
function pilih(ssid) {
  document.getElementById('input-ssid').value = ssid;
}
</script>
</body></html>)rawlit";

  webServer.send(200, "text/html", html);
}

// ============================================
// WEB: Halaman utama dashboard
// ============================================
void halamanUtama() {
  float kwh       = energi - kWhAwal;
  float biaya     = kwh * tarifKwh;
  float persenDaya = batasDayaW > 0 ?
    min((daya / batasDayaW) * 100.0f, 100.0f) : 0;
  String ipStr = (WiFi.status() == WL_CONNECTED) ?
    WiFi.localIP().toString() : "192.168.4.1";

  String html = R"rawlit(
<!DOCTYPE html><html><head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Smart Plug ESP32</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:sans-serif;background:#0f172a;
       color:#e2e8f0;padding:16px}
  .topbar{display:flex;justify-content:space-between;
          align-items:center;margin-bottom:14px}
  h1{font-size:17px;color:#94a3b8}
  .btn-wifi{font-size:12px;color:#3b82f6;text-decoration:none;
            padding:6px 12px;border:1px solid #1e3a5f;border-radius:8px}
  .grid{display:grid;grid-template-columns:1fr 1fr;
        gap:10px;margin-bottom:14px}
  .kartu{background:#1e293b;border-radius:12px;
         padding:14px;text-align:center}
  .kartu .judul{font-size:11px;color:#64748b;margin-bottom:4px}
  .kartu .nilai{font-size:22px;font-weight:600}
  .kartu.merah{border:2px solid #ef4444}
  .kartu.hijau .nilai{color:#22c55e}
  .relay-grid{display:grid;grid-template-columns:1fr 1fr;
              gap:10px;margin-bottom:12px}
  .kartu-relay{background:#1e293b;border-radius:12px;padding:14px}
  .dot{display:inline-block;width:10px;height:10px;
       border-radius:50%;margin-right:6px}
  .dot.on{background:#22c55e}
  .dot.off{background:#ef4444}
  .btn-relay{width:100%;padding:10px;border:none;border-radius:8px;
             font-size:14px;font-weight:600;cursor:pointer;margin-top:8px}
  .btn-on{background:#3b82f6;color:#fff}
  .btn-off{background:#475569;color:#cbd5e1}
  .btn-reset{width:100%;padding:10px;border:none;border-radius:10px;
             background:#1e293b;color:#64748b;font-size:13px;
             cursor:pointer;margin-bottom:8px;border:1px solid #334155}
  .btn-simpan{padding:8px 16px;background:#3b82f6;color:#fff;
              border:none;border-radius:8px;font-size:13px;cursor:pointer}
  .banner{border-radius:12px;padding:12px;text-align:center;
          font-weight:600;margin-bottom:12px;display:none}
  .banner.kuning{background:#78350f;color:#fde68a;display:block}
  .banner.merah{background:#7f1d1d;color:#fca5a5;display:block}
  .progress-wrap{background:#0f172a;border-radius:8px;
                 height:8px;margin-top:6px;overflow:hidden}
  .progress-bar{height:8px;border-radius:8px;background:#22c55e;
                transition:width 0.5s,background 0.5s}
  .kotak-set{background:#1e293b;border-radius:12px;
             padding:14px;margin-bottom:12px}
  .kotak-set h2{font-size:13px;color:#94a3b8;margin-bottom:10px}
  .baris-set{display:flex;align-items:center;gap:8px;margin-bottom:8px}
  .baris-set label{font-size:12px;color:#64748b;min-width:120px}
  .baris-set input{flex:1;padding:8px 10px;background:#0f172a;
                   border:1px solid #334155;border-radius:8px;
                   color:#e2e8f0;font-size:13px}
  .footer{font-size:11px;color:#475569;text-align:center;margin-top:8px}
</style>
<script>
setInterval(function() {
  fetch('/data')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      document.getElementById('tegangan').textContent = d.v + ' V';
      document.getElementById('arus').textContent     = d.i + ' A';
      document.getElementById('daya').textContent     = d.p + ' W';
      document.getElementById('kwh').textContent      =
        parseFloat(d.kwh).toFixed(4) + ' kWh';
      document.getElementById('saldo').textContent    =
        'Rp ' + parseInt(d.saldo).toLocaleString();
      document.getElementById('pf').textContent       = d.pf;
      document.getElementById('frek').textContent     = d.freq + ' Hz';
      document.getElementById('footer').textContent   =
        d.ip + ' | Tarif Rp ' +
        parseInt(d.tarif).toLocaleString() + '/kWh | Refresh 1.5s';

      var bar = document.getElementById('bar-daya');
      bar.style.width = d.persen + '%';
      bar.style.background =
        d.persen > 90 ? '#ef4444' :
        d.persen > 70 ? '#f59e0b' : '#22c55e';
      document.getElementById('info-daya').textContent =
        d.p + ' W / ' + d.batas + ' W (' + d.persen + '%)';

      document.getElementById('dot1').className =
        'dot ' + (d.r1 ? 'on' : 'off');
      document.getElementById('sts1').textContent =
        d.r1 ? 'Menyala' : 'Mati';
      document.getElementById('btn1').className =
        'btn-relay ' + (d.r1 ? 'btn-on' : 'btn-off');
      document.getElementById('btn1').textContent =
        d.r1 ? 'Matikan' : 'Nyalakan';

      document.getElementById('dot2').className =
        'dot ' + (d.r2 ? 'on' : 'off');
      document.getElementById('sts2').textContent =
        d.r2 ? 'Menyala' : 'Mati';
      document.getElementById('btn2').className =
        'btn-relay ' + (d.r2 ? 'btn-on' : 'btn-off');
      document.getElementById('btn2').textContent =
        d.r2 ? 'Matikan' : 'Nyalakan';

      var banner = document.getElementById('banner');
      if (d.trip) {
        banner.className = 'banner merah';
        banner.textContent =
          'TRIP! Daya melebihi batas — Tekan 2 tombol untuk reset!';
      } else if (d.habis) {
        banner.className = 'banner merah';
        banner.textContent = 'PULSA HABIS! Silakan Top Up Saldo.';
      } else if (d.alert) {
        banner.className = 'banner kuning';
        banner.textContent = 'PERINGATAN: Daya/Arus mendekati batas!';
      } else {
        banner.className = 'banner';
      }

      document.getElementById('kartu-daya').className =
        'kartu' + (d.alert || d.trip ? ' merah' : '');
    })
    .catch(function() {
      // Abaikan error jaringan (WiFi putus sesaat) — jangan crash JS
    });
}, 1500);
</script>
</head><body>

<div class='topbar'>
  <h1>Smart Plug ESP32</h1>
  <div style='display:flex;gap:8px'>
    <a href='/info' class='btn-wifi'>ℹ️ Info</a>
    <a href='/settings' class='btn-wifi'>⚙️ Pengaturan</a>
  </div>
</div>

<div id='banner' class='banner'></div>

<div class='grid'>
  <div class='kartu'>
    <div class='judul'>Tegangan</div>
    <div class='nilai' id='tegangan'>)rawlit";
  html += String(tegangan,1)+" V";
  html += R"rawlit(</div></div>
  <div class='kartu'>
    <div class='judul'>Arus</div>
    <div class='nilai' id='arus'>)rawlit";
  html += String(arus,3)+" A";
  html += R"rawlit(</div></div>
  <div class='kartu' id='kartu-daya'>
    <div class='judul'>Daya</div>
    <div class='nilai' id='daya'>)rawlit";
  html += String(daya,1)+" W";
  html += R"rawlit(</div></div>
  <div class='kartu'>
    <div class='judul'>Power Factor</div>
    <div class='nilai' id='pf'>)rawlit";
  html += String(powerFaktor,2);
  html += R"rawlit(</div></div>
  <div class='kartu'>
    <div class='judul'>Frekuensi</div>
    <div class='nilai' id='frek'>)rawlit";
  html += String(frekuensi,1)+" Hz";
  html += R"rawlit(</div></div>
  <div class='kartu'>
    <div class='judul'>Konsumsi</div>
    <div class='nilai' style='font-size:16px' id='kwh'>)rawlit";
  html += String(kwh,4)+" kWh";
  html += R"rawlit(</div></div>
  <div class='kartu hijau' style='grid-column:span 2'>
    <div class='judul'>Saldo Pulsa (Sisa)</div>
    <div class='nilai' id='saldo'>Rp )rawlit";
  html += String((long)saldoPulsa);
  html += R"rawlit(</div></div>
</div>

<div class='kotak-set' style='margin-bottom:12px'>
  <div style='display:flex;justify-content:space-between;margin-bottom:6px'>
    <span style='font-size:12px;color:#94a3b8'>Penggunaan Daya</span>
    <span style='font-size:12px;color:#64748b' id='info-daya'>)rawlit";
  html += String(daya,1)+" W / "+String(batasDayaW,0)+" W";
  html += R"rawlit(</span>
  </div>
  <div class='progress-wrap'>
    <div class='progress-bar' id='bar-daya'
         style='width:)rawlit";
  html += String(persenDaya,0);
  html += R"rawlit(%'></div>
  </div>
</div>

<div class='kotak-set'>
  <h2>Pengaturan</h2>
  <form action='/saveconfig' method='get'>
    <div class='baris-set'>
      <label>Tarif (Rp/kWh)</label>
      <input type='number' name='tarif' value=')rawlit";
  html += String((int)tarifKwh);
  html += R"rawlit(' min='0' step='1'>
    </div>
    <div class='baris-set'>
      <label>Batas Daya (W)</label>
      <input type='number' name='batas_w' value=')rawlit";
  html += String((int)batasDayaW);
  html += R"rawlit(' min='0' step='10'>
    </div>
    <div class='baris-set'>
      <label>Batas Arus (A)</label>
      <input type='number' name='batas_a' value=')rawlit";
  html += String(batastArusA,1);
  html += R"rawlit(' min='0' step='0.1'>
    </div>
    <div class='baris-set' style='border-top:1px solid #334155;padding-top:10px;margin-top:5px'>
      <label>Top Up Saldo (Rp)</label>
      <input type='number' name='topup' placeholder='Tambah Saldo...' min='0'>
    </div>
    <div class='baris-set'>
      <label>Set Saldo (Rp)</label>
      <input type='number' name='set_saldo' value=')rawlit";
  html += String((int)saldoPulsa);
  html += R"rawlit(' min='0'>
    </div>
    <button type='submit' class='btn-simpan'>Simpan & Update Saldo</button>
  </form>
</div>

<div class='relay-grid'>
  <div class='kartu-relay'>
    <div style='font-size:12px;color:#94a3b8;margin-bottom:6px'>
      <span class='dot )rawlit";
  html += relay1Nyala ? "on" : "off";
  html += R"rawlit(' id='dot1'></span>
      <span id='sts1'>)rawlit";
  html += relay1Nyala ? "Menyala" : "Mati";
  html += R"rawlit(</span></div>
    <div style='font-size:11px;color:#64748b'>Stopkontak 1</div>
    <form action='/toggle1' method='get'>
      <button id='btn1' class='btn-relay )rawlit";
  html += relay1Nyala ? "btn-on" : "btn-off";
  html += R"rawlit('>)rawlit";
  html += relay1Nyala ? "Matikan" : "Nyalakan";
  html += R"rawlit(</button></form>
  </div>
  <div class='kartu-relay'>
    <div style='font-size:12px;color:#94a3b8;margin-bottom:6px'>
      <span class='dot )rawlit";
  html += relay2Nyala ? "on" : "off";
  html += R"rawlit(' id='dot2'></span>
      <span id='sts2'>)rawlit";
  html += relay2Nyala ? "Menyala" : "Mati";
  html += R"rawlit(</span></div>
    <div style='font-size:11px;color:#64748b'>Stopkontak 2</div>
    <form action='/toggle2' method='get'>
      <button id='btn2' class='btn-relay )rawlit";
  html += relay2Nyala ? "btn-on" : "btn-off";
  html += R"rawlit('>)rawlit";
  html += relay2Nyala ? "Matikan" : "Nyalakan";
  html += R"rawlit(</button></form>
  </div>
</div>

<form action='/togglepulsa' method='get'>
  <button class='btn-reset' id='btn-mode-pulsa' style='color:)rawlit";
  html += modePulsa ? "#22c55e'" : "#ef4444'";
  html += R"rawlit(>Mode Pulsa: )rawlit";
  html += modePulsa ? "AKTIF (klik untuk nonaktifkan)" : "NONAKTIF (klik untuk aktifkan)";
  html += R"rawlit(</button>
</form>

<form action='/reset' method='get'>
  <button class='btn-reset'>Reset Counter kWh</button>
</form>

<button class='btn-reset' id='btn-kalibrasi'
  style='color:#f59e0b;border-color:#78350f;width:100%;padding:10px;
         border:1px solid #78350f;border-radius:10px;background:#1e293b;
         font-size:13px;cursor:pointer;margin-bottom:8px'
  onclick='kalibrasiPZEM()'>🔧 Kalibrasi PZEM (Reset Energi Chip)</button>

<div id='kalibrasi-msg' style='font-size:12px;text-align:center;padding:4px 0;
     margin-bottom:8px;display:none;border-radius:8px;padding:8px'></div>

<div class='footer' id='footer'>)rawlit";
  html += ipStr+" | Tarif Rp "+String((int)tarifKwh)+"/kWh | Refresh 1.5s";
  html += R"rawlit(</div>
</body></html>)rawlit";

  webServer.send(200, "text/html", html);
}

// ============================================
// WEB: Halaman Info
// ============================================
void halamanInfo() {
  uint32_t chipId = 0;
  for(int i=0; i<17; i=i+8) { chipId |= ((ESP.getEfuseMac()>>i)&0xff)<<i; }
  unsigned long uptimeSec = millis() / 1000;
  int hh = uptimeSec / 3600;
  int mm = (uptimeSec % 3600) / 60;
  int ss = uptimeSec % 60;
  String uptimeStr = String(hh)+"j "+String(mm)+"m "+String(ss)+"d";
  String ipLocal = (WiFi.status()==WL_CONNECTED) ? WiFi.localIP().toString() : "-";
  int rssi = WiFi.RSSI();
  String sinyalStr = (WiFi.status()==WL_CONNECTED) ?
    (rssi>-50?"Kuat":rssi>-65?"Sedang":rssi>-80?"Lemah":"Sangat Lemah") : "Tidak Terhubung";
  String macStr = WiFi.macAddress();
  uint32_t freeHeap = ESP.getFreeHeap();

  String html = R"rawlit(
<!DOCTYPE html><html><head>
<meta charset='UTF-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Info - Smart Plug</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:sans-serif;background:#0f172a;color:#e2e8f0;padding:16px}
  .back{font-size:12px;color:#3b82f6;text-decoration:none;display:block;margin-bottom:6px}
  h1{font-size:17px;color:#94a3b8;margin-bottom:16px}
  .card{background:#1e293b;border-radius:12px;padding:16px;margin-bottom:12px}
  .card h2{font-size:13px;color:#64748b;text-transform:uppercase;letter-spacing:1px;margin-bottom:12px}
  .row{display:flex;justify-content:space-between;align-items:center;
       padding:8px 0;border-bottom:1px solid #0f172a;font-size:13px}
  .row:last-child{border-bottom:none}
  .row .label{color:#94a3b8}
  .row .val{color:#e2e8f0;font-weight:600;text-align:right;max-width:55%}
  .badge{display:inline-block;padding:2px 8px;border-radius:20px;font-size:11px;font-weight:600}
  .badge.hijau{background:#14532d;color:#4ade80}
  .badge.merah{background:#7f1d1d;color:#fca5a5}
  .badge.biru{background:#1e3a5f;color:#60a5fa}
  .badge.kuning{background:#78350f;color:#fde68a}
  .fitur{padding:10px 12px;background:#0f172a;border-radius:8px;margin-bottom:8px}
  .fitur .nama{font-size:13px;font-weight:600;color:#e2e8f0;margin-bottom:3px}
  .fitur .desc{font-size:11px;color:#64748b;line-height:1.5}
  .sep{border:none;border-top:1px solid #334155;margin:12px 0}
</style>
</head><body>
<a href='/' class='back'>← Kembali ke Dashboard</a>
<h1>ℹ️ Info & Panduan</h1>

<div class='card'>
  <h2>Status Perangkat</h2>
  <div class='row'><span class='label'>Uptime</span>
    <span class='val'>)rawlit";
  html += uptimeStr;
  html += R"rawlit(</span></div>
  <div class='row'><span class='label'>Free RAM</span>
    <span class='val'>)rawlit";
  html += String(freeHeap / 1024) + " KB";
  html += R"rawlit(</span></div>
  <div class='row'><span class='label'>CPU Freq</span>
    <span class='val'>)rawlit";
  html += String(ESP.getCpuFreqMHz()) + " MHz";
  html += R"rawlit(</span></div>
  <div class='row'><span class='label'>Flash Size</span>
    <span class='val'>)rawlit";
  html += String(ESP.getFlashChipSize() / 1024 / 1024) + " MB";
  html += R"rawlit(</span></div>
  <div class='row'><span class='label'>Chip ID</span>
    <span class='val'>)rawlit";
  html += String(chipId, HEX);
  html += R"rawlit(</span></div>
</div>

<div class='card'>
  <h2>Jaringan</h2>
  <div class='row'><span class='label'>Status WiFi</span>
    <span class='val'>)rawlit";
  html += (WiFi.status()==WL_CONNECTED) ?
    "<span class='badge hijau'>Terhubung</span>" :
    "<span class='badge merah'>Terputus</span>";
  html += R"rawlit(</span></div>
  <div class='row'><span class='label'>SSID</span>
    <span class='val'>)rawlit";
  html += (wifiSSID != "") ? wifiSSID : "-";
  html += R"rawlit(</span></div>
  <div class='row'><span class='label'>IP Address</span>
    <span class='val'>)rawlit";
  html += ipLocal;
  html += R"rawlit(</span></div>
  <div class='row'><span class='label'>IP Hotspot</span>
    <span class='val'>192.168.4.1</span></div>
  <div class='row'><span class='label'>MAC Address</span>
    <span class='val'>)rawlit";
  html += macStr;
  html += R"rawlit(</span></div>
  <div class='row'><span class='label'>Sinyal WiFi</span>
    <span class='val'>)rawlit";
  html += sinyalStr;
  if (WiFi.status()==WL_CONNECTED) html += " (" + String(rssi) + " dBm)";
  html += R"rawlit(</span></div>
  <div class='row'><span class='label'>Blynk</span>
    <span class='val'>)rawlit";
  html += Blynk.connected() ?
    "<span class='badge hijau'>Terhubung</span>" :
    "<span class='badge merah'>Terputus</span>";
  html += R"rawlit(</span></div>
  <div class='row'><span class='label'>Mode WiFi</span>
    <span class='val'>)rawlit";
  html += modeLokal ? "<span class='badge kuning'>Lokal (AP Only)</span>" :
                      "<span class='badge biru'>AP + STA</span>";
  html += R"rawlit(</span></div>
</div>

<div class='card'>
  <h2>Pin Hardware</h2>
  <div class='row'><span class='label'>Relay 1</span><span class='val'>GPIO 26 (Active LOW)</span></div>
  <div class='row'><span class='label'>Relay 2</span><span class='val'>GPIO 25 (Active LOW)</span></div>
  <div class='row'><span class='label'>Buzzer</span><span class='val'>GPIO 32</span></div>
  <div class='row'><span class='label'>Tombol 1</span><span class='val'>GPIO 14 (Pull-Up)</span></div>
  <div class='row'><span class='label'>Tombol 2</span><span class='val'>GPIO 12 (Pull-Up)</span></div>
  <div class='row'><span class='label'>PZEM TX</span><span class='val'>GPIO 16 (RX2)</span></div>
  <div class='row'><span class='label'>PZEM RX</span><span class='val'>GPIO 17 (TX2)</span></div>
</div>

<div class='card'>
  <h2>Panduan Fitur</h2>

  <div class='fitur'>
    <div class='nama'>📊 Monitor Listrik (Real-time)</div>
    <div class='desc'>Membaca Tegangan (V), Arus (A), Daya (W), Frekuensi (Hz), Power Factor, dan total konsumsi kWh dari sensor PZEM-004T secara otomatis setiap 1 detik.</div>
  </div>

  <div class='fitur'>
    <div class='nama'>💰 Mode Pulsa (Prepaid)</div>
    <div class='desc'>Sistem token listrik prabayar. Saldo otomatis dipotong berdasarkan pemakaian kWh × Tarif. Relay dimatikan paksa saat saldo = Rp 0. Bisa dinonaktifkan untuk mode listrik bebas.</div>
  </div>

  <div class='fitur'>
    <div class='nama'>⚡ Trip Otomatis (Overload)</div>
    <div class='desc'>Jika daya melampaui Batas Daya yang diatur, semua relay dimatikan paksa sebagai proteksi. Reset dengan menekan KEDUA tombol fisik secara bersamaan selama 1 detik.</div>
  </div>

  <div class='fitur'>
    <div class='nama'>🔔 Peringatan Buzzer</div>
    <div class='desc'>1 beep = Relay ON | 2 beep cepat = Relay OFF | 3 beep = Ditolak (pulsa habis/trip) | 4 beep panjang = Pulsa habis | 5 beep = Trip overload | 3 beep = Masuk mode lokal.</div>
  </div>

  <div class='fitur'>
    <div class='nama'>📶 WiFi Auto-Reconnect</div>
    <div class='desc'>Alat mencoba menyambung ulang ke WiFi secara otomatis tiap 30 detik. Jika gagal 3 kali berturut-turut, alat masuk ke Mode Lokal (AP Only) dan berhenti mencoba, sehingga tidak membuang energi CPU.</div>
  </div>

  <div class='fitur'>
    <div class='nama'>☁️ Blynk Auto-Reconnect</div>
    <div class='desc'>Jika koneksi ke server Blynk terputus (bukan WiFi-nya), alat akan mencoba menyambung kembali tiap 10 detik secara mandiri di latar belakang.</div>
  </div>

  <div class='fitur'>
    <div class='nama'>💾 Perlindungan Flash Memory</div>
    <div class='desc'>Data saldo dan kWh hanya disimpan ke memori flash setiap 5 menit (bukan setiap detik) untuk mencegah kerusakan prematur pada chip memori ESP32.</div>
  </div>

  <div class='fitur'>
    <div class='nama'>🔌 Anti Flicker Relay</div>
    <div class='desc'>Relay diinisialisasi dengan status yang tepat sebelum pin aktif, sehingga tidak ada kedutan/bunyi klik saat ESP32 dinyalakan atau di-restart.</div>
  </div>

</div>

<div style='font-size:11px;color:#475569;text-align:center;margin-top:8px;padding-bottom:16px'>
  Smart Plug ESP32 &mdash; Firmware v2.0 | Akses: http://smartplug.local
</div>

</body></html>)rawlit";

  webServer.send(200, "text/html", html);
}