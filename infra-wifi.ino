#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

// ======= Wi-Fi =======
const char* WIFI_SSID = "LABVR";
const char* WIFI_PASS = "04753933";

// Broadcast por defecto; para unicast pon la IP de tu PC.
IPAddress UDP_REMOTE_IP(192,168,0,106);
const uint16_t UDP_PORT = 42101;
static const bool SEND_EVERY_SAMPLE = true;

// ======= Pines =======
#define PIN_LED1   25   // Emisor IR #1 (330 Ω serie)
#define PIN_LED2   19   // Emisor IR #2 (330 Ω serie)
#define PIN_ADC1   34   // Receptor #1 (nodo ● -> 100 Ω -> ADC1)
#define PIN_ADC2   32   // Receptor #2 (nodo ● -> 100 Ω -> ADC1)
#define PIN_IND     2   // LED indicador único (ON si CH1||CH2)

// ======= Modulación y ventanas (síncronas) =======
static const uint32_t CARRIER_HZ = 38000; // 38 kHz
static const uint32_t WINDOW_US  = 3000;  // ventana de promediado ON y OFF
static const uint32_t GUARD_US   = 1200;  // asentamiento tras cambiar ON/OFF (4.7k//100nF -> τ≈0.47 ms)
static const int      THRESH     = 8;     // umbral DIFF -> 1=detecta
static const bool     PRINT_DBG  = false;

WiFiUDP Udp;
bool last_ch1=false, last_ch2=false;
char pkt[16];

// ======= Portadora (sin LEDC) =======
inline void carrierSetupPin(uint8_t pin){ pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
inline void carrierOn(uint8_t pin)      { tone(pin, CARRIER_HZ); }
inline void carrierOff(uint8_t pin)     { noTone(pin); }

// ======= ADC =======
inline uint16_t readADC(uint8_t pin){ return analogRead(pin); }

// Promedia tantos samples como quepan en usWindow para AMBOS ADC (intercalados)
void avgSamplesTimedDual(uint32_t usWindow,
                         uint32_t &cnt1, uint32_t &cnt2,
                         uint32_t &avg1, uint32_t &avg2)
{
  uint32_t t0 = micros();
  uint64_t acc1=0, acc2=0;
  uint32_t n1=0, n2=0;
  while ((int32_t)(micros()-t0) < (int32_t)usWindow) {
    acc1 += readADC(PIN_ADC1); n1++;
    acc2 += readADC(PIN_ADC2); n2++;
  }
  cnt1 = n1; cnt2 = n2;
  avg1 = n1 ? (uint32_t)(acc1/n1) : 0;
  avg2 = n2 ? (uint32_t)(acc2/n2) : 0;
}

// Demodulación SÍNCRONA: devuelve CH1/CH2 de la MISMA ventana
void measureBoth(bool &ch1, bool &ch2)
{
  // OFF para ambos
  carrierOff(PIN_LED1); carrierOff(PIN_LED2);
  delayMicroseconds(GUARD_US);
  uint32_t cOff1=0,cOff2=0, off1=0, off2=0;
  avgSamplesTimedDual(WINDOW_US, cOff1, cOff2, off1, off2);

  // ON para ambos (38 kHz)
  carrierOn(PIN_LED1); carrierOn(PIN_LED2);
  delayMicroseconds(GUARD_US);
  uint32_t cOn1=0,cOn2=0, on1=0, on2=0;
  avgSamplesTimedDual(WINDOW_US, cOn1, cOn2, on1, on2);

  // Apaga emisores
  carrierOff(PIN_LED1); carrierOff(PIN_LED2);

  int32_t d1 = (int32_t)on1 - (int32_t)off1; if (d1<0) d1=0;
  int32_t d2 = (int32_t)on2 - (int32_t)off2; if (d2<0) d2=0;

  ch1 = d1 > THRESH;
  ch2 = d2 > THRESH;

  if (PRINT_DBG) {
    Serial.print(F("[DBG] off1="));Serial.print(off1);
    Serial.print(F(" on1="));Serial.print(on1);
    Serial.print(F(" d1="));Serial.print(d1);
    Serial.print(F(" | off2="));Serial.print(off2);
    Serial.print(F(" on2="));Serial.print(on2);
    Serial.print(F(" d2="));Serial.println(d2);
  }
}

void connectWiFi(){
  if (WiFi.status()==WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Conectando a Wi-Fi");
  uint32_t t0=millis();
  while (WiFi.status()!=WL_CONNECTED && (millis()-t0)<15000) {
    Serial.print("."); delay(250);
  }
  Serial.println();
  if (WiFi.status()==WL_CONNECTED) {
    Serial.print("Wi-Fi OK. IP: "); Serial.println(WiFi.localIP());
    Udp.begin(0);
  } else {
    Serial.println("Wi-Fi timeout. Reintento...");
  }
}

void setup(){
  Serial.begin(115200);
  carrierSetupPin(PIN_LED1);
  carrierSetupPin(PIN_LED2);
  pinMode(PIN_IND, OUTPUT); digitalWrite(PIN_IND, LOW);

#if defined(ARDUINO_ARCH_ESP32)
  analogSetWidth(12);
  analogSetPinAttenuation(PIN_ADC1, ADC_11db);
  analogSetPinAttenuation(PIN_ADC2, ADC_11db);
  // Nota: en core 3.x ya no existe analogSetClockDiv; no es necesario.
#endif

  connectWiFi();
  Serial.println(F("2CH IR | 38kHz síncrono | Ventanas cronometradas | UDP+Serial (0 1)"));
}

void loop(){
  if (WiFi.status()!=WL_CONNECTED) { connectWiFi(); delay(500); }

  bool ch1=false, ch2=false;
  measureBoth(ch1, ch2);

  digitalWrite(PIN_IND, (ch1||ch2) ? HIGH : LOW);

  // Serial mínimo "0 1"
  Serial.print(ch1 ? 1 : 0); Serial.print(' ');
  Serial.println(ch2 ? 1 : 0);

  // UDP "0 1\n"
  if (SEND_EVERY_SAMPLE || ch1!=last_ch1 || ch2!=last_ch2) {
    int n = snprintf(pkt, sizeof(pkt), "%d %d\n", ch1?1:0, ch2?1:0);
    Udp.beginPacket(UDP_REMOTE_IP, UDP_PORT);
    Udp.write((const uint8_t*)pkt, n);
    Udp.endPacket();
    last_ch1 = ch1; last_ch2 = ch2;
  }

  // Ajusta cadencia global si necesitas
  delay(10);
}
