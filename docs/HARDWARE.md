# Hardware, cableado y adaptación de niveles

> **Lee la sección de adaptación de niveles antes de conectar nada.** Varios
> sensores necesitan conversor y otros se estropean o dejan de funcionar si se
> lo pones. El criterio no es evidente y fue la causa de la mayoría de las
> fallas durante el desarrollo.

---

## Mapa de pines

| GPIO | Función | Dirección | Adaptación |
|---|---|---|---|
| 0 | Botón BOOT (reset de credenciales) | entrada | — |
| 2 | LED integrado de prueba | salida | — |
| 4 | Relé canal 1 — luz de cultivo | salida | directa |
| 5 | Buzzer, señal | salida | **vía conversor** |
| 13 | DHT11, línea de datos | bidireccional | directa a 3.3 V |
| 14 | L298N IN4 — ventilador | salida | directa |
| 16 | HC-SR04 TRIG | salida | directa |
| 17 | FC-28 VCC (alimentación conmutada) | salida | — |
| 18 | Relé canal 2 — chapa solenoide | salida | directa |
| 19 | AS608 RX (recibe del TX del sensor) | entrada UART1 | directa a 3.3 V |
| 21 | I2C SDA — BH1750 y OLED | bidireccional | **vía conversor** |
| 22 | I2C SCL — BH1750 y OLED | bidireccional | **vía conversor** |
| 23 | AS608 TX (va al RX del sensor) | salida UART1 | directa a 3.3 V |
| 25 | L298N IN1 — bomba | salida | directa |
| 26 | L298N IN2 — bomba | salida | directa |
| 27 | L298N IN3 — ventilador | salida | directa |
| 32 | FC-28 AO, humedad del suelo | entrada ADC1 | **ninguna** (ver nota) |
| 33 | L298N ENA — bomba, PWM | salida | directa |
| 34 | MQ-135, índice de aire | entrada ADC1 | **vía conversor** |
| 35 | MQ-7, índice de CO | entrada ADC1 | **vía conversor** |
| 36 (SVP) | HC-SR04 ECHO | entrada | **vía conversor** |
| 39 (SVN) | HC-SR501 OUT, movimiento | entrada | **ninguna** (ver nota) |

### Pines con restricciones

**GPIO 34 a 39 son solo de entrada** y **no tienen resistencias internas** de
pull-up ni pull-down. Un cable suelto deja el pin flotando y leyendo ruido
indistinguible de una señal real. Para el PIR en GPIO39 se recomienda una
**resistencia externa de 10 kΩ a GND**: con ella, la ausencia de señal se lee
como "sin movimiento", que es el comportamiento seguro para una entrada de
alarma.

**GPIO5 es pin de strapping.** La placa lo deja oscilando unos milisegundos
durante el arranque, antes de que el programa tome control. Con el módulo de
buzzer eso produce un chasquido corto en cada reinicio. Es inofensivo.

**Usar solo ADC1 (GPIO 32 a 39) para lecturas analógicas.** El ADC2 comparte
hardware con la radio Wi-Fi y sus lecturas son inservibles mientras el Wi-Fi
está activo.

---

## Adaptación de niveles: el criterio

El ESP32 trabaja a **3.3 V** y su máximo absoluto en un pin es **3.6 V**. La
mayoría de los módulos "para Arduino" asumen lógica de 5 V. Hay que decidir
módulo por módulo, y **el criterio no es con qué voltaje se alimenta, sino qué
voltaje entrega o espera realmente su pin de señal.**

### Conversor bidireccional de MOSFET (tipo BSS138)

Es el que usa el proyecto. Cada canal tiene un MOSFET y resistencias pull-up
de 10 kΩ en cada lado: una a 3.3 V en el lado LV y otra a 5 V en el lado HV.

Su funcionamiento clave: **solo transmite los ceros de forma activa**. El nivel
alto no lo empuja, lo levanta la resistencia pull-up del lado correspondiente.

De ahí se deducen sus dos límites:

- **Sirve** para señales digitales de dos estados cuyo nivel alto no tenga que
  entregar corriente: I2C, un ECHO, la base de un transistor en corte.
- **No sirve** para señales analógicas, porque las destruye, ni para cargas que
  necesiten que el nivel alto entregue corriente, porque 10 kΩ a 5 V dan medio
  miliamperio.

### Divisor de tensión resistivo

Dos resistencias en serie. Con 10 kΩ y 20 kΩ desde 5 V se obtienen 3.33 V.

- **Sirve** para atenuar una **señal** cuya carga no consuma casi nada, como
  la entrada de un GPIO, que pide nanoamperios.
- **No sirve** para **alimentar** nada. Un divisor de 10 k + 20 k hace circular
  0.167 mA por sí mismo, y la regla práctica deja unos 17 µA disponibles para
  la carga. Cualquier sensor pide cientos de veces más y la salida se desploma.

> **Un divisor divide tensión; no entrega potencia.** Esa distinción decide
> todos los casos de este proyecto.

---

## Conexiones por módulo

### DHT11 — temperatura y humedad del aire

```
VCC  -> 3V3        (NO a 5V)
GND  -> GND común
DATA -> GPIO13     (directo)
```

Se alimenta a **3.3 V** a propósito. Su protocolo es de un solo hilo con pulsos
de 26 a 70 microsegundos, y meter el conversor en una señal con esa exigencia
de tiempos agrega un punto de falla innecesario. A 3.3 V su línea de datos sale
a 3.3 V y es compatible directa.

El sensor desnudo de 4 patas necesita una **resistencia de 10 kΩ entre DATA y
VCC**. Los módulos sobre placa ya la traen.

El programa valida el rango además de `isnan()`: un DHT11 puede entregar un
dato perfectamente formado y absurdo si falta el pull-up o el cable es largo.
Fuera de −5 a 60 °C la lectura se descarta, porque esa temperatura alimenta la
compensación del ultrasonido.

### BH1750 (GY-30) — luz ambiental

```
VCC -> 5V
GND -> GND común
SDA -> lado HV del canal 1 del conversor
SCL -> lado HV del canal 2 del conversor
ADO -> sin conectar     (dirección 0x23)
```

### Pantalla OLED SSD1306 — indicación en la puerta

```
VCC -> 5V
GND -> GND común
SDA -> mismo nodo HV que el SDA del BH1750
SCL -> mismo nodo HV que el SCL del BH1750
```

**Comparte el bus con el BH1750**, sobre el mismo par de canales del conversor.
Direcciones distintas: BH1750 en `0x23`, OLED en `0x3C`. No gasta canales
nuevos ni carga el riel de 3.3 V.

Los dos módulos tienen sus propias pull-up, que al quedar en paralelo bajan la
resistencia efectiva de 10 kΩ a unos 5 kΩ. A 100 kHz sigue dentro de
especificación.

> **El programa fuerza el bus a 100 kHz a propósito.** La librería de Adafruit
> lo sube a 400 kHz por su cuenta al inicializar la pantalla, y con las pull-up
> en paralelo más el conversor los flancos se vuelven demasiado lentos para esa
> velocidad: el BH1750 empieza a fallar. El costo es que enviar el buffer de
> 1 KB tarda unos 90 ms, y por eso la pantalla no se refresca en cada vuelta
> del loop.

### AS608 — lector de huella

```
V+  (rojo)     -> 3V3
GND (negro)    -> GND común
TX  (amarillo) -> GPIO19     (TX del sensor al RX del ESP32)
RX  (blanco)   -> GPIO23     (RX del sensor al TX del ESP32)
TCH (azul)     -> sin conectar
VA  (verde)    -> sin conectar
```

**TX y RX van cruzados.** Es el error más común con este módulo.

### FC-28 — humedad del suelo

```
VCC -> GPIO17      (se energiza solo durante la medición)
GND -> GND común
AO  -> GPIO32      (directo, sin conversor ni divisor)
DO  -> sin conectar
```

Tres decisiones que importan:

**Se alimenta a 3.3 V, nunca a 5 V.** Su salida AO es analógica y proporcional
a la alimentación: a 5 V llegaría a 5 V y pondría en riesgo el GPIO32.

**No lleva conversor ni divisor.** Un conversor digital destruye una señal
analógica, y un divisor carga la salida de alta impedancia del sensor y
deforma la lectura de forma no lineal.

**Su VCC cuelga de un GPIO.** La sonda es resistiva: agua más tensión continua
hacen electrólisis y las pistas se comen en semanas. Energizándola 200 ms cada
20 segundos, el tiempo bajo tensión baja al 1 %.

Si el módulo tiene un LED muy brillante y el GPIO no alcanza a alimentarlo, pon
`ALIMENTAR_SUELO_POR_GPIO = false` y conecta VCC al riel de 3V3, aceptando la
oxidación.

El programa detecta un AO desconectado aprovechando que controla la
alimentación: lee el pin **antes** de energizar la sonda, y si ya marca alto
sin alimentación, el cable está flotando.

### HC-SR04 — nivel del tanque

```
VCC  -> 5V
GND  -> GND común
TRIG -> GPIO16     (directo, su umbral está cerca de 2 V)
ECHO -> lado HV de un canal libre del conversor
        lado LV de ese canal -> GPIO36 (SVP)
```

**TRIG va directo** porque es una entrada del sensor y los 3.3 V del GPIO lo
activan de sobra. **ECHO sí necesita adaptación**: entrega 5 V reales.

El divisor de 10 k + 20 k también es válido aquí, y para una señal donde el
tiempo es el dato tiene incluso mejor impedancia que el pull-up de 10 kΩ del
conversor.

**Montaje en el tanque:**

- En la tapa, perpendicular hacia abajo. Más de 15° de inclinación y el eco se
  va de lado.
- Mínimo 5 cm sobre el nivel máximo de agua: tiene una zona muerta de 2 cm.
- Al menos 5 cm de la pared del tanque: el haz es un cono de unos 15° y la
  pared devuelve ecos falsos.
- **No es resistente al agua.** En un tanque cerrado hay condensación
  permanente. Móntalo por fuera de la tapa con dos agujeros para los cilindros
  y sella con silicona, o usa la versión **JSN-SR04T**, con sonda sellada y
  compatible pin a pin.

> **El programa compensa la velocidad del sonido con la temperatura del
> DHT11.** El sonido sube unos 0.6 m/s por grado: 343.4 m/s a 20 °C contra
> 352.5 m/s a 35 °C, un 2.6 % de diferencia. En un invernadero en Zacapa esa
> diferencia es real y se traduce en varios puntos de porcentaje sobre el
> rango útil del tanque.

### HC-SR501 — movimiento

```
VCC -> 5V
GND -> GND común
OUT -> GPIO39 (SVN)     (DIRECTO, sin conversor)
```

**Va directo aunque se alimente con 5 V**, y esto merece explicación porque es
contraintuitivo. El módulo trae su propio regulador: el chip BISS0001 que lo
controla trabaja a 3.3 V, así que **OUT entrega 3.3 V en alto**.

Ponerle el conversor no era neutro, era el problema: su pull-up de 10 kΩ a 5 V
peleaba contra la salida débil del BISS0001 y la línea quedaba en un nivel
intermedio que el ESP32 no leía como alto limpio.

> **Una protección mal colocada se vuelve el problema.** El criterio no es con
> qué voltaje se alimenta el módulo, sino qué entrega su pin de señal.

Se recomienda una **resistencia de 10 kΩ de GPIO39 a GND**, porque ese pin no
tiene pull-down interno.

Ajustes del módulo: potenciómetro de tiempo (Tx) al mínimo, sensibilidad (Sx)
al centro, jumper en **H** (disparo repetido). Con la regla por conteo de
eventos, el ajuste de Tx ya no altera el comportamiento del programa.

### MQ-135 y MQ-7 — gases

```
VCC -> 5V
GND -> GND común
AO  -> lado HV de un canal del conversor
       lado LV -> GPIO34 (MQ-135) y GPIO35 (MQ-7)
```

Estos sí entregan niveles referidos a su alimentación de 5 V.

Necesitan un calentador que consume unos 150 mA cada uno. Con los dos
conectados a un regulador lineal el consumo total es excesivo: hace falta un
**convertidor reductor conmutado tipo LM2596**.

### Módulo de relé de 2 canales

```
VCC -> 5V
GND -> GND común
IN1 -> GPIO4      (luz de cultivo)
IN2 -> GPIO18     (chapa solenoide)
```

**El módulo es ACTIVO EN BAJO**: el contacto se cierra cuando el pin IN está en
LOW. Toda escritura pasa por `escribirRele()`, que invierte el nivel. Con la
lógica directa la chapa quedaba energizada de forma permanente y se
sobrecalentaba.

Si algún día se cambia por un módulo activo en alto, basta poner
`RELE_ACTIVO_EN_BAJO = false`.

### Chapa solenoide 12 V — fail-secure

```
Fuente 12V (+) -> COM del relé
NO del relé    -> Chapa (+)
Chapa (-)      -> GND de la fuente de 12V

Diodo 1N4007 en paralelo a la chapa, banda hacia el (+)
```

**Fail-secure** significa que 0 V es bloqueada: solo abre mientras recibe los
12 V. Es la polaridad correcta para una cerradura de seguridad.

El diodo absorbe el pico inverso que genera la bobina al cortarse. Sin él, ese
pico degrada los contactos del relé y mete ruido en todo el circuito.

Es de **servicio intermitente**: no está diseñada para quedar energizada de
forma continua. El programa la cierra sola a los 4 segundos.

> **Advertencia de seguridad:** tal como está, la chapa se abre si el ESP32
> pierde alimentación. Ver [SEGURIDAD.md](SEGURIDAD.md).

### Buzzer pasivo con módulo de disparo en bajo

```
+ -> 5V
- -> GND común
S -> lado HV de un canal libre del conversor
     lado LV de ese canal -> GPIO5
```

Este módulo fue el caso más instructivo del proyecto y se documenta completo en
[DIAGNOSTICO.md](DIAGNOSTICO.md). En resumen:

Su transistor es PNP con el emisor en los 5 V de alimentación, y conduce según
la diferencia base-emisor:

| Nivel en `S` | Base − Emisor | Transistor |
|---|---|---|
| 0 V | −5.0 V | conduce a fondo |
| 3.3 V | −1.7 V | **sigue conduciendo** |
| 5.0 V | 0 V | corta |

Para cortarlo, la base necesita llegar cerca de 4.3 V. Los 3.3 V de un GPIO no
alcanzan: el transistor nunca cortaba, quedaba conduciendo a medias de forma
permanente. De ahí tres síntomas que parecían inconexos — se calentaba en
reposo, sonaba muy flojo, y el consumo era idéntico en todas las frecuencias.

Hay **dos cableados válidos**, y la clave es que el nivel alto **iguale** la
tensión del emisor:

| `+` del módulo | Señal en `S` | Nivel alto | Resultado |
|---|---|---|---|
| 5 V | vía conversor | 5 V = emisor | corta ✓ |
| 3V3 | directa al GPIO | 3.3 V = emisor | corta ✓ |
| 5 V | directa al GPIO | 3.3 V ≠ 5 V | **conduce siempre ✗** |

Se eligió el primero: con el segundo, los 77 mA de la bobina saldrían del
regulador de 3.3 V del ESP32, que en plena alarma ya está entregando los picos
de Wi-Fi más el lector de huella. El código es idéntico para ambos.

Como el conversor levanta el nivel alto con una pull-up de 10 kΩ, queda un
residuo de conducción de unos 7 mW: el buzzer se tibia de forma apenas
perceptible. Se elimina con una **resistencia de 1 kΩ entre `S` y +5 V**, en
paralelo con la del conversor.

### L298N — bomba y ventilador

```
Fuente 12V -> +12V del L298N
GND        -> GND común con el ESP32

Bomba  (canal con PWM, jumper ENA RETIRADO):
  ENA -> GPIO33     IN1 -> GPIO25     IN2 -> GPIO26

Ventilador (canal sin PWM, jumper ENB PUESTO):
  IN3 -> GPIO27     IN4 -> GPIO14
```

La bomba trabaja al **75 %** de PWM. Sus pines IN quedan fijos en estados
opuestos y el encendido se hace con el duty del ENA, así nunca quedan ambos en
alto ni ambos en bajo — una combinación que en un puente H es un cortocircuito.

El PWM de la bomba usa el **canal LEDC 0** y el buzzer el **canal 2**, no el 1.
En el core de Arduino el temporizador se asigna como `canal / 2`, así que los
canales 0 y 1 comparten temporizador: con el buzzer en el canal 1, cambiar su
frecuencia habría cambiado también la del PWM de la bomba.

---

## Alimentación

### Presupuesto de corriente

| Carga | Riel | Consumo |
|---|---|---|
| ESP32 con Wi-Fi (pico) | 3.3 V | ~300 mA |
| AS608 lector de huella | 3.3 V | ~60 mA |
| DHT11 | 3.3 V | ~2 mA |
| FC-28 (solo al medir) | 3.3 V | ~8 mA |
| BH1750 | 5 V | ~0.2 mA |
| Pantalla OLED | 5 V | 20–27 mA |
| HC-SR04 | 5 V | ~15 mA |
| HC-SR501 | 5 V | ~65 µA |
| Buzzer (al sonar) | 5 V | 116 mA pico |
| MQ-135 y MQ-7 | 5 V | ~150 mA cada uno |

### El cuello de botella no es el regulador, es el USB

El regulador AMS1117 de la placa aguanta el riel de 3.3 V. El problema es que
un puerto USB 2.0 entrega **500 mA**, y el ESP32 con el lector de huella ya va
en unos 360 mA.

Durante el desarrollo esto causó reinicios en bucle al conectar el AS608.

**La solución es alimentar el ESP32 por el pin VIN con 5 V** desde la fuente
externa, y dejar el USB solo para programar y ver el monitor serial. Así los
picos de Wi-Fi los absorbe una fuente con corriente de sobra.

Esa medida es además una de las dos que cierran el hueco de seguridad de la
chapa: ver [SEGURIDAD.md](SEGURIDAD.md).

### Tierra común

Todas las fuentes —3.3 V del ESP32, 5 V externa y 12 V de motores y chapa—
deben compartir **GND**. Sin tierra común las señales no tienen referencia y el
sistema se comporta de forma errática o simplemente no funciona.
