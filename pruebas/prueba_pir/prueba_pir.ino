// ============================================================================
// PRUEBA DEL SENSOR PIR — diagnostico, sin WiFi ni RainMaker
// ============================================================================
// CAMBIOS RESPECTO A LA VERSION ANTERIOR:
//
//   1. Se usa GPIO16 en vez de GPIO39. Los pines 34 al 39 son solo de entrada
//      y NO tienen resistencias internas, asi que si la señal no llega el pin
//      queda flotando y lee basura. GPIO16 si acepta INPUT_PULLDOWN: con eso,
//      un cable suelto lee BAJO de forma definitiva y se puede distinguir
//      "no hay movimiento" de "no llega señal".
//
//   2. Conexion DIRECTA, sin el conversor de niveles. La salida del HC-SR501
//      ya es de 3.3V (el chip BISS0001 trabaja a 3.3V aunque alimentes el
//      modulo con 5V), asi que el conversor no aporta nada y su pull-up de
//      10k a 5V pelea contra la salida debil del sensor.
//
// CONEXION:
//   VCC del PIR -> 5V
//   GND del PIR -> GND comun con el ESP32
//   OUT del PIR -> GPIO16   (directo, SIN conversor)
//
// AJUSTE DE LOS POTENCIOMETROS ANTES DE PROBAR:
//   Tiempo (Tx)       -> todo a la izquierda (minimo, unos 3 s)
//   Sensibilidad (Sx) -> al centro
//   Jumper            -> en H (disparo repetido)
//
// NOTA: el HC-SR501 necesita de 30 a 60 segundos de calentamiento tras
// energizarse. Durante ese lapso puede dar lecturas erraticas. Quedate
// quieto mientras tanto.
// ============================================================================

constexpr uint8_t PIN_PIR = 16;

constexpr uint32_t TIEMPO_CALENTAMIENTO = 60000;  // 60 s

uint32_t ultimoResumen      = 0;
uint32_t cambiosDetectados  = 0;
uint32_t tiempoUltimoCambio = 0;
bool     estadoAnterior     = false;
bool     calentamientoListo = false;

void setup() {
  Serial.begin(115200);
  delay(1000);

  // INPUT_PULLDOWN: sin señal, el pin lee BAJO de forma confiable.
  pinMode(PIN_PIR, INPUT_PULLDOWN);

  Serial.println();
  Serial.println("###############################################");
  Serial.println("#      PRUEBA DEL SENSOR PIR - DIAGNOSTICO    #");
  Serial.println("###############################################");
  Serial.printf("Pin de lectura : GPIO%d (con pull-down interno)\n", PIN_PIR);
  Serial.println("Conexion       : OUT del PIR DIRECTO al pin, sin conversor");
  Serial.println("Potenciometros : Tx al minimo, Sx al centro, jumper en H");
  Serial.println();

  bool nivelInicial = (digitalRead(PIN_PIR) == HIGH);
  estadoAnterior = nivelInicial;

  Serial.printf("Nivel inicial  : %s\n", nivelInicial ? "ALTO" : "bajo");
  if (nivelInicial) {
    Serial.println("   OJO: el pin arranca en ALTO. Puede ser que el sensor");
    Serial.println("   este disparado o que el potenciometro de tiempo este");
    Serial.println("   muy alto. Espera a que baje antes de hacer la prueba.");
  }
  Serial.println();
  Serial.println("Calentando el sensor 60 s. Quedate quieto...");
  Serial.println();
}

void loop() {
  bool estadoActual = (digitalRead(PIN_PIR) == HIGH);

  // ---- Fin del calentamiento ------------------------------------------------
  if (!calentamientoListo && millis() > TIEMPO_CALENTAMIENTO) {
    calentamientoListo = true;
    Serial.println();
    Serial.println(">>> Calentamiento terminado. Pasa la mano frente al sensor.");
    Serial.println();
  }

  // ---- Deteccion de cambio de nivel -----------------------------------------
  if (estadoActual != estadoAnterior) {
    uint32_t duracion = millis() - tiempoUltimoCambio;
    tiempoUltimoCambio = millis();
    estadoAnterior = estadoActual;
    cambiosDetectados++;

    Serial.printf("[%7lu ms]  CAMBIO -> %-22s | nivel anterior duro %lu ms | cambio #%lu\n",
                  millis(),
                  estadoActual ? "ALTO (movimiento)" : "bajo (sin movimiento)",
                  duracion,
                  cambiosDetectados);
  }

  // ---- Resumen periodico ----------------------------------------------------
  // Confirma que el sketch sigue corriendo aunque el pin nunca cambie.
  if (millis() - ultimoResumen > 5000) {
    ultimoResumen = millis();
    Serial.printf("[%7lu ms]  estado: %-4s | cambios: %-3lu | %s\n",
                  millis(),
                  estadoActual ? "ALTO" : "bajo",
                  cambiosDetectados,
                  calentamientoListo ? "listo" : "calentando...");

    // Diagnostico automatico: si tras el calentamiento no hubo ni un cambio.
    if (calentamientoListo && cambiosDetectados == 0 && millis() > 90000) {
      Serial.println();
      Serial.println("   ---- SIN NINGUN CAMBIO ----");
      if (estadoActual) {
        Serial.println("   El pin esta fijo en ALTO. Baja el potenciometro de");
        Serial.println("   tiempo (Tx) todo a la izquierda y reinicia.");
      } else {
        Serial.println("   El pin esta fijo en bajo. Verifica con multimetro:");
        Serial.println("    1. Que VCC del PIR tenga 5V reales");
        Serial.println("    2. Que el pin OUT suba a 3.3V al pasar la mano");
        Serial.println("       (midelo con OUT desconectado del ESP32)");
        Serial.println("    3. Que el lente blanco este puesto");
        Serial.println("    4. Que GND del PIR y del ESP32 sean el mismo");
      }
      Serial.println();
    }
  }

  delay(50);
}
