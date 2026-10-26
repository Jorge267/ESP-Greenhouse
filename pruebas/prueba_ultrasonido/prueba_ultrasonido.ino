// ============================================================================
// PRUEBA DEL SENSOR ULTRASONICO HC-SR04 — para Arduino Mega
// ============================================================================
// Sirve para decidir si el sensor esta bueno o malo, sin que el conversor de
// nivel, el divisor de tension ni el ESP32 puedan influir en el resultado.
//
// POR QUE EN EL MEGA:
//   El Mega trabaja a 5V, igual que el HC-SR04. Eso permite conectar ECHO
//   DIRECTO al pin, sin conversor y sin divisor. Si aqui el sensor responde
//   bien, el problema esta en la adaptacion de nivel del otro montaje; si
//   aqui tampoco responde, el sensor o su alimentacion son el problema.
//
// NO NECESITA NINGUNA LIBRERIA.
//
// CONEXION EN EL ARDUINO MEGA:
//   VCC  -> 5V
//   GND  -> GND
//   TRIG -> pin 9
//   ECHO -> pin 10     (DIRECTO, el Mega tolera 5V)
//
// NOTA SOBRE EL CODIGO: el Arduino Mega NO soporta Serial.printf(), porque
// esa funcion es una extension propia del core del ESP32. Aqui todo se
// imprime con Serial.print(). Tampoco se usa %f en sprintf: el sprintf de
// AVR no trae soporte de flotantes, asi que los decimales se imprimen con
// Serial.print(valor, 1).
// ============================================================================

constexpr uint8_t PIN_TRIG = 9;
constexpr uint8_t PIN_ECHO = 10;

// Velocidad del sonido a 20 C. En el programa del invernadero esto se
// compensa con la temperatura del DHT11; aqui se deja fijo porque lo que
// interesa es saber si el sensor responde, no la precision absoluta.
constexpr float VELOCIDAD_CM_POR_US = 0.0343f;

constexpr uint8_t  MEDIDAS_POR_CICLO = 10;
constexpr uint32_t ESPERA_INICIO_ECO_US = 50000UL;  // 50 ms
constexpr uint32_t ESPERA_FIN_ECO_US    = 40000UL;  // 40 ms (~6.8 m)

// Codigos de resultado de una medida
constexpr uint8_t RES_OK            = 0;
constexpr uint8_t RES_SIN_INICIO    = 1;  // El eco nunca empezo
constexpr uint8_t RES_PEGADO_ALTO   = 2;  // Empezo y nunca termino
constexpr uint8_t RES_ALTO_PREVIO   = 3;  // Ya estaba alto antes de disparar

uint32_t cicloActual = 0;

// ----------------------------------------------------------------------------
// Una medida, con diagnostico de en que paso falla
// ----------------------------------------------------------------------------
// No se usa pulseIn() a proposito: esa funcion devuelve 0 tanto si el eco no
// llego como si llego mal, y aqui lo que interesa es distinguir el motivo.
uint8_t medir(uint32_t *duracion, float *distancia) {
  *duracion  = 0;
  *distancia = -1.0f;

  // Paso 1: en reposo el pin ECHO tiene que estar en bajo. Si esta alto, o
  // el cable esta mal, o el sensor quedo colgado de una medida anterior.
  if (digitalRead(PIN_ECHO) == HIGH) {
    return RES_ALTO_PREVIO;
  }

  // Paso 2: pulso de disparo de 10 us, como pide el datasheet
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(4);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  // Paso 3: esperar a que el sensor levante ECHO
  uint32_t marca = micros();
  while (digitalRead(PIN_ECHO) == LOW) {
    if (micros() - marca > ESPERA_INICIO_ECO_US) {
      return RES_SIN_INICIO;
    }
  }

  // Paso 4: medir cuanto dura el pulso alto
  uint32_t inicio = micros();
  while (digitalRead(PIN_ECHO) == HIGH) {
    if (micros() - inicio > ESPERA_FIN_ECO_US) {
      return RES_PEGADO_ALTO;
    }
  }
  uint32_t fin = micros();

  *duracion  = fin - inicio;
  *distancia = (*duracion) * VELOCIDAD_CM_POR_US / 2.0f;
  return RES_OK;
}

// ----------------------------------------------------------------------------
// Utilidades de impresion alineada
// ----------------------------------------------------------------------------
void imprimirRelleno(uint32_t valor, uint8_t ancho) {
  uint32_t limite = 10;
  for (uint8_t i = 1; i < ancho; i++) {
    if (valor < limite) Serial.print(' ');
    limite *= 10;
  }
  Serial.print(valor);
}

void explicarCodigo(uint8_t codigo) {
  switch (codigo) {
    case RES_SIN_INICIO:
      Serial.println(F("SIN ECO - el sensor no levanto la linea"));
      break;
    case RES_PEGADO_ALTO:
      Serial.println(F("ECHO PEGADO EN ALTO - no bajo nunca"));
      break;
    case RES_ALTO_PREVIO:
      Serial.println(F("ECHO YA ESTABA ALTO antes de disparar"));
      break;
    default:
      break;
  }
}

// ----------------------------------------------------------------------------
// Revision del nivel de reposo de ECHO antes de empezar
// ----------------------------------------------------------------------------
void revisarReposo() {
  Serial.println();
  Serial.println(F("--- Nivel de ECHO en reposo (sin disparar) ---"));

  uint16_t vecesAlto = 0;
  for (uint16_t i = 0; i < 200; i++) {
    if (digitalRead(PIN_ECHO) == HIGH) vecesAlto++;
    delay(1);
  }

  Serial.print(F("  Lecturas en alto: "));
  Serial.print(vecesAlto);
  Serial.println(F(" de 200"));

  if (vecesAlto == 0) {
    Serial.println(F("  Correcto: la linea esta en reposo."));
  } else if (vecesAlto >= 190) {
    Serial.println(F("  MAL: ECHO esta fijo en alto. Posibles causas:"));
    Serial.println(F("   - ECHO y TRIG invertidos"));
    Serial.println(F("   - ECHO conectado al 5V por error"));
    Serial.println(F("   - El sensor esta averiado"));
  } else {
    Serial.println(F("  MAL: la linea oscila sola. Eso es ruido:"));
    Serial.println(F("   - ECHO desconectado (el pin queda flotando)"));
    Serial.println(F("   - Falta el GND comun"));
  }
}

// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  digitalWrite(PIN_TRIG, LOW);

  Serial.println();
  Serial.println(F("###############################################"));
  Serial.println(F("#      PRUEBA DEL HC-SR04 EN ARDUINO MEGA     #"));
  Serial.println(F("###############################################"));
  Serial.print(F("TRIG en el pin "));
  Serial.print(PIN_TRIG);
  Serial.print(F("   |   ECHO en el pin "));
  Serial.println(PIN_ECHO);
  Serial.println(F("ECHO va DIRECTO: el Mega es de 5V y tolera su salida."));

  revisarReposo();

  Serial.println();
  Serial.println(F("--- Como interpretar la prueba ---"));
  Serial.println(F("Apunta el sensor a una pared plana y lisa, perpendicular,"));
  Serial.println(F("a unos 30 cm. Compara con cinta metrica."));
  Serial.println(F("Recuerda que por debajo de 2 cm no mide: es su zona muerta."));
  Serial.println();
}

// ----------------------------------------------------------------------------
void loop() {
  cicloActual++;

  Serial.println();
  Serial.print(F("============ CICLO "));
  Serial.print(cicloActual);
  Serial.println(F(" ============"));
  Serial.println(F(" #    eco (us)    distancia      resultado"));
  Serial.println(F("---  ----------  -----------  ----------------"));

  uint8_t  correctas = 0;
  uint8_t  fallos[4] = {0, 0, 0, 0};
  float    suma = 0.0f;
  float    minimo = 99999.0f;
  float    maximo = -1.0f;

  for (uint8_t i = 0; i < MEDIDAS_POR_CICLO; i++) {
    uint32_t duracion;
    float    distancia;
    uint8_t  resultado = medir(&duracion, &distancia);

    Serial.print(' ');
    imprimirRelleno(i + 1, 2);
    Serial.print(F("  "));

    if (resultado == RES_OK) {
      imprimirRelleno(duracion, 8);
      Serial.print(F("  "));
      if (distancia < 100.0f) Serial.print(' ');
      if (distancia < 10.0f)  Serial.print(' ');
      Serial.print(distancia, 1);
      Serial.println(F(" cm     ok"));

      correctas++;
      suma += distancia;
      if (distancia < minimo) minimo = distancia;
      if (distancia > maximo) maximo = distancia;
    } else {
      Serial.print(F("       ---          ---     "));
      explicarCodigo(resultado);
      fallos[resultado]++;
    }

    // El datasheet pide al menos 60 ms entre disparos. Con menos, el eco de
    // la medida anterior todavia anda rebotando y contamina la siguiente.
    delay(70);
  }

  // --- Resumen ---
  Serial.println();
  Serial.print(F("Medidas validas: "));
  Serial.print(correctas);
  Serial.print(F(" de "));
  Serial.println(MEDIDAS_POR_CICLO);

  if (correctas > 0) {
    Serial.print(F("  promedio ... "));
    Serial.print(suma / correctas, 1);
    Serial.println(F(" cm"));
    Serial.print(F("  minimo ..... "));
    Serial.print(minimo, 1);
    Serial.println(F(" cm"));
    Serial.print(F("  maximo ..... "));
    Serial.print(maximo, 1);
    Serial.println(F(" cm"));
    Serial.print(F("  dispersion . "));
    Serial.print(maximo - minimo, 1);
    Serial.println(F(" cm"));
  }

  if (fallos[RES_SIN_INICIO])  {
    Serial.print(F("  sin eco .......... "));
    Serial.println(fallos[RES_SIN_INICIO]);
  }
  if (fallos[RES_PEGADO_ALTO]) {
    Serial.print(F("  echo pegado ...... "));
    Serial.println(fallos[RES_PEGADO_ALTO]);
  }
  if (fallos[RES_ALTO_PREVIO]) {
    Serial.print(F("  alto antes ....... "));
    Serial.println(fallos[RES_ALTO_PREVIO]);
  }

  // --- Veredicto ---
  Serial.println();
  if (correctas == MEDIDAS_POR_CICLO && (maximo - minimo) < 1.0f) {
    Serial.println(F("VEREDICTO: el sensor esta BIEN y es estable."));
    Serial.println(F("  Si en el ESP32 no funciona, el problema esta en la"));
    Serial.println(F("  adaptacion de nivel de ECHO o en la alimentacion."));
  } else if (correctas == 0) {
    Serial.println(F("VEREDICTO: el sensor NO responde."));
    Serial.println(F("  1. Mide VCC contra GND: deben ser 5V reales"));
    Serial.println(F("  2. Confirma que TRIG y ECHO no esten invertidos"));
    Serial.println(F("  3. Si todo esta bien, el sensor esta averiado"));
  } else if (correctas < MEDIDAS_POR_CICLO) {
    Serial.println(F("VEREDICTO: responde de forma INTERMITENTE."));
    Serial.println(F("  Suele ser alimentacion debil, un cable con falso"));
    Serial.println(F("  contacto, o una superficie que dispersa el sonido."));
    Serial.println(F("  Prueba apuntando a una pared lisa y perpendicular."));
  } else {
    Serial.println(F("VEREDICTO: responde siempre, pero con mucha dispersion."));
    Serial.println(F("  Revisa que apunte perpendicular a una superficie plana"));
    Serial.println(F("  y que nada cruce por delante de los dos cilindros."));
  }

  delay(2000);
}
