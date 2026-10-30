import os
import struct
import time

from django.core.management.base import BaseCommand

from monitoring.models import Sensor
from monitoring.mqtt_publish import publish_command

try:
    from s7 import Client
except ImportError as exc:
    raise RuntimeError(
        "python-snap7 no esta instalado. Instala las dependencias de backend."
    ) from exc


TELEMETRY_SIGNALS = [
    ("ender-esp32", "temperatura_carpa", 0, "TemperaturaCarpa"),
    ("ender-rpi", "temp_hotend", 4, "TempHotend"),
    ("ender-rpi", "temp_bed", 12, "TempBed"),
    ("ender-rpi", "print_progress", 20, "ProgresoImpresion"),
    ("ender-rpi", "print_time_s", 24, "TiempoImpresion"),
    ("ender-rpi", "printer_state", 28, "EstadoImpresora"),
]

COMMAND_SIGNALS = [
    ("ender-rpi", "sp_hotend", 8, "SPHotend", 0.0, 240.0),
    ("ender-rpi", "sp_bed", 16, "SPBed", 0.0, 100.0),
]



class Command(BaseCommand):
    help = "Sincroniza variables Django/PostgreSQL hacia DB_Ender_IoT del PLC S7-1200."

    def handle(self, *args, **options):
        host = os.getenv("PLC_HOST", "10.41.32.1")
        rack = int(os.getenv("PLC_RACK", "0"))
        slot = int(os.getenv("PLC_SLOT", "1"))
        db_number = int(os.getenv("PLC_DB_NUMBER", "1"))
        interval = float(os.getenv("PLC_SYNC_INTERVAL", "2"))
        commands_enabled = os.getenv("PLC_COMMANDS_ENABLED", "false").lower() in {
            "1", "true", "yes", "on"
        }

        plc = Client()
        last_commands = {}

        self.stdout.write(
            self.style.SUCCESS(
                f"PLC gateway iniciado: {host} rack={rack} slot={slot} DB={db_number}"
            )
        )
        self.stdout.write(
            self.style.SUCCESS("PLC -> MQTT: HABILITADO")
            if commands_enabled
            else self.style.WARNING(
                "PLC -> MQTT: DESHABILITADO (solo lectura; define PLC_COMMANDS_ENABLED=true para enviar setpoints)"
            )
        )

        try:
            while True:
                try:
                    if not plc.get_connected():
                        plc.connect(host, rack, slot)
                        if not plc.get_connected():
                            raise ConnectionError("No fue posible conectar al PLC")
                        self.stdout.write(self.style.SUCCESS("PLC conectado"))

                    values = {}
                    for slug, variable, offset, label in TELEMETRY_SIGNALS:
                        sensor = Sensor.objects.get(
                            device__slug=slug,
                            variable=variable,
                            is_active=True,
                        )
                        reading = sensor.readings.order_by("-timestamp").first()
                        value = float(reading.value) if reading else 0.0
                        values[label] = value

                        plc.db_write(
                            db_number,
                            offset,
                            bytearray(struct.pack(">f", value)),
                        )

                    sensor = Sensor.objects.get(
                        device__slug="ender-rpi",
                        variable="raspberry_online",
                        is_active=True,
                    )
                    reading = sensor.readings.order_by("-timestamp").first()
                    online = bool(float(reading.value)) if reading else False

                    status_byte = plc.db_read(db_number, 32, 1)
                    if online:
                        status_byte[0] |= 0x01
                    else:
                        status_byte[0] &= 0xFE
                    plc.db_write(db_number, 32, status_byte)

                    # Los setpoints son comandos PLC -> Ender. Al conectar por
                    # primera vez se inicializan con el ultimo valor conocido de Django
                    # para evitar enviar un comando accidental.
                    if not last_commands:
                        for slug, variable, offset, label, min_value, max_value in COMMAND_SIGNALS:
                            sensor = Sensor.objects.get(
                                device__slug=slug,
                                variable=variable,
                                is_active=True,
                            )
                            reading = sensor.readings.order_by("-timestamp").first()
                            value = float(reading.value) if reading else 0.0
                            value = max(min_value, min(max_value, value))
                            plc.db_write(
                                db_number,
                                offset,
                                bytearray(struct.pack(">f", value)),
                            )
                            last_commands[variable] = value
                            values[label] = value
                    else:
                        for slug, variable, offset, label, min_value, max_value in COMMAND_SIGNALS:
                            raw = plc.db_read(db_number, offset, 4)
                            value = struct.unpack(">f", bytes(raw))[0]
                            values[label] = value

                            if not (min_value <= value <= max_value):
                                self.stderr.write(
                                    self.style.ERROR(
                                        f"COMANDO RECHAZADO {label}={value:.2f}; "
                                        f"rango permitido {min_value:.0f}-{max_value:.0f}"
                                    )
                                )
                                continue

                            previous = last_commands.get(variable)
                            if previous is None or abs(value - previous) >= 0.1:
                                if commands_enabled:
                                    sensor = Sensor.objects.get(
                                        device__slug=slug,
                                        variable=variable,
                                        is_active=True,
                                    )
                                    ok = publish_command(sensor.device, variable, value)
                                    if ok:
                                        self.stdout.write(
                                            self.style.SUCCESS(
                                                f"COMANDO PLC -> MQTT OK | {label}={value:.2f}"
                                            )
                                        )
                                        last_commands[variable] = value
                                    else:
                                        self.stderr.write(
                                            self.style.ERROR(
                                                f"COMANDO PLC -> MQTT ERROR | {label}={value:.2f}"
                                            )
                                        )
                                else:
                                    self.stdout.write(
                                        f"COMANDO PLC detectado (MQTT deshabilitado) | "
                                        f"{label}={value:.2f}"
                                    )
                                    last_commands[variable] = value

                    self.stdout.write(
                        "SYNC OK | "
                        + " | ".join(
                            f"{label}={values[label]:.2f}"
                            for _, _, _, label in TELEMETRY_SIGNALS
                        )
                        + f" | RaspberryOnline={online}"
                    )

                    time.sleep(interval)

                except KeyboardInterrupt:
                    break
                except Exception as exc:
                    self.stderr.write(self.style.ERROR(f"PLC gateway error: {exc}"))
                    try:
                        plc.disconnect()
                    except Exception:
                        pass
                    time.sleep(5)
        finally:
            try:
                plc.disconnect()
            except Exception:
                pass
