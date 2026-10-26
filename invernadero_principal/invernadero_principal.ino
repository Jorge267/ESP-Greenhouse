// ============================================================================
// INVERNADERO INTELIGENTE — Programa principal
// ============================================================================
// Dispositivos en la app: LED de Prueba, Ventilador, Bomba de Agua,
//   Luz de Cultivo, Chapa de Acceso, Clima, Calidad de Aire, Seguridad,
//   Control de Acceso (enrolar / borrar huellas en remoto).
//
// CAMBIOS DE ESTA VERSION:
//  1. Se agrego el lector de huella AS608 (UART1: GPIO19 RX / GPIO23 TX).
//  2. Se agrego la chapa solenoide 12V en el canal 2 del rele (GPIO18).
//  3. El enrolamiento es NO BLOQUEANTE (maquina de estados). Si fuera
//     bloqueante, el loop se detendria y la app dejaria de responder, que es
//     justo el problema que costo tanto encontrar antes.
//  4. Bloqueo anti fuerza bruta: 5 intentos fallidos -> 60 s sin leer.
//  0. El modulo de rele es ACTIVO EN BAJO. Toda escritura pasa por
//     escribirRele(), que invierte el nivel. Con la logica directa la chapa
//     quedaba energizada de forma permanente y se sobrecalentaba.
//  5. Alarma disuasiva: con "Alarma Armada" encendida, el PIR hace parpadear
//     la luz de cultivo y sonar el buzzer. Mientras parpadea, el control por
//     lux se suspende y la luz vuelve despues a su estado anterior.
//  6. Pantalla OLED SSD1306 en la puerta: indica si el sistema esta en modo
//     DETECCION o ENROLAMIENTO, muestra los intentos fallidos, el bloqueo del
//     lector, el resultado de cada lectura y el clima.
//  7. El PIR va DIRECTO al GPIO39, sin pasar por el conversor de nivel. Su
//     salida ya es de 3.3V (el chip BISS0001 trabaja a 3.3V aunque el modulo
//     se alimente con 5V), y el pull-up de 10k del conversor peleaba contra
//     esa salida dejando la linea en un nivel intermedio que no se leia.
//  8. La alarma ya NO pide movimiento sostenido. Ahora cuenta eventos: 3
//     detecciones separadas dentro de 1 minuto. Asi el ajuste del
//     potenciometro Tx del PIR deja de alterar el comportamiento del
//     programa, que era el acoplamiento hardware-software del punto 7.
//  9. El riego lo decide el FC-28 (humedad del SUELO), no el DHT11. El DHT11
//     mide humedad del AIRE y se conserva solo para el ventilador.
// 10. HC-SR04 en el tanque: proteccion contra marcha en seco. La bomba no
//     arranca por debajo del nivel minimo, venga la orden del automatico o
//     de la app.
// 11. Una sola funcion decide el ventilador. Antes clima y gases mandaban por
//     separado sobre el mismo actuador y se contradecian.
// 12. Notificaciones push para lo que exige ir al invernadero, con espera por
//     tipo y separacion global para no agotar la cuota del plan gratuito.
//
// LIBRERIAS:
//   - "DHT sensor library" de Adafruit (+ "Adafruit Unified Sensor")
//   - "BH1750" de Christopher Laws
//   - "Adafruit Fingerprint Sensor Library"
//   - "Adafruit SSD1306"  +  "Adafruit GFX Library"
//
// CABLEADO NUEVO:
//   AS608:  V+ -> 3V3 | GND -> GND comun | TX -> GPIO19 | RX -> GPIO23
//   Chapa:  rele IN2 -> GPIO18
//           12V(+) -> COM | NO -> Chapa(+) | Chapa(-) -> GND de 12V
//           Diodo 1N4007 en paralelo a la chapa, banda hacia el (+)
//   PIR:    VCC -> 5V | GND -> GND comun | OUT -> GPIO39 (DIRECTO)
//   BUZZER: dos opciones validas, elegir UNA (ver nota junto a PIN_BUZZER):
//           A) + -> 5V  | - -> GND comun | S -> GPIO5 VIA CONVERSOR
//              (lado LV al GPIO, lado HV al modulo).  <- recomendada
//           B) + -> 3V3 | - -> GND comun | S -> GPIO5 DIRECTO
//           Lo que NO funciona: + a 5V con S directo de 3.3V.
//   FC-28:  VCC -> GPIO17 (se energiza solo al medir) | GND -> GND comun
//           AO  -> GPIO32 | DO -> sin conectar
//           OJO: alimentar con 3.3V, NO con 5V (su AO es analogica)
//   HC-SR04: VCC -> 5V | GND -> GND comun
//           TRIG -> GPIO16 (directo, su umbral esta cerca de 2V)
//           ECHO -> GPIO36 (SVP) PASANDO POR EL CONVERSOR: entrega 5V
//           Montarlo en la tapa del tanque mirando hacia abajo
//   OLED:   VCC -> 5V | GND -> GND comun
//           SDA y SCL al MISMO par de nodos del lado de 5V del conversor
//           donde ya esta el BH1750. No gasta canales nuevos ni carga el
//           riel de 3.3V del ESP32.
// ============================================================================

#include <Arduino.h>
#include <stdarg.h>
#include "RMaker.h"
#include "WiFi.h"
#include "WiFiProv.h"
#include <esp_rmaker_utils.h>   // esp_rmaker_raise_alert() para las push
#include <esp_task_wdt.h>       // Perro guardian: ver nota en setup()
#include <DHT.h>
#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_Fingerprint.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

// ----------------------------------------------------------------------------
// Red y aprovisionamiento
// ----------------------------------------------------------------------------
const char *NOMBRE_SERVICIO_PROV = "PROV_INV";
const char *CLAVE_POP = "abcd1234";

// ----------------------------------------------------------------------------
// Pines
// ----------------------------------------------------------------------------
#if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6
constexpr uint8_t PIN_BOTON_CONFIG = 9;
#else
constexpr uint8_t PIN_BOTON_CONFIG = 0;   // BOOT
#endif

constexpr uint8_t PIN_LED_PRUEBA  = 2;    // LED integrado de la placa
constexpr uint8_t PIN_RELE_LUZ    = 4;    // Rele canal 1
constexpr uint8_t PIN_RELE_CHAPA  = 18;   // Rele canal 2 (chapa solenoide)

// BUZZER PASIVO con modulo de disparo en BAJO, alimentado a 5V.
//
// Su señal SI pasa por el conversor de nivel, y el motivo es el inverso del
// que me hizo descartarlo al principio.
//
// El transistor del modulo es PNP con el emisor en los 5V de alimentacion, y
// conduce segun la diferencia base-emisor:
//
//     S en 0 V    ->  0 - 5 = -5.0 V   ->  conduce a fondo
//     S en 3.3 V  ->  3.3 - 5 = -1.7 V ->  SIGUE CONDUCIENDO
//
// Para cortarlo, la base necesita llegar cerca de 4.3 V. Los 3.3 V de un GPIO
// del ESP32 no alcanzan, asi que el transistor nunca cortaba: quedaba
// conduciendo a medias de forma permanente. De ahi que el buzzer se calentara
// en reposo con CUALQUIER nivel, y que el tono saliera flojo -la bobina nunca
// veia la conmutacion completa, solo un rizado sobre corriente continua.
//
// La clave es que el nivel ALTO IGUALE la tension del emisor. Hay dos formas
// de conseguirlo, y las dos se verificaron sonando fuerte en el Arduino Mega:
//
//   A) modulo a 5V  + señal por el CONVERSOR  -> alto = 5.0V = emisor
//   B) modulo a 3V3 + señal DIRECTA al GPIO   -> alto = 3.3V = emisor
//
// Lo que NO funciona es mezclarlas: modulo a 5V con señal directa de 3.3V,
// que era precisamente el montaje original.
//
// Se recomienda (A). Con (B) los 77 mA de la bobina saldrian del regulador de
// 3.3V del ESP32, que en plena alarma ya esta entregando los picos de WiFi
// mas el lector de huella; con (A) esa corriente la da la fuente externa y el
// GPIO solo maneja la base. Ademas a 5V la bobina recibe 116 mA en vez de 77.
//
// Para el conversor este caso es ideal, por la razon inversa a la que me hizo
// descartarlo al principio:
//   para SONAR  -> transmite el cero de forma activa con su MOSFET
//   para CALLAR -> su pull-up de 10k lleva la base a 5V, y en corte un PNP
//                  no consume corriente, asi que esos 10k bastan de sobra
//
// Es justo lo contrario de un buzzer ACTIVO en alto, que necesitaria que el
// nivel alto entregase corriente: ahi el pull-up de 10k si se quedaria corto.
//
// EL CODIGO ES IDENTICO para (A) y (B): en ambos el reposo es ALTO. La
// eleccion es solo de cableado.
constexpr uint8_t PIN_BUZZER = 5;

// HAY DOS TIPOS DE BUZZER Y NO SE CONECTAN IGUAL:
//
//   ACTIVO  -> trae su propio oscilador dentro. Se le da un nivel alto fijo
//              y suena. Es el que asumia el codigo original.
//   PASIVO  -> es solo un transductor, sin oscilador. Hay que entregarle la
//              frecuencia nosotros. Si se le da un nivel alto fijo NO SUENA y
//              su bobina queda como una resistencia pura: consume mucha
//              corriente y SE CALIENTA. Ese es el sintoma clasico.
//
// CONFIRMADO PASIVO: con la polaridad ya corregida seguia calentandose y solo
// emitia un sonido raro. Eso solo lo hace un transductor sin oscilador: los
// 300 ms de encendido de la alarma le llegan como corriente continua, que no
// produce tono y si disipa calor. Ahora el tono lo genera el LEDC.
constexpr bool     BUZZER_ES_PASIVO      = true;
constexpr uint32_t FRECUENCIA_BUZZER_HZ  = 2300;   // Cerca del pico de un piezo
constexpr uint8_t  RESOLUCION_PWM_BUZZER = 10;

// CANAL 2, no el 1, y el motivo importa.
//
// En el core de Arduino para ESP32 el temporizador se asigna como
// canal / 2, asi que los canales 0 y 1 COMPARTEN el temporizador 0. La bomba
// usa el canal 0 a 5000 Hz. Si el buzzer usara el canal 1, al llamar
// ledcWriteTone() para sonar a 2300 Hz cambiaria la frecuencia del
// temporizador compartido, y con ella la del PWM de la bomba.
// El canal 2 cae en el temporizador 1 y queda aislado.
constexpr uint8_t  CANAL_PWM_BUZZER      = 2;

// La prueba manual del buzzer se apaga sola. Sin este limite, dejar el
// interruptor encendido en la app lo mantiene energizado indefinidamente, y
// un buzzer con corriente continua se calienta.
constexpr uint32_t DURACION_PRUEBA_BUZZER_MS = 5000;   // 5 s

// Y ADEMAS hay modulos que disparan con nivel BAJO (low level trigger).
//
// Este es el mismo tipo de error que sobrecalento la chapa solenoide: el
// codigo escribia LOW creyendo que apagaba, pero LOW es justo lo que ACTIVA
// el modulo. Resultado: el buzzer quedaba energizado el 100 % del tiempo
// desde el instante en que arrancaba el programa, disipando todo en calor.
//
// true  -> el modulo suena con nivel BAJO (low level trigger)
// false -> el modulo suena con nivel ALTO
// Solo aplica si BUZZER_ES_PASIVO es false.
//
// Se corrigio a false por evidencia: en reposo con el pin en ALTO el buzzer
// se calentaba y zumbaba, asi que ALTO es su estado activo, no el de reposo.
// Lo que diga la etiqueta "low level trigger" del modulo no coincide con como
// quedo cableado el tuyo.
constexpr bool BUZZER_ACTIVO_EN_BAJO = false;

// NIVEL DE REPOSO PARA EL BUZZER PASIVO.
//
// En un buzzer pasivo el silencio no depende de "disparar o no", sino de que
// NO pase corriente continua por la bobina. Y cual es ese nivel depende del
// cableado del modulo, que no se puede deducir desde el codigo:
//
//   modulo con transistor y disparo en bajo  -> reposo en ALTO  (true)
//   buzzer conectado directo entre pin y GND -> reposo en BAJO  (false)
//
// REPOSO EN ALTO, con la señal pasando por el conversor.
//
// Se probo en BAJO y tambien calentaba, lo que descartaba un simple nivel
// invertido. La prueba decisiva fue desconectar solo el cable S dejando + y -:
// el buzzer quedo FRIO, asi que el transistor no estaba en corto y el calor
// dependia del pin. La unica explicacion compatible con las dos cosas es que
// 3.3 V no cortan el PNP del modulo (ver la nota junto a PIN_BUZZER).
//
// Con el conversor en medio, el nivel alto del GPIO se convierte en 5 V
// reales en la base y el transistor si corta.
constexpr bool BUZZER_PASIVO_REPOSO_ALTO = true;

// Barrido de frecuencias al arrancar. Ya no hace falta: la prueba en el
// Arduino Mega, con logica de 5V, sono fuerte en TODAS las frecuencias del
// barrido. Eso descarto la resonancia como causa del volumen bajo y dejo al
// nivel de tension como unico culpable. Se deja el modo disponible por si
// algun dia se cambia el buzzer por otro.
constexpr bool MODO_BARRIDO_BUZZER = false;

// Este modulo de rele es ACTIVO EN BAJO: el contacto se cierra cuando el pin
// IN esta en LOW, no en HIGH. Con la logica directa la chapa quedaba
// energizada de forma permanente y se sobrecalentaba.
// Si algun dia se cambia por un modulo activo en alto, basta poner false.
constexpr bool RELE_ACTIVO_EN_BAJO = true;

// Traduce "quiero activar la carga" al nivel real que necesita el modulo.
inline void escribirRele(uint8_t pin, bool activar) {
  digitalWrite(pin, (activar != RELE_ACTIVO_EN_BAJO) ? HIGH : LOW);
}

// Unico punto por el que se maneja el buzzer, para que el tipo de buzzer sea
// un detalle de una sola linea y no haya que tocar la logica de la alarma.
inline void escribirBuzzer(bool sonar) {
  if (BUZZER_ES_PASIVO) {
    if (sonar) {
      // ledcWriteTone genera la onda cuadrada por hardware.
      ledcWriteTone(CANAL_PWM_BUZZER, FRECUENCIA_BUZZER_HZ);
    } else {
      // Para callar hay que dejar el pin en un nivel FIJO, el que no haga
      // circular corriente continua por la bobina. Cual es, lo decide
      // BUZZER_PASIVO_REPOSO_ALTO segun como este cableado el modulo.
      ledcWrite(CANAL_PWM_BUZZER,
                BUZZER_PASIVO_REPOSO_ALTO ? ((1 << RESOLUCION_PWM_BUZZER) - 1) : 0);
    }
  } else {
    // Misma traduccion que escribirRele(): arriba se pide la intencion y
    // aqui se convierte al nivel real que espera el modulo.
    digitalWrite(PIN_BUZZER, (sonar != BUZZER_ACTIVO_EN_BAJO) ? HIGH : LOW);
  }
}

// BOMBA DE AGUA — canal con PWM del L298N
constexpr uint8_t PIN_BOMBA_ENA = 33;     // ENA (PWM, sin jumper)
constexpr uint8_t PIN_BOMBA_IN1 = 25;
constexpr uint8_t PIN_BOMBA_IN2 = 26;

// VENTILADOR — canal sin PWM, siempre a maxima potencia (jumper ENB puesto)
constexpr uint8_t PIN_VENT_IN3 = 27;
constexpr uint8_t PIN_VENT_IN4 = 14;

constexpr uint8_t PIN_DHT11 = 13;
constexpr uint8_t PIN_MQ135 = 34;         // ADC1
constexpr uint8_t PIN_MQ7   = 35;         // ADC1

// FC-28 — humedad del SUELO. Es el sensor que ahora decide el riego.
//
// AO es analogica y debe quedar por debajo de 3.3V, asi que el modulo se
// alimenta con 3.3V, NO con 5V. A 5V su salida llegaria a 5V y pondria en
// riesgo el pin.
//
// La sonda es resistiva: si se le deja corriente continua todo el tiempo, se
// oxida en semanas. Por eso su VCC cuelga de un GPIO y solo se energiza los
// milisegundos que dura la medicion, una vez cada 20 s.
constexpr uint8_t PIN_SUELO_AO  = 32;     // ADC1, entrada analogica
constexpr uint8_t PIN_SUELO_VCC = 17;     // Alimenta la sonda solo al medir

// Si tu modulo trae un LED muy brillante y el GPIO no alcanza a alimentarlo,
// pon esto en false y conecta VCC del FC-28 directo al riel de 3V3.
constexpr bool ALIMENTAR_SUELO_POR_GPIO = true;

// HC-SR04 — nivel del tanque de agua, para no hacer trabajar la bomba en seco.
// TRIG acepta 3.3V directo (su umbral esta cerca de 2V).
// ECHO entrega 5V y SI debe pasar por el conversor de nivel.
constexpr uint8_t PIN_ULTRA_TRIG = 16;    // Salida, directa
constexpr uint8_t PIN_ULTRA_ECHO = 36;    // Entrada (SVP), via conversor

// PIR: conexion DIRECTA, sin conversor de nivel (su salida ya es de 3.3V).
// GPIO39 es solo de entrada y no tiene resistencias internas, asi que el
// cable debe estar firme: un contacto flojo deja el pin flotando.
constexpr uint8_t PIN_PIR   = 39;

// BUS I2C compartido: BH1750 (GY-30) en 0x23 y pantalla OLED en 0x3C.
// Ambos van del lado de 5V del conversor de nivel, sobre el mismo par de
// nodos SDA/SCL. Las direcciones son distintas, asi que no hay conflicto.
constexpr uint8_t PIN_I2C_SDA = 21;
constexpr uint8_t PIN_I2C_SCL = 22;

// AS608 por UART1 (van CRUZADOS: TX del sensor al RX del ESP32)
constexpr uint8_t PIN_HUELLA_RX = 19;
constexpr uint8_t PIN_HUELLA_TX = 23;

// ----------------------------------------------------------------------------
// Umbrales (ajustar en pruebas de campo)
// ----------------------------------------------------------------------------
constexpr float TEMP_VENTILADOR_ON_C  = 30.0f;
constexpr float TEMP_VENTILADOR_OFF_C = 27.0f;

// La humedad del aire tambien necesita histeresis. Antes encendia por encima
// de 85 % y apagaba por debajo de 85 %: con el valor oscilando alrededor del
// umbral el ventilador se prendia y apagaba sin parar.
constexpr float HUMEDAD_VENTILADOR_ON  = 85.0f;
constexpr float HUMEDAD_VENTILADOR_OFF = 78.0f;

// --- Riego por humedad del SUELO (FC-28) ---
// CALIBRACION MEDIDA EN SITIO con el modo de calibracion:
//   sonda totalmente seca      -> crudo 4095  (ADC saturado)
//   sonda sumergida en agua    -> crudo 2535
//
// Fijate en lo estrecho del rango: 1560 cuentas de las 4095 posibles. Eso es
// normal en una sonda resistiva alimentada a 3.3 V, y es la razon por la que
// NO se pueden usar valores "de catalogo": con la escala anterior (3200..1300)
// una sonda seca daba 0 % pero una tierra apenas humeda ya marcaba 42 %, y el
// riego nunca llegaba a pedirse.
constexpr int SUELO_CRUDO_SECO   = 4095;   // MEDIDO: sonda al aire, seca
constexpr int SUELO_CRUDO_HUMEDO = 2535;   // MEDIDO: sonda sumergida

constexpr float SUELO_BOMBA_ON  = 35.0f;   // Riega por debajo de este %
constexpr float SUELO_BOMBA_OFF = 60.0f;   // Corta al alcanzar este %

// --- Riego por PULSOS ---
// Regar de corrido hasta alcanzar el umbral siempre se pasa de largo, porque
// el agua tarda en bajar por la tierra y llegar a la sonda. Mientras no llega,
// el sensor sigue marcando seco y la bomba sigue echando agua: cuando la
// lectura finalmente sube, ya hay mucha mas agua en el suelo de la necesaria.
//
// Por eso el riego va en pulsos: se echa un poco, se ESPERA a que el agua se
// reparta, se vuelve a medir y se decide. Ese tiempo de espera es la parte
// imprescindible; sin el, los pulsos solo trocean el mismo exceso de agua.
constexpr uint32_t DURACION_PULSO_RIEGO_MS = 1000;   // 1 s de bomba por pulso
constexpr uint32_t TIEMPO_ABSORCION_MS     = 10000;  // 10 s para que baje

// Tope de seguridad. 20 pulsos son 20 s de bomba repartidos en unos 3.7 min.
// Si tras eso la humedad no subio, el agua no esta llegando a la tierra:
// manguera desconectada, goteo tapado o la sonda enterrada en otro sitio.
constexpr uint8_t MAX_PULSOS_POR_CICLO = 20;

// Espera obligatoria antes de volver a intentar una tanda de riego.
//
// Sin esto el loop entraba en bucle: si el tanque rechazaba el primer pulso,
// el estado volvia a INACTIVO y en la vuelta siguiente -10 ms despues- se
// cumplian otra vez todas las condiciones de arranque. El resultado era el
// encabezado de "INICIO DE RIEGO" repetido cientos de veces por segundo.
constexpr uint32_t ESPERA_REINTENTO_RIEGO_MS = 60000;   // 1 min

// MODO CALIBRACION DEL FC-28. Ya se uso: dio 4095 en seco y 2535 en agua, y
// esos valores estan aplicados arriba. Se apaga para que el log no imprima
// una linea cada 2 segundos. Ponlo en true si hay que recalibrar.
constexpr bool MODO_CALIBRAR_SUELO = false;
constexpr uint32_t INTERVALO_CALIBRACION_MS = 2000;

// --- Nivel del tanque (HC-SR04) ---
// El sensor se monta en la tapa, mirando hacia abajo. Mide la distancia hasta
// la superficie del agua: a MAYOR distancia, MENOS agua.
//
// CALIBRACION HECHA EN SITIO:
// Con el tanque VACIO el sensor midio 27.3 cm, estable (tres muestras de
// 1588 us identicas). Aqui se pone 28.0 y no 27.3 a proposito: ese margen de
// 0.7 cm absorbe el ruido de medida. Sin el, una lectura de 27.5 cm daria
// porcentaje negativo y el log se llenaria de avisos de calibracion en cada
// ciclo, aunque el comportamiento fuera correcto.
constexpr float DIST_TANQUE_VACIO_CM  = 28.0f;   // MEDIDO con el tanque vacio

// PENDIENTE DE MEDIR AL LLENAR EL TANQUE.
// Este valor solo fija la ESCALA: cuantos centimetros de agua representan el
// 100 %. NO influye en el bloqueo por tanque vacio, que ya queda resuelto con
// la constante de arriba. Mide del sensor a la superficie con el tanque lleno
// y ajustalo. Deja al menos 5 cm de aire: el HC-SR04 tiene una zona muerta de
// 2 cm y por debajo de eso no mide nada.
constexpr float DIST_TANQUE_LLENO_CM  = 6.0f;    // SUPUESTO, ajustar al llenar
constexpr float NIVEL_TANQUE_MINIMO   = 15.0f;   // % por debajo del cual no se riega

constexpr float DIST_MAXIMA_VALIDA_CM = 400.0f;  // Alcance del HC-SR04
constexpr uint8_t MUESTRAS_ULTRASONIDO = 3;      // Se toma la mediana

// INTERRUPTOR DE PRUEBAS. En true (normal) la bomba no arranca si el tanque
// esta bajo. Ponlo en false SOLO para probar en mesa, cuando el HC-SR04 no
// esta montado en el tanque y mide la distancia al techo o al piso: en ese
// caso el nivel calcula 0 % y la bomba queda bloqueada para siempre.
// Con false, el nivel se sigue midiendo y registrando, pero no bloquea.
constexpr bool PROTECCION_TANQUE_ACTIVA = true;

constexpr float LUX_LUZ_ON  = 300.0f;
constexpr float LUX_LUZ_OFF = 500.0f;

constexpr int UMBRAL_AIRE_MALO_RAW     = 2000;
constexpr int UMBRAL_CO_EMERGENCIA_RAW = 2500;

// --- Regla de alarma por conteo de detecciones ---
// Un intruso que entra se QUEDA dentro, no solo pasa. Asi que en vez de pedir
// un movimiento sostenido, se cuentan eventos separados en el tiempo: hacen
// falta 3 detecciones dentro de una ventana de 1 minuto.
//
// Por que esto quita la dependencia del hardware: la regla anterior exigia
// que el pin siguiera en alto 3 segundos continuos, y cuanto tiempo sigue en
// alto lo decide el potenciometro Tx del HC-SR501, no el programa. Contando
// eventos, el ajuste de ese potenciometro ya no cambia el comportamiento.
constexpr uint32_t SEPARACION_MINIMA_DETECCION_MS = 15000;  // 15 s entre conteos
constexpr uint8_t  DETECCIONES_PARA_ALARMA        = 3;
constexpr uint32_t VENTANA_DETECCIONES_MS         = 60000;  // 1 min

// El HC-SR501 necesita de 30 a 60 segundos tras energizarse para estabilizar
// su referencia termica. Durante ese lapso su salida OSCILA sola, sin que haya
// movimiento: en el log se ve como cambios de nivel varias veces por segundo.
// Esas oscilaciones no deben contarse, porque tres de ellas dispararian una
// alarma falsa cada vez que se reinicia el sistema.
constexpr uint32_t PIR_CALENTAMIENTO_MS = 60000;   // 60 s

// El PIR cambia de nivel muchas veces por minuto y cada cambio generaba una
// linea. Eso enterraba las lecturas de clima, suelo y tanque, que son las que
// interesan ahora. En false solo se registran las detecciones CONTADAS y el
// disparo de la alarma. Ponlo en true cuando toque depurar el sensor.
constexpr bool LOG_PIR_DETALLADO = false;

constexpr uint32_t TIEMPO_RIEGO_MAXIMO_MS     = 180000UL;  // 3 min

// Alarma disuasiva: con la alarma armada, el PIR hace parpadear la luz de
// cultivo para ahuyentar al intruso. El parpadeo se extiende mientras siga
// habiendo movimiento y se apaga sola pasado este tiempo sin detecciones.
constexpr uint32_t DURACION_ALARMA_MS    = 30000;  // 30 s desde la ultima deteccion
constexpr uint32_t INTERVALO_PARPADEO_MS = 300;    // Medio ciclo del parpadeo

// Control de acceso
constexpr uint32_t TIEMPO_CHAPA_ABIERTA_MS = 4000;    // Cierre automatico
constexpr uint8_t  INTENTOS_FALLIDOS_MAX   = 5;
constexpr uint32_t BLOQUEO_LECTOR_MS       = 60000;   // 60 s
constexpr uint32_t TIMEOUT_ENROLAMIENTO_MS = 30000;   // 30 s para poner el dedo

constexpr uint32_t INTERVALO_CLIMA_MS     = 20000;
constexpr uint32_t INTERVALO_AIRE_MS      = 15000;
constexpr uint32_t INTERVALO_SEGURIDAD_MS = 200;
constexpr uint32_t INTERVALO_HUELLA_MS    = 300;

// El tanque va en su propio intervalo, desfasado del clima, para que las dos
// medidas lentas (el DHT11 tarda ~250 ms y el ultrasonido ~200 ms) no caigan
// en la misma vuelta del loop y retrasen de golpe la lectura de la huella.
constexpr uint32_t INTERVALO_TANQUE_MS    = 30000;

// --- Notificaciones push ---
// RainMaker en su plan gratuito limita las alertas, asi que cada tipo tiene
// su propio tiempo de espera y ademas hay una separacion global para que
// nunca salgan dos push seguidas.
constexpr uint32_t SEPARACION_GLOBAL_ALERTAS_MS = 60000UL;      // 1 min

// Pantalla: enviar el buffer completo de 1 KB por I2C a 100 kHz toma unos
// 90 ms, asi que NO se refresca en cada vuelta del loop. En reposo basta un
// refresco lento; cuando hay una cuenta regresiva en pantalla (enrolamiento,
// bloqueo, puerta abierta) se refresca una vez por segundo.
constexpr uint32_t INTERVALO_PANTALLA_REPOSO_MS = 5000;
constexpr uint32_t INTERVALO_PANTALLA_ACTIVA_MS = 1000;
constexpr uint32_t DURACION_MENSAJE_OLED_MS     = 4000;

// PWM de la bomba
constexpr uint32_t FRECUENCIA_PWM = 5000;
constexpr uint8_t  RESOLUCION_PWM = 8;
constexpr uint8_t  CANAL_PWM = 0;
constexpr uint8_t  PORCENTAJE_BOMBA = 75;
constexpr uint8_t  DUTY_CYCLE_BOMBA = (255 * PORCENTAJE_BOMBA) / 100;

// Boton
constexpr uint32_t TIEMPO_RESET_WIFI_MS    = 3000;
constexpr uint32_t TIEMPO_RESET_FABRICA_MS = 10000;

// ----------------------------------------------------------------------------
// Objetos y estado
// ----------------------------------------------------------------------------
DHT dht11(PIN_DHT11, DHT11);
BH1750 sensorLuz;
HardwareSerial serialHuella(1);
Adafruit_Fingerprint lector(&serialHuella);

// Pantalla de la puerta. El -1 indica que el modulo no tiene pin de reset.
constexpr uint8_t OLED_ANCHO     = 128;
constexpr uint8_t OLED_ALTO      = 64;
constexpr uint8_t OLED_DIRECCION = 0x3C;
Adafruit_SSD1306 pantalla(OLED_ANCHO, OLED_ALTO, &Wire, -1);

bool  bh1750Disponible = false;
bool  lectorDisponible = false;
bool  oledDisponible   = false;
float luxActual = 0.0f;

bool estadoLedPrueba   = false;
bool estadoVentilador  = false;
bool estadoBomba       = false;
bool estadoLuzCultivo  = false;
bool estadoChapa       = false;

float temperaturaActual     = 0.0f;
float humedadAmbienteActual = 0.0f;
bool  dht11Valido = false;

int mq135Crudo = 0;
int mq7Crudo   = 0;

// --- Humedad del suelo (FC-28) ---
int   sueloCrudo      = 0;
float humedadSuelo    = 0.0f;    // 0 a 100 %
bool  sueloValido     = false;

// --- Maquina de estados del riego por pulsos ---
// No bloquea en ningun momento: se consulta en cada vuelta del loop y solo
// mira si ya toca cambiar de etapa.
enum EstadoRiego {
  RIEGO_INACTIVO,      // Nada que hacer
  RIEGO_PULSO,         // Bomba encendida, contando el pulso
  RIEGO_ABSORBIENDO    // Bomba apagada, esperando que el agua baje
};
EstadoRiego estadoRiego               = RIEGO_INACTIVO;
uint32_t    tiempoCambioRiego         = 0;
uint8_t     pulsosAplicados           = 0;
float       humedadAlIniciarRiego     = 0.0f;
uint32_t    tiempoProximoIntentoRiego = 0;   // Freno contra el reintento inmediato

// --- Nivel del tanque (HC-SR04) ---
float distanciaTanqueCm = 0.0f;
float nivelTanque       = 0.0f;  // 0 a 100 %
bool  tanqueValido      = false; // false = el sensor no respondio
bool  tanqueConAgua     = true;  // Permiso para que la bomba arranque

bool     movimientoDetectado    = false;
uint32_t tiempoInicioRiego      = 0;
uint32_t tiempoAperturaChapa    = 0;

// --- Conteo de detecciones del PIR ---
// Guarda las marcas de tiempo de las ultimas detecciones contadas. La alarma
// se dispara cuando el arreglo esta lleno y la diferencia entre la mas nueva
// y la mas vieja cabe dentro de la ventana.
uint32_t marcasDeteccion[DETECCIONES_PARA_ALARMA] = {0};
uint8_t  deteccionesEnVentana  = 0;
uint32_t ultimaDeteccionContada = 0;

// --- Alarma disuasiva ---
bool     alarmaArmada        = false;  // Lo enciende el usuario desde la app
bool     alarmaSonando       = false;  // La luz esta parpadeando ahora mismo
uint32_t tiempoFinAlarma     = 0;
uint32_t ultimoParpadeo      = 0;
bool     parpadeoEncendido   = false;
bool     luzAntesDeAlarma    = false;  // Estado al que se vuelve al terminar
uint32_t finPruebaBuzzer     = 0;      // 0 = no hay prueba en curso

// --- Control de acceso ---
// El AS608 solo guarda plantillas numeradas (1 a 162); no tiene espacio para
// nombres. La relacion nombre <-> ID se guarda aqui, en la memoria NVS del
// ESP32, y sobrevive a reinicios y a nuevas cargas de sketch.
constexpr uint8_t MAX_PERSONAS      = 20;
constexpr uint8_t LARGO_NOMBRE      = 16;   // 15 caracteres + terminador

Preferences almacenHuellas;
char    nombresPersonas[MAX_PERSONAS][LARGO_NOMBRE];
uint8_t idsPersonas[MAX_PERSONAS];
uint8_t totalPersonas = 0;

char     nombreSeleccionado[LARGO_NOMBRE] = "";  // Lo escribe el usuario
char     ultimoNombreReconocido[LARGO_NOMBRE] = "-";
int      huellasGuardadas     = 0;
uint8_t  intentosFallidos     = 0;
uint32_t finBloqueoLector     = 0;

// Texto de estado que se publica en la app para que el usuario sepa que esta
// pasando sin tener que mirar el monitor serial ni la pantalla de la puerta.
char     estadoAcceso[48] = "Iniciando";

// RainMaker RESERVA el tipo "esp.param.name" para el nombre del dispositivo.
// Antes los campos de texto usaban ese tipo y la app los trataba como si
// fueran el renombrado del dispositivo: por eso el campo de nombre no se
// comportaba bien. Un tipo propio evita la colision y la app lo dibuja como
// un campo de texto normal gracias a addUIType(ESP_RMAKER_UI_TEXT).
constexpr const char *TIPO_TEXTO = "custom.param.texto";

// --- Solicitudes pendientes desde la app ---
// El callback de RainMaker corre en SU PROPIA tarea, no en el loop. Hacer ahi
// el trabajo del sensor tenia dos problemas:
//   1. Si el usuario escribia el nombre y activaba "Enrolar" en el mismo
//      envio, no hay garantia de en que orden llegan los dos parametros. Si
//      "Enrolar" llegaba primero, el nombre todavia estaba vacio y el
//      enrolamiento se rechazaba. Eso explica que a veces funcionara y a
//      veces no.
//   2. Hablar con el AS608 desde otra tarea mientras el loop tambien lo
//      consulta puede entremezclar tramas en el UART.
// Ahora el callback SOLO anota la intencion y el loop la ejecuta en la vuelta
// siguiente, cuando ya llegaron todos los parametros del mismo envio.
enum SolicitudAcceso {
  SOLICITUD_NINGUNA,
  SOLICITUD_ENROLAR,
  SOLICITUD_BORRAR,
  SOLICITUD_BORRAR_TODAS,
  SOLICITUD_CANCELAR
};
volatile SolicitudAcceso solicitudPendiente = SOLICITUD_NINGUNA;

// Maquina de estados del enrolamiento (NO bloqueante)
enum EstadoEnrolamiento {
  ENROL_INACTIVO,
  ENROL_ESPERA_DEDO_1,
  ENROL_ESPERA_RETIRO,
  ENROL_ESPERA_DEDO_2
};
EstadoEnrolamiento estadoEnrolamiento = ENROL_INACTIVO;
uint32_t tiempoInicioEnrolamiento = 0;
int      idEnEnrolamiento = 0;
char     nombreEnEnrolamiento[LARGO_NOMBRE] = "";

// --- Pantalla ---
uint32_t ultimoRefrescoPantalla = 0;
bool     pantallaRefrescoPedido = true;

// Mensaje temporal que tapa la vista de reposo (acceso concedido, denegado...)
// 22 = 21 caracteres + terminador. Es justo lo que cabe en una linea de la
// pantalla con la fuente de 6 px, asi que un texto largo se recorta en vez
// de saltar de renglon y descuadrar la vista.
char     mensajeTitulo[22] = "";
char     mensajeLinea1[22] = "";
// La segunda linea se imprime en tamano 1 dejando que la libreria salte de
// renglon sola, asi que admite textos mas largos (dos lineas de 21).
char     mensajeLinea2[40] = "";
bool     mensajeResaltado  = false;
uint32_t mensajeExpira     = 0;

// --- Notificaciones push ---
// Un tiempo de espera por tipo, para que un problema que persiste no genere
// una push cada ciclo de lectura.
enum TipoAlerta {
  ALERTA_INTRUSO,
  ALERTA_CO,
  ALERTA_TANQUE,
  ALERTA_RIEGO_FALLIDO,
  ALERTA_ACCESO_BLOQUEADO,
  ALERTA_APERTURA_REMOTA,
  TOTAL_TIPOS_ALERTA
};

const uint32_t esperaPorAlerta[TOTAL_TIPOS_ALERTA] = {
   600000UL,   // ALERTA_INTRUSO ........... 10 min
   900000UL,   // ALERTA_CO ................ 15 min
  3600000UL,   // ALERTA_TANQUE ............  1 h
  7200000UL,   // ALERTA_RIEGO_FALLIDO .....  2 h
  1800000UL,   // ALERTA_ACCESO_BLOQUEADO .. 30 min
    60000UL    // ALERTA_APERTURA_REMOTA ...  1 min
};

// El perro guardian del ESP32. Si el loop se cuelga, reinicia el chip.
//
// Importa para la chapa: revisarCierreChapa() es lo que la vuelve a bloquear
// a los 4 s, y esa funcion corre DENTRO del loop. Si el loop se cuelga con la
// chapa abierta, se queda abierta indefinidamente. Con el perro guardian, un
// cuelgue se convierte en un reinicio, y al reiniciar la chapa queda bloqueada.
//
// 30 s es holgado a proposito: la vuelta mas lenta del loop (clima, tanque y
// pantalla juntos) no pasa de 1.5 s, asi que no hay riesgo de reinicios
// espurios, y sigue detectando un cuelgue real en medio minuto.
constexpr uint32_t TIMEOUT_WATCHDOG_S = 30;

uint32_t ultimoEnvioAlerta[TOTAL_TIPOS_ALERTA] = {0};
uint32_t ultimoEnvioGlobal = 0;
uint32_t alertasEnviadas   = 0;

uint32_t ultimaLecturaClima      = 0;
uint32_t ultimaLecturaAire       = 0;
uint32_t ultimaLecturaTanque     = 0;
uint32_t ultimaRevisionSeguridad = 0;
uint32_t ultimaRevisionHuella    = 0;

static Device *dispositivoLed        = nullptr;
static Device *dispositivoVentilador = nullptr;
static Device *dispositivoBomba      = nullptr;
static Device *dispositivoLuz        = nullptr;
static Device *dispositivoChapa      = nullptr;
static Device *dispositivoClima      = nullptr;
static Device *dispositivoAire       = nullptr;
static Device *dispositivoSeguridad  = nullptr;
static Device *dispositivoAcceso     = nullptr;

// ----------------------------------------------------------------------------
// Registro en consola con formato uniforme:  [hh:mm:ss]  CATEGORIA  | mensaje
// ----------------------------------------------------------------------------
void registrar(const char *categoria, const char *formato, ...) {
  uint32_t segundosTotales = millis() / 1000;
  uint8_t  horas    = (segundosTotales / 3600) % 24;
  uint8_t  minutos  = (segundosTotales / 60) % 60;
  uint8_t  segundos = segundosTotales % 60;

  char mensaje[160];
  va_list argumentos;
  va_start(argumentos, formato);
  vsnprintf(mensaje, sizeof(mensaje), formato, argumentos);
  va_end(argumentos);

  Serial.printf("[%02u:%02u:%02u]  %-12s | %s\n",
                horas, minutos, segundos, categoria, mensaje);
}

void registrarSeparador(const char *titulo) {
  Serial.println();
  Serial.printf("============ %s ============\n", titulo);
}

// ----------------------------------------------------------------------------
// NOTIFICACIONES PUSH
// ----------------------------------------------------------------------------
// esp_rmaker_raise_alert() manda un aviso al telefono. Solo se usa para lo
// que obliga a ir al invernadero: no sirve de nada avisar de algo que el
// sistema ya corrigio por su cuenta.
//
// Hay dos frenos encadenados:
//   1. Una espera propia por cada tipo de alerta (tabla esperaPorAlerta).
//   2. Una separacion global de 1 minuto, para que dos problemas simultaneos
//      no manden dos push a la vez.
//
// Sin esto, un tanque vacio generaria una push cada 30 segundos y en una
// noche se agotaria la cuota del plan gratuito.
// ----------------------------------------------------------------------------
// OJO con la firma: el parametro es uint8_t y NO TipoAlerta, aunque se llame
// siempre con un valor del enum.
//
// El motivo es el preprocesador del IDE de Arduino. Antes de compilar, genera
// automaticamente los prototipos de todas las funciones del .ino y los inserta
// justo despues de los #include, es decir ARRIBA de las declaraciones de tipos
// del usuario. Con la firma original el prototipo generado quedaba asi:
//
//     bool notificar(TipoAlerta tipo, const char *formato, ...);   <- insertado
//     ...
//     enum TipoAlerta { ... };                                     <- linea 421
//
// y el compilador veia TipoAlerta antes de que existiera. De ahi el error
// "'TipoAlerta' was not declared in this scope" apuntando a la definicion.
//
// Usar uint8_t evita el problema de raiz: el enum se convierte solo a entero
// al llamar, asi que notificar(ALERTA_TANQUE, ...) sigue funcionando igual.
// Por eso tampoco fallaron EstadoEnrolamiento ni SolicitudAcceso: ninguno de
// los dos aparece en la FIRMA de una funcion, solo dentro de los cuerpos.
bool notificar(uint8_t tipo, const char *formato, ...) {
  // Al perder el tipo fuerte, el indice se valida a mano antes de usarlo.
  if (tipo >= TOTAL_TIPOS_ALERTA) {
    registrar("ALERTA", "Tipo de alerta invalido (%u), no se envia", tipo);
    return false;
  }

  char texto[96];
  va_list argumentos;
  va_start(argumentos, formato);
  vsnprintf(texto, sizeof(texto), formato, argumentos);
  va_end(argumentos);

  uint32_t ahora = millis();

  // La primera alerta tras encender se permite: ultimoEnvioAlerta arranca en
  // 0 y la resta da un numero grande.
  if (ultimoEnvioAlerta[tipo] != 0 &&
      (ahora - ultimoEnvioAlerta[tipo]) < esperaPorAlerta[tipo]) {
    registrar("ALERTA", "(en espera) %s", texto);
    return false;
  }
  if (ultimoEnvioGlobal != 0 &&
      (ahora - ultimoEnvioGlobal) < SEPARACION_GLOBAL_ALERTAS_MS) {
    registrar("ALERTA", "(separacion global) %s", texto);
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    registrar("ALERTA", "(sin red, no se envia) %s", texto);
    return false;
  }

  ultimoEnvioAlerta[tipo] = ahora;
  ultimoEnvioGlobal       = ahora;
  alertasEnviadas++;

  esp_rmaker_raise_alert(texto);
  registrar("ALERTA", ">> PUSH #%lu enviada: %s",
            (unsigned long)alertasEnviadas, texto);
  return true;
}

// ----------------------------------------------------------------------------
// REPORTE DE PARAMETROS CON HISTORICO (time series)
// ----------------------------------------------------------------------------
// RainMaker guarda el historico asociando cada dato a una fecha y hora, asi
// que no puede aceptar nada hasta que el reloj del ESP32 se sincronice por
// NTP. Eso tarda entre 10 y 30 segundos despues de conectarse a la red.
//
// Si se reporta antes, cada parametro con PROP_FLAG_TIME_SERIES suelta un
// error como este:
//
//   E esp_rmaker_param: Current time not yet available.
//                       Cannot report time series data.
//
// Son siete parametros con historico, asi que salian siete errores de golpe
// en cada ciclo de lectura hasta que la hora llegaba. No rompia nada, pero
// tapaba el resto del log justo en los primeros segundos, que es cuando mas
// se necesita leerlo. Estas dos funciones esperan a que haya hora.
// ----------------------------------------------------------------------------

bool horaDisponible() {
  // Una vez sincronizada, ya no se vuelve a preguntar.
  static bool listo = false;
  if (listo) return true;

  listo = esp_rmaker_time_check();
  if (listo) {
    registrar("SISTEMA", "Hora sincronizada: desde ahora se guarda el historico");
  }
  return listo;
}

void reportarSerie(Device *dispositivo, const char *parametro, float valor) {
  if (!dispositivo) return;
  if (!horaDisponible()) return;
  dispositivo->updateAndReportParam(parametro, valor);
}

void reportarSerieEntero(Device *dispositivo, const char *parametro, int valor) {
  if (!dispositivo) return;
  if (!horaDisponible()) return;
  dispositivo->updateAndReportParam(parametro, valor);
}

// ----------------------------------------------------------------------------
// PANTALLA OLED DE LA PUERTA
// ----------------------------------------------------------------------------
// La pantalla no guarda estado propio: en cada refresco se decide que mostrar
// a partir de las variables que ya maneja el sistema. Asi no hay dos verdades
// que se puedan desincronizar.
//
// Prioridad de las vistas, de mayor a menor:
//   1. Mensaje temporal (acceso concedido / denegado / huella guardada)
//   2. Alarma sonando
//   3. Lector bloqueado por intentos fallidos
//   4. Enrolamiento en curso
//   5. Puerta abierta
//   6. Reposo (modo deteccion)
// ----------------------------------------------------------------------------

inline void pedirRefrescoPantalla() { pantallaRefrescoPedido = true; }

// Muestra un aviso que tapa la vista de reposo durante unos segundos.
void mostrarMensajeTemporal(const char *titulo, const char *linea1,
                            const char *linea2, bool resaltado) {
  snprintf(mensajeTitulo, sizeof(mensajeTitulo), "%s", titulo ? titulo : "");
  snprintf(mensajeLinea1, sizeof(mensajeLinea1), "%s", linea1 ? linea1 : "");
  snprintf(mensajeLinea2, sizeof(mensajeLinea2), "%s", linea2 ? linea2 : "");
  mensajeResaltado = resaltado;
  mensajeExpira    = millis() + DURACION_MENSAJE_OLED_MS;
  pedirRefrescoPantalla();
}

// Escribe un texto centrado horizontalmente. La fuente por defecto mide
// 6x8 px por caracter en tamano 1, asi que el ancho escala con el tamano.
void escribirCentrado(const char *texto, uint8_t tamano, int16_t y) {
  int16_t anchoTexto = (int16_t)strlen(texto) * 6 * tamano;
  int16_t x = (OLED_ANCHO - anchoTexto) / 2;
  if (x < 0) x = 0;
  pantalla.setTextSize(tamano);
  pantalla.setCursor(x, y);
  pantalla.print(texto);
}

// Barra superior con el nombre de la vista. Resaltada = fondo blanco con
// letras negras, para que se distinga de un vistazo a varios metros.
void dibujarEncabezado(const char *texto, bool resaltado) {
  if (resaltado) {
    pantalla.fillRect(0, 0, OLED_ANCHO, 13, SSD1306_WHITE);
    pantalla.setTextColor(SSD1306_BLACK);
  } else {
    pantalla.drawFastHLine(0, 12, OLED_ANCHO, SSD1306_WHITE);
    pantalla.setTextColor(SSD1306_WHITE);
  }
  escribirCentrado(texto, 1, 3);
  pantalla.setTextColor(SSD1306_WHITE);
}

// --- Vista 6: reposo / modo deteccion ---------------------------------------
void dibujarReposo() {
  dibujarEncabezado("MODO DETECCION", false);

  pantalla.setTextSize(1);
  escribirCentrado("Coloque su dedo", 1, 20);

  char linea[24];

  // Intentos fallidos y conteo de movimiento: los dos indicadores de
  // seguridad que interesa ver de pie frente a la puerta.
  snprintf(linea, sizeof(linea), "Fallos %u/%u   Mov %u/%u",
           intentosFallidos, INTENTOS_FALLIDOS_MAX,
           deteccionesEnVentana, DETECCIONES_PARA_ALARMA);
  pantalla.setCursor(2, 33);
  pantalla.print(linea);

  // Suelo y tanque: lo que decide si el riego puede funcionar.
  if (sueloValido && tanqueValido) {
    snprintf(linea, sizeof(linea), "Suelo %2.0f%%  Tanq %2.0f%%",
             humedadSuelo, nivelTanque);
  } else if (sueloValido) {
    snprintf(linea, sizeof(linea), "Suelo %2.0f%%  Tanq --", humedadSuelo);
  } else if (tanqueValido) {
    snprintf(linea, sizeof(linea), "Suelo --   Tanq %2.0f%%", nivelTanque);
  } else {
    snprintf(linea, sizeof(linea), "Riego sin sensores");
  }
  pantalla.setCursor(2, 43);
  pantalla.print(linea);

  // Pie con el clima y el estado de la red
  pantalla.drawFastHLine(0, 54, OLED_ANCHO, SSD1306_WHITE);
  if (dht11Valido) {
    snprintf(linea, sizeof(linea), "%.0fC %.0f%% %.0flx %s",
             temperaturaActual, humedadAmbienteActual, luxActual,
             WiFi.status() == WL_CONNECTED ? "ON" : "--");
  } else {
    snprintf(linea, sizeof(linea), "Clima sin lectura  %s",
             WiFi.status() == WL_CONNECTED ? "ON" : "--");
  }
  pantalla.setCursor(2, 56);
  pantalla.print(linea);
}

// --- Vista 4: enrolamiento en curso -----------------------------------------
void dibujarEnrolamiento() {
  dibujarEncabezado("MODO ENROLAR", true);

  char linea[24];
  pantalla.setTextSize(1);

  snprintf(linea, sizeof(linea), "Nombre: %s", nombreEnEnrolamiento);
  pantalla.setCursor(2, 18);
  pantalla.print(linea);

  const char *paso  = "";
  const char *orden = "";
  switch (estadoEnrolamiento) {
    case ENROL_ESPERA_DEDO_1:
      paso  = "Paso 1 de 2";
      orden = "Coloque el dedo";
      break;
    case ENROL_ESPERA_RETIRO:
      paso  = "Paso 1 completo";
      orden = "Retire el dedo";
      break;
    case ENROL_ESPERA_DEDO_2:
      paso  = "Paso 2 de 2";
      orden = "Repita el mismo dedo";
      break;
    default:
      paso  = "Procesando";
      orden = "Espere...";
      break;
  }

  pantalla.setCursor(2, 32);
  pantalla.print(paso);
  pantalla.setCursor(2, 42);
  pantalla.print(orden);

  // Cuenta regresiva del tiempo que queda para poner el dedo
  uint32_t transcurrido = millis() - tiempoInicioEnrolamiento;
  uint32_t restante = (transcurrido >= TIMEOUT_ENROLAMIENTO_MS)
                          ? 0
                          : (TIMEOUT_ENROLAMIENTO_MS - transcurrido) / 1000;
  pantalla.drawFastHLine(0, 54, OLED_ANCHO, SSD1306_WHITE);
  snprintf(linea, sizeof(linea), "Quedan %lu s", (unsigned long)restante);
  pantalla.setCursor(2, 56);
  pantalla.print(linea);
}

// --- Vista 3: lector bloqueado ----------------------------------------------
void dibujarBloqueo() {
  dibujarEncabezado("ACCESO BLOQUEADO", true);

  pantalla.setTextSize(1);
  escribirCentrado("Demasiados intentos", 1, 20);
  escribirCentrado("fallidos", 1, 30);

  uint32_t restante = (millis() >= finBloqueoLector)
                          ? 0
                          : (finBloqueoLector - millis()) / 1000;
  char linea[24];
  snprintf(linea, sizeof(linea), "Espere %lu s", (unsigned long)restante);
  escribirCentrado(linea, 2, 44);
}

// --- Vista 2: alarma sonando ------------------------------------------------
void dibujarAlarma() {
  // El recuadro acompana el parpadeo del buzzer y de la luz, para que la
  // pantalla tambien comunique urgencia.
  dibujarEncabezado("!! ALARMA !!", parpadeoEncendido);

  pantalla.setTextSize(1);
  escribirCentrado("MOVIMIENTO", 2, 24);
  escribirCentrado("DETECTADO", 2, 42);
}

// --- Vista 5: puerta abierta ------------------------------------------------
void dibujarPuertaAbierta() {
  dibujarEncabezado("PUERTA ABIERTA", true);

  pantalla.setTextSize(1);
  escribirCentrado("Puede pasar", 1, 22);

  uint32_t transcurrido = millis() - tiempoAperturaChapa;
  uint32_t restante = (transcurrido >= TIEMPO_CHAPA_ABIERTA_MS)
                          ? 0
                          : (TIEMPO_CHAPA_ABIERTA_MS - transcurrido) / 1000 + 1;
  char linea[24];
  snprintf(linea, sizeof(linea), "Cierra en %lu", (unsigned long)restante);
  escribirCentrado(linea, 2, 38);
}

// --- Vista 1: mensaje temporal ----------------------------------------------
void dibujarMensaje() {
  dibujarEncabezado(mensajeTitulo, mensajeResaltado);
  escribirCentrado(mensajeLinea1, 2, 22);
  // Sin centrar: se imprime desde el margen para que la libreria pueda
  // partir el texto en dos renglones si el motivo es largo.
  pantalla.setTextSize(1);
  pantalla.setCursor(2, 46);
  pantalla.print(mensajeLinea2);
}

// --- Vista de arranque ------------------------------------------------------
void dibujarBienvenida() {
  if (!oledDisponible) return;
  pantalla.clearDisplay();
  dibujarEncabezado("INVERNADERO", true);
  pantalla.setTextSize(1);
  escribirCentrado("Frutos de Guatemala", 1, 22);
  escribirCentrado("Iniciando...", 1, 38);
  escribirCentrado("Control de acceso", 1, 52);
  pantalla.display();
}

// Decide cual vista toca y la dibuja. Se llama solo desde revisarPantalla().
void refrescarPantalla() {
  pantalla.clearDisplay();
  pantalla.setTextColor(SSD1306_WHITE);

  if (millis() < mensajeExpira) {
    dibujarMensaje();
  } else if (alarmaSonando) {
    dibujarAlarma();
  } else if (millis() < finBloqueoLector) {
    dibujarBloqueo();
  } else if (estadoEnrolamiento != ENROL_INACTIVO) {
    dibujarEnrolamiento();
  } else if (estadoChapa) {
    dibujarPuertaAbierta();
  } else {
    dibujarReposo();
  }

  pantalla.display();
}

// Se llama en cada vuelta del loop, pero solo dibuja cuando toca. Las vistas
// con cuenta regresiva necesitan refresco cada segundo; en reposo no hay nada
// que cambie rapido, asi que se espacia para no gastar el bus I2C.
void revisarPantalla() {
  if (!oledDisponible) return;

  bool vistaConCuentaRegresiva = (millis() < mensajeExpira) || alarmaSonando ||
                                 (millis() < finBloqueoLector) ||
                                 (estadoEnrolamiento != ENROL_INACTIVO) ||
                                 estadoChapa;

  uint32_t intervalo = vistaConCuentaRegresiva ? INTERVALO_PANTALLA_ACTIVA_MS
                                               : INTERVALO_PANTALLA_REPOSO_MS;

  if (!pantallaRefrescoPedido && (millis() - ultimoRefrescoPantalla) < intervalo) {
    return;
  }

  pantallaRefrescoPedido = false;
  ultimoRefrescoPantalla = millis();
  refrescarPantalla();
}

// ----------------------------------------------------------------------------
// Salidas
// ----------------------------------------------------------------------------
void aplicarLedPrueba(bool encender) {
  estadoLedPrueba = encender;
  digitalWrite(PIN_LED_PRUEBA, encender ? HIGH : LOW);
  registrar("ACTUADOR", "LED de Prueba .... %s", encender ? "ENCENDIDO" : "APAGADO");
}

// La BOMBA usa el canal con PWM: sus IN quedan fijos en estados opuestos
// y el encendido/apagado se hace con el duty del ENA, asi nunca quedan
// ambos en HIGH ni ambos en LOW.
//
// Esta funcion es el UNICO punto por el que se enciende la bomba, y por eso
// la proteccion contra marcha en seco vive aqui: da igual si la orden viene
// del riego automatico o de un boton de la app, las dos pasan por este filtro.
// Una bomba centrifuga sin agua se calienta y destruye su sello en minutos.
//
// Devuelve false si la orden fue rechazada.
bool aplicarBomba(bool encender) {
  // El rechazo se anuncia UNA VEZ por episodio. Antes, mientras el suelo
  // siguiera seco, cada ciclo de clima volvia a pedir riego y repetia el
  // aviso cada 20 s: el log se llenaba y la pantalla de la puerta parpadeaba
  // "SIN AGUA" sin parar.
  static bool rechazoYaAvisado = false;

  if (encender && !tanqueConAgua) {
    if (!rechazoYaAvisado) {
      rechazoYaAvisado = true;
      registrar("SEGURIDAD", "Bomba BLOQUEADA: %s",
                tanqueValido ? "el tanque esta por debajo del minimo"
                             : "el sensor de nivel no responde");
      notificar(ALERTA_TANQUE,
                tanqueValido
                    ? "Tanque al %.0f%%: riego detenido para no quemar la bomba"
                    : "Sensor de nivel sin respuesta: riego detenido",
                nivelTanque);
      mostrarMensajeTemporal("SIN AGUA", "RIEGO OFF", "Revise el tanque", true);
    }
    return false;
  }

  // Al conseguir permiso otra vez, se rearma el aviso para el proximo episodio.
  rechazoYaAvisado = false;

  estadoBomba = encender;
  ledcWrite(CANAL_PWM, encender ? DUTY_CYCLE_BOMBA : 0);
  if (encender) tiempoInicioRiego = millis();
  registrar("ACTUADOR", "Bomba ............ %s",
            encender ? "ENCENDIDA (75% PWM)" : "APAGADA");
  return true;
}

void aplicarVentilador(bool encender) {
  estadoVentilador = encender;
  digitalWrite(PIN_VENT_IN3, encender ? HIGH : LOW);
  digitalWrite(PIN_VENT_IN4, LOW);
  registrar("ACTUADOR", "Ventilador ....... %s",
            encender ? "ENCENDIDO (100%)" : "APAGADO");
}

void aplicarLuzCultivo(bool encender) {
  estadoLuzCultivo = encender;
  escribirRele(PIN_RELE_LUZ, encender);
  registrar("ACTUADOR", "Luz de Cultivo ... %s",
            encender ? "ENCENDIDA" : "APAGADA");
}

// ----------------------------------------------------------------------------
// Alarma disuasiva: la luz de cultivo parpadea para ahuyentar al intruso
// ----------------------------------------------------------------------------
void iniciarAlarma() {
  // Cada deteccion extiende la alarma, pero el estado previo de la luz solo
  // se guarda la primera vez; si no, al re-detectar se guardaria un estado
  // del parpadeo y la luz quedaria mal al terminar.
  if (!alarmaSonando) {
    luzAntesDeAlarma = estadoLuzCultivo;
    alarmaSonando = true;
    parpadeoEncendido = false;
    ultimoParpadeo = millis();
    registrar("SEGURIDAD", "!! ALARMA ACTIVADA !! Luz parpadeando y buzzer sonando");
    pedirRefrescoPantalla();
  }
  tiempoFinAlarma = millis() + DURACION_ALARMA_MS;
}

void detenerAlarma(const char *motivo) {
  if (!alarmaSonando) return;
  alarmaSonando = false;
  registrar("SEGURIDAD", "Alarma terminada: %s", motivo);
  pedirRefrescoPantalla();

  escribirBuzzer(false);   // Siempre callar el buzzer al terminar

  // Devolver la luz al estado que tenia antes del parpadeo
  aplicarLuzCultivo(luzAntesDeAlarma);
  if (dispositivoLuz) {
    dispositivoLuz->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, estadoLuzCultivo);
  }
}

// Corta la prueba manual del buzzer pasado su tiempo, y devuelve el
// interruptor de la app a false para que no quede mintiendo.
void revisarPruebaBuzzer() {
  if (finPruebaBuzzer == 0) return;
  if (millis() < finPruebaBuzzer) return;

  finPruebaBuzzer = 0;

  // Si la alarma arranco durante la prueba, el buzzer le pertenece a ella:
  // no se toca, solo se apaga el interruptor de la app.
  if (!alarmaSonando) {
    escribirBuzzer(false);
  }
  registrar("SEGURIDAD", "Prueba de buzzer terminada por tiempo");

  if (dispositivoSeguridad) {
    dispositivoSeguridad->updateAndReportParam("Probar Buzzer", false);
  }
}

// Se llama en cada vuelta del loop. No bloquea: solo revisa si ya toca
// cambiar el estado del parpadeo.
void revisarAlarma() {
  if (!alarmaSonando) return;

  if (millis() > tiempoFinAlarma) {
    detenerAlarma("sin movimiento por 30 s");
    return;
  }

  if ((millis() - ultimoParpadeo) >= INTERVALO_PARPADEO_MS) {
    ultimoParpadeo = millis();
    parpadeoEncendido = !parpadeoEncendido;
    // Se escribe al rele sin pasar por aplicarLuzCultivo(), para no ensuciar
    // el log con un mensaje cada 300 ms ni alterar estadoLuzCultivo.
    escribirRele(PIN_RELE_LUZ, parpadeoEncendido);
    // El buzzer suena al mismo ritmo que la luz, en fase con ella.
    escribirBuzzer(parpadeoEncendido);
  }
}

// La chapa es fail-secure: cerrada por defecto, abre solo mientras el rele
// esta activo. El cierre automatico lo maneja revisarCierreChapa().
void aplicarChapa(bool abrir) {
  estadoChapa = abrir;
  escribirRele(PIN_RELE_CHAPA, abrir);
  if (abrir) tiempoAperturaChapa = millis();
  registrar("ACTUADOR", "Chapa ............ %s", abrir ? "ABIERTA" : "CERRADA");
  pedirRefrescoPantalla();
}

void reportarChapa() {
  if (dispositivoChapa) {
    dispositivoChapa->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, estadoChapa);
  }
}

void revisarCierreChapa() {
  if (!estadoChapa) return;
  if ((millis() - tiempoAperturaChapa) > TIEMPO_CHAPA_ABIERTA_MS) {
    aplicarChapa(false);
    reportarChapa();
  }
}

// ----------------------------------------------------------------------------
// Control de acceso — huella
// ----------------------------------------------------------------------------
// El AS608 solo guarda plantillas numeradas. La relacion nombre <-> ID vive
// en la NVS del ESP32, en una sola cadena con formato "nombre:id;nombre:id;"
// porque Preferences no permite recorrer claves con facilidad.
//
// Como los dos por separado pueden desincronizarse, hay reglas estrictas:
//   - El registro de nombres se carga SIEMPRE, aunque el sensor no responda.
//     Antes solo se cargaba si el lector estaba disponible, y entonces un
//     guardado posterior sobreescribia la NVS con una lista vacia: se perdian
//     todos los nombres.
//   - Un ID se considera libre solo si NO esta en el registro Y el sensor
//     confirma que la ranura esta vacia. Las huellas enroladas con el sketch
//     de prueba ocupan ranuras sin nombre; sin esta comprobacion el programa
//     las sobreescribia y quedaban huellas que abrian la puerta como
//     "desconocido".
// ----------------------------------------------------------------------------

// Publica el estado en la app y lo deja en el log. Es la unica via por la que
// se escribe estadoAcceso, para que app, consola y pantalla digan lo mismo.
void publicarEstadoAcceso(const char *formato, ...) {
  va_list argumentos;
  va_start(argumentos, formato);
  vsnprintf(estadoAcceso, sizeof(estadoAcceso), formato, argumentos);
  va_end(argumentos);

  registrar("ACCESO", "%s", estadoAcceso);
  if (dispositivoAcceso) {
    dispositivoAcceso->updateAndReportParam("Estado", estadoAcceso);
  }
}

// Limpia lo que escribio el usuario en la app: quita espacios de los extremos
// y descarta ':' y ';', que son los separadores del registro en NVS. Sin esto
// un nombre como "Jorge; Ana" corrompia el archivo entero al guardarlo.
// Devuelve false si no queda nada utilizable.
bool sanitizarNombre(const char *entrada, char *salida, size_t tamano) {
  salida[0] = '\0';
  if (entrada == nullptr) return false;

  // Saltar espacios iniciales
  while (*entrada == ' ' || *entrada == '\t') entrada++;

  size_t escritos = 0;
  for (const char *p = entrada; *p && escritos < tamano - 1; p++) {
    if (*p == ':' || *p == ';') continue;        // Separadores reservados
    // El cast a unsigned es necesario: en este compilador char es con signo,
    // asi que una letra acentuada (dos bytes en UTF-8, ambos por encima de
    // 127) daria negativo y se descartaria. "Jose" perderia la tilde... y
    // "Nuñez" se convertiria en "Nuez".
    if ((unsigned char)*p < 32) continue;        // Solo caracteres de control
    salida[escritos++] = *p;
  }
  salida[escritos] = '\0';

  // Quitar espacios finales
  while (escritos > 0 && (salida[escritos - 1] == ' ' || salida[escritos - 1] == '\t')) {
    salida[--escritos] = '\0';
  }

  return escritos > 0;
}

void guardarRegistroPersonas() {
  char cadena[MAX_PERSONAS * (LARGO_NOMBRE + 5)] = "";
  for (uint8_t i = 0; i < totalPersonas; i++) {
    char entrada[LARGO_NOMBRE + 6];
    snprintf(entrada, sizeof(entrada), "%s:%u;", nombresPersonas[i], idsPersonas[i]);
    strncat(cadena, entrada, sizeof(cadena) - strlen(cadena) - 1);
  }
  almacenHuellas.begin("huellas", false);
  almacenHuellas.putString("registro", cadena);
  almacenHuellas.end();
}

void cargarRegistroPersonas() {
  almacenHuellas.begin("huellas", true);
  String cadena = almacenHuellas.getString("registro", "");
  almacenHuellas.end();

  totalPersonas = 0;
  int inicio = 0;
  while (inicio < (int)cadena.length() && totalPersonas < MAX_PERSONAS) {
    int separador = cadena.indexOf(':', inicio);
    int fin       = cadena.indexOf(';', inicio);
    if (separador < 0 || fin < 0) break;

    String nombre = cadena.substring(inicio, separador);
    int    id     = cadena.substring(separador + 1, fin).toInt();

    strncpy(nombresPersonas[totalPersonas], nombre.c_str(), LARGO_NOMBRE - 1);
    nombresPersonas[totalPersonas][LARGO_NOMBRE - 1] = '\0';
    idsPersonas[totalPersonas] = (uint8_t)id;
    totalPersonas++;

    inicio = fin + 1;
  }
}

// Devuelve la posicion en el arreglo, o -1 si no existe. No distingue
// mayusculas, para que "Jorge" y "jorge" sean la misma persona.
int buscarPersonaPorNombre(const char *nombre) {
  for (uint8_t i = 0; i < totalPersonas; i++) {
    if (strcasecmp(nombresPersonas[i], nombre) == 0) return i;
  }
  return -1;
}

// Primer ID realmente libre: ni en el registro de nombres ni en el sensor.
//
// La doble comprobacion es la correccion importante de este refactor. Antes
// solo se miraba el registro local, asi que si el sensor ya tenia plantillas
// sin nombre -por ejemplo las que se enrolaron con el sketch de prueba, donde
// el ID se escribia a mano- storeModel() las SOBREESCRIBIA en silencio. El
// resultado era una huella que abria la puerta pero sin nombre asociado, y un
// dedo registrado que de pronto dejaba de funcionar.
int buscarIdLibre() {
  for (uint8_t candidato = 1; candidato <= 162; candidato++) {
    bool enRegistro = false;
    for (uint8_t i = 0; i < totalPersonas; i++) {
      if (idsPersonas[i] == candidato) { enRegistro = true; break; }
    }
    if (enRegistro) continue;

    // loadModel() devuelve OK solo si esa ranura tiene una plantilla. Es
    // seguro llamarlo aqui porque todavia no hemos capturado nada.
    if (lectorDisponible && lector.loadModel(candidato) == FINGERPRINT_OK) {
      continue;   // Ocupada en el sensor aunque no tenga nombre
    }
    return candidato;
  }
  return -1;
}

void construirListaPersonas(char *destino, size_t tamano) {
  destino[0] = '\0';
  if (totalPersonas == 0) {
    strncpy(destino, "(vacio)", tamano - 1);
    destino[tamano - 1] = '\0';
    return;
  }
  for (uint8_t i = 0; i < totalPersonas; i++) {
    char entrada[LARGO_NOMBRE + 8];
    snprintf(entrada, sizeof(entrada), "%s%s", i ? ", " : "", nombresPersonas[i]);
    strncat(destino, entrada, tamano - strlen(destino) - 1);
  }
}

void reportarEstadoAcceso() {
  if (!dispositivoAcceso) return;
  char lista[MAX_PERSONAS * (LARGO_NOMBRE + 2)];
  construirListaPersonas(lista, sizeof(lista));
  dispositivoAcceso->updateAndReportParam("Ultimo Acceso", ultimoNombreReconocido);
  dispositivoAcceso->updateAndReportParam("Registradas", lista);
  dispositivoAcceso->updateAndReportParam("Huellas Guardadas", huellasGuardadas);
  dispositivoAcceso->updateAndReportParam("Estado", estadoAcceso);
}

void actualizarConteoHuellas() {
  if (!lectorDisponible) return;
  if (lector.getTemplateCount() == FINGERPRINT_OK) {
    huellasGuardadas = lector.templateCount;
  }
}

// Borra TODO: las plantillas del sensor y el registro de nombres. Es la salida
// cuando el sensor y el registro quedaron desincronizados y no vale la pena
// adivinar que plantilla corresponde a quien.
void borrarTodasLasHuellas() {
  if (!lectorDisponible) {
    publicarEstadoAcceso("Sin lector: no se puede borrar");
    return;
  }

  if (lector.emptyDatabase() != FINGERPRINT_OK) {
    publicarEstadoAcceso("El sensor no pudo vaciar su memoria");
    return;
  }

  totalPersonas = 0;
  guardarRegistroPersonas();
  strncpy(ultimoNombreReconocido, "-", LARGO_NOMBRE - 1);
  intentosFallidos = 0;
  actualizarConteoHuellas();

  publicarEstadoAcceso("Memoria vaciada. Vuelva a enrolar a todos.");
  mostrarMensajeTemporal("REGISTRO BORRADO", "VACIO", "Vuelva a enrolar", true);
  reportarEstadoAcceso();
}

// Al arrancar, compara cuantas plantillas tiene el sensor con cuantos nombres
// hay en el registro. Si no coinciden, el sistema puede conceder acceso a un
// dedo sin nombre, asi que conviene avisarlo de forma visible en vez de
// dejarlo pasar.
void verificarConsistenciaRegistro() {
  if (!lectorDisponible) return;

  if (huellasGuardadas == totalPersonas) {
    publicarEstadoAcceso("Listo. %u persona(s) registrada(s)", totalPersonas);
    return;
  }

  registrar("ACCESO", "AVISO: el sensor tiene %d plantillas y el registro %u nombres",
            huellasGuardadas, totalPersonas);
  registrar("ACCESO", "Las plantillas sin nombre abriran como \"desconocido\".");
  registrar("ACCESO", "Use \"Borrar Todas\" en la app y vuelva a enrolar.");
  publicarEstadoAcceso("Descuadre: %d huellas / %u nombres",
                       huellasGuardadas, totalPersonas);
}

// Apaga el interruptor de enrolar en la app. Se usa en todas las salidas por
// error, para que el control no quede encendido sugiriendo que sigue activo.
void apagarInterruptorEnrolar() {
  if (dispositivoAcceso) {
    dispositivoAcceso->updateAndReportParam("Modo Enrolar", false);
  }
}

void cancelarEnrolamiento(const char *motivo) {
  estadoEnrolamiento = ENROL_INACTIVO;
  idEnEnrolamiento = 0;
  nombreEnEnrolamiento[0] = '\0';
  publicarEstadoAcceso("Enrolamiento cancelado: %s", motivo);
  mostrarMensajeTemporal("ENROLAR", "CANCELADO", motivo, true);
  apagarInterruptorEnrolar();
}

void iniciarEnrolamiento(const char *nombreCrudo) {
  // Se valida en el mismo orden en que el usuario se puede equivocar, y cada
  // rechazo deja un texto en la app: antes estos errores solo salian por el
  // monitor serial, asi que desde el telefono parecia que no pasaba nada.
  char nombre[LARGO_NOMBRE];

  if (!lectorDisponible) {
    publicarEstadoAcceso("Sin lector de huella conectado");
    apagarInterruptorEnrolar();
    return;
  }
  if (!sanitizarNombre(nombreCrudo, nombre, sizeof(nombre))) {
    publicarEstadoAcceso("Escriba primero un nombre valido");
    apagarInterruptorEnrolar();
    return;
  }
  if (buscarPersonaPorNombre(nombre) >= 0) {
    publicarEstadoAcceso("\"%s\" ya existe. Borrelo primero.", nombre);
    apagarInterruptorEnrolar();
    return;
  }
  if (totalPersonas >= MAX_PERSONAS) {
    publicarEstadoAcceso("Registro lleno (maximo %u)", MAX_PERSONAS);
    apagarInterruptorEnrolar();
    return;
  }
  if (estadoEnrolamiento != ENROL_INACTIVO) {
    publicarEstadoAcceso("Ya hay un enrolamiento en curso");
    return;
  }

  int id = buscarIdLibre();
  if (id < 0) {
    publicarEstadoAcceso("No hay ranuras libres en el sensor");
    apagarInterruptorEnrolar();
    return;
  }

  idEnEnrolamiento = id;
  strncpy(nombreEnEnrolamiento, nombre, LARGO_NOMBRE - 1);
  nombreEnEnrolamiento[LARGO_NOMBRE - 1] = '\0';
  estadoEnrolamiento = ENROL_ESPERA_DEDO_1;
  tiempoInicioEnrolamiento = millis();
  publicarEstadoAcceso("Enrolando a %s (ID %d): coloque el dedo",
                       nombreEnEnrolamiento, id);

  // Se limpia cualquier mensaje temporal pendiente para que la vista de
  // enrolamiento aparezca de inmediato y no quede tapada unos segundos.
  mensajeExpira = 0;
  pedirRefrescoPantalla();
}

void borrarHuella(const char *nombreCrudo) {
  char nombre[LARGO_NOMBRE];

  if (!lectorDisponible) {
    publicarEstadoAcceso("Sin lector de huella conectado");
    return;
  }
  if (!sanitizarNombre(nombreCrudo, nombre, sizeof(nombre))) {
    publicarEstadoAcceso("Escriba el nombre que quiere borrar");
    return;
  }

  int posicion = buscarPersonaPorNombre(nombre);
  if (posicion < 0) {
    publicarEstadoAcceso("No hay nadie llamado \"%s\"", nombre);
    return;
  }

  uint8_t id = idsPersonas[posicion];
  if (lector.deleteModel(id) != FINGERPRINT_OK) {
    publicarEstadoAcceso("El sensor no pudo borrar el ID %u", id);
    return;
  }

  // Quitar del registro desplazando el resto una posicion.
  // Se usa el nombre tal como estaba guardado, no el que escribio el usuario,
  // porque la busqueda no distingue mayusculas.
  char nombreGuardado[LARGO_NOMBRE];
  strncpy(nombreGuardado, nombresPersonas[posicion], LARGO_NOMBRE - 1);
  nombreGuardado[LARGO_NOMBRE - 1] = '\0';

  for (uint8_t i = posicion; i + 1 < totalPersonas; i++) {
    strncpy(nombresPersonas[i], nombresPersonas[i + 1], LARGO_NOMBRE - 1);
    nombresPersonas[i][LARGO_NOMBRE - 1] = '\0';
    idsPersonas[i] = idsPersonas[i + 1];
  }
  totalPersonas--;
  guardarRegistroPersonas();

  // Si la persona borrada era el ultimo acceso mostrado, se limpia para no
  // seguir anunciando a alguien que ya no tiene permiso.
  if (strcasecmp(ultimoNombreReconocido, nombreGuardado) == 0) {
    strncpy(ultimoNombreReconocido, "-", LARGO_NOMBRE - 1);
  }

  publicarEstadoAcceso("Acceso de %s revocado (ID %u)", nombreGuardado, id);
  mostrarMensajeTemporal("ACCESO REVOCADO", "BORRADA", nombreGuardado, true);
  actualizarConteoHuellas();
  reportarEstadoAcceso();
}

// Avanza la maquina de estados del enrolamiento. Se llama desde el loop y
// nunca espera: hace una sola consulta al sensor y devuelve el control.
void avanzarEnrolamiento() {
  if ((millis() - tiempoInicioEnrolamiento) > TIMEOUT_ENROLAMIENTO_MS) {
    cancelarEnrolamiento("tiempo agotado");
    return;
  }

  uint8_t resultado;

  switch (estadoEnrolamiento) {
    case ENROL_ESPERA_DEDO_1:
      if (lector.getImage() != FINGERPRINT_OK) return;
      if (lector.image2Tz(1) != FINGERPRINT_OK) {
        cancelarEnrolamiento("imagen no utilizable, reintente");
        return;
      }
      estadoEnrolamiento = ENROL_ESPERA_RETIRO;
      publicarEstadoAcceso("%s: paso 1 listo, retire el dedo", nombreEnEnrolamiento);
      pedirRefrescoPantalla();   // La pantalla pasa al paso siguiente
      break;

    case ENROL_ESPERA_RETIRO:
      if (lector.getImage() != FINGERPRINT_NOFINGER) return;
      estadoEnrolamiento = ENROL_ESPERA_DEDO_2;
      publicarEstadoAcceso("%s: repita el MISMO dedo", nombreEnEnrolamiento);
      pedirRefrescoPantalla();
      break;

    case ENROL_ESPERA_DEDO_2:
      if (lector.getImage() != FINGERPRINT_OK) return;
      if (lector.image2Tz(2) != FINGERPRINT_OK) {
        cancelarEnrolamiento("segunda imagen no utilizable");
        return;
      }
      resultado = lector.createModel();
      if (resultado != FINGERPRINT_OK) {
        cancelarEnrolamiento("las dos capturas no coinciden");
        return;
      }
      resultado = lector.storeModel(idEnEnrolamiento);
      if (resultado != FINGERPRINT_OK) {
        cancelarEnrolamiento("el sensor no pudo guardar");
        return;
      }

      // El orden importa: la plantilla ya quedo en el sensor, asi que el
      // nombre DEBE quedar en la NVS en seguida. Si se reiniciara el ESP32
      // entre ambos pasos, el sensor tendria una huella sin nombre.
      strncpy(nombresPersonas[totalPersonas], nombreEnEnrolamiento, LARGO_NOMBRE - 1);
      nombresPersonas[totalPersonas][LARGO_NOMBRE - 1] = '\0';
      idsPersonas[totalPersonas] = (uint8_t)idEnEnrolamiento;
      totalPersonas++;
      guardarRegistroPersonas();

      mostrarMensajeTemporal("ENROLAMIENTO LISTO", "GUARDADA",
                             nombreEnEnrolamiento, true);
      publicarEstadoAcceso("%s registrado en el ID %d",
                           nombreEnEnrolamiento, idEnEnrolamiento);

      estadoEnrolamiento = ENROL_INACTIVO;
      idEnEnrolamiento = 0;
      nombreEnEnrolamiento[0] = '\0';
      actualizarConteoHuellas();
      reportarEstadoAcceso();
      apagarInterruptorEnrolar();
      break;

    default:
      break;
  }
}

// Ejecuta la solicitud que dejo anotada el callback de la app. Corre en el
// loop, nunca en la tarea de RainMaker, y por eso cuando llega aqui ya se
// procesaron TODOS los parametros del mismo envio: el nombre ya esta puesto
// aunque el telefono lo haya mandado junto con el interruptor.
void atenderSolicitudAcceso() {
  SolicitudAcceso solicitud = solicitudPendiente;
  if (solicitud == SOLICITUD_NINGUNA) return;
  solicitudPendiente = SOLICITUD_NINGUNA;

  switch (solicitud) {
    case SOLICITUD_ENROLAR:
      iniciarEnrolamiento(nombreSeleccionado);
      break;
    case SOLICITUD_BORRAR:
      borrarHuella(nombreSeleccionado);
      break;
    case SOLICITUD_BORRAR_TODAS:
      borrarTodasLasHuellas();
      break;
    case SOLICITUD_CANCELAR:
      if (estadoEnrolamiento != ENROL_INACTIVO) {
        cancelarEnrolamiento("cancelado desde la app");
      }
      break;
    default:
      break;
  }
}

// Lectura normal: identifica huellas y abre la chapa.
void revisarHuella() {
  if (!lectorDisponible) return;

  // Durante el enrolamiento el sensor esta ocupado con esa tarea.
  if (estadoEnrolamiento != ENROL_INACTIVO) {
    avanzarEnrolamiento();
    return;
  }

  // Bloqueo anti fuerza bruta
  if (millis() < finBloqueoLector) return;

  if (lector.getImage() != FINGERPRINT_OK) return;
  if (lector.image2Tz() != FINGERPRINT_OK) return;

  if (lector.fingerFastSearch() == FINGERPRINT_OK) {
    intentosFallidos = 0;

    int posicion = -1;
    for (uint8_t i = 0; i < totalPersonas; i++) {
      if (idsPersonas[i] == lector.fingerID) { posicion = i; break; }
    }

    if (posicion < 0) {
      // La huella existe en el sensor pero NO tiene nombre en el registro.
      // Pasa con las plantillas que quedaron del sketch de prueba. Se niega
      // el paso a proposito: un acceso sin identidad no se puede auditar, y
      // el sistema entero existe para saber QUIEN entro.
      registrar("ACCESO", "Huella del ID %d sin nombre en el registro",
                lector.fingerID);
      publicarEstadoAcceso("Huella ID %d sin nombre: use Borrar Todas",
                           lector.fingerID);
      mostrarMensajeTemporal("HUELLA SIN NOMBRE", "DENEGADO",
                             "Registro desactualizado", true);
      return;
    }

    const char *quien = nombresPersonas[posicion];
    strncpy(ultimoNombreReconocido, quien, LARGO_NOMBRE - 1);
    ultimoNombreReconocido[LARGO_NOMBRE - 1] = '\0';

    publicarEstadoAcceso("Entro %s (ID %d, confianza %d)",
                         quien, lector.fingerID, lector.confidence);
    mostrarMensajeTemporal("ACCESO CONCEDIDO", "BIENVENIDO", quien, false);
    aplicarChapa(true);
    reportarChapa();
    reportarEstadoAcceso();

  } else {
    intentosFallidos++;
    publicarEstadoAcceso("Huella no reconocida (%u de %u)",
                         intentosFallidos, INTENTOS_FALLIDOS_MAX);

    char detalle[24];
    snprintf(detalle, sizeof(detalle), "Intento %u de %u",
             intentosFallidos, INTENTOS_FALLIDOS_MAX);
    mostrarMensajeTemporal("ACCESO DENEGADO", "DENEGADO", detalle, true);

    if (intentosFallidos >= INTENTOS_FALLIDOS_MAX) {
      finBloqueoLector = millis() + BLOQUEO_LECTOR_MS;
      intentosFallidos = 0;
      registrar("SEGURIDAD", "Lector BLOQUEADO 60 s por intentos fallidos");
      publicarEstadoAcceso("Lector bloqueado 60 s");
      // Cinco huellas desconocidas seguidas en la puerta es alguien intentando
      // entrar, no un dedo mal puesto. Merece aviso al telefono.
      notificar(ALERTA_ACCESO_BLOQUEADO,
                "%u huellas no reconocidas en la puerta. Lector bloqueado.",
                INTENTOS_FALLIDOS_MAX);
      // Se descarta el mensaje de "denegado" para que la vista de bloqueo,
      // con su cuenta regresiva, se vea de inmediato.
      mensajeExpira = 0;
      pedirRefrescoPantalla();
    }
  }
}

// ----------------------------------------------------------------------------
// Callback de comandos de la app
// ----------------------------------------------------------------------------
void gestionarEscrituraRainMaker(Device *dispositivo, Param *parametro,
                                 const param_val_t valor, void *datosPrivados,
                                 write_ctx_t *contexto) {
  const char *nombreDispositivo = dispositivo->getDeviceName();
  const char *nombreParametro   = parametro->getParamName();

  registrar("APP", "Comando -> \"%s\" / \"%s\"",
            nombreDispositivo ? nombreDispositivo : "(null)",
            nombreParametro ? nombreParametro : "(null)");

  // --- Control de Acceso: tiene parametros booleanos y numericos ---
  // IMPORTANTE: se compara por NOMBRE, no por puntero. Al hacer
  // nodo.addDevice(*dispositivo) la libreria maneja una copia del objeto,
  // asi que el puntero que llega aqui NO es el que guardamos arriba.
  if (strcmp(nombreDispositivo, "Control de Acceso") == 0) {
    if (strcmp(nombreParametro, "Nombre") == 0 &&
        valor.type == RMAKER_VAL_TYPE_STRING) {
      // valor.val.s puede venir nulo si el usuario deja el campo vacio.
      // Antes esto se copiaba sin comprobar y el ESP32 se reiniciaba.
      if (valor.val.s != nullptr) {
        strncpy(nombreSeleccionado, valor.val.s, LARGO_NOMBRE - 1);
        nombreSeleccionado[LARGO_NOMBRE - 1] = '\0';
      } else {
        nombreSeleccionado[0] = '\0';
      }
      registrar("ACCESO", "Nombre recibido: \"%s\"", nombreSeleccionado);
      parametro->updateAndReport(valor);

    } else if (strcmp(nombreParametro, "Modo Enrolar") == 0 &&
               valor.type == RMAKER_VAL_TYPE_BOOLEAN) {
      // Solo se anota la intencion. El trabajo se hace en el loop, donde ya
      // llegaron todos los parametros de este mismo envio (incluido el
      // nombre, que el telefono puede mandar en cualquier orden).
      solicitudPendiente = valor.val.b ? SOLICITUD_ENROLAR : SOLICITUD_CANCELAR;
      parametro->updateAndReport(valor);

    } else if (strcmp(nombreParametro, "Borrar Huella") == 0 &&
               valor.type == RMAKER_VAL_TYPE_BOOLEAN) {
      if (valor.val.b) solicitudPendiente = SOLICITUD_BORRAR;
      // Se devuelve a false: es un boton momentaneo, no un interruptor.
      dispositivo->updateAndReportParam("Borrar Huella", false);

    } else if (strcmp(nombreParametro, "Borrar Todas") == 0 &&
               valor.type == RMAKER_VAL_TYPE_BOOLEAN) {
      if (valor.val.b) solicitudPendiente = SOLICITUD_BORRAR_TODAS;
      dispositivo->updateAndReportParam("Borrar Todas", false);

    } else {
      registrar("APP", "Parametro de acceso no manejado: \"%s\"", nombreParametro);
    }
    return;
  }

  // --- Resto de dispositivos: solo interruptores booleanos ---
  if (valor.type != RMAKER_VAL_TYPE_BOOLEAN) {
    registrar("APP", "Ignorado: el valor no es booleano");
    return;
  }

  bool encender = valor.val.b;

  if (strcmp(nombreDispositivo, "LED de Prueba") == 0) {
    aplicarLedPrueba(encender);
  } else if (strcmp(nombreDispositivo, "Ventilador") == 0) {
    aplicarVentilador(encender);
  } else if (strcmp(nombreDispositivo, "Bomba de Agua") == 0) {
    // Si el tanque esta bajo, la orden manual tambien se rechaza. Se devuelve
    // el interruptor a su estado real para que la app no muestre la bomba
    // encendida cuando en realidad no arranco.
    if (!aplicarBomba(encender)) {
      dispositivo->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, estadoBomba);
      return;
    }
  } else if (strcmp(nombreDispositivo, "Luz de Cultivo") == 0) {
    if (alarmaSonando) {
      // La luz esta parpadeando: no se toca el rele, pero se recuerda el
      // estado que el usuario pidio para aplicarlo al terminar la alarma.
      luzAntesDeAlarma = encender;
      estadoLuzCultivo = encender;
      registrar("APP", "Luz guardada para despues (alarma activa)");
    } else {
      aplicarLuzCultivo(encender);
    }
  } else if (strcmp(nombreDispositivo, "Chapa de Acceso") == 0) {
    aplicarChapa(encender);
    // Una apertura remota no deja huella de quien entro: es el unico camino
    // de acceso sin identidad en todo el sistema. Por eso se notifica al
    // telefono. Si la cuenta de RainMaker estuviera comprometida, este aviso
    // es lo que lo delata.
    if (encender) {
      registrar("SEGURIDAD", "AUDITORIA: chapa abierta en REMOTO desde la app");
      notificar(ALERTA_APERTURA_REMOTA,
                "La puerta se abrio desde la app, sin huella. Cierra en %lu s.",
                (unsigned long)(TIEMPO_CHAPA_ABIERTA_MS / 1000));
    }
  } else if (strcmp(nombreDispositivo, "Seguridad") == 0 &&
             strcmp(nombreParametro, "Alarma Armada") == 0) {
    alarmaArmada = encender;
    registrar("SEGURIDAD", "Alarma %s", encender ? "ARMADA" : "DESARMADA");
    // Desarmar corta un parpadeo en curso de inmediato
    if (!encender) {
      detenerAlarma("desarmada desde la app");
    }
  } else if (strcmp(nombreDispositivo, "Seguridad") == 0 &&
             strcmp(nombreParametro, "Probar Buzzer") == 0) {
    // Prueba manual: suena mientras el interruptor este encendido.
    // Si la alarma esta activa no se toca, para no interferir con ella.
    if (!alarmaSonando) {
      escribirBuzzer(encender);
      // La prueba se apaga sola a los 5 s. Dejar el buzzer energizado de
      // forma indefinida es justo lo que lo recalienta.
      finPruebaBuzzer = encender ? (millis() + DURACION_PRUEBA_BUZZER_MS) : 0;
      registrar("SEGURIDAD", "Prueba de buzzer: %s%s",
                encender ? "SONANDO" : "callado",
                encender ? " (se apaga sola en 5 s)" : "");
    } else {
      registrar("SEGURIDAD", "Prueba ignorada: la alarma esta activa");
    }
  } else {
    registrar("APP", "Dispositivo no reconocido: \"%s\"", nombreDispositivo);
    return;
  }

  parametro->updateAndReport(valor);
}

// ----------------------------------------------------------------------------
// Eventos de Wi-Fi / aprovisionamiento
// ----------------------------------------------------------------------------
void gestionarEventoAprovisionamiento(arduino_event_t *evento) {
  switch (evento->event_id) {
    case ARDUINO_EVENT_PROV_START:
      registrar("WIFI", "Aprovisionamiento SoftAP: \"%s\" | PoP: \"%s\"",
                NOMBRE_SERVICIO_PROV, CLAVE_POP);
      WiFiProv.printQR(NOMBRE_SERVICIO_PROV, CLAVE_POP, "softap");
      break;

    case ARDUINO_EVENT_PROV_CRED_SUCCESS:
      registrar("WIFI", "Credenciales recibidas correctamente");
      break;

    case ARDUINO_EVENT_PROV_CRED_FAIL:
      registrar("WIFI", "Aprovisionamiento FALLIDO");
      break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
      // Solo una vez: volver a llamar WiFi.config() en cada GOT_IP provoca
      // una reconexion que dispara este mismo evento (bucle infinito).
      static bool dnsYaForzado = false;
      if (!dnsYaForzado) {
        IPAddress dns1(8, 8, 8, 8);
        IPAddress dns2(8, 8, 4, 4);
        WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns1, dns2);
        registrar("WIFI", "DNS forzado a 8.8.8.8 / 8.8.4.4");
        dnsYaForzado = true;
      }
      break;
    }

    default:
      break;
  }
}

// ----------------------------------------------------------------------------
// Lecturas periodicas
// ----------------------------------------------------------------------------

// --- Humedad del suelo (FC-28) ---
// La sonda se energiza, se le da tiempo de estabilizarse, se lee y se apaga.
// Mantenerla con corriente continua la oxida: el agua del suelo mas una
// tension constante hacen electrolisis y las pistas se comen en semanas.
void leerHumedadSuelo() {
  // --- Comprobacion de cable suelto, aprovechando que controlamos el VCC ---
  // Con la sonda SIN alimentar, el divisor del modulo debe caer casi a cero.
  // Si aqui ya se lee un valor alto, AO esta flotando o conectado a otra
  // cosa. Esto reemplaza la deteccion anterior, que daba por invalido
  // cualquier valor cercano a 4095... justo donde cae el suelo SECO en este
  // sensor. Con esa regla, un suelo totalmente seco se descartaba como
  // "lectura invalida" y el riego nunca arrancaba: lo contrario de lo que
  // hace falta.
  int crudoSinAlimentar = -1;
  if (ALIMENTAR_SUELO_POR_GPIO) {
    digitalWrite(PIN_SUELO_VCC, LOW);
    delay(30);
    crudoSinAlimentar = analogRead(PIN_SUELO_AO);

    digitalWrite(PIN_SUELO_VCC, HIGH);
    delay(200);   // El comparador del modulo necesita asentarse
  }

  // Se promedian varias muestras: la lectura de una sonda resistiva es
  // ruidosa, sobre todo con la bomba trabajando cerca.
  uint32_t suma = 0;
  for (uint8_t i = 0; i < 8; i++) {
    suma += analogRead(PIN_SUELO_AO);
    delay(5);
  }
  sueloCrudo = suma / 8;

  if (ALIMENTAR_SUELO_POR_GPIO) {
    digitalWrite(PIN_SUELO_VCC, LOW);
  }

  // 4095 es un valor LEGITIMO: es lo que marca la sonda al aire o en suelo
  // bien seco, con el ADC saturado. Solo se descarta un cero absoluto, que
  // significa pin a tierra.
  sueloValido = (sueloCrudo > 20);

  if (sueloValido && crudoSinAlimentar > 500) {
    sueloValido = false;
    registrar("FC-28", "AO parece desconectado: sin alimentar ya lee %d",
              crudoSinAlimentar);
    registrar("FC-28", "  Revisar el cable de AO al GPIO%d y el GND comun.",
              PIN_SUELO_AO);
  }

  if (!sueloValido) {
    humedadSuelo = 0.0f;
    return;
  }

  // La sonda da MAS tension cuando esta seca, asi que el mapeo va invertido.
  float porcentaje = (float)(SUELO_CRUDO_SECO - sueloCrudo) * 100.0f /
                     (float)(SUELO_CRUDO_SECO - SUELO_CRUDO_HUMEDO);
  if (porcentaje < 0.0f)   porcentaje = 0.0f;
  if (porcentaje > 100.0f) porcentaje = 100.0f;
  humedadSuelo = porcentaje;
}

// ----------------------------------------------------------------------------
// RIEGO POR PULSOS
// ----------------------------------------------------------------------------
// Cierra el ciclo y deja la bomba apagada.
void terminarRiego(const char *motivo) {
  if (estadoBomba) {
    aplicarBomba(false);
    if (dispositivoBomba) {
      dispositivoBomba->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, false);
    }
  }
  registrar("RIEGO", "Ciclo terminado: %s", motivo);
  registrar("RIEGO", "  %u pulso(s) de %lu ms  |  suelo %.1f %% -> %.1f %%",
            pulsosAplicados, (unsigned long)DURACION_PULSO_RIEGO_MS,
            humedadAlIniciarRiego, humedadSuelo);

  estadoRiego = RIEGO_INACTIVO;
  pulsosAplicados = 0;

  // Tambien al terminar bien: el agua sigue repartiendose un rato despues del
  // ultimo pulso, asi que medir de nuevo en seguida daria un valor que aun no
  // refleja lo que ya se echo. Y si la tanda acabo por tope de pulsos, sin
  // este freno volveria a arrancar de inmediato.
  tiempoProximoIntentoRiego = millis() + ESPERA_REINTENTO_RIEGO_MS;
}

// Enciende la bomba para un pulso. Devuelve false si la orden fue rechazada
// (tanque bajo), en cuyo caso el ciclo no puede continuar.
bool arrancarPulso() {
  if (!aplicarBomba(true)) return false;
  if (dispositivoBomba) {
    dispositivoBomba->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, true);
  }
  pulsosAplicados++;
  estadoRiego = RIEGO_PULSO;
  tiempoCambioRiego = millis();
  registrar("RIEGO", "Pulso %u de %u: bomba %lu ms",
            pulsosAplicados, MAX_PULSOS_POR_CICLO,
            (unsigned long)DURACION_PULSO_RIEGO_MS);
  return true;
}

// Se llama en cada vuelta del loop. Es el UNICO sitio que decide el riego
// automatico: el ciclo de clima solo lee y reporta la humedad, para que no
// haya dos funciones peleando por la bomba.
void revisarRiego() {
  switch (estadoRiego) {

    case RIEGO_INACTIVO:
      // Si la bomba ya esta encendida, la orden vino de la app. El riego
      // automatico no se mete: el usuario manda, y el watchdog protege.
      if (estadoBomba) return;
      if (!sueloValido) return;
      if (humedadSuelo >= SUELO_BOMBA_ON) return;

      // Freno contra el bucle: tras una tanda terminada o un arranque
      // rechazado hay que esperar antes de volver a intentarlo.
      if (tiempoProximoIntentoRiego != 0 && millis() < tiempoProximoIntentoRiego) {
        return;
      }

      // El permiso del tanque se comprueba ANTES de anunciar la tanda. Si no
      // lo hay, no tiene sentido abrir un bloque de "inicio de riego" que no
      // va a empezar.
      if (!tanqueConAgua) {
        tiempoProximoIntentoRiego = millis() + ESPERA_REINTENTO_RIEGO_MS;
        registrar("RIEGO", "Suelo al %.1f %% pide riego, pero el tanque no da permiso",
                  humedadSuelo);
        registrar("RIEGO", "  Nuevo intento en %lu s",
                  (unsigned long)(ESPERA_REINTENTO_RIEGO_MS / 1000));
        return;
      }

      pulsosAplicados = 0;
      humedadAlIniciarRiego = humedadSuelo;

      registrarSeparador("INICIO DE RIEGO POR PULSOS");
      registrar("RIEGO", "Suelo al %.1f %% (umbral %.0f %%). Objetivo: %.0f %%",
                humedadSuelo, SUELO_BOMBA_ON, SUELO_BOMBA_OFF);

      if (!arrancarPulso()) {
        // Red de seguridad: si por cualquier motivo el pulso no arranco,
        // tambien se espera. Nunca se vuelve a INACTIVO sin poner el freno.
        tiempoProximoIntentoRiego = millis() + ESPERA_REINTENTO_RIEGO_MS;
        estadoRiego = RIEGO_INACTIVO;
      }
      break;

    case RIEGO_PULSO:
      if ((millis() - tiempoCambioRiego) < DURACION_PULSO_RIEGO_MS) return;

      aplicarBomba(false);
      if (dispositivoBomba) {
        dispositivoBomba->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, false);
      }
      estadoRiego = RIEGO_ABSORBIENDO;
      tiempoCambioRiego = millis();
      break;

    case RIEGO_ABSORBIENDO:
      if ((millis() - tiempoCambioRiego) < TIEMPO_ABSORCION_MS) return;

      // Medida fresca: es el dato que decide si hace falta otro pulso.
      leerHumedadSuelo();
      registrar("RIEGO", "Tras el pulso %u: suelo %.1f %% (crudo %d)",
                pulsosAplicados, humedadSuelo, sueloCrudo);
      reportarSerie(dispositivoClima, "Humedad Suelo", humedadSuelo);

      if (!sueloValido) {
        terminarRiego("se perdio la lectura del suelo");
        return;
      }
      if (humedadSuelo >= SUELO_BOMBA_OFF) {
        terminarRiego("objetivo alcanzado");
        return;
      }
      if (pulsosAplicados >= MAX_PULSOS_POR_CICLO) {
        terminarRiego("tope de pulsos alcanzado SIN llegar al objetivo");
        // Que la humedad no suba tras tantos pulsos es una averia que el
        // sistema no puede corregir solo.
        notificar(ALERTA_RIEGO_FALLIDO,
                  "%u pulsos de riego y el suelo sigue al %.0f%%. Revise mangueras y sonda.",
                  MAX_PULSOS_POR_CICLO, humedadSuelo);
        mostrarMensajeTemporal("RIEGO SIN EFECTO", "REVISAR", "Mangueras o sonda", true);
        return;
      }

      if (!arrancarPulso()) {
        terminarRiego("el tanque bajo del minimo a media tanda");
      }
      break;
  }
}

// --- Nivel del tanque (HC-SR04) ---
// Una sola medida. Devuelve -1 si no llego el eco.
// La velocidad del sonido NO es constante: sube unos 0.6 m/s por cada grado.
// Es la razon por la que el HC-SR04 "lee mal" cuando se calibra en un sitio
// y se usa en otro mas caliente.
//
//   a 20 C -> 343.4 m/s
//   a 35 C -> 352.5 m/s   (2.6 % de diferencia)
//
// En un invernadero en Zacapa esa diferencia es real: sobre 30 cm medidos son
// casi 8 mm de error, y sobre el rango util del tanque se traduce en varios
// puntos de porcentaje. Como ya tenemos el DHT11, se compensa.
// Si el DHT11 no da lectura valida, se asume 20 C.
float velocidadSonidoCmPorUs() {
  float t = dht11Valido ? temperaturaActual : 20.0f;
  return (331.3f + 0.606f * t) / 10000.0f;   // m/s -> cm/us
}

// Devuelve la distancia en cm, o -1 si no llego el eco.
// 'duracionUs' sale por referencia para poder registrarla en el log crudo.
float medirDistanciaUnaVez(uint32_t *duracionUs) {
  digitalWrite(PIN_ULTRA_TRIG, LOW);
  delayMicroseconds(4);
  digitalWrite(PIN_ULTRA_TRIG, HIGH);
  delayMicroseconds(10);          // El HC-SR04 pide un pulso de 10 us
  digitalWrite(PIN_ULTRA_TRIG, LOW);

  // 30 ms de espera maxima: equivale a unos 5 m, mas que el alcance del
  // sensor. Sin este limite, un eco perdido dejaria el loop colgado.
  uint32_t duracion = pulseIn(PIN_ULTRA_ECHO, HIGH, 30000UL);
  if (duracionUs) *duracionUs = duracion;
  if (duracion == 0) return -1.0f;

  // Se divide entre 2 porque el sonido va y vuelve.
  return (duracion * velocidadSonidoCmPorUs()) / 2.0f;
}

// Revisa el nivel de reposo de ECHO, SIN disparar el sensor. Es la prueba que
// distingue tres fallas que desde el log se veian iguales:
//
//   siempre bajo  -> la linea llega bien y el sensor simplemente no contesta
//   siempre alto  -> el conversor no esta transmitiendo el cero de ECHO, o el
//                    canal esta cruzado. El pull-up de 3.3V del lado LV manda
//                    y GPIO36 no tiene resistencia interna para discutirlo
//   oscilando     -> el pin esta flotando: cable suelto o sin GND comun
//
// El caso de "siempre alto" importa especialmente: pulseIn() espera a que el
// pin baje antes de medir, asi que con la linea pegada en alto se agota la
// espera y devuelve 0, que es indistinguible de "no llego ningun eco".
void revisarReposoEcho() {
  uint16_t vecesAlto = 0;
  for (uint16_t i = 0; i < 100; i++) {
    if (digitalRead(PIN_ULTRA_ECHO) == HIGH) vecesAlto++;
    delayMicroseconds(200);
  }

  registrar("HC-SR04", "ECHO en reposo .... %u de 100 lecturas en alto", vecesAlto);

  if (vecesAlto == 0) {
    registrar("HC-SR04", "  La linea esta en reposo correcto.");
    registrar("HC-SR04", "  Entonces el cero SI pasa por el conversor.");
    registrar("HC-SR04", "  Revisar TRIG (GPIO%d) y los 5V del sensor.",
              PIN_ULTRA_TRIG);
  } else if (vecesAlto >= 95) {
    registrar("HC-SR04", "  PEGADA EN ALTO. El conversor no transmite el cero.");
    registrar("HC-SR04", "  Mide con el sensor en reposo:");
    registrar("HC-SR04", "    lado HV de ese canal  -> debe dar ~0 V");
    registrar("HC-SR04", "    lado LV de ese canal  -> debe dar ~0 V tambien");
    registrar("HC-SR04", "  Si HV da 0 V y LV da 3.3 V, ese canal no sirve:");
    registrar("HC-SR04", "  cambia de canal o usa el divisor de 10k y 20k.");
  } else {
    registrar("HC-SR04", "  OSCILANDO SOLA: el pin esta flotando.");
    registrar("HC-SR04", "  GPIO%d no tiene resistencias internas, asi que un",
              PIN_ULTRA_ECHO);
    registrar("HC-SR04", "  cable suelto se lee como ruido. Revisar la conexion");
    registrar("HC-SR04", "  de ECHO y que el GND de los 5V sea el mismo.");
  }
}

void actualizarNivelTanque() {
  registrarSeparador("LECTURA DEL TANQUE");

  float velocidad = velocidadSonidoCmPorUs();
  registrar("HC-SR04", "Temperatura ...... %6.1f C %s -> sonido a %.1f m/s",
            dht11Valido ? temperaturaActual : 20.0f,
            dht11Valido ? "(DHT11)" : "(SUPUESTA, DHT11 sin lectura)",
            velocidad * 10000.0f);

  // Se toma la MEDIANA de tres medidas, no el promedio: si una medida falla,
  // el promedio queda contaminado, mientras que la mediana la descarta.
  float muestras[MUESTRAS_ULTRASONIDO];
  uint8_t buenas = 0;

  for (uint8_t i = 0; i < MUESTRAS_ULTRASONIDO; i++) {
    uint32_t duracion = 0;
    float d = medirDistanciaUnaVez(&duracion);

    if (d > 0.0f && d < DIST_MAXIMA_VALIDA_CM) {
      muestras[buenas++] = d;
      registrar("HC-SR04", "  muestra %u ...... eco %5lu us  ->  %6.1f cm",
                i + 1, (unsigned long)duracion, d);
    } else if (duracion == 0) {
      registrar("HC-SR04", "  muestra %u ...... SIN ECO (se agoto la espera)", i + 1);
    } else {
      registrar("HC-SR04", "  muestra %u ...... eco %5lu us  ->  %6.1f cm  DESCARTADA",
                i + 1, (unsigned long)duracion, d);
    }
    delay(60);   // El HC-SR04 necesita >60 ms entre disparos
  }

  if (buenas == 0) {
    tanqueValido = false;
    // Sin lectura NO se permite regar. Es la decision conservadora: preferimos
    // un riego perdido a una bomba quemada por marcha en seco.
    tanqueConAgua = !PROTECCION_TANQUE_ACTIVA;
    registrar("HC-SR04", "Sin ninguna medida valida. Revisando la linea ECHO...");
    revisarReposoEcho();
    return;
  }

  // Ordenamiento simple; con 3 elementos no hace falta nada mas elaborado.
  for (uint8_t i = 0; i < buenas; i++) {
    for (uint8_t j = i + 1; j < buenas; j++) {
      if (muestras[j] < muestras[i]) {
        float t = muestras[i]; muestras[i] = muestras[j]; muestras[j] = t;
      }
    }
  }
  distanciaTanqueCm = muestras[buenas / 2];
  tanqueValido = true;

  // A MAYOR distancia, MENOS agua: el sensor mira hacia abajo desde la tapa.
  float porcentaje = (DIST_TANQUE_VACIO_CM - distanciaTanqueCm) * 100.0f /
                     (DIST_TANQUE_VACIO_CM - DIST_TANQUE_LLENO_CM);

  // Antes de recortar, se avisa si la medida cae FUERA del rango configurado.
  // Esta es la causa mas comun de que la bomba quede bloqueada sin motivo
  // aparente: el porcentaje se recorta a 0 y el sistema cree que no hay agua.
  if (porcentaje < 0.0f) {
    registrar("HC-SR04", "CALIBRACION: %.1f cm es MAS que DIST_TANQUE_VACIO_CM (%.1f)",
              distanciaTanqueCm, DIST_TANQUE_VACIO_CM);
    registrar("HC-SR04", "  El nivel se recorta a 0 %% y la bomba quedara bloqueada.");
    registrar("HC-SR04", "  Mide con cinta del sensor al fondo y ajusta esa constante.");
  } else if (porcentaje > 100.0f) {
    registrar("HC-SR04", "CALIBRACION: %.1f cm es MENOS que DIST_TANQUE_LLENO_CM (%.1f)",
              distanciaTanqueCm, DIST_TANQUE_LLENO_CM);
    registrar("HC-SR04", "  Puede ser la zona muerta: el sensor necesita >2 cm de aire.");
  }

  if (porcentaje < 0.0f)   porcentaje = 0.0f;
  if (porcentaje > 100.0f) porcentaje = 100.0f;
  nivelTanque = porcentaje;

  bool habiaAgua = tanqueConAgua;
  tanqueConAgua  = PROTECCION_TANQUE_ACTIVA
                       ? (nivelTanque >= NIVEL_TANQUE_MINIMO)
                       : true;

  registrar("HC-SR04", "Mediana de %u ..... %6.1f cm  ->  %5.1f %%  [rango %.0f-%.0f cm]",
            buenas, distanciaTanqueCm, nivelTanque,
            DIST_TANQUE_LLENO_CM, DIST_TANQUE_VACIO_CM);
  registrar("HC-SR04", "Permiso de riego .. %s  [minimo %.0f %%]%s",
            tanqueConAgua ? "SI" : "NO", NIVEL_TANQUE_MINIMO,
            PROTECCION_TANQUE_ACTIVA ? "" : "  (PROTECCION DESACTIVADA)");

  reportarSerie(dispositivoBomba, "Nivel Tanque", nivelTanque);

  // Si se acaba de cruzar el umbral hacia abajo, se corta el riego en curso
  // y se avisa una sola vez.
  if (habiaAgua && !tanqueConAgua) {
    registrar("SEGURIDAD", "El tanque bajo del minimo: se corta el riego");
    if (estadoBomba) {
      estadoBomba = false;
      ledcWrite(CANAL_PWM, 0);
      if (dispositivoBomba) {
        dispositivoBomba->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, false);
      }
    }
    notificar(ALERTA_TANQUE,
              "Tanque al %.0f%%. El riego queda suspendido hasta rellenarlo.",
              nivelTanque);
    mostrarMensajeTemporal("TANQUE BAJO", "SIN AGUA", "Riego suspendido", true);
  }
}

// --- Diagnostico de la bomba ---
// Imprime POR QUE la bomba esta o no esta encendida. Antes habia que cruzar
// varias lineas del log para deducirlo, y la causa mas comun -el permiso del
// tanque- quedaba escondida entre las lecturas de los sensores.
void registrarDiagnosticoBomba() {
  // Etapa del riego por pulsos, que explica por si sola el estado de la bomba.
  const char *etapa =
      estadoRiego == RIEGO_PULSO       ? "en pulso"    :
      estadoRiego == RIEGO_ABSORBIENDO ? "absorbiendo" :
                                         "inactivo";

  if (estadoBomba) {
    registrar("BOMBA", "ENCENDIDA al %u %%  |  riego %s, pulso %u de %u",
              PORCENTAJE_BOMBA, etapa, pulsosAplicados, MAX_PULSOS_POR_CICLO);
    return;
  }

  if (estadoRiego == RIEGO_ABSORBIENDO) {
    uint32_t restante = (millis() - tiempoCambioRiego >= TIEMPO_ABSORCION_MS)
                            ? 0
                            : (TIEMPO_ABSORCION_MS - (millis() - tiempoCambioRiego)) / 1000;
    registrar("BOMBA", "APAGADA entre pulsos: esperando %lu s de absorcion",
              (unsigned long)restante);
    registrar("BOMBA", "  pulso %u de %u  |  suelo %.1f %% -> objetivo %.0f %%",
              pulsosAplicados, MAX_PULSOS_POR_CICLO, humedadSuelo, SUELO_BOMBA_OFF);
    return;
  }

  // Las condiciones se evaluan en el mismo orden en que bloquean.
  const char *motivo;
  if (!sueloValido) {
    motivo = "el FC-28 no da lectura valida";
  } else if (!tanqueConAgua) {
    motivo = tanqueValido ? "el tanque esta bajo el minimo"
                          : "el HC-SR04 no da lectura valida";
  } else if (humedadSuelo >= SUELO_BOMBA_ON) {
    motivo = "el suelo todavia no baja del umbral";
  } else {
    motivo = "sin causa identificada (revisar ENA y el L298N)";
  }

  registrar("BOMBA", "APAGADA porque %s", motivo);
  registrar("BOMBA", "  suelo %s (%.1f %%, enciende por debajo de %.0f %%)",
            sueloValido ? "ok" : "INVALIDO", humedadSuelo, SUELO_BOMBA_ON);
  registrar("BOMBA", "  tanque %s (%.1f %%, minimo %.0f %%) -> permiso %s",
            tanqueValido ? "ok" : "INVALIDO", nivelTanque, NIVEL_TANQUE_MINIMO,
            tanqueConAgua ? "SI" : "NO");
  registrar("BOMBA", "  salidas: ENA GPIO%d (PWM canal %u), IN1 GPIO%d, IN2 GPIO%d",
            PIN_BOMBA_ENA, CANAL_PWM, PIN_BOMBA_IN1, PIN_BOMBA_IN2);
}

// --- Ventilador: una sola funcion decide, con todas las entradas a la vista ---
// Antes habia DOS sitios que mandaban sobre el ventilador: actualizarClima()
// lo gobernaba por temperatura y humedad, y actualizarCalidadAire() lo forzaba
// por gases. El resultado era que si el aire seguia malo pero el clima estaba
// en rango, el siguiente ciclo de clima lo APAGABA pese a la alerta de gas.
// Juntar las cuatro condiciones en un solo lugar elimina ese conflicto.
void evaluarVentilador() {
  bool tempAlta  = dht11Valido && (temperaturaActual > TEMP_VENTILADOR_ON_C);
  bool tempBaja  = dht11Valido && (temperaturaActual < TEMP_VENTILADOR_OFF_C);
  bool humAlta   = dht11Valido && (humedadAmbienteActual > HUMEDAD_VENTILADOR_ON);
  bool humBaja   = dht11Valido && (humedadAmbienteActual < HUMEDAD_VENTILADOR_OFF);
  bool aireMalo  = (mq135Crudo > UMBRAL_AIRE_MALO_RAW);
  bool coCritico = (mq7Crudo   > UMBRAL_CO_EMERGENCIA_RAW);

  bool debeEncender = estadoVentilador;   // Por defecto se mantiene
  const char *motivo = nullptr;

  if (coCritico) {
    debeEncender = true;  motivo = "MONOXIDO critico";
  } else if (aireMalo) {
    debeEncender = true;  motivo = "mala calidad de aire";
  } else if (tempAlta) {
    debeEncender = true;  motivo = "temperatura alta";
  } else if (humAlta) {
    debeEncender = true;  motivo = "humedad del aire alta";
  } else if (tempBaja && humBaja) {
    // Para APAGAR se exigen las dos condiciones por debajo de su umbral de
    // salida, que es distinto del de entrada: esa separacion es la histeresis
    // que evita que el ventilador conmute sin parar en el limite.
    debeEncender = false; motivo = "clima y aire dentro de rango";
  }

  if (debeEncender != estadoVentilador) {
    registrar("AUTOMATICO", "Ventilador cambia por: %s", motivo ? motivo : "condicion");
    aplicarVentilador(debeEncender);
    if (dispositivoVentilador) {
      dispositivoVentilador->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME,
                                                  estadoVentilador);
    }
  }
}

void actualizarClima() {
  registrarSeparador("LECTURA DE CLIMA");

  float humedadLeida     = dht11.readHumidity();
  float temperaturaLeida = dht11.readTemperature();

  bool llegaronDatos = (!isnan(humedadLeida) && !isnan(temperaturaLeida));

  // Que el dato llegue no significa que sea creible. El DHT11 solo especifica
  // 0 a 50 C y 20 a 90 % de humedad; fuera de ahi la lectura no es confiable
  // aunque la libreria no la marque como error. Y como esta temperatura ahora
  // alimenta la compensacion del ultrasonido, un valor absurdo contaminaria
  // tambien la medida del tanque: por eso se valida el rango.
  bool rangoCreible = llegaronDatos &&
                      (temperaturaLeida > -5.0f && temperaturaLeida < 60.0f) &&
                      (humedadLeida >= 0.0f && humedadLeida <= 100.0f);

  dht11Valido = rangoCreible;

  if (!llegaronDatos) {
    registrar("DHT11", "Sin respuesta. Revisar en este orden:");
    registrar("DHT11", "  1. DATA en GPIO%d y resistencia pull-up de 10k a VCC",
              PIN_DHT11);
    registrar("DHT11", "  2. Alimentacion: si va a 5V, su DATA tambien sale a 5V");
    registrar("DHT11", "     y DEBE pasar por el conversor de nivel");
    registrar("DHT11", "  3. GND comun con el ESP32");
  } else if (!rangoCreible) {
    // Dato recibido pero fuera de especificacion: casi siempre es ruido en la
    // linea de datos, un pull-up ausente o un cable demasiado largo.
    registrar("DHT11", "DESCARTADA: %.1f C / %.1f %% esta fuera de especificacion",
              temperaturaLeida, humedadLeida);
    registrar("DHT11", "  El DHT11 solo mide 0-50 C y 20-90 %%. Revisar el pull-up");
    registrar("DHT11", "  de 10k y que el cable de datos no supere los 20 cm.");
  } else {
    humedadAmbienteActual = humedadLeida;
    temperaturaActual     = temperaturaLeida;

    registrar("DHT11", "Temperatura ...... %6.1f C   [vent ON >%.0f / OFF <%.0f]",
              temperaturaActual, TEMP_VENTILADOR_ON_C, TEMP_VENTILADOR_OFF_C);
    registrar("DHT11", "Humedad aire ..... %6.1f %%   [vent ON >%.0f / OFF <%.0f]",
              humedadAmbienteActual, HUMEDAD_VENTILADOR_ON, HUMEDAD_VENTILADOR_OFF);
    registrar("DHT11", "Compensacion ..... sonido a %.1f m/s para el HC-SR04",
              velocidadSonidoCmPorUs() * 10000.0f);

    reportarSerie(dispositivoClima, "Temperatura", temperaturaActual);
    reportarSerie(dispositivoClima, "Humedad Ambiente", humedadAmbienteActual);
  }

  // --- Lectura del BH1750 ---
  if (bh1750Disponible) {
    float luxLeido = sensorLuz.readLightLevel();
    if (luxLeido < 0) {
      registrar("BH1750", "Lectura invalida");
    } else {
      luxActual = luxLeido;
      const char *interpretacion =
          luxActual < LUX_LUZ_ON   ? "OSCURO -> luz ON" :
          luxActual >= LUX_LUZ_OFF ? "CLARO -> luz OFF" :
                                     "intermedio, sin cambio";
      registrar("BH1750", "Luz ambiental .... %6.1f lux  [%s]",
                luxActual, interpretacion);
      reportarSerie(dispositivoClima, "Luz Ambiental", luxActual);
    }
  } else {
    registrar("BH1750", "No disponible; sin control automatico de luz");
  }

  // --- Lectura de la humedad del SUELO (FC-28) ---
  leerHumedadSuelo();
  if (!sueloValido) {
    registrar("FC-28", "Lectura invalida (crudo %d). Revisar AO en GPIO%d y los 3.3V.",
              sueloCrudo, PIN_SUELO_AO);
  } else {
    const char *interpretacion =
        humedadSuelo < SUELO_BOMBA_ON   ? "SECO -> riega"   :
        humedadSuelo >= SUELO_BOMBA_OFF ? "HUMEDO -> corta" :
                                          "intermedio, sin cambio";
    registrar("FC-28", "Humedad suelo .... %6.1f %%  (crudo %4d)  [ON <%.0f / OFF >=%.0f] %s",
              humedadSuelo, sueloCrudo, SUELO_BOMBA_ON, SUELO_BOMBA_OFF,
              interpretacion);
    reportarSerie(dispositivoClima, "Humedad Suelo", humedadSuelo);
  }

  // --- Ventilador: una sola decision, con clima y gases juntos ---
  evaluarVentilador();

  // --- El riego NO se decide aqui ---
  // La decision vive en revisarRiego(), que corre desde el loop porque
  // necesita resolucion de 1 segundo para los pulsos. Este ciclo solo lee y
  // reporta la humedad; si tambien decidiera, las dos funciones se pisarian
  // -el mismo error que ya tuvimos con el ventilador, donde clima y gases
  // mandaban por separado sobre el mismo actuador.
  //
  // Lo unico que si corresponde aqui es cortar por seguridad si la lectura
  // del suelo se pierde con la bomba encendida por orden manual.
  if (!sueloValido && estadoBomba && estadoRiego == RIEGO_INACTIVO) {
    registrar("SEGURIDAD", "Sin lectura de suelo: se corta el riego manual");
    aplicarBomba(false);
    if (dispositivoBomba) {
      dispositivoBomba->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, false);
    }
  }

  // Se imprime SIEMPRE, encendida o apagada, para no tener que deducir el
  // estado del riego cruzando lineas sueltas del log.
  registrarDiagnosticoBomba();

  // --- Luz de cultivo por lux ---
  // Si la alarma esta sonando, el control por lux queda suspendido: la luz
  // pertenece al parpadeo hasta que la alarma termine.
  if (alarmaSonando) {
    registrar("AUTOMATICO", "Control de luz suspendido (alarma activa)");
  } else if (bh1750Disponible) {
    bool debeEncenderLuz = estadoLuzCultivo;
    if (luxActual < LUX_LUZ_ON) {
      debeEncenderLuz = true;
    } else if (luxActual >= LUX_LUZ_OFF) {
      debeEncenderLuz = false;
    }
    if (debeEncenderLuz != estadoLuzCultivo) {
      registrar("AUTOMATICO", "Luz cambia por %.1f lux", luxActual);
      aplicarLuzCultivo(debeEncenderLuz);
      if (dispositivoLuz) {
        dispositivoLuz->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, estadoLuzCultivo);
      }
    }
  }
}

void actualizarCalidadAire() {
  registrarSeparador("LECTURA DE CALIDAD DE AIRE");

  mq135Crudo = analogRead(PIN_MQ135);
  mq7Crudo   = analogRead(PIN_MQ7);

  float mq135Porcentaje = (mq135Crudo * 100.0f) / 4095.0f;
  float mq7Porcentaje   = (mq7Crudo   * 100.0f) / 4095.0f;
  float mq135Voltaje    = (mq135Crudo * 3.3f) / 4095.0f;
  float mq7Voltaje      = (mq7Crudo   * 3.3f) / 4095.0f;

  const char *estadoMq135 =
      mq135Crudo == 0                     ? "SIN SENAL (revisar cableado)" :
      mq135Crudo > UMBRAL_AIRE_MALO_RAW   ? "ALTO -> fuerza ventilador"    :
                                            "normal";
  const char *estadoMq7 =
      mq7Crudo == 0                       ? "SIN SENAL (revisar cableado)" :
      mq7Crudo > UMBRAL_CO_EMERGENCIA_RAW ? "CRITICO -> fuerza ventilador" :
                                            "normal";

  registrar("MQ135", "Aire ..... %4d /4095  %5.1f%%  %4.2f V  [umbral %d] %s",
            mq135Crudo, mq135Porcentaje, mq135Voltaje, UMBRAL_AIRE_MALO_RAW, estadoMq135);
  registrar("MQ7", "CO ....... %4d /4095  %5.1f%%  %4.2f V  [umbral %d] %s",
            mq7Crudo, mq7Porcentaje, mq7Voltaje, UMBRAL_CO_EMERGENCIA_RAW, estadoMq7);

  reportarSerieEntero(dispositivoAire, "Indice Aire", mq135Crudo);
  reportarSerieEntero(dispositivoAire, "Indice CO", mq7Crudo);

  // La decision del ventilador ya no se toma aqui. Se delega en la misma
  // funcion que usa el ciclo de clima, para que no haya dos sitios peleando
  // por el mismo actuador.
  evaluarVentilador();

  // Monoxido de carbono critico es la unica lectura de gas que justifica una
  // push: es un riesgo para quien entre al invernadero. El aire "malo" del
  // MQ135 lo resuelve el ventilador solo, asi que no se notifica.
  if (mq7Crudo > UMBRAL_CO_EMERGENCIA_RAW) {
    notificar(ALERTA_CO,
              "Monoxido de carbono alto (%d/4095). Ventile antes de entrar.",
              mq7Crudo);
    mostrarMensajeTemporal("ALERTA DE GAS", "CO ALTO", "No entre al area", true);
  }
}

// Quita del arreglo las marcas que ya salieron de la ventana de 1 minuto.
// Despues de llamarla, TODAS las marcas que quedan estan dentro de la ventana,
// asi que basta contar cuantas hay para saber si se cumple la regla.
void purgarDetecciones() {
  uint32_t ahora = millis();
  uint8_t validas = 0;

  for (uint8_t i = 0; i < deteccionesEnVentana; i++) {
    if ((ahora - marcasDeteccion[i]) <= VENTANA_DETECCIONES_MS) {
      marcasDeteccion[validas++] = marcasDeteccion[i];
    }
  }

  if (validas != deteccionesEnVentana) {
    if (LOG_PIR_DETALLADO) {
      registrar("PIR", "Conteo expirado: quedan %u de %u detecciones",
                validas, deteccionesEnVentana);
    }
    deteccionesEnVentana = validas;
    pedirRefrescoPantalla();
  }
}

// Anota una deteccion nueva. El arreglo guarda solo las ultimas 3: cuando se
// llena, la mas vieja se descarta.
void anotarDeteccion() {
  uint32_t ahora = millis();

  if (deteccionesEnVentana < DETECCIONES_PARA_ALARMA) {
    marcasDeteccion[deteccionesEnVentana++] = ahora;
  } else {
    for (uint8_t i = 0; i + 1 < DETECCIONES_PARA_ALARMA; i++) {
      marcasDeteccion[i] = marcasDeteccion[i + 1];
    }
    marcasDeteccion[DETECCIONES_PARA_ALARMA - 1] = ahora;
  }
  ultimaDeteccionContada = ahora;
}

void actualizarSeguridad() {
  bool pirActivo = (digitalRead(PIN_PIR) == HIGH);

  static bool pirAnterior = false;
  bool flancoDeSubida = (pirActivo && !pirAnterior);
  bool huboCambio = (pirActivo != pirAnterior);
  if (huboCambio) pirAnterior = pirActivo;

  // --- Calentamiento del sensor ---
  // Durante el primer minuto la salida del HC-SR501 oscila sola. Se ignora
  // por completo, pero se informa una vez al entrar y otra al salir para que
  // quede claro en el log por que no se cuenta nada en ese tramo.
  if (millis() < PIR_CALENTAMIENTO_MS) {
    static uint32_t ultimoAvisoCalentamiento = 0;
    // Se cuentan los cambios solo para dejar constancia de cuanto ruido hubo.
    static uint16_t cambiosEnCalentamiento = 0;
    if (huboCambio) cambiosEnCalentamiento++;

    // Un aviso cada 15 s en vez de una linea por cambio: durante el
    // calentamiento puede haber varios cambios por segundo y el log se
    // volvia ilegible.
    if (ultimoAvisoCalentamiento == 0 ||
        (millis() - ultimoAvisoCalentamiento) >= 30000) {
      ultimoAvisoCalentamiento = millis();
      registrar("PIR", "Calentando (%lu s de %lu). Ignorando %u cambios de nivel.",
                (unsigned long)(millis() / 1000),
                (unsigned long)(PIR_CALENTAMIENTO_MS / 1000),
                cambiosEnCalentamiento);
    }
    return;
  }

  // Aviso unico al terminar el calentamiento.
  static bool calentamientoAnunciado = false;
  if (!calentamientoAnunciado) {
    calentamientoAnunciado = true;
    registrar("PIR", "Calentamiento terminado. El sensor ya cuenta detecciones.");
    // Se descarta cualquier marca que haya quedado del arranque.
    deteccionesEnVentana  = 0;
    ultimaDeteccionContada = 0;
  }

  // Cada cambio de nivel solo se registra en modo detallado.
  if (huboCambio && LOG_PIR_DETALLADO) {
    registrar("PIR", "Pin GPIO%d en %s   [alarma %s]",
              PIN_PIR, pirActivo ? "ALTO" : "bajo",
              alarmaArmada ? "ARMADA" : "desarmada");
  }

  purgarDetecciones();

  // El pin se sigue leyendo cada 200 ms, pero una deteccion solo se CUENTA si
  // pasaron al menos 15 s desde la anterior.
  //
  // Esta es la diferencia con muestrear el pin cada 15 s: el HC-SR501
  // mantiene su salida en alto el tiempo que marca su potenciometro Tx, y si
  // ese tiempo es mas corto que el intervalo de muestreo, un movimiento real
  // puede caer entre dos muestras y perderse por completo. Leyendo seguido y
  // limitando el CONTEO se obtiene el mismo comportamiento -como maximo un
  // incremento cada 15 s- sin ese punto ciego.
  bool nuevaDeteccion = false;
  if (flancoDeSubida) {
    bool esPrimera = (ultimaDeteccionContada == 0);
    bool yaPaso    = (millis() - ultimaDeteccionContada) >= SEPARACION_MINIMA_DETECCION_MS;

    if (esPrimera || yaPaso) {
      anotarDeteccion();
      nuevaDeteccion = true;
      registrar("PIR", "Deteccion contada: %u de %u en la ventana de %lu s",
                deteccionesEnVentana, DETECCIONES_PARA_ALARMA,
                (unsigned long)(VENTANA_DETECCIONES_MS / 1000));
      pedirRefrescoPantalla();
    } else if (LOG_PIR_DETALLADO) {
      uint32_t faltan = (SEPARACION_MINIMA_DETECCION_MS -
                         (millis() - ultimaDeteccionContada)) / 1000;
      registrar("PIR", "Movimiento no contado (faltan %lu s de separacion)",
                (unsigned long)faltan);
    }
  }

  // El parametro de la app refleja si hay actividad reciente, no el nivel
  // instantaneo del pin: asi no parpadea en la app cada vez que el sensor
  // suelta su pulso.
  bool hayActividad = (deteccionesEnVentana > 0);
  if (hayActividad != movimientoDetectado) {
    movimientoDetectado = hayActividad;
    if (dispositivoSeguridad) {
      dispositivoSeguridad->updateAndReportParam("Movimiento", movimientoDetectado);
    }
  }

  if (!alarmaArmada) {
    // Desarmada: el conteo sigue corriendo y se registra, pero no suena nada.
    if (nuevaDeteccion && deteccionesEnVentana >= DETECCIONES_PARA_ALARMA) {
      registrar("SEGURIDAD", "Se cumplio la regla, pero la alarma esta desarmada");
    }
    return;
  }

  if (alarmaSonando) {
    // Ya esta sonando: cada deteccion nueva reempuja el final 30 s adelante.
    if (nuevaDeteccion) iniciarAlarma();
    return;
  }

  if (deteccionesEnVentana >= DETECCIONES_PARA_ALARMA) {
    registrar("SEGURIDAD", "REGLA CUMPLIDA: %u detecciones en menos de %lu s",
              deteccionesEnVentana,
              (unsigned long)(VENTANA_DETECCIONES_MS / 1000));
    iniciarAlarma();
    notificar(ALERTA_INTRUSO,
              "Intruso en el invernadero: %u detecciones en menos de %lu s",
              deteccionesEnVentana,
              (unsigned long)(VENTANA_DETECCIONES_MS / 1000));

    // El conteo ya cumplio su proposito. Se reinicia para que, cuando la
    // alarma termine, haga falta otra secuencia completa para volver a
    // dispararla y no la reactive un solo movimiento suelto.
    deteccionesEnVentana = 0;
  }
}

void revisarWatchdogRiego() {
  if (!estadoBomba) return;
  if ((millis() - tiempoInicioRiego) > TIEMPO_RIEGO_MAXIMO_MS) {
    registrar("SEGURIDAD", "Bomba cortada por watchdog (3 min continuos)");
    aplicarBomba(false);
    if (dispositivoBomba) {
      dispositivoBomba->updateAndReportParam(ESP_RMAKER_DEF_POWER_NAME, false);
    }

    // Llegar al watchdog significa que la bomba estuvo 3 minutos seguidos sin
    // que el suelo alcanzara el objetivo. Eso NO es riego normal: apunta a
    // una manguera desconectada, un goteo tapado o la sonda mal enterrada.
    // Es una averia que el sistema no puede corregir solo, asi que se avisa.
    notificar(ALERTA_RIEGO_FALLIDO,
              "Rego 3 min y el suelo sigue al %.0f%%. Revise mangueras y sonda.",
              humedadSuelo);
    mostrarMensajeTemporal("RIEGO SIN EFECTO", "REVISAR", "Mangueras o sonda", true);
  }
}

// ----------------------------------------------------------------------------
// Boton BOOT: solo reset
// ----------------------------------------------------------------------------
void gestionarBotonReinicio() {
  static bool botonPresionado = false;
  static uint32_t tiempoInicioPulsacion = 0;

  bool estadoActual = (digitalRead(PIN_BOTON_CONFIG) == LOW);
  uint32_t ahora = millis();

  if (estadoActual && !botonPresionado) {
    tiempoInicioPulsacion = ahora;
  } else if (!estadoActual && botonPresionado) {
    uint32_t duracion = ahora - tiempoInicioPulsacion;
    if (duracion > TIEMPO_RESET_FABRICA_MS) {
      registrar("SISTEMA", "Reset de fabrica...");
      RMakerFactoryReset(2);
    } else if (duracion > TIEMPO_RESET_WIFI_MS) {
      registrar("SISTEMA", "Reset de credenciales Wi-Fi...");
      RMakerWiFiReset(2);
    }
  }
  botonPresionado = estadoActual;
}

// ----------------------------------------------------------------------------
// Dispositivos RainMaker
// ----------------------------------------------------------------------------
void crearDispositivosRainMaker(Node &nodo) {
  dispositivoLed = new Switch("LED de Prueba", nullptr);
  if (dispositivoLed) {
    dispositivoLed->addCb(gestionarEscrituraRainMaker);
    nodo.addDevice(*dispositivoLed);
  }

  dispositivoVentilador = new Switch("Ventilador", nullptr);
  if (dispositivoVentilador) {
    dispositivoVentilador->addCb(gestionarEscrituraRainMaker);
    nodo.addDevice(*dispositivoVentilador);
  }

  dispositivoBomba = new Switch("Bomba de Agua", nullptr);
  if (dispositivoBomba) {
    // El nivel del tanque vive en el mismo dispositivo que la bomba, porque
    // es la condicion que decide si puede arrancar. Verlos juntos en la app
    // explica por si solo por que el riego no se activo.
    // Tipo propio: RainMaker no define uno estandar para "nivel de deposito",
    // y usar uno reservado traeria el mismo problema que ya tuvimos con
    // esp.param.name en el control de acceso.
    Param pNivel("Nivel Tanque", "custom.param.nivel", value(0.0f),
                 PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
    dispositivoBomba->addParam(pNivel);

    dispositivoBomba->addCb(gestionarEscrituraRainMaker);
    nodo.addDevice(*dispositivoBomba);
  }

  dispositivoLuz = new Switch("Luz de Cultivo", nullptr);
  if (dispositivoLuz) {
    dispositivoLuz->addCb(gestionarEscrituraRainMaker);
    nodo.addDevice(*dispositivoLuz);
  }

  dispositivoChapa = new Switch("Chapa de Acceso", nullptr);
  if (dispositivoChapa) {
    dispositivoChapa->addCb(gestionarEscrituraRainMaker);
    nodo.addDevice(*dispositivoChapa);
  }

  dispositivoClima = new Device("Clima", "esp.device.sensor", nullptr);
  if (dispositivoClima) {
    dispositivoClima->addNameParam();
    Param pTemp("Temperatura", ESP_RMAKER_PARAM_TEMPERATURE, value(0.0f),
                PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
    dispositivoClima->addParam(pTemp);
    Param pHum("Humedad Ambiente", "esp.param.humidity", value(0.0f),
               PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
    dispositivoClima->addParam(pHum);
    Param pLux("Luz Ambiental", "esp.param.illuminance", value(0.0f),
               PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
    dispositivoClima->addParam(pLux);

    // Humedad del suelo: es la variable que gobierna el riego, asi que
    // interesa verla en el historico para ajustar los umbrales.
    Param pSuelo("Humedad Suelo", "esp.param.humidity", value(0.0f),
                 PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
    dispositivoClima->addParam(pSuelo);

    nodo.addDevice(*dispositivoClima);
  }

  dispositivoAire = new Device("Calidad de Aire", "esp.device.sensor", nullptr);
  if (dispositivoAire) {
    dispositivoAire->addNameParam();
    Param pAire("Indice Aire", "esp.param.air-quality", value(0),
                PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
    dispositivoAire->addParam(pAire);
    Param pCO("Indice CO", "esp.param.co-level", value(0),
              PROP_FLAG_READ | PROP_FLAG_TIME_SERIES);
    dispositivoAire->addParam(pCO);
    nodo.addDevice(*dispositivoAire);
  }

  dispositivoSeguridad = new Device("Seguridad", "esp.device.security", nullptr);
  if (dispositivoSeguridad) {
    dispositivoSeguridad->addNameParam();

    Param pArmada("Alarma Armada", ESP_RMAKER_PARAM_TOGGLE, value(false),
                  PROP_FLAG_READ | PROP_FLAG_WRITE);
    pArmada.addUIType(ESP_RMAKER_UI_TOGGLE);
    dispositivoSeguridad->addParam(pArmada);

    Param pMov("Movimiento", "esp.param.motion", value(false), PROP_FLAG_READ);
    dispositivoSeguridad->addParam(pMov);

    // Permite escuchar el buzzer sin tener que disparar la alarma completa
    Param pProbar("Probar Buzzer", ESP_RMAKER_PARAM_TOGGLE, value(false),
                  PROP_FLAG_READ | PROP_FLAG_WRITE);
    pProbar.addUIType(ESP_RMAKER_UI_TOGGLE);
    dispositivoSeguridad->addParam(pProbar);

    // Mismo motivo que en Control de Acceso: sin parametro primario la app no
    // tiene que poner en el mosaico del dispositivo.
    dispositivoSeguridad->assignPrimaryParam(
        dispositivoSeguridad->getParamByName("Alarma Armada"));

    dispositivoSeguridad->addCb(gestionarEscrituraRainMaker);
    nodo.addDevice(*dispositivoSeguridad);
  }

  // --- Control de Acceso: enrolar y revocar huellas desde la app ---
  dispositivoAcceso = new Device("Control de Acceso", "esp.device.other", nullptr);
  if (dispositivoAcceso) {
    dispositivoAcceso->addNameParam();

    // Campo de texto donde se escribe el nombre de la persona. Sobre ese
    // nombre actuan "Modo Enrolar" y "Borrar Huella".
    //
    // El tipo es TIPO_TEXTO, propio, NO "esp.param.name". Ese tipo lo reserva
    // RainMaker para el nombre del dispositivo, y al tener tres parametros
    // declarados con el mismo tipo reservado la app no sabia cual era cual:
    // de ahi venia el comportamiento raro del campo de nombre.
    Param pNombre("Nombre", TIPO_TEXTO, value(""),
                  PROP_FLAG_READ | PROP_FLAG_WRITE);
    pNombre.addUIType(ESP_RMAKER_UI_TEXT);
    dispositivoAcceso->addParam(pNombre);

    Param pEnrolar("Modo Enrolar", ESP_RMAKER_PARAM_TOGGLE, value(false),
                   PROP_FLAG_READ | PROP_FLAG_WRITE);
    pEnrolar.addUIType(ESP_RMAKER_UI_TOGGLE);
    dispositivoAcceso->addParam(pEnrolar);

    Param pBorrar("Borrar Huella", ESP_RMAKER_PARAM_TOGGLE, value(false),
                  PROP_FLAG_READ | PROP_FLAG_WRITE);
    pBorrar.addUIType(ESP_RMAKER_UI_TOGGLE);
    dispositivoAcceso->addParam(pBorrar);

    // Salida de emergencia cuando el sensor y el registro de nombres quedaron
    // descuadrados: vacia ambos para volver a empezar desde cero.
    Param pBorrarTodas("Borrar Todas", ESP_RMAKER_PARAM_TOGGLE, value(false),
                       PROP_FLAG_READ | PROP_FLAG_WRITE);
    pBorrarTodas.addUIType(ESP_RMAKER_UI_TOGGLE);
    dispositivoAcceso->addParam(pBorrarTodas);

    // Texto de estado: dice en la app si el enrolamiento va en el paso 1 o 2,
    // por que se rechazo un nombre, o si el lector esta bloqueado. Antes esos
    // mensajes solo existian en el monitor serial.
    Param pEstado("Estado", TIPO_TEXTO, value("Iniciando"), PROP_FLAG_READ);
    dispositivoAcceso->addParam(pEstado);

    Param pUltimo("Ultimo Acceso", TIPO_TEXTO, value("-"), PROP_FLAG_READ);
    dispositivoAcceso->addParam(pUltimo);

    Param pLista("Registradas", TIPO_TEXTO, value("(vacio)"), PROP_FLAG_READ);
    dispositivoAcceso->addParam(pLista);

    // Un parametro de rango SIN limites declarados la app no lo puede dibujar.
    // Faltaba el addBounds y por eso el contador no aparecia.
    Param pTotal("Huellas Guardadas", ESP_RMAKER_PARAM_RANGE, value(0),
                 PROP_FLAG_READ);
    pTotal.addBounds(value(0), value(162), value(1));
    dispositivoAcceso->addParam(pTotal);

    // Sin parametro primario la app no sabe que mostrar en el mosaico del
    // dispositivo y la vista de detalle queda incompleta.
    dispositivoAcceso->assignPrimaryParam(dispositivoAcceso->getParamByName("Nombre"));

    dispositivoAcceso->addCb(gestionarEscrituraRainMaker);
    nodo.addDevice(*dispositivoAcceso);
  }
}

// ----------------------------------------------------------------------------
// setup
// ----------------------------------------------------------------------------
void setup() {
  // ==========================================================================
  // PRIMERISIMO: poner las cargas peligrosas en estado seguro.
  // ==========================================================================
  // Esto va ANTES de Serial.begin() y antes de cualquier mensaje, a proposito.
  //
  // El rele es activo en bajo y la chapa es fail-secure, asi que cada
  // milisegundo que GPIO18 pase flotando o en bajo es un milisegundo con la
  // chapa ABIERTA. Antes este bloque estaba despues del banner y de unas
  // veinte lineas de configuracion: a 115200 baudios eso son mas de 100 ms de
  // puerta abierta en cada arranque, mas los 300 ms del delay inicial.
  //
  // Lo que el software NO puede evitar es la ventana del gestor de arranque
  // del propio chip, de unos 200 a 300 ms antes de que setup() exista. Esa
  // parte se resuelve con hardware: ver la nota de seguridad al final del
  // archivo.
  pinMode(PIN_RELE_LUZ, OUTPUT);
  pinMode(PIN_RELE_CHAPA, OUTPUT);
  escribirRele(PIN_RELE_LUZ, false);
  escribirRele(PIN_RELE_CHAPA, false);   // Fail-secure: arranca bloqueada

  // El buzzer entra en el mismo grupo: tambien dispara en bajo, asi que cada
  // milisegundo con su pin en bajo es un milisegundo sonando y calentandose.
  if (BUZZER_ES_PASIVO) {
    ledcSetup(CANAL_PWM_BUZZER, FRECUENCIA_BUZZER_HZ, RESOLUCION_PWM_BUZZER);
    ledcAttachPin(PIN_BUZZER, CANAL_PWM_BUZZER);
  } else {
    pinMode(PIN_BUZZER, OUTPUT);
  }
  escribirBuzzer(false);

  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("################################################################");
  Serial.println("#          INVERNADERO INTELIGENTE - Frutos de Guatemala        #");
  Serial.println("################################################################");
  registrar("SISTEMA", "Arrancando...");
  registrar("CONFIG", "Ventilador ....... ON >%.0f C  o  humedad >%.0f %%",
            TEMP_VENTILADOR_ON_C, HUMEDAD_VENTILADOR_ON);
  registrar("CONFIG", "Ventilador ....... OFF <%.0f C  y  humedad <%.0f %%",
            TEMP_VENTILADOR_OFF_C, HUMEDAD_VENTILADOR_ON);
  registrar("CONFIG", "Bomba ............ ON <%.0f %%  /  OFF >=%.0f %% de SUELO",
            SUELO_BOMBA_ON, SUELO_BOMBA_OFF);
  registrar("CONFIG", "Tanque ........... no riega por debajo del %.0f %%",
            NIVEL_TANQUE_MINIMO);
  registrar("CONFIG", "Tanque rango ..... lleno a %.0f cm / vacio a %.0f cm",
            DIST_TANQUE_LLENO_CM, DIST_TANQUE_VACIO_CM);
  if (!PROTECCION_TANQUE_ACTIVA) {
    registrar("CONFIG", "*** PROTECCION DE TANQUE DESACTIVADA (modo pruebas) ***");
    registrar("CONFIG", "*** La bomba puede arrancar en seco. Volver a true. ***");
  }
  registrar("CONFIG", "Alarma PIR ....... %u detecciones en %lu s, 1 conteo cada %lu s",
            DETECCIONES_PARA_ALARMA,
            (unsigned long)(VENTANA_DETECCIONES_MS / 1000),
            (unsigned long)(SEPARACION_MINIMA_DETECCION_MS / 1000));
  registrar("CONFIG", "Luz de cultivo ... ON <%.0f lux  /  OFF >=%.0f lux",
            LUX_LUZ_ON, LUX_LUZ_OFF);
  registrar("CONFIG", "MQ135 alerta ..... >%d /4095", UMBRAL_AIRE_MALO_RAW);
  registrar("CONFIG", "MQ7 alerta ....... >%d /4095", UMBRAL_CO_EMERGENCIA_RAW);
  registrar("CONFIG", "Chapa ............ cierra sola a los %lu ms",
            (unsigned long)TIEMPO_CHAPA_ABIERTA_MS);
  registrar("CONFIG", "Lector ........... bloqueo de 60 s tras %d fallos",
            INTENTOS_FALLIDOS_MAX);
  registrar("CONFIG", "Parpadeo alarma .. cada %lu ms, dura %lu s tras el ultimo aviso",
            (unsigned long)INTERVALO_PARPADEO_MS,
            (unsigned long)(DURACION_ALARMA_MS / 1000));
  if (BUZZER_ES_PASIVO) {
    registrar("CONFIG", "Buzzer ........... GPIO%d, PASIVO, tono de %lu Hz por LEDC canal %u",
              PIN_BUZZER, (unsigned long)FRECUENCIA_BUZZER_HZ, CANAL_PWM_BUZZER);
    registrar("CONFIG", "                   reposo en %s, señal via conversor",
              BUZZER_PASIVO_REPOSO_ALTO ? "ALTO" : "BAJO");
  } else {
    registrar("CONFIG", "Buzzer ........... GPIO%d, ACTIVO, dispara en %s",
              PIN_BUZZER, BUZZER_ACTIVO_EN_BAJO ? "BAJO" : "ALTO");
  }
  registrar("CONFIG", "PIR .............. GPIO%d, señal directa de 3.3V", PIN_PIR);
  registrar("CONFIG", "OLED ............. I2C 0x%02X, compartido con el BH1750",
            OLED_DIRECCION);
  Serial.println();

  // Los reles y el buzzer ya quedaron en estado seguro al inicio de setup(),
  // antes incluso de abrir el puerto serie.

  if (MODO_BARRIDO_BUZZER && BUZZER_ES_PASIVO) {
    // Recorre las frecuencias de resonancia tipicas de un buzzer magnetico.
    // Cada una suena 400 ms y se anuncia en el log, para poder oir y leer al
    // mismo tiempo cual sale mas fuerte.
    const uint32_t frecuenciasPrueba[] = {
      1000, 1500, 2000, 2048, 2300, 2700, 3000, 3500, 4000, 4500
    };
    constexpr uint8_t totalFrecuencias =
        sizeof(frecuenciasPrueba) / sizeof(frecuenciasPrueba[0]);

    registrarSeparador("BARRIDO DE FRECUENCIAS DEL BUZZER");
    registrar("BUZZER", "Escucha en cual suena MAS FUERTE y anotala.");

    for (uint8_t i = 0; i < totalFrecuencias; i++) {
      registrar("BUZZER", "  %2u/%u ... %lu Hz",
                i + 1, totalFrecuencias, (unsigned long)frecuenciasPrueba[i]);
      ledcWriteTone(CANAL_PWM_BUZZER, frecuenciasPrueba[i]);
      delay(400);
      escribirBuzzer(false);
      delay(250);
    }

    registrar("BUZZER", "Fin del barrido. Pon la mejor en FRECUENCIA_BUZZER_HZ");
    registrar("BUZZER", "y luego MODO_BARRIDO_BUZZER en false.");
    Serial.println();
  } else {
    // Pitido corto para confirmar que el buzzer responde, sin depender de
    // la app. 120 ms no alcanzan a calentar nada ni en el peor caso.
    escribirBuzzer(true);
    delay(120);
    escribirBuzzer(false);
  }

  pinMode(PIN_BOTON_CONFIG, INPUT);
  pinMode(PIN_LED_PRUEBA, OUTPUT);
  pinMode(PIN_BOMBA_IN1, OUTPUT);
  pinMode(PIN_BOMBA_IN2, OUTPUT);
  pinMode(PIN_VENT_IN3, OUTPUT);
  pinMode(PIN_VENT_IN4, OUTPUT);
  pinMode(PIN_MQ135, INPUT);
  pinMode(PIN_MQ7, INPUT);
  pinMode(PIN_PIR, INPUT);

  // FC-28: la sonda arranca SIN alimentacion y solo se energiza al medir.
  pinMode(PIN_SUELO_AO, INPUT);
  if (ALIMENTAR_SUELO_POR_GPIO) {
    pinMode(PIN_SUELO_VCC, OUTPUT);
    digitalWrite(PIN_SUELO_VCC, LOW);
  }

  // HC-SR04
  pinMode(PIN_ULTRA_TRIG, OUTPUT);
  digitalWrite(PIN_ULTRA_TRIG, LOW);
  pinMode(PIN_ULTRA_ECHO, INPUT);

  // Direccion fija de la bomba: sus IN quedan siempre opuestos.
  digitalWrite(PIN_BOMBA_IN1, HIGH);
  digitalWrite(PIN_BOMBA_IN2, LOW);

  ledcSetup(CANAL_PWM, FRECUENCIA_PWM, RESOLUCION_PWM);
  ledcAttachPin(PIN_BOMBA_ENA, CANAL_PWM);

  digitalWrite(PIN_LED_PRUEBA, LOW);
  aplicarVentilador(false);
  aplicarBomba(false);

  dht11.begin();

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  // --- Pantalla OLED de la puerta ---
  // Se inicia antes del BH1750 para que la vista de bienvenida aparezca
  // mientras ese sensor hace sus reintentos.
  // Los dos ultimos argumentos importan:
  //   reset = true       -> inofensivo, este modulo no tiene pin de reset
  //   periphBegin = false -> NO volver a llamar Wire.begin(). La libreria lo
  //                          hace sin argumentos y eso reconfiguraria el bus
  //                          que ya abrimos arriba con los pines correctos.
  oledDisponible = pantalla.begin(SSD1306_SWITCHCAPVCC, OLED_DIRECCION, true, false);

  // La libreria de Adafruit sube el bus a 400 kHz por su cuenta. Aqui el bus
  // es compartido y pasa por el conversor de nivel: con las resistencias
  // pull-up de los dos modulos en paralelo los flancos se vuelven lentos y a
  // 400 kHz el BH1750 empieza a fallar. Se regresa a 100 kHz a proposito.
  Wire.setClock(100000);

  if (oledDisponible) {
    registrar("OLED", "Pantalla detectada en 0x%02X", OLED_DIRECCION);
    dibujarBienvenida();
  } else {
    registrar("OLED", "NO responde en 0x%02X. El sistema sigue sin pantalla.",
              OLED_DIRECCION);
    registrar("OLED", "Revisar SDA/SCL compartidos con el BH1750 y los 5V.");
  }

  // El BH1750 necesita un momento tras recibir alimentacion antes de
  // responder, y se reintenta en vez de darlo por perdido al primer fallo.
  delay(1000);
  for (uint8_t intento = 1; intento <= 3 && !bh1750Disponible; intento++) {
    bh1750Disponible = sensorLuz.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
    if (!bh1750Disponible) {
      registrar("BH1750", "Intento %d fallido, reintentando...", intento);
      delay(500);
    }
  }
  registrar("BH1750", "%s", bh1750Disponible
                ? "Sensor detectado correctamente."
                : "NO respondio despues de 3 intentos.");

  // --- Primera medida del tanque ---
  // Se hace antes de arrancar RainMaker para que tanqueConAgua tenga un valor
  // real desde el primer ciclo. Si se dejara en su valor inicial, la bomba
  // podria arrancar antes de la primera medicion.
  actualizarNivelTanque();

  // --- Registro de nombres ---
  // Se carga SIEMPRE, antes de tocar el sensor y sin importar si responde.
  // Si solo se cargara cuando el lector esta disponible, un arranque con el
  // sensor desconectado dejaria el arreglo vacio, y el primer guardado
  // posterior borraria todos los nombres de la NVS.
  cargarRegistroPersonas();
  registrar("ACCESO", "Registro cargado: %u nombre(s) en memoria", totalPersonas);

  // --- Lector de huella ---
  serialHuella.begin(57600, SERIAL_8N1, PIN_HUELLA_RX, PIN_HUELLA_TX);
  delay(100);
  lector.begin(57600);
  delay(300);
  lectorDisponible = lector.verifyPassword();
  if (lectorDisponible) {
    actualizarConteoHuellas();
    char lista[MAX_PERSONAS * (LARGO_NOMBRE + 2)];
    construirListaPersonas(lista, sizeof(lista));
    registrar("AS608", "Lector detectado. %d plantillas en el sensor.", huellasGuardadas);
    registrar("AS608", "Registradas: %s", lista);
    verificarConsistenciaRegistro();
  } else {
    registrar("AS608", "NO responde. Revisar TX->GPIO19 / RX->GPIO23 y 3V3.");
    publicarEstadoAcceso("Sin lector de huella");
  }

  Node nodoRainMaker = RMaker.initNode("Invernadero Inteligente");
  crearDispositivosRainMaker(nodoRainMaker);

  // Los dispositivos acaban de existir, asi que ahora si se puede volcar el
  // estado que ya se calculo arriba (nombres cargados, consistencia del
  // sensor). Hacerlo antes de RMaker.start() deja los valores listos para
  // que se publiquen en cuanto el nodo se conecte.
  reportarEstadoAcceso();

  // El historial (time series) necesita hora sincronizada.
  RMaker.enableTZService();
  RMaker.enableSystemService(SYSTEM_SERV_FLAGS_ALL, 2, 2, 2);

  // NOTA: aqui NO va ningun WiFi.scanNetworks() ni WiFi.mode(). Ese
  // pre-escaneo bloqueaba ~6 s entre initNode() y start().
  RMaker.start();
  WiFi.onEvent(gestionarEventoAprovisionamiento);

  WiFiProv.beginProvision(WIFI_PROV_SCHEME_SOFTAP, WIFI_PROV_SCHEME_HANDLER_NONE,
                          WIFI_PROV_SECURITY_1, CLAVE_POP, NOMBRE_SERVICIO_PROV);

  // Perro guardian, al final para que no muerda durante el arranque, que es
  // la parte mas lenta (aprovisionamiento, reintentos del BH1750, lector).
  // Si estas lineas no compilan, tu version del core usa la API nueva con
  // struct de configuracion: avisame y las cambio.
  esp_task_wdt_init(TIMEOUT_WATCHDOG_S, true);   // true = reinicia al morder
  esp_task_wdt_add(NULL);                         // Vigila la tarea del loop
  registrar("SISTEMA", "Perro guardian activo: reinicia si el loop se cuelga %lu s",
            (unsigned long)TIMEOUT_WATCHDOG_S);
}

// ----------------------------------------------------------------------------
// loop — todo no bloqueante; nada aqui retiene la CPU mas de unos ms
// ----------------------------------------------------------------------------
void loop() {
  uint32_t ahora = millis();

  // Avisa al perro guardian de que el loop sigue vivo. Si esta linea deja de
  // ejecutarse mas de TIMEOUT_WATCHDOG_S, el chip se reinicia y la chapa
  // vuelve a su estado bloqueado.
  esp_task_wdt_reset();

  gestionarBotonReinicio();
  atenderSolicitudAcceso();   // Ejecuta lo que pidio la app en el envio previo
  revisarRiego();             // Pulsos de riego: necesita resolucion de 1 s
  revisarWatchdogRiego();
  revisarCierreChapa();
  revisarAlarma();
  revisarPruebaBuzzer();
  revisarPantalla();

  if (ahora - ultimaLecturaClima > INTERVALO_CLIMA_MS) {
    ultimaLecturaClima = ahora;
    actualizarClima();
  }
  if (ahora - ultimaLecturaAire > INTERVALO_AIRE_MS) {
    ultimaLecturaAire = ahora;
    actualizarCalidadAire();
  }
  if (ahora - ultimaLecturaTanque > INTERVALO_TANQUE_MS) {
    ultimaLecturaTanque = ahora;
    actualizarNivelTanque();
  }

  // Modo calibracion del FC-28: lectura rapida para encontrar los dos
  // extremos de la escala. No altera el control de riego, que sigue usando
  // la lectura del ciclo de clima.
  if (MODO_CALIBRAR_SUELO) {
    static uint32_t ultimaCalibracion = 0;
    if (ahora - ultimaCalibracion > INTERVALO_CALIBRACION_MS) {
      ultimaCalibracion = ahora;

      if (ALIMENTAR_SUELO_POR_GPIO) {
        digitalWrite(PIN_SUELO_VCC, HIGH);
        delay(200);
      }
      uint32_t suma = 0;
      for (uint8_t i = 0; i < 8; i++) { suma += analogRead(PIN_SUELO_AO); delay(5); }
      int crudo = suma / 8;
      if (ALIMENTAR_SUELO_POR_GPIO) digitalWrite(PIN_SUELO_VCC, LOW);

      // Se muestra tambien a que porcentaje equivale con las constantes
      // ACTUALES, para ver de inmediato si la escala esta bien puesta.
      float pct = (float)(SUELO_CRUDO_SECO - crudo) * 100.0f /
                  (float)(SUELO_CRUDO_SECO - SUELO_CRUDO_HUMEDO);
      if (pct < 0.0f)   pct = 0.0f;
      if (pct > 100.0f) pct = 100.0f;

      registrar("CALIBRAR", "crudo %4d  ->  %5.1f %%   [escala actual %d..%d]",
                crudo, pct, SUELO_CRUDO_SECO, SUELO_CRUDO_HUMEDO);
    }
  }
  if (ahora - ultimaRevisionSeguridad > INTERVALO_SEGURIDAD_MS) {
    ultimaRevisionSeguridad = ahora;
    actualizarSeguridad();
  }
  if (ahora - ultimaRevisionHuella > INTERVALO_HUELLA_MS) {
    ultimaRevisionHuella = ahora;
    revisarHuella();
  }

  delay(10);
}

// ============================================================================
// NOTA DE SEGURIDAD — LA CHAPA SE ABRE AL PERDER ALIMENTACION
// ============================================================================
// SINTOMA OBSERVADO: al desconectar el ESP32, la chapa se abre unos segundos.
//
// CAUSA. El modulo de rele es ACTIVO EN BAJO y se alimenta de la fuente
// externa de 5V, mientras el ESP32 se alimenta por USB. Son dos dominios de
// energia distintos. Cuando el ESP32 se apaga, GPIO18 deja de ser una salida
// y queda en alta impedancia; el pin IN del modulo, sin nadie que lo sujete,
// deriva hacia cero. Para un modulo activo en bajo, cero es la orden de
// ACTIVAR: el rele cierra y la chapa recibe sus 12V.
//
// El mismo mecanismo actua en cada arranque y en cada reinicio, durante los
// 200 a 300 ms que tarda el gestor de arranque del chip antes de que exista
// setup(). Esa ventana NO se puede cerrar por software.
//
// LO QUE SE HIZO EN ESTE PROGRAMA:
//
//   1. El estado seguro de los reles se escribe en las PRIMERAS lineas de
//      setup(), antes de Serial.begin() y antes de cualquier mensaje. Antes
//      estaba despues del banner y de unas veinte lineas de log: mas de
//      400 ms de puerta abierta en cada arranque.
//
//   2. Perro guardian sobre la tarea del loop. revisarCierreChapa() es quien
//      vuelve a bloquear la chapa a los 4 s, y corre dentro del loop: si el
//      loop se colgaba con la chapa abierta, se quedaba abierta para siempre.
//      Ahora un cuelgue se convierte en reinicio, y al reiniciar la chapa
//      queda bloqueada.
//
//   3. Toda apertura remota desde la app se registra y se notifica al
//      telefono. Es el unico camino de acceso sin identidad del sistema.
//
// LO QUE FALTA, Y ES DE HARDWARE. El software no puede sujetar un pin
// mientras esta apagado, asi que estas dos medidas son imprescindibles:
//
//   A) RESISTENCIA DE 10k ENTRE GPIO18 Y EL VCC DEL MODULO DE RELE.
//      Mientras el ESP32 este apagado o arrancando, esa resistencia mantiene
//      el pin IN en alto, es decir en INACTIVO, y la chapa sigue bloqueada.
//      Con el ESP32 encendido no estorba: el GPIO la domina sin esfuerzo.
//      Es la medida que cierra la ventana del gestor de arranque.
//
//   B) ALIMENTAR EL MODULO DE RELE DEL MISMO DOMINIO QUE EL ESP32.
//      Si los dos salen de la misma fuente, perder la alimentacion apaga
//      tambien la bobina del rele, sus contactos vuelven a reposo y la chapa
//      se bloquea. Un unico fallo deja el sistema en estado seguro, que es la
//      propiedad que se busca en una cerradura fail-secure.
//
// RIESGO RESIDUAL QUE NO SE MITIGA, y conviene decirlo: quien tenga acceso
// fisico al tablero puede abrir la chapa puenteando el rele o cortando su
// alimentacion de 12V. Ninguna medida de firmware cubre eso. La defensa ahi
// es fisica: tablero cerrado con llave y cableado de 12V fuera de alcance.
// ============================================================================
