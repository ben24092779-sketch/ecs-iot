# Supervisión IIoT Ender 3 Pro

**Tópicos de Especialidad II — Tarea 2 (UNAB, octubre de 2026)**

Plataforma web de supervisión y control remoto para una **impresora 3D Creality Ender 3 Pro**, desarrollada con **Django (Python)** a partir del repositorio base [urbanatika/ecs-iot](https://github.com/urbanatika/ecs-iot).

El proyecto integra una **ESP32 con sensor DS18B20**, una **Raspberry Pi 3B+ con OctoPrint**, **EMQX Cloud (MQTT/TLS)**, **PostgreSQL** y un **gateway Python/Snap7 para un PLC Siemens desde TIA Portal**. El dashboard y el login fueron rediseñados con identidad visual **ENDER IIoT**.

> **Versión del proyecto para evaluación:** rama [`hito9-plc`](https://github.com/ben24092779-sketch/ecs-iot/tree/hito9-plc). Hasta integrar esta rama a `main`, usar expresamente `hito9-plc` al revisar o clonar el trabajo.

## 1. Objetivo y funcionamiento

Supervisar las variables reales de la impresora y la temperatura de su carpa, visualizarlas en una interfaz web con control de acceso y permitir modificar setpoints de manera remota.

La impresora, la ESP32 y la Raspberry pueden permanecer en la **casa**, mientras la plataforma Django y el PLC se encuentran en el **laboratorio**. La comunicación entre redes utiliza el broker EMQX Cloud, sin necesidad de que Django acceda a la IP privada de OctoPrint.

```text
CASA                                           NUBE / LABORATORIO

DS18B20 -- 1-Wire -- ESP32
                         \                        EMQX Cloud
                          \--- MQTT/TLS:8883 --- [broker] ---- MQTT/TLS ---- mqtt_listener
                          /                             |                         |
Ender 3 Pro -- USB -- Raspberry Pi                  comandos                   PostgreSQL
                       | OctoPrint API               MQTT /set                    |
                       | gateway.py                    |                        Django
                       +-------------------------------+                         |
                                                                               Login
                                                                            Dashboard
                                                                               |
                                                                          Gateway Snap7
                                                                               |
                                                                           PLC Siemens
                                                                           / TIA Portal
```

**Importante:** OctoEverywhere se puede usar por separado para abrir la interfaz completa de OctoPrint desde Internet. **No es la tecnología que comunica el dashboard con la Raspberry**; ese camino funciona mediante EMQX Cloud.

## 2. Hardware y software utilizados

| Elemento | Función |
|---|---|
| **Ender 3 Pro** | Proceso físico: hotend, cama y estado de impresión |
| **Raspberry Pi 3B+** | Ejecuta OctoPrint y un servicio Python que consulta su API, publica telemetría y recibe comandos MQTT |
| **ESP32 + DS18B20** | Mide la temperatura de la carpa, independiente de las temperaturas internas de la impresora |
| **PLC Siemens + TIA Portal** | Lee variables y permite introducir setpoints en un DB; comunicación con Python mediante Snap7 |
| **EMQX Cloud** | Broker MQTT accesible desde redes diferentes, con autenticación/autorización y TLS |
| **Django (Python)** | Login, permisos, vistas, API y publicación de comandos |
| **PostgreSQL** | Dispositivos registrados, usuarios, tokens, lecturas y configuración del dashboard |
| **Docker Compose** | Despliega los servicios del PC |
| **Chart.js + JavaScript** | Gráficas, tarjetas y actualización de valores por consulta periódica (cada 2,5 s) |

**Montaje final:** no utiliza servo ni actuador de luz de la ESP32. La actuación física demostrada corresponde a los setpoints de **cama** y **hotend** de la Ender, aplicados por OctoPrint. El repositorio conserva un sketch histórico del Hito 4 con funciones de servo, pero **no forma parte del alcance ni se utiliza como actuador en la configuración final**.

## 3. Variables del dashboard

Las nueve variables configuradas se agrupan así:

| Dispositivo | Variable | Unidad | Descripción |
|---|---|---|---|
| `ender-esp32` | `temperatura_carpa` | °C | Temperatura medida por DS18B20 |
| `ender-rpi` | `temp_hotend` | °C | Temperatura actual del hotend |
| `ender-rpi` | `sp_hotend` | °C | Objetivo del hotend; control mediante slider |
| `ender-rpi` | `temp_bed` | °C | Temperatura actual de la cama |
| `ender-rpi` | `sp_bed` | °C | Objetivo de la cama; control mediante slider |
| `ender-rpi` | `printer_state` | código | 0: offline; 1: operativa; 2: imprimiendo; 3: pausada; 4: error |
| `ender-rpi` | `print_progress` | % | Porcentaje de progreso de impresión |
| `ender-rpi` | `print_time_s` | s | Tiempo de impresión transcurrido |
| `ender-rpi` | `raspberry_online` | 0/1 | Estado reportado por el gateway Raspberry; el dashboard también verifica la antigüedad de la lectura |

**Temperatura real** y **setpoint** son magnitudes diferentes. Una lectura real de 45 °C puede persistir un rato incluso después de cambiar el objetivo a 0 °C.

## 4. Comunicación MQTT

### Publicación de lecturas

- Broker: **EMQX Cloud**, puerto **8883**, conexión **TLS**.
- Topic: `ecs/<device_slug>/<variable>`.
- Ejemplos: `ecs/ender-esp32/temperatura_carpa`, `ecs/ender-rpi/temp_bed`.
- Payload JSON: `{"value":22.5,"token":"TOKEN_DEL_DISPOSITIVO"}`.
- QoS 1, sin `retain`.

El proceso `run_mqtt_listener` se suscribe a `ecs/+/+`, verifica el dispositivo y su token y guarda lecturas en PostgreSQL. Si el token no coincide, no acepta el dato como telemetría válida.

### Comandos de control

- Topic: `ecs/ender-rpi/sp_bed/set` o `ecs/ender-rpi/sp_hotend/set`.
- Payload: `{"value":45,"token":"TOKEN_DEL_DISPOSITIVO"}`.
- Django publica el comando; la Raspberry suscrita lo valida y llama a la API REST de OctoPrint; después vuelve a publicar el setpoint leído para confirmarlo en el dashboard.

**“Comando enviado” no equivale a “comando ejecutado”.** El valor se considera confirmado cuando retorna como telemetría y coincide con el objetivo enviado. Si se interrumpe la señal, hay que verificar el objetivo en OctoPrint antes de asumir que se aplicó.

### EMQX: conceptos utilizados

- **Authentication:** verifica las credenciales de los clientes MQTT, como `esp32-ender`, `django-ender` y `rpi-ender`.
- **Authorization:** reglas `Publish`/`Subscribe`, `Allow`/`Deny` para limitar cada cliente a los topics que necesita.
- **Clients:** muestra conexiones activas, Client ID, usuario y suscripciones.
- **TLS y certificado CA:** cifran la conexión y permiten validar el servidor.
- **Token Django:** es independiente del usuario/contraseña EMQX; identifica al dispositivo registrado dentro de Django.

## 5. Acceso web, base de datos y diseño

- URL local: **http://localhost:8000/monitoring/**.
- Login: **http://localhost:8000/accounts/login/**.
- La vista del dashboard requiere autenticación con Django (`@login_required`). Los comandos web usan sesión y protección CSRF.
- Django almacena usuarios/roles, dispositivos, variables y lecturas en **PostgreSQL**.
- Diseño propio: identidad **ENDER IIoT**, pantalla de login, cabecera, resumen de cinco variables, tarjetas ESP32/Raspberry, gráficas, indicadores de conexión, colores diferenciados para telemetría y controles.
- Visualización: el navegador consulta la API de últimas lecturas aproximadamente cada **2,5 s**. No se implementaron WebSockets.

## 6. Integración PLC — TIA Portal y Snap7

El gateway `backend/monitoring/management/commands/run_plc_gateway.py` utiliza **python-snap7** para comunicar el PC con el PLC. En la configuración registrada se utiliza DB 1, rack 0, slot 1, con actualización cada 2 s y dirección del PLC configurable.

| Offset DB (byte) | Nombre en TIA Portal | Dirección | Tipo |
|---:|---|---|---|
| 0 | `TemperaturaCarpa` | Django → PLC | REAL |
| 4 | `TempHotend` | Django → PLC | REAL |
| 8 | `SPHotend` | PLC → gateway | REAL |
| 12 | `TempBed` | Django → PLC | REAL |
| 16 | `SPBed` | PLC → gateway | REAL |
| 20 | `ProgresoImpresion` | Django → PLC | REAL |
| 24 | `TiempoImpresion` | Django → PLC | REAL |
| 28 | `EstadoImpresora` | Django → PLC | REAL |
| 32, bit 0 | `RaspberryOnline` | Django → PLC | BOOL |

El gateway escribe telemetría desde PostgreSQL al DB, y lee los setpoints desde el PLC. Al establecer la conexión inicial sincroniza los SP con los últimos valores conocidos para evitar ejecutar objetivos antiguos de forma accidental. Los comandos nuevos se leen desde el DB después de esa inicialización.

La publicación de cambios desde el PLC tiene una **habilitación deliberada**:

```dotenv
PLC_COMMANDS_ENABLED=false  # Por defecto: solo lectura; no publica comandos
PLC_COMMANDS_ENABLED=true   # Para pruebas supervisadas PLC -> EMQX -> Ender
```

En la demostración del laboratorio se comprobó que **TIA Portal/PLC → Snap7 → gateway en PowerShell** leyó `SPBed=30`. No se verificó en esa demostración la aplicación de dicho setpoint a la Ender desde el PLC, porque la publicación MQTT estaba deshabilitada. El recorrido **Django → EMQX → Raspberry → OctoPrint → Ender → Django** sí fue probado por separado con los sliders del dashboard.

> Antes de habilitar comandos reales desde un PLC, comprobar la conexión de OctoPrint, la lectura del DB, los rangos, los setpoints iniciales y la posibilidad de detener calentadores; realizar las pruebas con supervisión. Un mensaje de publicación MQTT aceptada no garantiza por sí solo ejecución física.

## 7. Estructura del repositorio

```text
ecs-iot/
├── backend/                    Django, PostgreSQL models, login, dashboard, API
│   ├── monitoring/
│   │   ├── management/commands/
│   │   │   ├── run_mqtt_listener.py
│   │   │   └── run_plc_gateway.py
│   │   ├── static/monitoring/   CSS y JavaScript del dashboard
│   │   └── templates/monitoring/
│   └── certs/emqxsl-ca.crt
├── firmware/                   Firmware ESP32 y archivos de referencia
├── rpi_gateway/
│   ├── gateway.py              Puente OctoPrint ↔ MQTT
│   ├── ender-rpi-gateway.service
│   └── ender-rpi-gateway.env.example
├── docker-compose.yml
├── .env.example
└── README.md
```

Los servicios `mosquitto` y `simulator` proceden del proyecto de referencia y se conservan como herramientas de desarrollo. **La demostración remota de la Ender usa EMQX Cloud, no el Mosquitto local ni los dispositivos simulados**.

## 8. Preparación del PC (Windows)

### Requisitos

- Git y Docker Desktop en ejecución.
- Internet para conectarse con EMQX Cloud.
- Credenciales y tokens reales de EMQX, dispositivos y Django (no se incluyen en GitHub).
- Para integrar el PLC en laboratorio: conexión física de red PC ↔ PLC y acceso a TIA Portal/DB correspondiente.
- Para telemetría real: ESP32 y Raspberry con sus respectivas configuraciones y servicios funcionando en casa.

### Obtener la versión del proyecto

```powershell
git clone -b hito9-plc https://github.com/ben24092779-sketch/ecs-iot.git
cd ecs-iot
Copy-Item .env.example .env
```

Editar `.env` con contraseñas y parámetros reales. **No publicar el archivo `.env` ni tokens/API keys**. El certificado CA de EMQX está en `backend/certs/`.

Arrancar el entorno web, la base de datos y el listener MQTT (sin el simulador ni el PLC):

```powershell
docker compose up -d --build db migrate web mqtt_listener
docker compose ps
```

Abrir **http://localhost:8000/**. Si no hay sesión activa, Django redirige al login. La creación inicial del superusuario se configura mediante `DJANGO_SUPERUSER_*` en `.env`.

Los dispositivos `ender-esp32` y `ender-rpi` deben existir en PostgreSQL, con sus tokens idénticos a los configurados en cada emisor. **Una instalación nueva no contiene automáticamente las lecturas o los dispositivos de la instalación de desarrollo**.

Para reconstruir el frontend si se modificó código:

```powershell
docker compose up -d --build --force-recreate --no-deps web
```

### PLC (solo con equipo en laboratorio)

Configurar en `.env` los valores `PLC_HOST`, `PLC_RACK`, `PLC_SLOT`, `PLC_DB_NUMBER` y, tras revisar las condiciones de prueba, `PLC_COMMANDS_ENABLED`. Arrancar solo cuando el PLC esté disponible:

```powershell
docker compose --profile plc up -d --build plc_gateway
docker compose logs -f plc_gateway
```

Si el PC está en casa sin PLC, puede detenerse con:

```powershell
docker compose stop plc_gateway
```

### Raspberry Pi

El servicio instalado mediante systemd en la Raspberry es `ender-rpi-gateway`. Consulta la API local de OctoPrint y se comunica con EMQX por MQTT/TLS. La configuración privada se mantiene en la Raspberry en `/etc/ender-rpi-gateway.env` y **no está en el repositorio**. El archivo `rpi_gateway/ender-rpi-gateway.env.example` describe las variables necesarias.

La dirección IP local de la Raspberry **puede cambiar por DHCP**. No se debe asumir que una dirección antigua sigue perteneciendo a OctoPrint. OctoEverywhere es una vía opcional e independiente de acceso a la interfaz completa; la plataforma Django no depende de ese servicio.

## 9. Pruebas realizadas y límites

**Comprobado:** ESP32/DS18B20 publica temperatura de carpa; Raspberry/OctoPrint publica temperaturas, estado y progreso; Django recibe y muestra telemetría; login con usuarios y PostgreSQL; control de setpoints de hotend y cama desde el dashboard, con confirmación de retorno; funcionamiento con PC conectado a una Wi-Fi distinta a la red de la casa.

**Comprobado en laboratorio:** lectura de setpoint desde un PLC mediante Snap7, visible en consola.

**Pendiente de validación extremo a extremo con PLC:** habilitar el envío de comandos desde el PLC hacia EMQX (`PLC_COMMANDS_ENABLED=true`) y confirmar su aplicación/retorno en OctoPrint y el dashboard.

**Nota sobre los datos:** el indicador del gateway Raspberry y el estado de conexión real de OctoPrint son distintos. La Raspberry puede estar conectada a EMQX mientras la impresora aparece `Offline`.

## 10. Correspondencia con rúbrica de Tarea 2

| Criterio | Evidencia del proyecto |
|---|---|
| Repositorio en cuenta personal | Fork público y ramas de desarrollo del proyecto |
| Diseño acorde con logo/colores | Login y panel propios **ENDER IIoT**; gráficos y controles específicos |
| Sensor IoT y datos en tiempo real | ESP32 + DS18B20 por EMQX y Raspberry + OctoPrint, actualizaciones periódicas |
| Control de usuarios en base de datos | Autenticación y roles de Django + PostgreSQL |
| Control de variables leído/ejecutado | Setpoints hotend/cama con retorno confirmado; PLC leído mediante Snap7 (envío PLC → MQTT pendiente de ensayo final) |

---

**Créditos:** desarrollo y adaptación del proyecto académico a partir de [urbanatika/ecs-iot](https://github.com/urbanatika/ecs-iot). Se conserva el historial del fork original.
