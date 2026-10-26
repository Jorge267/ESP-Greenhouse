// ============================================================================
// PRUEBA Y ENROLAMIENTO DEL SENSOR DE HUELLA AS608
// ============================================================================
// Sketch de diagnóstico independiente. No usa Wi-Fi ni RainMaker, solo el
// lector de huella, para poder aislar cualquier problema de cableado o
// comunicación antes de integrarlo al programa principal.
//
// LIBRERIA NECESARIA (Gestor de Librerias):
//   - "Adafruit Fingerprint Sensor Library"
//
// CONEXION (segun el mapa de pines del proyecto):
//   V+  (rojo)    -> 3V3 del ESP32
//   TX  (amarillo)-> GPIO19   (el TX del sensor va al RX del ESP32)
//   RX  (blanco)  -> GPIO23   (el RX del sensor va al TX del ESP32)
//   GND (negro)   -> GND comun
//   TCH (azul)    -> sin conectar
//   VA  (verde)   -> sin conectar
//
// USO:
//   Abre el Monitor Serial a 115200 baudios y escribe el numero de la opcion
//   que quieras. IMPORTANTE: en la parte de arriba del Monitor Serial,
//   selecciona "Nueva linea" o "Ambos NL & CR" para que funcione el menu.
// ============================================================================

#include <Adafruit_Fingerprint.h>

constexpr uint8_t PIN_HUELLA_RX = 19;  // Recibe del TX del sensor
constexpr uint8_t PIN_HUELLA_TX = 23;  // Envia al RX del sensor

HardwareSerial serialHuella(1);
Adafruit_Fingerprint lector(&serialHuella);

// ----------------------------------------------------------------------------
// Utilidades
// ----------------------------------------------------------------------------
void mostrarMenu() {
  Serial.println();
  Serial.println("================= MENU =================");
  Serial.println("  1  ->  Enrolar una huella nueva");
  Serial.println("  2  ->  Verificar huella (identificar)");
  Serial.println("  3  ->  Borrar una huella por ID");
  Serial.println("  4  ->  Ver cuantas huellas hay guardadas");
  Serial.println("  5  ->  BORRAR TODAS las huellas");
  Serial.println("========================================");
  Serial.print("Opcion: ");
}

// Espera a que el usuario escriba un numero en el Monitor Serial
int leerNumero() {
  while (!Serial.available()) {
    delay(50);
  }
  int numero = Serial.parseInt();
  // Limpia lo que haya quedado en el buffer (saltos de linea, etc.)
  while (Serial.available()) {
    Serial.read();
  }
  return numero;
}

// Traduce los codigos de error de la libreria a algo legible
void explicarError(uint8_t codigo) {
  switch (codigo) {
    case FINGERPRINT_PACKETRECIEVEERR:
      Serial.println("   ERROR: fallo de comunicacion con el sensor.");
      break;
    case FINGERPRINT_IMAGEFAIL:
      Serial.println("   ERROR: no se pudo capturar la imagen.");
      break;
    case FINGERPRINT_IMAGEMESS:
      Serial.println("   ERROR: imagen muy borrosa. Limpia el dedo y el lente.");
      break;
    case FINGERPRINT_FEATUREFAIL:
    case FINGERPRINT_INVALIDIMAGE:
      Serial.println("   ERROR: no se pudieron extraer las caracteristicas.");
      break;
    case FINGERPRINT_ENROLLMISMATCH:
      Serial.println("   ERROR: las dos capturas no coinciden. Usa el mismo dedo.");
      break;
    case FINGERPRINT_BADLOCATION:
      Serial.println("   ERROR: ID invalido.");
      break;
    case FINGERPRINT_FLASHERR:
      Serial.println("   ERROR: no se pudo escribir en la memoria del sensor.");
      break;
    case FINGERPRINT_NOTFOUND:
      Serial.println("   La huella NO esta registrada.");
      break;
    default:
      Serial.printf("   ERROR: codigo desconocido (0x%02X)\n", codigo);
      break;
  }
}

// ----------------------------------------------------------------------------
// 1. Enrolar
// ----------------------------------------------------------------------------
void enrolarHuella() {
  Serial.println();
  Serial.print("Con que ID quieres guardarla? (1 a 162): ");
  int id = leerNumero();

  if (id < 1 || id > 162) {
    Serial.println();
    Serial.println("ID fuera de rango. Debe ser de 1 a 162.");
    return;
  }
  Serial.println(id);

  uint8_t resultado;

  // --- Primera captura ---
  Serial.println();
  Serial.println(">> Coloca el dedo sobre el sensor...");
  do {
    resultado = lector.getImage();
    delay(100);
  } while (resultado != FINGERPRINT_OK);
  Serial.println("   Imagen capturada.");

  resultado = lector.image2Tz(1);
  if (resultado != FINGERPRINT_OK) {
    explicarError(resultado);
    return;
  }

  // --- Pedir que retire el dedo ---
  Serial.println(">> Retira el dedo.");
  delay(2000);
  do {
    resultado = lector.getImage();
    delay(100);
  } while (resultado != FINGERPRINT_NOFINGER);

  // --- Segunda captura ---
  Serial.println(">> Coloca el MISMO dedo otra vez...");
  do {
    resultado = lector.getImage();
    delay(100);
  } while (resultado != FINGERPRINT_OK);
  Serial.println("   Imagen capturada.");

  resultado = lector.image2Tz(2);
  if (resultado != FINGERPRINT_OK) {
    explicarError(resultado);
    return;
  }

  // --- Comparar y guardar ---
  Serial.println(">> Comparando las dos capturas...");
  resultado = lector.createModel();
  if (resultado != FINGERPRINT_OK) {
    explicarError(resultado);
    return;
  }

  resultado = lector.storeModel(id);
  if (resultado == FINGERPRINT_OK) {
    Serial.printf(">> LISTO: huella guardada en el ID %d\n", id);
  } else {
    explicarError(resultado);
  }
}

// ----------------------------------------------------------------------------
// 2. Verificar
// ----------------------------------------------------------------------------
void verificarHuella() {
  Serial.println();
  Serial.println(">> Coloca el dedo para identificarlo...");

  uint8_t resultado;
  do {
    resultado = lector.getImage();
    delay(100);
  } while (resultado != FINGERPRINT_OK);

  resultado = lector.image2Tz();
  if (resultado != FINGERPRINT_OK) {
    explicarError(resultado);
    return;
  }

  resultado = lector.fingerFastSearch();
  if (resultado == FINGERPRINT_OK) {
    Serial.printf(">> RECONOCIDA: ID %d  (confianza: %d)\n",
                  lector.fingerID, lector.confidence);
    Serial.println("   En el sistema real, aqui se abriria la chapa.");
  } else {
    explicarError(resultado);
  }
}

// ----------------------------------------------------------------------------
// 3. Borrar una huella
// ----------------------------------------------------------------------------
void borrarHuella() {
  Serial.println();
  Serial.print("Que ID quieres borrar? (1 a 162): ");
  int id = leerNumero();

  if (id < 1 || id > 162) {
    Serial.println();
    Serial.println("ID fuera de rango.");
    return;
  }
  Serial.println(id);

  uint8_t resultado = lector.deleteModel(id);
  if (resultado == FINGERPRINT_OK) {
    Serial.printf(">> Huella del ID %d borrada.\n", id);
  } else {
    explicarError(resultado);
  }
}

// ----------------------------------------------------------------------------
// 4. Contar huellas
// ----------------------------------------------------------------------------
void contarHuellas() {
  lector.getTemplateCount();
  Serial.println();
  Serial.printf(">> Huellas guardadas en el sensor: %d de 162\n",
                lector.templateCount);
}

// ----------------------------------------------------------------------------
// 5. Borrar todas
// ----------------------------------------------------------------------------
void borrarTodas() {
  Serial.println();
  Serial.println("Esto borra TODAS las huellas y no se puede deshacer.");
  Serial.print("Escribe 1234 para confirmar: ");
  int confirmacion = leerNumero();
  Serial.println(confirmacion);

  if (confirmacion != 1234) {
    Serial.println(">> Cancelado.");
    return;
  }

  uint8_t resultado = lector.emptyDatabase();
  if (resultado == FINGERPRINT_OK) {
    Serial.println(">> Base de datos vaciada.");
  } else {
    explicarError(resultado);
  }
}

// ----------------------------------------------------------------------------
// setup
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("###############################################");
  Serial.println("#   PRUEBA DEL SENSOR DE HUELLA AS608         #");
  Serial.println("###############################################");
  Serial.printf("Pines: RX del ESP32 = GPIO%d | TX del ESP32 = GPIO%d\n",
                PIN_HUELLA_RX, PIN_HUELLA_TX);

  // 57600 baudios es la velocidad por defecto del AS608
  serialHuella.begin(57600, SERIAL_8N1, PIN_HUELLA_RX, PIN_HUELLA_TX);
  delay(100);
  lector.begin(57600);
  delay(500);

  if (lector.verifyPassword()) {
    Serial.println("\n>> Sensor detectado correctamente.");
    lector.getParameters();
    Serial.printf("   Capacidad: %d huellas\n", lector.capacity);
    Serial.printf("   Nivel de seguridad: %d\n", lector.security_level);
    contarHuellas();
  } else {
    Serial.println("\n>> NO se encontro el sensor.");
    Serial.println("   Revisa en este orden:");
    Serial.println("   1. Que TX del sensor vaya a GPIO19 y RX a GPIO23");
    Serial.println("      (van CRUZADOS, es el error mas comun)");
    Serial.println("   2. Que V+ tenga 3.3V y el GND este unido al del ESP32");
    Serial.println("   3. Que el cable este bien insertado en el conector");
    while (true) {
      delay(1000);  // Sin sensor no hay nada que hacer
    }
  }

  mostrarMenu();
}

// ----------------------------------------------------------------------------
// loop
// ----------------------------------------------------------------------------
void loop() {
  if (!Serial.available()) {
    delay(100);
    return;
  }

  int opcion = Serial.parseInt();
  while (Serial.available()) {
    Serial.read();
  }

  switch (opcion) {
    case 1: enrolarHuella();  break;
    case 2: verificarHuella(); break;
    case 3: borrarHuella();   break;
    case 4: contarHuellas();  break;
    case 5: borrarTodas();    break;
    default:
      if (opcion != 0) {
        Serial.println("\nOpcion no valida.");
      }
      break;
  }

  mostrarMenu();
}
