# Invernadero Inteligente — Frutos de Guatemala

Sistema de automatización, control microclimático y seguridad biométrica para
invernadero, construido sobre un ESP32 y controlado en remoto desde ESP
RainMaker.

Proyecto final del curso de **Electrónica Digital** (6.º ciclo), Universidad
Mariano Gálvez de Guatemala. Cliente: Frutos de Guatemala, Zacapa.

---

## Qué hace

| Subsistema | Sensor que decide | Actuador |
|---|---|---|
| **Riego** | FC-28, humedad del suelo | Bomba por PWM, con riego por pulsos |
| **Protección de la bomba** | HC-SR04, nivel del tanque | Bloquea el riego si no hay agua |
| **Ventilación** | DHT11 + MQ-135 + MQ-7 | Ventilador a máxima potencia |
| **Iluminación** | BH1750, luz ambiental | Luz de cultivo por relé |
| **Acceso** | AS608, huella dactilar | Chapa solenoide fail-secure 12 V |
| **Alarma** | HC-SR501, movimiento | Buzzer + parpadeo de la luz |
| **Indicación local** | — | Pantalla OLED en la puerta |

Todo se supervisa desde la app de ESP RainMaker, con histórico de las variables
y notificaciones push para los eventos que exigen ir al invernadero.

---

## Contenido del repositorio

```
.
├── invernadero_principal/
│   └── invernadero_principal.ino      Programa completo del sistema
├── pruebas/                           Sketches de diagnóstico, uno por sensor
│   ├── escaner_i2c/                   Lista las direcciones del bus I2C
│   ├── prueba_blink/                  Verifica que el ESP32 esté vivo
│   ├── prueba_wifi_scan/              Verifica la radio Wi-Fi
│   ├── prueba_huella/                 Menú de enrolar, verificar y borrar
│   ├── prueba_oled/                   Pantalla SSD1306 (Arduino Mega)
│   ├── prueba_pir/                    Sensor de movimiento
│   ├── prueba_ultrasonido/            HC-SR04 (Arduino Mega)
│   └── prueba_buzzer/                 Buzzer activo o pasivo (Arduino Mega)
└── docs/
    ├── HARDWARE.md                    Mapa de pines, cableado y adaptación de niveles
    ├── CALIBRACION.md                 Procedimientos y valores medidos en sitio
    ├── SEGURIDAD.md                   Análisis de riesgos y mitigaciones
    └── DIAGNOSTICO.md                 Guía de fallas a partir del log
```

Los sketches de `pruebas/` no son descartables: son la herramienta con la que
se aisló cada falla durante el desarrollo, y siguen siendo la forma más rápida
de decidir si un problema está en un sensor, en el cableado o en el programa.
Varios corren en un **Arduino Mega** a propósito, porque sus salidas de 5 V
permiten probar un módulo sin que la adaptación de niveles del ESP32 sea una
variable. Ver [DIAGNOSTICO.md](docs/DIAGNOSTICO.md).

---

## Requisitos

**Placa:** ESP32 DEVKIT V1 (módulo ESP32-WROOM-32, chip ESP32-D0WD-V3).

**Entorno:** Arduino IDE con el core `esp32` de Espressif, rama **2.x**.

> El programa usa `ledcSetup()` y `ledcAttachPin()`, que el core 3.x reemplazó
> por `ledcAttach()`. Y la inicialización del perro guardián usa la firma
> `esp_task_wdt_init(timeout, panic)` de IDF 4.x. En el core 3.x hay que
> adaptar ambas cosas.

**Librerías** (Gestor de Librerías del IDE):

| Librería | Autor |
|---|---|
| ESP RainMaker | incluida en el core esp32 |
| DHT sensor library | Adafruit |
| Adafruit Unified Sensor | Adafruit |
| BH1750 | Christopher Laws |
| Adafruit Fingerprint Sensor Library | Adafruit |
| Adafruit SSD1306 | Adafruit |
| Adafruit GFX Library | Adafruit |

**Configuración obligatoria en el IDE**, bajo Herramientas:

| Opción | Valor |
|---|---|
| Board | ESP32 Dev Module |
| Partition Scheme | **RainMaker 4MB No OTA** |
| Erase All Flash Before Sketch Upload | **Enable** (solo la primera vez) |

El esquema de particiones es obligatorio: sin una partición `fctry`, RainMaker
falla al arrancar con `esp_rmaker_fctry: NVS Flash init failed`. Y el esquema
con OTA no deja espacio para este sketch.

---

## Puesta en marcha

### 1. Cableado

Sigue [docs/HARDWARE.md](docs/HARDWARE.md). **Léelo antes de conectar nada**:
varios sensores necesitan adaptación de niveles y otros no la toleran, y el
criterio no es evidente.

### 2. Compilar y subir

Abre `invernadero_principal/invernadero_principal.ino`, aplica la
configuración del IDE de arriba y súbelo. El monitor serial va a **115200
baudios**.

### 3. Vincular con la app

1. Instala **ESP RainMaker** (Android o iOS) e inicia sesión.
2. En el monitor serial aparece un código QR al arrancar.
3. En la app, añade un dispositivo y escanea ese QR.
4. Si pide la clave de aprovisionamiento (PoP), es `abcd1234`.

Para borrar las credenciales Wi-Fi y volver a vincular: mantén pulsado el
botón **BOOT** entre 3 y 10 segundos. Más de 10 segundos hace un reset de
fábrica, que además borra el nodo. **No uses el botón EN**: ese solo reinicia.

### 4. Calibrar

El sistema **no funciona bien sin calibrar**: las constantes de fábrica son
valores de ejemplo. Sigue [docs/CALIBRACION.md](docs/CALIBRACION.md) para el
sensor de suelo, el tanque y los sensores de gas.

### 5. Registrar huellas

En la app, dispositivo **Control de Acceso**:

1. Escribe el nombre en el campo **Nombre**.
2. Activa **Modo Enrolar**.
3. Sigue las instrucciones en la pantalla de la puerta: dedo, retirar, mismo
   dedo otra vez.

El campo **Estado** informa cada paso y cada rechazo. Para revocar un acceso,
escribe el nombre y pulsa **Borrar Huella**.

> Si el sensor ya tiene huellas enroladas con `pruebas/prueba_huella`, esas no
> tienen nombre asociado y el sistema les **niega** el paso: un acceso sin
> identidad no se puede auditar. Usa **Borrar Todas** una vez y vuelve a
> enrolar a todos desde la app.

---

## Lo que ve el usuario

### En la puerta (OLED)

La pantalla decide qué mostrar según el estado del sistema, con esta prioridad:

1. Mensaje temporal — acceso concedido con el nombre, denegado con el número
   de intento, huella guardada, acceso revocado
2. Alarma activa
3. Lector bloqueado, con cuenta regresiva
4. Enrolamiento en curso, con el paso y el tiempo restante
5. Puerta abierta, con cuenta regresiva del cierre
6. Reposo: `MODO DETECCION`, fallos acumulados, movimiento contado, humedad
   del suelo, nivel del tanque, clima y estado de la red

### En la app

Nueve dispositivos: **LED de Prueba**, **Ventilador**, **Bomba de Agua**,
**Luz de Cultivo**, **Chapa de Acceso**, **Clima**, **Calidad de Aire**,
**Seguridad** y **Control de Acceso**.

Siete variables se guardan con histórico: temperatura, humedad del aire, luz
ambiental, humedad del suelo, índice de aire, índice de CO y nivel del tanque.

### Notificaciones push

Solo lo que obliga a ir al invernadero. Cada tipo tiene su propia espera, y
además hay una separación global de un minuto para que dos problemas
simultáneos no manden dos avisos a la vez.

| Evento | Espera entre avisos |
|---|---|
| Intruso detectado | 10 min |
| CO crítico | 15 min |
| Tanque bajo el mínimo | 1 h |
| Riego sin efecto en el suelo | 2 h |
| Lector bloqueado por intentos fallidos | 30 min |
| Puerta abierta en remoto | 1 min |

Deliberadamente **no** se notifican: el aire malo del MQ-135 (el ventilador lo
resuelve solo), los riegos normales, los accesos concedidos y las lecturas de
clima. Notificar algo que el sistema ya corrigió entrena al usuario a ignorar
los avisos, y entonces se pierde el que sí importaba.

---

## Reglas de automatización

### Riego — FC-28 y HC-SR04

Riega cuando la humedad del suelo baja del **35 %**, hasta alcanzar el **60 %**.

El riego va **por pulsos**: un segundo de bomba, diez segundos de espera para
que el agua se reparta, y vuelve a medir. Regar de corrido siempre se pasa de
largo, porque el agua tarda en llegar a la sonda y mientras tanto el sensor
sigue marcando seco.

Tope de **20 pulsos** por tanda. Si tras eso la humedad no subió, el agua no
está llegando a la tierra y se avisa.

La bomba **no arranca** si el tanque está por debajo del 15 %, venga la orden
del automático o de la app. Si el HC-SR04 no responde, tampoco riega: es
preferible un riego perdido a una bomba quemada por marcha en seco.

### Ventilador — DHT11, MQ-135 y MQ-7

Enciende si se cumple **cualquiera** de estas:

| Condición | Umbral |
|---|---|
| Temperatura | > 30 °C |
| Humedad del aire | > 85 % |
| Índice de aire (MQ-135) | > 2000 |
| Índice de CO (MQ-7) | > 2500 |

Apaga solo si se cumplen **todas**: temperatura < 27 °C **y** humedad < 78 %
**y** los dos gases normales. Esa separación entre el umbral de entrada y el
de salida es la histéresis que evita que conmute sin parar en el límite.

### Luz de cultivo — BH1750

Enciende por debajo de **300 lux**, apaga por encima de **500 lux**. Mientras
la alarma esté sonando, el control por luz queda suspendido: la luz pertenece
al parpadeo disuasivo.

### Alarma — HC-SR501

Requiere el interruptor **Alarma Armada** activo. La regla es por **conteo de
eventos**, no por movimiento sostenido: hacen falta **3 detecciones dentro de
una ventana de 1 minuto**, con un mínimo de 15 segundos entre detecciones
contadas.

Un intruso que entra se queda dentro, no solo pasa. Y contar eventos en lugar
de exigir un nivel sostenido quita la dependencia del potenciómetro Tx del
sensor, que es hardware y no debería alterar el comportamiento del programa.

El primer minuto tras encender se ignora por completo: el HC-SR501 necesita
ese tiempo para estabilizar su referencia térmica y mientras tanto su salida
oscila sola.

Al disparar, la luz de cultivo parpadea cada 300 ms con el buzzer en fase,
durante 30 segundos desde la última detección. Al terminar, la luz vuelve
exactamente al estado que tenía antes.

### Acceso — AS608

Huella reconocida con nombre en el registro → abre la chapa 4 segundos y se
cierra sola. Cinco huellas no reconocidas seguidas → lector bloqueado 60
segundos y notificación.

La relación nombre ↔ ID vive en la NVS del ESP32 y sobrevive a reinicios y a
nuevas cargas del sketch, porque el AS608 solo almacena plantillas numeradas.

---

## Modos de diagnóstico

Banderas al inicio del sketch que se pueden activar sin tocar nada más:

| Bandera | Para qué |
|---|---|
| `MODO_CALIBRAR_SUELO` | Imprime el crudo del FC-28 cada 2 s |
| `MODO_BARRIDO_BUZZER` | Recorre frecuencias para hallar la resonancia |
| `LOG_PIR_DETALLADO` | Registra cada cambio de nivel del PIR |
| `PROTECCION_TANQUE_ACTIVA` | En `false` permite probar la bomba sin tanque |
| `ALIMENTAR_SUELO_POR_GPIO` | En `false` alimenta el FC-28 del riel de 3V3 |

`PROTECCION_TANQUE_ACTIVA = false` deja la bomba sin protección contra marcha
en seco. El arranque lo avisa en mayúsculas. **No la dejes así.**

---

## Advertencia de seguridad

Tal como está cableado en el prototipo, **la chapa se abre unos segundos si el
ESP32 pierde alimentación**. El programa mitiga lo que puede, pero el cierre
completo del hueco necesita dos medidas de hardware.

Lee [docs/SEGURIDAD.md](docs/SEGURIDAD.md) antes de instalar esto donde
realmente haya algo que proteger.

---

## Estado del proyecto

**Funcionando y verificado en banco:** riego por pulsos con FC-28, protección
de bomba con HC-SR04, ventilación, iluminación por BH1750, acceso por huella
con nombres, alarma por conteo, buzzer, pantalla OLED, notificaciones push e
histórico en la app.

**Pendiente:**

- `DIST_TANQUE_LLENO_CM` está sin medir. Solo afecta la escala del porcentaje,
  no el bloqueo por tanque vacío.
- Los umbrales de riego (35 % y 60 %) están sobre una referencia de aire y
  agua. Necesitan afinarse con la sonda enterrada en tierra real.
- Los umbrales de los MQ son valores de partida sin calibrar.
- Las dos medidas de hardware de [SEGURIDAD.md](docs/SEGURIDAD.md).
- Alimentar el ESP32 por VIN desde la fuente externa, en lugar del USB.

---

## Licencia

Sin licencia asignada todavía. Añade un archivo `LICENSE` antes de publicar
si quieres permitir que otros lo reutilicen.
