// ============================================================================
// PRUEBA DEL MODULO DE BUZZER — para Arduino Mega  (version automatica)
// ============================================================================
// No hay que escribir nada en el Monitor Serial: todo corre solo y se repite.
// La version anterior esperaba un Enter entre pruebas y se quedaba colgada si
// el Monitor Serial tenia el final de linea en "Sin ajuste de linea".
//
// CONEXION EN EL ARDUINO MEGA:
//   S (señal) -> pin 8
//   +         -> 5V del MEGA
//   -         -> GND del MEGA
//
// IMPORTANTISIMO: los tres cables al MEGA, no a la fuente externa.
// Si el modulo se alimenta de otra fuente y solo S va al Mega, falta la
// referencia comun y la señal no significa nada: el modulo no suena y parece
// averiado. Si quieres usar la fuente externa, hay que unir su GND con el
// GND del Mega.
//
// NIVEL DE REPOSO: tu modulo es de disparo en BAJO, asi que en reposo el pin
// va en ALTO. Con el Mega eso son 5V reales, que es justo lo que necesita su
// transistor PNP para cortar (con los 3.3V del ESP32 no cortaba: ese era el
// problema original).
//
// NOTA: el Mega no soporta Serial.printf(); todo va con Serial.print().
// ============================================================================

constexpr uint8_t PIN_BUZZER = 8;

// true  -> en reposo el pin queda en ALTO  (modulos de disparo en BAJO)
// false -> en reposo el pin queda en BAJO  (modulos de disparo en ALTO)
constexpr bool NIVEL_REPOSO_ALTO = true;

const uint16_t frecuencias[] = {
  1000, 1500, 2000, 2048, 2300, 2700, 3000, 3500, 4000, 4500
};
constexpr uint8_t TOTAL_FRECUENCIAS =
    sizeof(frecuencias) / sizeof(frecuencias[0]);

uint32_t vuelta = 0;

// ----------------------------------------------------------------------------
void callar() {
  noTone(PIN_BUZZER);
  // noTone() deja el pin en BAJO, que en un modulo de disparo en bajo lo
  // mantendria energizado. Por eso el nivel de reposo se fija a proposito.
  digitalWrite(PIN_BUZZER, NIVEL_REPOSO_ALTO ? HIGH : LOW);
}

void cuentaRegresiva(uint8_t segundos) {
  for (uint8_t s = segundos; s > 0; s--) {
    Serial.print(F("    "));
    Serial.print(s);
    Serial.println(F(" s"));
    delay(1000);
  }
}

// ----------------------------------------------------------------------------
// PRUEBA 1: suena o no suena
// ----------------------------------------------------------------------------
void pruebaPitidos() {
  Serial.println(F("--- PRUEBA 1: cinco pitidos a 2300 Hz ---"));
  Serial.println(F("Si no oyes NADA aqui, no sigas con el resto:"));
  Serial.println(F("revisa el pin 8, y que +, - y S vayan todos al Mega."));

  for (uint8_t i = 1; i <= 5; i++) {
    Serial.print(F("  pitido "));
    Serial.println(i);
    tone(PIN_BUZZER, 2300);
    delay(300);
    callar();
    delay(300);
  }
  Serial.println();
}

// ----------------------------------------------------------------------------
// PRUEBA 2: barrido de frecuencias
// ----------------------------------------------------------------------------
void pruebaBarrido() {
  Serial.println(F("--- PRUEBA 2: barrido de frecuencias ---"));
  Serial.println(F("Anota en cual suena MAS FUERTE."));

  for (uint8_t i = 0; i < TOTAL_FRECUENCIAS; i++) {
    Serial.print(F("  "));
    if (i + 1 < 10) Serial.print(' ');
    Serial.print(i + 1);
    Serial.print(F("/"));
    Serial.print(TOTAL_FRECUENCIAS);
    Serial.print(F(" ...... "));
    if (frecuencias[i] < 1000) Serial.print(' ');
    Serial.print(frecuencias[i]);
    Serial.println(F(" Hz"));

    tone(PIN_BUZZER, frecuencias[i]);
    delay(800);
    callar();
    delay(400);
  }
  Serial.println();
}

// ----------------------------------------------------------------------------
// PRUEBA 3: temperatura en reposo
// ----------------------------------------------------------------------------
void pruebaReposo() {
  Serial.println(F("--- PRUEBA 3: temperatura en reposo ---"));
  Serial.print(F("El pin queda en "));
  Serial.print(NIVEL_REPOSO_ALTO ? F("ALTO") : F("BAJO"));
  Serial.println(F(" durante 30 s. Toca el buzzer al final."));
  Serial.println(F("Debe quedar FRIO y en silencio absoluto."));

  callar();
  cuentaRegresiva(30);

  Serial.println(F("  Tocalo ahora."));
  Serial.println(F("  Si esta TIBIO, cambia NIVEL_REPOSO_ALTO y repite."));
  Serial.println();
}

// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_BUZZER, OUTPUT);
  callar();

  Serial.println();
  Serial.println(F("###############################################"));
  Serial.println(F("#     PRUEBA DEL BUZZER EN ARDUINO MEGA       #"));
  Serial.println(F("###############################################"));
  Serial.print(F("Señal en el pin "));
  Serial.print(PIN_BUZZER);
  Serial.print(F("   |   reposo en "));
  Serial.println(NIVEL_REPOSO_ALTO ? F("ALTO") : F("BAJO"));
  Serial.println();
  Serial.println(F("Los tres cables (S, + y -) van al MEGA."));
  Serial.println(F("Si el modulo usa otra fuente, une los dos GND."));
  Serial.println();
  Serial.println(F("Las pruebas corren solas y se repiten. No hay que"));
  Serial.println(F("escribir nada en el Monitor Serial."));
  Serial.println();
  delay(2000);
}

void loop() {
  vuelta++;

  Serial.println();
  Serial.print(F("============ VUELTA "));
  Serial.print(vuelta);
  Serial.println(F(" ============"));

  pruebaPitidos();
  pruebaBarrido();
  pruebaReposo();

  Serial.println(F("Fin de la vuelta. Empieza otra en 3 s."));
  delay(3000);
}
