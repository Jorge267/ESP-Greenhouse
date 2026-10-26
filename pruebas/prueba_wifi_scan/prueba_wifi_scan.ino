// ============================================================================
// PRUEBA DE HARDWARE — Solo escaneo de WiFi (sin Bluetooth, sin RainMaker)
// ============================================================================
// Objetivo: confirmar si el ESP32 puede ver redes WiFi cercanas, aislado de
// cualquier proceso de aprovisionamiento por Bluetooth.
// ============================================================================

#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(1000);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  Serial.println("=== Prueba de escaneo WiFi iniciada ===");
}

void loop() {
  Serial.println("Escaneando redes...");
  int redesEncontradas = WiFi.scanNetworks();

  if (redesEncontradas == 0) {
    Serial.println(">>> No se encontró ninguna red <<<");
  } else {
    Serial.printf(">>> Se encontraron %d redes:\n", redesEncontradas);
    for (int i = 0; i < redesEncontradas; i++) {
      Serial.printf("  %d: %s (RSSI: %d dBm) %s\n",
                    i + 1,
                    WiFi.SSID(i).c_str(),
                    WiFi.RSSI(i),
                    (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "[abierta]" : "[con contraseña]");
    }
  }

  WiFi.scanDelete();
  delay(5000);
}
