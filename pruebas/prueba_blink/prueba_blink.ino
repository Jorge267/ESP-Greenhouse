// ============================================================================
// PRUEBA DE HARDWARE — Blink simple, sin WiFi ni Bluetooth
// ============================================================================
// Objetivo: confirmar que el chip ESP32 en sí funciona, aislado de cualquier
// librería de red. Si esto no funciona, el problema es de hardware o de la
// alimentación/cable USB — ya no del código de RainMaker.
//
// La mayoría de placas ESP32 DEVKIT V1 traen un LED integrado en GPIO2.
// Si no ves nada parpadear, revisa la placa buscando un LED pequeño cerca
// del puerto USB, y si el número no coincide, dime qué dice el silkscreen.
// ============================================================================

constexpr uint8_t PIN_LED_INTEGRADO = 2;

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(PIN_LED_INTEGRADO, OUTPUT);
  Serial.println("=== Prueba de hardware iniciada ===");
}

void loop() {
  digitalWrite(PIN_LED_INTEGRADO, HIGH);
  Serial.println("LED: ENCENDIDO");
  delay(500);

  digitalWrite(PIN_LED_INTEGRADO, LOW);
  Serial.println("LED: apagado");
  delay(500);
}
