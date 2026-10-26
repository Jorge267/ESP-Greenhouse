// ============================================================================
// ESCÁNER I2C — Diagnóstico del BH1750 (GY-30)
// ============================================================================
// Recorre todas las direcciones del bus I2C y reporta cuáles responden.
// El BH1750 debe aparecer en 0x23 (con ADDR sin conectar o a GND)
// o en 0x5C (con ADDR conectado a VCC).
// ============================================================================

#include <Wire.h>

constexpr uint8_t PIN_I2C_SDA = 21;
constexpr uint8_t PIN_I2C_SCL = 22;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Serial.println("\n=== Escaner I2C iniciado ===");
  Serial.printf("SDA = GPIO%d | SCL = GPIO%d\n\n", PIN_I2C_SDA, PIN_I2C_SCL);
}

void loop() {
  uint8_t encontrados = 0;

  Serial.println("Escaneando direcciones 0x01 a 0x7E...");

  for (uint8_t direccion = 1; direccion < 127; direccion++) {
    Wire.beginTransmission(direccion);
    uint8_t resultado = Wire.endTransmission();

    if (resultado == 0) {
      Serial.printf("  >>> Dispositivo encontrado en 0x%02X", direccion);
      if (direccion == 0x23) Serial.print("   <-- BH1750 (direccion por defecto)");
      if (direccion == 0x5C) Serial.print("   <-- BH1750 (ADDR conectado a VCC)");
      if (direccion == 0x27 || direccion == 0x3F) Serial.print("   <-- probablemente una pantalla LCD");
      Serial.println();
      encontrados++;
    }
  }

  if (encontrados == 0) {
    Serial.println("  NINGUN dispositivo respondio en el bus.");
    Serial.println("  Revisa: alimentacion del sensor, GND comun, y que");
    Serial.println("  SDA/SCL no esten cruzados o sueltos.");
  } else {
    Serial.printf("  Total encontrados: %d\n", encontrados);
  }

  Serial.println();
  delay(5000);
}
