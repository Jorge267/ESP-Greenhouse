# Guía de diagnóstico

El programa está escrito para que **el log explique la causa**, no solo el
síntoma. Esta guía recoge las fallas reales encontradas durante el desarrollo,
qué las producía, y cómo se aislaron.

Monitor serial a **115200 baudios**. Formato del log:

```
[hh:mm:ss]  CATEGORIA    | mensaje
```

---

## Método: aislar antes de corregir

La mayoría de las fallas de este proyecto **no estaban en el código**. Estaban
en niveles de tensión, en alimentación o en calibración, y se presentaban como
si fueran errores de programa.

El patrón que funcionó, en este orden:

1. **Medir con el multímetro** antes de cambiar código. Tensión en reposo,
   corriente en operación, resistencia con todo desconectado.
2. **Desconectar un solo cable** y observar. Es la prueba más barata y a menudo
   la más decisiva.
3. **Probar el módulo en un Arduino Mega**, cuyas salidas son de 5 V. Eso saca
   la adaptación de niveles del ESP32 de la ecuación.
4. Solo entonces, tocar el programa.

Los sketches de `pruebas/` existen para el paso 3. No son código desechable.

---

## Fallas de arranque y conectividad

### `esp_rmaker_fctry: NVS Flash init failed`

No hay partición `fctry`. En Herramientas, **Partition Scheme → RainMaker 4MB
No OTA**, y **Erase All Flash Before Sketch Upload → Enable** la primera vez.

### `Sketch too big`

El esquema de particiones con OTA no deja espacio. Usa **RainMaker 4MB No
OTA**.

### `region 'iram0_0_seg' overflowed by N bytes`

El sketch excede la IRAM. Lo que lo resolvió, en orden de efectividad:

1. Quitar servicios de RainMaker que no se usan: `enableOTA()`,
   `enableSchedule()`, `enableScenes()`, `AppInsights`.
2. Quitar librerías no esenciales.

> Cambiar el esquema de aprovisionamiento de BLE a SoftAP **no libera IRAM**.
> Se probó y el desbordamiento fue idéntico al byte.

Si vuelve a aparecer, el primer candidato es `RMaker.enableTZService()`: se
pierde el histórico con hora, pero gana espacio.

### `Assisted claiming not supported for SoftAP`

El ESP32 clásico requiere **BLE** para el claiming asistido. Con SoftAP no se
puede completar el vinculado inicial.

### `getaddrinfo() returns 202` tras conectar al Wi-Fi

Falla la resolución DNS, normalmente porque la red entrega un DNS IPv6 primero.
El programa fuerza 8.8.8.8 y 8.8.4.4 en el evento `STA_GOT_IP`.

> Ese forzado **debe hacerse una sola vez**. `WiFi.config()` provoca una
> reconexión que dispara el mismo evento: sin una bandera estática, el
> resultado es un bucle infinito de "DNS forzado".

### `NO_AP_FOUND`

Hay credenciales guardadas de una red que no está disponible. Mantén pulsado
**BOOT** entre 3 y 10 segundos para borrarlas. **No uses el botón EN**: ese
solo reinicia el chip.

### Los comandos de la app llegan al log pero no ejecutan nada

Este fue el error más costoso de encontrar. El callback comparaba **punteros**
de dispositivo:

```cpp
if (dispositivo == dispositivoLed) { ... }   // NUNCA se cumple
```

Al hacer `nodo.addDevice(*dispositivo)`, la librería maneja una **copia** del
objeto, así que el puntero que llega al callback no es el que se guardó. La
comparación debe ser por **nombre**:

```cpp
if (strcmp(nombreDispositivo, "LED de Prueba") == 0) { ... }
```

### `E esp_rmaker_param: Current time not yet available`

RainMaker asocia cada dato del histórico a una fecha, y no acepta nada hasta
que el reloj se sincroniza por NTP, entre 10 y 30 segundos después de
conectarse.

El programa espera a que haya hora antes de reportar las siete variables con
histórico. Verás una sola línea cuando sincronice:

```
SISTEMA | Hora sincronizada: desde ahora se guarda el historico
```

### `'TipoAlerta' was not declared in this scope`

El IDE de Arduino genera automáticamente los prototipos de las funciones del
`.ino` y los inserta **después de los `#include`**, es decir por encima de las
declaraciones de tipos del usuario. Una función con un tipo propio en su firma
falla con ese error, apuntando a su propia definición.

**Regla: en un `.ino`, no uses tipos propios (enum, struct, class) en la firma
de una función.** Dentro del cuerpo no hay problema. Por eso `notificar()`
recibe `uint8_t` y no `TipoAlerta`.

---

## Fallas por nivel de tensión

Fueron la mayoría, y todas comparten la misma forma: el módulo "funciona mal"
en lugar de "no funciona", lo que las hace difíciles de reconocer.

### El PIR no dispara la alarma

**Causa:** la salida del HC-SR501 **ya es de 3.3 V**, porque su chip BISS0001
trabaja a 3.3 V aunque el módulo se alimente con 5 V. Pasarla por el conversor
era el problema: su pull-up de 10 kΩ a 5 V peleaba contra esa salida débil y la
línea quedaba en un nivel intermedio.

**Solución:** conexión directa al GPIO.

Dos sensores distintos fallando igual descartaron el sensor. Lo confirmó medir
el pin OUT con el sensor aislado: saltaba de 0 a 3.3 V correctamente.

### El HC-SR04 devuelve `SIN ECO` siempre

`pulseIn(pin, HIGH)` espera primero a que el pin esté en **bajo**, luego el
flanco de subida, y entonces mide. Si la línea está **pegada en alto**, nunca
encuentra el punto de partida, se agota la espera y devuelve `0` — exactamente
lo mismo que devuelve si no llegó ningún eco.

Por eso el programa revisa el **nivel de reposo** de ECHO cuando las tres
muestras fallan:

```
HC-SR04 | ECHO en reposo .... 100 de 100 lecturas en alto
HC-SR04 |   PEGADA EN ALTO. El conversor no transmite el cero.
```

| Lecturas en alto | Significa |
|---|---|
| 0 de 100 | La línea llega bien; el sensor no contesta. Revisar TRIG y los 5 V |
| 100 de 100 | El conversor no transmite el cero, o el canal está cruzado |
| Intermedio | El pin está flotando: cable suelto o sin GND común |

Medición que lo confirma, con el sensor en reposo: el lado HV del canal debe
dar ~0 V y el lado LV también. Si HV da 0 V y LV da 3.3 V, ese canal no sirve.

### El HC-SR04 mide distancias erróneas

La **velocidad del sonido no es constante**: sube unos 0.6 m/s por grado.

| Temperatura | Velocidad | Error sobre 30 cm |
|---|---|---|
| 20 °C | 343.4 m/s | referencia |
| 35 °C | 352.5 m/s | 8 mm |

El programa compensa con la temperatura del DHT11 y lo informa en el log:

```
HC-SR04 | Temperatura ......   31.0 C (DHT11) -> sonido a 350.1 m/s
```

Si el DHT11 no da lectura válida, asume 20 °C y **lo dice**, para no dejar una
compensación silenciosamente inventada.

### El buzzer se calienta y suena flojo

El caso más instructivo del proyecto. **Tres síntomas que parecían inconexos**:

- Se calentaba en reposo con **cualquier** nivel en el pin
- Sonaba muy flojo
- El consumo era **idéntico** en todas las frecuencias del barrido

Resultaron ser un solo hecho. El módulo lleva un transistor PNP con el emisor
en los 5 V de alimentación:

| Nivel en `S` | Base − Emisor | Transistor |
|---|---|---|
| 0 V | −5.0 V | conduce a fondo |
| 3.3 V | −1.7 V | **sigue conduciendo** |

Para cortarlo la base necesita llegar cerca de 4.3 V. Los 3.3 V de un GPIO no
alcanzan: **el transistor nunca llegaba a corte.** Quedaba conduciendo a medias
de forma permanente, así que se calentaba siempre, la bobina nunca veía la
conmutación completa, y la frecuencia no cambiaba nada.

**Cómo se aisló, en cuatro pasos:**

1. **Resistencia de la bobina: 43 Ω.** Un valor de catálogo para un buzzer
   magnético pasivo. Confirmó que era pasivo y no activo.
2. **Corriente en la señal: 0.15 mA.** Eso es corriente de base, no de bobina.
   Confirmó que el módulo tiene driver y que el GPIO no estaba sobrecargado.
3. **Desconectar solo el cable `S`**, dejando `+` y `-`: el buzzer quedó frío.
   Descartó un transistor en corto.
4. **Probarlo en el Arduino Mega**, con lógica de 5 V: sonó fortísimo en todas
   las frecuencias. Confirmó el diagnóstico y descartó la resonancia.

**Solución:** pasar la señal por el conversor, para que el nivel alto sean 5 V
reales. Ver [HARDWARE.md](HARDWARE.md) para los dos cableados válidos.

### El divisor de tensión no alimenta sensores

Un divisor de 10 kΩ y 20 kΩ desde 5 V entrega 3.33 V correctos **sin carga**.
Pero su corriente propia es 0.167 mA, y la regla práctica deja unos **17 µA**
disponibles. Un FC-28 pide unos **5000 µA**: la salida se desploma.

**Un divisor divide tensión; no entrega potencia.** Para señales sirve, porque
la entrada de un GPIO pide nanoamperios. Para alimentar, nunca.

Donde se necesitan 3.3 V conmutados, un **GPIO en estado alto** es la
alimentación: entrega 3.3 V y hasta 20 mA. Es lo que hace el FC-28 con GPIO17.

---

## Fallas de alimentación

### El ESP32 se reinicia en bucle al conectar el AS608

Un puerto USB 2.0 entrega **500 mA**. El ESP32 con picos de Wi-Fi ya va en unos
300 mA, y el AS608 añade 60 mA.

**Solución:** alimentar el ESP32 por el pin **VIN** con 5 V de la fuente
externa, y dejar el USB solo para programar.

### El BH1750 no responde aunque el escáner I2C lo vea

Dos causas encontradas, en este orden:

1. **`Wire.setTimeOut(50)` cortaba la inicialización.** Se quitó.
2. El sensor necesita un momento tras recibir alimentación. Se añadió 1 s de
   espera y 3 reintentos.

Y una tercera, posterior, al agregar la pantalla: **la librería de Adafruit
sube el bus a 400 kHz** por su cuenta, y con las pull-up de dos módulos en
paralelo más el conversor, los flancos se vuelven demasiado lentos. El programa
fuerza 100 kHz después de inicializar la OLED.

---

## Fallas de calibración

Estas se presentan como averías y no lo son.

### La bomba nunca arranca

```
FC-28  | Humedad suelo ....   42.3 %  (crudo 2396)  [ON <35 / OFF >=60]
BOMBA  | APAGADA porque el suelo todavia no baja del umbral
```

La escala del suelo estaba mal: con `SECO = 3200` supuesto cuando el valor real
era 4095, tierra seca marcaba 42 % y nunca llegaba al umbral de 35 %.

Ver [CALIBRACION.md](CALIBRACION.md).

### La bomba queda bloqueada por el tanque

```
HC-SR04 | CALIBRACION: 45.0 cm es MAS que DIST_TANQUE_VACIO_CM (28.0)
BOMBA   | APAGADA porque el tanque esta bajo el minimo
```

Si la distancia medida cae fuera del rango configurado, el porcentaje calcula
negativo, se recorta a 0 % y la protección bloquea la bomba. Hacía lo correcto
según los datos que tenía; los datos estaban mal.

### Una lectura válida y absurda del DHT11

El programa valida el **rango** además de `isnan()`. Un DHT11 puede entregar un
dato perfectamente formado y absurdo si falta el pull-up de 10 kΩ o el cable de
datos es largo.

```
DHT11 | DESCARTADA: 85.0 C / 12.0 % esta fuera de especificacion
```

Los dos mensajes distinguen las dos causas:

| Mensaje | Causa |
|---|---|
| `Sin respuesta` | Cableado: orden de pines, falta el pull-up, sin GND común |
| `DESCARTADA ... fuera de especificacion` | Contesta pero la trama llega corrupta |

---

## Fallas de lógica encontradas en revisión

### Dos funciones peleando por el mismo actuador

`actualizarClima()` gobernaba el ventilador por temperatura y humedad, y
`actualizarCalidadAire()` lo forzaba por gases. Si el aire seguía contaminado
pero el clima estaba en rango, **el siguiente ciclo de clima lo apagaba** pese
a la alerta de gas. Se contradecían cada 20 segundos.

**Solución:** una sola función, `evaluarVentilador()`, que ve las cuatro
entradas juntas.

El mismo criterio se aplicó al riego: la decisión vive **solo** en
`revisarRiego()`, y el ciclo de clima solo lee y reporta.

### Umbral único en lugar de histéresis

La humedad encendía el ventilador por encima de 85 % y lo apagaba por debajo de
85 %. Con el valor oscilando alrededor del umbral, conmutaba sin parar. Se
separó el umbral de salida a 78 %.

### Bucle de reintento inmediato

Cuando el tanque rechazaba el primer pulso de riego, el estado volvía a
`RIEGO_INACTIVO` —donde ya estaba— y en la vuelta siguiente del loop, 10 ms
después, se cumplían otra vez todas las condiciones de arranque.

**Solución:** una espera obligatoria de 1 minuto. La invariante es que **nunca
se vuelve a `RIEGO_INACTIVO` sin poner el freno**.

### Carrera entre dos parámetros del mismo envío

Si el usuario escribía el nombre y activaba `Modo Enrolar` casi al mismo
tiempo, el teléfono manda los dos parámetros en un solo mensaje y **no hay
garantía del orden**. Si llegaba primero el interruptor, el nombre estaba vacío
y el enrolamiento se rechazaba. A veces funcionaba y a veces no.

**Solución:** el callback solo **anota la intención** y el loop la ejecuta en la
vuelta siguiente, cuando ya se procesaron todos los parámetros del mensaje.
Como beneficio adicional, el AS608 ya no se consulta desde la tarea de
RainMaker mientras el loop también lo consulta.

### Un tipo reservado usado para campos de texto

Tres parámetros estaban declarados como `esp.param.name`. RainMaker **reserva**
ese tipo para el nombre del dispositivo, y con tres parámetros del mismo tipo
reservado la app no sabía cuál era cuál.

**Solución:** un tipo propio, `custom.param.texto`, con
`addUIType(ESP_RMAKER_UI_TEXT)`.

Relacionado: un parámetro de rango **sin `addBounds()`** la app no lo puede
dibujar, y un dispositivo **sin `assignPrimaryParam()`** no tiene qué mostrar
en su mosaico.

### El cronómetro que se reiniciaba solo

La confirmación del PIR reiniciaba `tiempoInicioMovimiento` en cada vuelta del
loop mientras el pin siguiera alto, así que la resta daba siempre ~0 y la
confirmación de 3 segundos nunca se cumplía. Se corrigió con detección de
flanco de subida — y más tarde la regla entera se reemplazó por el conteo de
eventos.

### El PIR dispara falsas alarmas al arrancar

El HC-SR501 necesita de 30 a 60 segundos para estabilizar su referencia
térmica, y durante ese tiempo **su salida oscila sola**, varias veces por
segundo. Esas oscilaciones se contaban como detecciones reales: tres de ellas
disparaban la alarma completa en cada reinicio.

**Solución:** el primer minuto se ignora por completo, y al terminar se
descartan las marcas acumuladas.

### `Serial.printf()` no existe en Arduino AVR

Es una extensión del core del ESP32. En los sketches para Arduino Mega hay que
usar `Serial.print()`. Y el `sprintf()` de AVR **no soporta flotantes**: los
decimales se imprimen con `Serial.print(valor, 1)`.

---

## Índice de categorías del log

| Categoría | Subsistema |
|---|---|
| `SISTEMA` | Arranque, perro guardián, sincronización de hora |
| `CONFIG` | Volcado de la configuración al arrancar |
| `WIFI` | Aprovisionamiento y red |
| `APP` | Comandos recibidos de RainMaker |
| `DHT11` | Temperatura y humedad del aire |
| `BH1750` | Luz ambiental |
| `FC-28` | Humedad del suelo |
| `HC-SR04` | Nivel del tanque |
| `MQ135` / `MQ7` | Gases |
| `PIR` | Movimiento y conteo de detecciones |
| `AS608` | Lector de huella |
| `OLED` | Pantalla |
| `ACCESO` | Control de acceso y enrolamiento |
| `RIEGO` | Ciclo de riego por pulsos |
| `BOMBA` | Diagnóstico del estado de la bomba |
| `ACTUADOR` | Cambios de estado de salidas |
| `AUTOMATICO` | Decisiones de las reglas |
| `SEGURIDAD` | Alarma, bloqueos y cortes de protección |
| `ALERTA` | Notificaciones push, enviadas y en espera |
| `CALIBRAR` | Modo de calibración del FC-28 |
| `BUZZER` | Barrido de frecuencias |
