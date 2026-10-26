# Análisis de seguridad y mitigación de riesgos

Este documento describe los riesgos identificados en el subsistema de acceso y
en los actuadores de potencia, lo que el programa mitiga, y lo que **no puede
mitigarse por software**.

---

## Riesgo crítico: la chapa se abre al perder alimentación

### Síntoma observado

Al desconectar el ESP32, la chapa solenoide se abre durante unos segundos.

### Causa

Son **dos dominios de energía distintos**. El módulo de relé se alimenta de la
fuente externa de 5 V; el ESP32, del puerto USB.

```
ESP32 se apaga
  -> GPIO18 deja de ser salida y queda en alta impedancia
  -> el pin IN del módulo, sin nadie que lo sujete, deriva hacia cero
  -> el módulo es ACTIVO EN BAJO: cero es la orden de ACTIVAR
  -> el relé cierra y la chapa recibe sus 12 V
  -> LA PUERTA SE ABRE
```

El relé sigue alimentado y obedeciendo a un pin que ya no manda nada.

El mismo mecanismo actúa en **cada arranque y cada reinicio**, durante los 200
a 300 ms que tarda el gestor de arranque del chip antes de que `setup()`
exista. Esa ventana no se puede cerrar por software.

### Por qué importa

La cerradura es **fail-secure**: 0 V es bloqueada, y esa es la elección
correcta para una cerradura de seguridad. Pero la propiedad que se busca en un
sistema fail-secure es que **un único fallo deje el sistema en estado seguro**.

Tal como está cableado el prototipo, un único fallo —perder el USB— lo deja en
estado **abierto**. La elección correcta de cerradura queda anulada por el
cableado de su control.

---

## Lo que mitiga el programa

### 1. El estado seguro es lo primero de `setup()`

Las escrituras que dejan los relés inactivos están **antes de
`Serial.begin()`** y antes de cualquier mensaje.

Antes estaban después del banner y de unas veinte líneas de configuración: a
115200 baudios eso son más de 100 ms, más los 300 ms del `delay()` inicial.
Más de 400 ms de puerta abierta en cada arranque, que ahora se reducen a lo
que tarda el chip en arrancar.

El buzzer está en el mismo grupo por la misma razón: también dispara en bajo,
así que cada milisegundo con su pin en bajo es un milisegundo sonando y
calentándose.

### 2. Perro guardián sobre la tarea del loop

```cpp
esp_task_wdt_init(TIMEOUT_WATCHDOG_S, true);   // 30 s, reinicia al morder
esp_task_wdt_add(NULL);
```

Esto cubre un hueco más grave que el síntoma observado. `revisarCierreChapa()`
es lo que vuelve a bloquear la chapa a los 4 segundos, y corre **dentro del
loop**. Si el loop se colgaba con la chapa abierta, se quedaba abierta
indefinidamente.

Ahora un cuelgue se convierte en reinicio, y al reiniciar la chapa queda
bloqueada.

Los 30 segundos de timeout son holgados a propósito: la vuelta más lenta del
loop —clima, tanque y pantalla en la misma iteración— no pasa de 1.5 s, así
que no hay riesgo de reinicios espurios, y sigue detectando un cuelgue real en
medio minuto.

### 3. Auditoría de la apertura remota

Toda apertura desde la app se registra y manda una notificación push.

Es el **único camino de acceso sin identidad** del sistema: la huella deja
constancia de quién entró, la app no. Si la cuenta de RainMaker se viera
comprometida, ese aviso es lo que lo delata.

### 4. Una huella sin nombre niega el paso

Si el AS608 reconoce una plantilla que no tiene nombre en el registro de la
NVS, el sistema **no abre**.

La primera versión devolvía el literal `"desconocido"` y abría igual. Un acceso
sin identidad no se puede auditar, y el sistema entero existe para saber quién
entró.

Esto pasa con las plantillas enroladas desde `pruebas/prueba_huella`, que se
numeran a mano sin nombre asociado.

### 5. Un ID se considera libre solo si lo está en los dos sitios

`buscarIdLibre()` comprueba el registro de nombres **y** pregunta al sensor con
`loadModel()`.

Mirando solo el registro local, si el sensor ya tenía plantillas sin nombre,
`storeModel()` las **sobrescribía en silencio**: el resultado era una huella
que abría la puerta sin nombre, y un dedo registrado que de pronto dejaba de
funcionar.

### 6. El registro de nombres se carga siempre

`cargarRegistroPersonas()` corre antes de tocar el sensor y sin importar si
responde.

Cuando solo se cargaba con el lector disponible, un arranque con el sensor
desconectado dejaba el arreglo vacío, y el siguiente guardado **borraba todos
los nombres de la NVS** sin aviso.

### 7. Bloqueo anti fuerza bruta

Cinco huellas no reconocidas seguidas bloquean el lector 60 segundos y mandan
una notificación. Cinco desconocidas seguidas en la puerta es alguien
intentando entrar, no un dedo mal puesto.

### 8. Cierre automático en todas las vías

Los 4 segundos de apertura aplican a cualquier origen, incluida la orden manual
desde la app. No hay forma de dejar la chapa abierta indefinidamente por
diseño.

### 9. Protección de la bomba contra marcha en seco

`aplicarBomba()` es el **único punto** por el que se enciende la bomba, y el
filtro del nivel de tanque vive ahí. Da igual si la orden viene del riego
automático o de un botón de la app: las dos pasan por el mismo sitio.

Si el HC-SR04 no responde, tampoco se riega. Es la decisión conservadora: es
preferible un riego perdido a una bomba destruida por marcha en seco.

### 10. Watchdog de riego

Si la bomba lleva 3 minutos continuos encendida, se corta y se notifica. Con el
riego por pulsos ese caso ya no se da en operación normal, pero sigue
protegiendo la orden manual desde la app.

---

## Lo que NO puede mitigarse por software

El firmware no puede sujetar un pin mientras está apagado. **Estas dos medidas
son de hardware y son las que realmente cierran el hueco.**

### A. Resistencia de 10 kΩ entre GPIO18 y el VCC del módulo de relé

```
VCC del módulo de relé ---[10 kΩ]--- GPIO18  (y pin IN del módulo)
```

Mientras el ESP32 esté apagado o arrancando, esa resistencia mantiene el pin
`IN` en alto —es decir, en **inactivo**— y la chapa sigue bloqueada.

Con el ESP32 encendido no estorba: el GPIO la domina con unos 170 µA, muy
dentro de lo que puede manejar.

**Es la única medida que cierra la ventana del gestor de arranque.**

### B. Alimentar el módulo de relé del mismo dominio que el ESP32

Si los dos salen de la misma fuente, perder la alimentación apaga también la
bobina del relé, sus contactos vuelven a reposo y la chapa se bloquea.

Esto se consigue alimentando el ESP32 por el pin **VIN** con los 5 V de la
fuente externa, en lugar del USB. Es además la medida recomendada en
[HARDWARE.md](HARDWARE.md) para resolver el presupuesto de corriente.

Con A y B juntas, **un único fallo deja el sistema en estado seguro**, que es
la propiedad que se busca.

---

## Riesgos residuales que no se mitigan

Conviene enunciarlos de forma explícita.

### Acceso físico al tablero

Quien tenga acceso físico puede abrir la chapa puenteando el relé o cortando
su alimentación de 12 V. **Ninguna medida de firmware cubre esto.**

La defensa es física: tablero cerrado con llave, y el cableado de 12 V fuera de
alcance desde el exterior.

### El botón BOOT borra las credenciales

Mantener pulsado BOOT entre 3 y 10 segundos borra las credenciales Wi-Fi, y más
de 10 segundos hace un reset de fábrica. Quien alcance el botón puede dejar el
sistema fuera de la red.

No se mitiga porque quien alcanza el botón ya tiene acceso físico al tablero, y
en ese escenario hay vectores más directos. Se resuelve con la misma defensa
física.

### La seguridad del acceso remoto depende de la cuenta de RainMaker

El dispositivo **Chapa de Acceso** permite abrir la puerta desde la app. La
seguridad de eso es exactamente la seguridad de la cuenta de ESP RainMaker.

Mitigación parcial: toda apertura remota se notifica. Si no se quiere ese
camino, se puede quitar el dispositivo `Chapa de Acceso` de
`crearDispositivosRainMaker()` y dejar la huella como único acceso.

### El AS608 no es un lector de grado forense

Un lector óptico de este rango de precio es vulnerable a réplicas de huella.
Es adecuado para control de acceso de personal en un invernadero; no lo es
para proteger algo de alto valor.

### No es un detector de CO certificado

El MQ-7 mide monóxido de carbono, pero este sistema **no sustituye a un
detector de CO homologado**. Si en el invernadero hay combustión, instala uno
certificado e independiente de este proyecto.

---

## Resumen

| Riesgo | Estado |
|---|---|
| Chapa abierta al perder alimentación | **Mitigado parcialmente** — faltan A y B |
| Chapa abierta durante el arranque | **Pendiente** — necesita la medida A |
| Chapa abierta por cuelgue del loop | Mitigado con perro guardián |
| Huella sin identidad abre la puerta | Mitigado |
| Sobrescritura silenciosa de plantillas | Mitigado |
| Pérdida del registro de nombres | Mitigado |
| Fuerza bruta en el lector | Mitigado |
| Apertura remota sin rastro | Mitigado con auditoría y push |
| Bomba en marcha en seco | Mitigado |
| Acceso físico al tablero | **No mitigable por firmware** |
| Seguridad del acceso remoto | Depende de la cuenta de RainMaker |

**Antes de instalar este sistema donde realmente haya algo que proteger,
implementa las medidas A y B.**
