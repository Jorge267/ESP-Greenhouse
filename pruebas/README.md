# Sketches de diagnóstico

Cada sketch prueba **un solo módulo**, sin Wi-Fi, sin RainMaker y sin el resto
del sistema. Sirven para decidir si un problema está en el sensor, en el
cableado o en el programa principal.

Varios corren en un **Arduino Mega** a propósito: sus salidas son de 5 V, así
que permiten probar un módulo sin que la adaptación de niveles del ESP32 sea
una variable. Si un módulo funciona en el Mega y no en el ESP32, el problema
está en los niveles de tensión, no en el módulo.

El método completo y las fallas que se resolvieron con estos sketches están en
[../docs/DIAGNOSTICO.md](../docs/DIAGNOSTICO.md).

---

## Índice

| Sketch | Placa | Prueba | Pines |
|---|---|---|---|
| `prueba_blink` | ESP32 | Que el chip esté vivo | LED integrado, GPIO2 |
| `prueba_wifi_scan` | ESP32 | Que la radio Wi-Fi funcione | — |
| `escaner_i2c` | ESP32 | Direcciones del bus I2C | SDA 21, SCL 22 |
| `prueba_huella` | ESP32 | Lector AS608, menú completo | RX 19, TX 23 |
| `prueba_pir` | ESP32 | Sensor de movimiento | OUT a GPIO16 |
| `prueba_oled` | **Mega** | Pantalla SSD1306 | SDA 20, SCL 21 |
| `prueba_ultrasonido` | **Mega** | HC-SR04 | TRIG 9, ECHO 10 |
| `prueba_buzzer` | **Mega** | Buzzer activo o pasivo | S a pin 8 |

> Los pines de los sketches de prueba **no son los mismos** que los del
> programa principal. Están elegidos para que la prueba sea cómoda, no para
> coincidir con el montaje final.

---

## Qué hace cada uno

### `prueba_blink` — ESP32

Hace parpadear el LED integrado. Es la primera prueba cuando un ESP32 "no
funciona": si el LED parpadea, el chip y la carga del programa están bien, y el
problema está en otra parte.

### `prueba_wifi_scan` — ESP32

Escanea y lista las redes al alcance. Separa un problema de radio de un
problema de aprovisionamiento: si aquí aparecen redes, la radio funciona.

### `escaner_i2c` — ESP32

Recorre las direcciones del bus y reporta las que responden. Esperado en este
proyecto: `0x23` para el BH1750 y `0x3C` para la OLED.

Si un dispositivo I2C no responde en el programa principal pero sí aparece
aquí, el problema no es el cableado.

### `prueba_huella` — ESP32

Menú por el monitor serial: enrolar, verificar, borrar por ID, contar y vaciar
la base de datos del sensor.

> En el monitor serial, pon el final de línea en **"Nueva línea"** o **"Ambos
> NL & CR"**, o el menú no responde.

Importante: las huellas enroladas con este sketch **no tienen nombre
asociado**, y el programa principal les niega el paso por eso. Tras usarlo, hay
que usar **Borrar Todas** desde la app y volver a enrolar.

### `prueba_pir` — ESP32

Lee el PIR en **GPIO16** con `INPUT_PULLDOWN`, no en el GPIO39 del montaje
final. La diferencia es deliberada: GPIO16 tiene pull-down interno, así que un
cable suelto se lee como "sin movimiento" de forma definitiva, y eso distingue
"no hay señal" de "no hay movimiento".

Incluye 60 segundos de calentamiento y un diagnóstico automático si no registra
ningún cambio.

El OUT va **directo**, sin conversor: la salida del HC-SR501 ya es de 3.3 V.

### `prueba_oled` — Arduino Mega

Escanea el bus para encontrar la dirección real de la pantalla (`0x3C` o
`0x3D`) y corre una secuencia de pruebas visuales: texto en dos tamaños, marco
completo para buscar píxeles muertos en los bordes, pantalla totalmente
encendida para buscar puntos negros, figuras, inversión y los mensajes reales
del proyecto.

Incluye las comprobaciones previas a energizar: inspección visual y prueba de
continuidad entre VCC y GND.

### `prueba_ultrasonido` — Arduino Mega

ECHO va **directo** al pin: el Mega tolera sus 5 V. Eso saca el conversor de la
ecuación.

No usa `pulseIn()` a propósito, porque esa función devuelve `0` tanto si el eco
no llegó como si llegó mal. La medición está partida en pasos y reporta **en
cuál falla**:

| Diagnóstico | Significa |
|---|---|
| `SIN ECO` | El sensor no levantó la línea |
| `ECHO PEGADO EN ALTO` | La levantó y no la bajó |
| `ECHO YA ESTABA ALTO` | Cables invertidos |

Antes de empezar revisa el nivel de reposo de ECHO durante 200 lecturas, lo que
distingue línea correcta, línea fija en alto y línea flotando.

Cierra cada ciclo con un veredicto: sensor bien, sin respuesta, intermitente, o
con mucha dispersión.

### `prueba_buzzer` — Arduino Mega

Corre solo, en bucle, sin escribir nada en el monitor serial. Tres pruebas:

1. **Cinco pitidos** a 2300 Hz. Si no suena nada aquí, es cableado — y lo más
   probable es que falte el GND común.
2. **Barrido de frecuencias**, para ver si el volumen depende de la resonancia.
3. **Temperatura en reposo**, 30 segundos, con la tabla de interpretación.

La prueba 3 es la que distingue las tres causas que desde el ESP32 se
confunden: nivel de reposo invertido, transistor en corto, y volumen bajo por
nivel de tensión insuficiente.

**Los tres cables al Mega.** Si el módulo se alimenta de otra fuente y solo la
señal va al Mega, falta la referencia común y el módulo no suena aunque esté
perfecto.
