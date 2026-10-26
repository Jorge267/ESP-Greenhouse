// ============================================================================
// PRUEBA DE PANTALLA OLED SSD1306 — para Arduino Mega
// ============================================================================
// Primero escanea el bus I2C para encontrar la direccion real de la pantalla
// (0x3C o 0x3D segun el fabricante) y luego corre una secuencia de pruebas
// visuales. Asi, si algo falla, se sabe si es de comunicacion o de la
// pantalla en si.
//
// LIBRERIAS NECESARIAS (Gestor de Librerias):
//   - "Adafruit SSD1306"
//   - "Adafruit GFX Library"
//
// CONEXION EN EL ARDUINO MEGA:
//   VCC -> 5V
//   GND -> GND
//   SDA -> pin 20
//   SCL -> pin 21
//
// IMPORTANTE: revisa el orden de los pines en la serigrafia de TU pantalla.
// Algunas vienen GND-VCC-SCL-SDA y otras VCC-GND-SCL-SDA.
//
// NOTA SOBRE EL CODIGO: el Arduino Mega NO soporta Serial.printf(), porque esa
// funcion es una extension propia del core del ESP32. Aqui se usa sprintf()
// sobre un buffer de texto y luego Serial.print(), que es la forma portable.
// ============================================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

constexpr uint8_t ANCHO_PANTALLA = 128;
constexpr uint8_t ALTO_PANTALLA  = 64;
constexpr int8_t  PIN_RESET      = -1;   // Esta pantalla no usa pin de reset

Adafruit_SSD1306 pantalla(ANCHO_PANTALLA, ALTO_PANTALLA, &Wire, PIN_RESET);

uint8_t direccionEncontrada = 0;
bool    pantallaLista = false;
uint32_t contador = 0;

// Buffer de uso general para armar los mensajes del Monitor Serial.
char lineaTexto[64];

// ----------------------------------------------------------------------------
// Escaner I2C: recorre todas las direcciones y reporta cuales responden
// ----------------------------------------------------------------------------
uint8_t escanearBusI2C() {
  uint8_t candidata = 0;
  uint8_t encontrados = 0;

  Serial.println();
  Serial.println("Escaneando el bus I2C...");

  for (uint8_t direccion = 1; direccion < 127; direccion++) {
    Wire.beginTransmission(direccion);
    if (Wire.endTransmission() == 0) {
      sprintf(lineaTexto, "  Dispositivo en 0x%02X", direccion);
      Serial.print(lineaTexto);
      if (direccion == 0x3C || direccion == 0x3D) {
        Serial.print("   <-- esta es la OLED");
        candidata = direccion;
      }
      Serial.println();
      encontrados++;
    }
  }

  if (encontrados == 0) {
    Serial.println("  NINGUN dispositivo respondio.");
    Serial.println("  Revisa en este orden:");
    Serial.println("   1. Que SDA este en el pin 20 y SCL en el 21");
    Serial.println("   2. Que VCC y GND no esten invertidos");
    Serial.println("   3. Que los cables hagan buen contacto");
  }
  return candidata;
}

// ----------------------------------------------------------------------------
// Pruebas visuales
// ----------------------------------------------------------------------------
void pruebaTexto() {
  pantalla.clearDisplay();
  pantalla.setTextColor(SSD1306_WHITE);

  pantalla.setTextSize(1);
  pantalla.setCursor(0, 0);
  pantalla.println(F("PRUEBA DE PANTALLA"));
  pantalla.println(F("--------------------"));
  pantalla.println(F("Tamano 1: texto"));
  pantalla.println(F("pequeno, 21 letras"));
  pantalla.println(F("por linea."));

  pantalla.setTextSize(2);
  pantalla.setCursor(0, 48);
  pantalla.println(F("Tamano 2"));

  pantalla.display();
  delay(3000);
}

void pruebaBordes() {
  // Dibuja un marco en todo el perimetro. Sirve para detectar columnas o
  // filas de pixeles muertos en los extremos.
  pantalla.clearDisplay();
  pantalla.drawRect(0, 0, ANCHO_PANTALLA, ALTO_PANTALLA, SSD1306_WHITE);
  pantalla.drawRect(2, 2, ANCHO_PANTALLA - 4, ALTO_PANTALLA - 4, SSD1306_WHITE);

  pantalla.setTextSize(1);
  pantalla.setCursor(20, 28);
  pantalla.println(F("MARCO COMPLETO"));

  pantalla.display();
  delay(3000);
}

void pruebaTodosLosPixeles() {
  // Enciende toda la pantalla. Cualquier punto negro es un pixel muerto.
  pantalla.clearDisplay();
  pantalla.fillRect(0, 0, ANCHO_PANTALLA, ALTO_PANTALLA, SSD1306_WHITE);
  pantalla.display();
  delay(2000);

  pantalla.clearDisplay();
  pantalla.display();
  delay(500);
}

void pruebaFiguras() {
  pantalla.clearDisplay();
  pantalla.drawCircle(32, 32, 20, SSD1306_WHITE);
  pantalla.fillCircle(96, 32, 20, SSD1306_WHITE);
  pantalla.drawLine(0, 0, 127, 63, SSD1306_WHITE);
  pantalla.drawLine(0, 63, 127, 0, SSD1306_WHITE);
  pantalla.display();
  delay(3000);
}

void pruebaInvertido() {
  pantalla.clearDisplay();
  pantalla.setTextSize(2);
  pantalla.setCursor(10, 25);
  pantalla.println(F("INVERSO"));
  pantalla.display();

  for (uint8_t i = 0; i < 6; i++) {
    pantalla.invertDisplay(true);
    delay(300);
    pantalla.invertDisplay(false);
    delay(300);
  }
}

// Simula lo que mostraria en el invernadero, para ver como se ve el texto
// real del proyecto.
void pruebaMensajesDelProyecto() {
  const char *estados[][2] = {
    {"SISTEMA LISTO",   "Coloque su dedo"},
    {"MODO ENROLAR",    "Nombre: Jorge"},
    {"ACCESO CONCEDIDO","Bienvenido Jorge"},
    {"ACCESO DENEGADO", "Huella no valida"}
  };

  for (uint8_t i = 0; i < 4; i++) {
    pantalla.clearDisplay();
    pantalla.setTextSize(1);
    pantalla.setCursor(0, 16);
    pantalla.println(estados[i][0]);
    pantalla.setCursor(0, 36);
    pantalla.println(estados[i][1]);
    pantalla.display();
    delay(2000);
  }
}

// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("###############################################");
  Serial.println("#      PRUEBA DE PANTALLA OLED SSD1306        #");
  Serial.println("###############################################");

  Wire.begin();

  direccionEncontrada = escanearBusI2C();

  if (direccionEncontrada == 0) {
    Serial.println();
    Serial.println("No se encontro la pantalla. Revisa el cableado y reinicia.");
    return;
  }

  Serial.println();
  sprintf(lineaTexto, "Iniciando la pantalla en 0x%02X...", direccionEncontrada);
  Serial.println(lineaTexto);

  if (!pantalla.begin(SSD1306_SWITCHCAPVCC, direccionEncontrada)) {
    Serial.println("FALLO: el bus la ve pero la pantalla no arranca.");
    Serial.println("Puede ser un controlador distinto al SSD1306.");
    return;
  }

  pantallaLista = true;
  Serial.println("Pantalla iniciada correctamente.");
  Serial.println();
  Serial.println("Observa la pantalla. La secuencia de pruebas es:");
  Serial.println("  1. Logo de Adafruit (viene en el buffer por defecto)");
  Serial.println("  2. Texto en dos tamanos");
  Serial.println("  3. Marco completo (busca pixeles muertos en los bordes)");
  Serial.println("  4. Pantalla totalmente encendida (busca puntos negros)");
  Serial.println("  5. Figuras geometricas");
  Serial.println("  6. Inversion de colores");
  Serial.println("  7. Mensajes reales del invernadero");
  Serial.println();

  // El logo de Adafruit ya viene cargado en el buffer tras begin()
  pantalla.display();
  delay(2000);
}

void loop() {
  if (!pantallaLista) {
    Serial.println("Pantalla no disponible. Revisa el cableado y reinicia.");
    delay(5000);
    return;
  }

  contador++;
  Serial.print(F("--- Ciclo de pruebas numero "));
  Serial.print(contador);
  Serial.println(F(" ---"));

  pruebaTexto();
  pruebaBordes();
  pruebaTodosLosPixeles();
  pruebaFiguras();
  pruebaInvertido();
  pruebaMensajesDelProyecto();

  // Pantalla de resumen entre ciclos
  pantalla.clearDisplay();
  pantalla.setTextSize(1);
  pantalla.setCursor(0, 20);
  pantalla.println(F("Ciclos completados:"));
  pantalla.setTextSize(3);
  pantalla.setCursor(45, 35);
  pantalla.println(contador);
  pantalla.display();
  delay(3000);
}
