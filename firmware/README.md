# ESP32 — temperatura de carpa (Tarea 2 / Ender IIoT)

## Configuración utilizada en la demostración

La **ESP32** funciona como nodo sensor IoT para medir la temperatura de la carpa de una impresora **Ender 3 Pro** usando un **DS18B20**. Publica la variable `temperatura_carpa` en **EMQX Cloud** mediante MQTT cifrado (TLS, puerto 8883); el listener Django recibe las lecturas y las guarda en PostgreSQL.

| Configuración | Valor / función |
|---|---|
| Hardware | ESP32 + sensor digital DS18B20 |
| Entrada física | Sensor DS18B20 (1-Wire) |
| Nombre del dispositivo Django | `ender-esp32` |
| Topic publicado | `ecs/ender-esp32/temperatura_carpa` |
| Unidad | °C |
| Tipo de mensaje | JSON: `{"value":22.5,"token":"TOKEN_DJANGO"}` |
| Broker | EMQX Cloud (TLS 8883) |
| Cuenta MQTT | `esp32-ender` |
| Dashboard | Gráfica de temperatura de carpa |

La contraseña MQTT y el token del dispositivo **no se publican** en el repositorio. Deben coincidir con los configurados en EMQX y Django, respectivamente.

## Sketch de referencia del montaje final

En [`ender_esp32_ds18b20/ender_esp32_ds18b20.ino`](ender_esp32_ds18b20/ender_esp32_ds18b20.ino) se incluye una implementación **solo de temperatura** para revisar y reproducir la integración. Usa OneWire, DallasTemperature, PubSubClient y WiFiClientSecure; no incluye servo, LED ni Google Sheets.

Para prepararla en Arduino IDE:

1. Instalar soporte ESP32, librerías **OneWire**, **DallasTemperature** y **PubSubClient**.
2. Copiar `ender_esp32_ds18b20/local_secrets.example.h` a `ender_esp32_ds18b20/local_secrets.h` y completar Wi-Fi, contraseña EMQX del cliente `esp32-ender` y token de Django para `ender-esp32`.
3. Abrir el `.ino` ubicado en esa misma carpeta; el certificado CA público ya está como `emqx_ca.h`.
4. Seleccionar la placa y el puerto, compilar y cargar **solo después de verificar conexiones y configuración**, sin sobrescribir sin necesidad el firmware que ya está funcionando en la ESP32.
5. Monitor serie a 115200 baudios; comprobar publicación `ecs/ender-esp32/temperatura_carpa`.

**Este sketch limpio es un entregable de referencia añadido después de la demostración; no se ha probado ni cargado en la placa física durante esta revisión.** El comportamiento real de publicación de temperatura ya fue comprobado con la ESP32 existente.

## Alcance final (sin servo)

**La configuración final del proyecto no utiliza servo ni control de luz desde la ESP32.** En el dashboard solo se muestra la temperatura de carpa. La actuación física sobre la Ender se realiza a través de **Raspberry Pi → OctoPrint** con comandos `sp_bed` y `sp_hotend`.

### Aclaración sobre archivos históricos del repositorio

- `temperatura_mqtt_hito4.ino` es un **respaldo de un prototipo anterior**, que incluye rutinas de servo, LED, registro Google Sheets y otras funciones. **No corresponde al montaje final sin servo y no debe flashearse como firmware final sin adaptación y validación**.
- `esp32_ecs_iot/esp32_ecs_iot.ino` es el **ejemplo genérico heredado del repositorio docente**, no el firmware operativo específico del DS18B20.
- `local_secrets.example.h` ilustra el manejo de credenciales locales. Los secretos reales deben permanecer fuera de GitHub.

Se conserva el histórico para trazabilidad de desarrollo. La evidencia operativa es la **ESP32 física publicando `temperatura_carpa`** y su gráfica activa en Django; el nuevo sketch de referencia permite revisar por separado la versión final simplificada.

## Comprobar desde Django

1. Entrar a `http://localhost:8000/` e iniciar sesión.
2. Identificar `Temperatura de carpa - ESP32` en el dashboard.
3. Confirmar actualización de `temperatura_carpa` y estado de dispositivo con lectura reciente.
4. Si no llegan mensajes, comprobar en EMQX `Clients` (conexión), `Authentication` (credenciales) y `Authorization` (permiso de publicación), además del slug/token Django.

Consulta el [README principal](../README.md) para arquitectura y criterios de la rúbrica.
