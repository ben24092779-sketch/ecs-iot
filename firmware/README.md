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

## Alcance final (sin servo)

**La configuración final del proyecto no utiliza servo ni control de luz desde la ESP32.** En el dashboard solo se muestra la temperatura de carpa. La actuación física sobre la Ender se realiza a través de **Raspberry Pi → OctoPrint** con comandos `sp_bed` y `sp_hotend`.

### Aclaración sobre archivos históricos del repositorio

- `temperatura_mqtt_hito4.ino` es un **respaldo de un prototipo anterior**, que incluye rutinas de servo, LED, registro Google Sheets y otras funciones. **No corresponde al montaje final sin servo y no debe flashearse como firmware final sin adaptación y validación**.
- `esp32_ecs_iot/esp32_ecs_iot.ino` es el **ejemplo genérico heredado del repositorio docente**, no el firmware operativo específico del DS18B20.
- `local_secrets.example.h` ilustra el manejo de credenciales locales. Los secretos reales deben permanecer fuera de GitHub.

Se conserva el histórico para trazabilidad de desarrollo, pero la evidencia de operación final es la **ESP32 física publicando `temperatura_carpa`** y su gráfica activa en Django. El programa histórico por sí solo **no constituye un firmware mínimo y reproducible del montaje final**.

## Comprobar desde Django

1. Entrar a `http://localhost:8000/` e iniciar sesión.
2. Identificar `Temperatura de carpa - ESP32` en el dashboard.
3. Confirmar actualización de `temperatura_carpa` y estado de dispositivo con lectura reciente.
4. Si no llegan mensajes, comprobar en EMQX `Clients` (conexión), `Authentication` (credenciales) y `Authorization` (permiso de publicación), además del slug/token Django.

Consulta el [README principal](../README.md) para arquitectura y criterios de la rúbrica.
