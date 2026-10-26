# Calibración

El sistema **no funciona correctamente sin calibrar**. Las constantes que
vienen en el código son, salvo donde se indica, valores medidos en este
prototipo concreto: otro sensor, otra tierra u otro tanque dan números
distintos.

Dos fallas del desarrollo fueron puramente de calibración, y las dos se
presentaron como si fueran averías:

- Con la escala del suelo mal puesta, tierra seca marcaba 42 % y **la bomba
  nunca llegaba a su umbral**.
- Con el rango del tanque mal puesto, el nivel calculaba 0 % y **la bomba
  quedaba bloqueada** por una protección que creía estar haciendo lo correcto.

---

## 1. FC-28 — humedad del suelo

### Valores actuales

```cpp
constexpr int SUELO_CRUDO_SECO   = 4095;   // medido: sonda al aire, seca
constexpr int SUELO_CRUDO_HUMEDO = 2535;   // medido: sonda sumergida
```

Fíjate en lo estrecho del rango: **1560 cuentas de las 4095 posibles**. Es
normal en una sonda resistiva alimentada a 3.3 V, y es la razón por la que no
se pueden usar valores de catálogo.

El 4095 es el ADC saturado. Es un valor **legítimo**, no un error: la primera
versión del código descartaba como inválida cualquier lectura cercana al techo,
y con eso el suelo más seco posible se rechazaba y el riego nunca arrancaba.

### Procedimiento

1. Pon `MODO_CALIBRAR_SUELO = true` y sube el programa.
2. El log imprime una línea cada 2 segundos:

   ```
   [00:00:12]  CALIBRAR     | crudo 2396  ->   42.3 %   [escala actual 4095..2535]
   ```

3. **Sonda al aire**, limpia y seca. Espera unos 10 segundos a que el valor se
   estabilice. Ese crudo es tu `SUELO_CRUDO_SECO`.
4. **Sonda en un vaso con agua**, hasta donde llegarían las patas metálicas en
   la tierra. No sumerjas la placa del comparador. Ese crudo es tu
   `SUELO_CRUDO_HUMEDO`.
5. Escribe los dos valores en el sketch y pon `MODO_CALIBRAR_SUELO = false`.

### Refinamiento recomendado

Calibrar el extremo seco **al aire** es lo habitual, pero comprime el rango
útil: incluso la tierra más seca conduce más que el aire, así que los
porcentajes reales nunca llegan a 0.

Si quieres precisión, mide el extremo seco con la **sonda en tierra bien seca**
en lugar de al aire. El rango se ensancha y los porcentajes se vuelven
representativos de lo que realmente pasa en la maceta.

### Los umbrales de riego son otra cosa

```cpp
constexpr float SUELO_BOMBA_ON  = 35.0f;
constexpr float SUELO_BOMBA_OFF = 60.0f;
```

**Estos siguen sin calibrar.** Están definidos sobre una referencia de aire y
agua, y ni el suelo más seco llega a 0 % ni el más mojado llega a 100 %.

Para fijarlos de verdad, con la sonda enterrada en su posición definitiva:

1. Observa el porcentaje con la tierra **como la dejarías justo antes de
   regar**. Ese es tu `SUELO_BOMBA_ON`.
2. Observa el porcentaje con la tierra **recién regada como quieres dejarla**.
   Ese es tu `SUELO_BOMBA_OFF`.

Deja al menos 15 puntos de separación entre los dos, o el riego conmutará
demasiado seguido.

### Escala resultante con los valores actuales

| Crudo | Nivel | Decisión |
|---|---|---|
| 4095 | 0 % | riega |
| 3700 | 25 % | riega |
| 3500 | 38 % | sin cambio |
| 3100 | 64 % | corta |
| 2535 | 100 % | corta |

---

## 2. HC-SR04 — nivel del tanque

### Valores actuales

```cpp
constexpr float DIST_TANQUE_VACIO_CM = 28.0f;   // MEDIDO
constexpr float DIST_TANQUE_LLENO_CM = 6.0f;    // SUPUESTO, pendiente
constexpr float NIVEL_TANQUE_MINIMO  = 15.0f;   // % bajo el cual no riega
```

Con el tanque vacío el sensor midió **27.3 cm**, estable: tres muestras de
1588 µs idénticas. Se configuró **28.0** y no 27.3 a propósito, para que ese
margen de 0.7 cm absorba el ruido de medida. Sin él, una lectura de 27.5 cm
daría porcentaje negativo y el log se llenaría de avisos de calibración en cada
ciclo, aunque el comportamiento fuera correcto.

### Procedimiento

Con el sensor ya montado en la tapa y cinta métrica:

1. **`DIST_TANQUE_VACIO_CM`**: distancia del sensor al fondo del tanque. Súmale
   medio centímetro de margen.
2. **`DIST_TANQUE_LLENO_CM`**: distancia del sensor a la superficie con el
   tanque lleno. **Deja al menos 5 cm**: el HC-SR04 tiene una zona muerta de
   2 cm y por debajo de eso no mide nada.

El log te avisa si la medida cae fuera del rango configurado, que es la causa
más común de que la bomba quede bloqueada sin motivo aparente:

```
HC-SR04 | CALIBRACION: 45.0 cm es MAS que DIST_TANQUE_VACIO_CM (28.0)
HC-SR04 |   El nivel se recorta a 0 % y la bomba quedara bloqueada.
HC-SR04 |   Mide con cinta del sensor al fondo y ajusta esa constante.
```

### Escala resultante con los valores actuales

| Distancia | Nivel | Permiso de riego |
|---|---|---|
| 27.3 cm (vacío) | 3.2 % | NO |
| 25.0 cm | 13.6 % | NO |
| 21.7 cm | 15.0 % | límite |
| 20.0 cm | 36.4 % | SI |
| 6.0 cm (lleno) | 100 % | SI |

El riego se habilita con unos **6 cm de agua** en el fondo. Para una bomba
sumergible es un margen razonable; si la tuya necesita más columna de agua,
sube `NIVEL_TANQUE_MINIMO`.

### Para probar la bomba sin tanque

```cpp
constexpr bool PROTECCION_TANQUE_ACTIVA = false;
```

El nivel se sigue midiendo y registrando, pero no bloquea. El arranque lo
avisa en mayúsculas. **Vuélvela a `true` antes de dejar el sistema
funcionando**: con `false` la bomba puede arrancar en seco y destruir su sello
en minutos.

---

## 3. Riego por pulsos

```cpp
constexpr uint32_t DURACION_PULSO_RIEGO_MS = 1000;   // 1 s de bomba
constexpr uint32_t TIEMPO_ABSORCION_MS     = 10000;  // 10 s de espera
constexpr uint8_t  MAX_PULSOS_POR_CICLO    = 20;
```

El tiempo de absorción es la parte imprescindible: sin él, los pulsos solo
trocean el mismo exceso de agua. El agua tarda en bajar por la tierra y llegar
a la sonda, y mientras no llega el sensor sigue marcando seco.

**Los 10 segundos están puestos para que la demostración sea visible.** Para
tierra real y una maceta de buen tamaño, **súbelo a 30 o 60 segundos**.

El log te da el dato para afinar el pulso:

```
RIEGO | Ciclo terminado: objetivo alcanzado
RIEGO |   3 pulso(s) de 1000 ms  |  suelo 33.5 % -> 61.4 %
```

Si siempre hacen falta 2 o 3 pulsos, el segundo está bien. Si son 15, alarga el
pulso. Si con uno ya se pasa del objetivo, acórtalo.

---

## 4. MQ-135 y MQ-7 — gases

```cpp
constexpr int UMBRAL_AIRE_MALO_RAW     = 2000;
constexpr int UMBRAL_CO_EMERGENCIA_RAW = 2500;
```

**Ambos siguen sin calibrar.** Son valores de partida.

Estos sensores tienen dos particularidades:

- Necesitan **de 24 a 48 horas de precalentamiento** la primera vez para
  estabilizar su resistencia. Las lecturas de los primeros minutos tras cada
  encendido tampoco son fiables.
- Su lectura depende de la temperatura y la humedad ambientales, así que el
  umbral que funciona en el banco no es el mismo que en el invernadero.

### Procedimiento

1. Déjalos funcionando 48 horas en el sitio definitivo.
2. Registra el valor crudo en aire limpio durante varios días, a distintas
   horas. El log lo imprime en cada ciclo:

   ```
   MQ135 | Aire .....  153 /4095    3.7%  0.12 V  [umbral 2000] normal
   MQ7   | CO .......   55 /4095    1.3%  0.04 V  [umbral 2500] normal
   ```

3. Fija el umbral bastante por encima del máximo observado en condiciones
   normales. Un umbral demasiado cercano produce falsas alarmas cada vez que
   suba la humedad.

Con las lecturas de este prototipo —153 y 55 sobre 4095— los umbrales de 2000
y 2500 están muy holgados. Eso es deliberado mientras no haya datos de campo:
es preferible que no dispare a que dispare sin motivo.

### Advertencia sobre el MQ-7

El MQ-7 **mide monóxido de carbono**, un gas que mata sin olor ni color. Este
sistema **no es un detector de CO certificado** y no debe usarse como tal. Si
en el invernadero hay combustión —un calefactor, un motor— instala un detector
de CO homologado, independiente de este proyecto.

---

## 5. Buzzer

```cpp
constexpr uint32_t FRECUENCIA_BUZZER_HZ = 2300;
```

Se verificó con el barrido del Arduino Mega que **todas las frecuencias suenan
igual de fuerte** una vez que el transistor conmuta correctamente, así que la
resonancia no es un factor en este módulo.

Los 2300 Hz se eligieron por eficacia perceptiva: el oído humano es más
sensible entre 2 y 4 kHz, que es justo donde quieres el tono de una alarma.

Si cambias de buzzer, `MODO_BARRIDO_BUZZER = true` recorre diez frecuencias
típicas al arrancar, anunciando cada una en el log para que identifiques de
oído la más fuerte.

---

## 6. Alarma por movimiento

```cpp
constexpr uint32_t SEPARACION_MINIMA_DETECCION_MS = 15000;  // 15 s
constexpr uint8_t  DETECCIONES_PARA_ALARMA        = 3;
constexpr uint32_t VENTANA_DETECCIONES_MS         = 60000;  // 1 min
constexpr uint32_t PIR_CALENTAMIENTO_MS           = 60000;  // 60 s
```

La regla por conteo tiene una propiedad deliberada: **el ajuste del
potenciómetro Tx del sensor ya no altera el comportamiento del programa.** La
versión anterior exigía que el pin siguiera en alto 3 segundos continuos, y
cuánto tiempo sigue en alto lo decide ese potenciómetro, que es hardware.

Para afinar la sensibilidad del sistema, no del sensor:

- **Más reactivo**: baja `DETECCIONES_PARA_ALARMA` a 2, o
  `SEPARACION_MINIMA_DETECCION_MS` a 10000.
- **Menos falsas alarmas**: sube la ventana a 90000, que exige detecciones más
  sostenidas en el tiempo.

El minuto de calentamiento no es negociable: durante ese tiempo la salida del
HC-SR501 oscila sola y esas oscilaciones se contaban como detecciones reales.
Tres de ellas disparaban la alarma completa en cada reinicio del sistema.

Para ver el conteo en acción, `LOG_PIR_DETALLADO = true` registra cada cambio
de nivel del pin.

---

## Resumen: qué está calibrado y qué no

| Constante | Estado |
|---|---|
| `SUELO_CRUDO_SECO` / `SUELO_CRUDO_HUMEDO` | **Medido** |
| `DIST_TANQUE_VACIO_CM` | **Medido** |
| `DIST_TANQUE_LLENO_CM` | Supuesto — solo afecta la escala |
| `SUELO_BOMBA_ON` / `SUELO_BOMBA_OFF` | Sin calibrar en tierra real |
| `UMBRAL_AIRE_MALO_RAW` / `UMBRAL_CO_EMERGENCIA_RAW` | Sin calibrar |
| `TIEMPO_ABSORCION_MS` | Puesto para demostración, subir en campo |
| `FRECUENCIA_BUZZER_HZ` | Verificado |
| Umbrales de clima y luz | Razonables, sin validar en campo |
