import os
import struct
import time

from django.core.management.base import BaseCommand

from monitoring.models import Sensor

try:
    from s7 import Client
except ImportError as exc:
    raise RuntimeError(
        "python-snap7 no esta instalado. Instala las dependencias de backend."
    ) from exc


REAL_SIGNALS = [
    ("ender-esp32", "temperatura_carpa", 0, "TemperaturaCarpa"),
    ("ender-rpi", "temp_hotend", 4, "TempHotend"),
    ("ender-rpi", "sp_hotend", 8, "SPHotend"),
    ("ender-rpi", "temp_bed", 12, "TempBed"),
    ("ender-rpi", "sp_bed", 16, "SPBed"),
    ("ender-rpi", "print_progress", 20, "ProgresoImpresion"),
    ("ender-rpi", "print_time_s", 24, "TiempoImpresion"),
    ("ender-rpi", "printer_state", 28, "EstadoImpresora"),
]


class Command(BaseCommand):
    help = "Sincroniza variables Django/PostgreSQL hacia DB_Ender_IoT del PLC S7-1200."

    def handle(self, *args, **options):
        host = os.getenv("PLC_HOST", "10.41.32.1")
        rack = int(os.getenv("PLC_RACK", "0"))
        slot = int(os.getenv("PLC_SLOT", "1"))
        db_number = int(os.getenv("PLC_DB_NUMBER", "1"))
        interval = float(os.getenv("PLC_SYNC_INTERVAL", "2"))

        plc = Client()

        self.stdout.write(
            self.style.SUCCESS(
                f"PLC gateway iniciado: {host} rack={rack} slot={slot} DB={db_number}"
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

                    for slug, variable, offset, label in REAL_SIGNALS:
                        sensor = Sensor.objects.get(
                            device__slug=slug,
                            variable=variable,
                            is_active=True,
                        )
                        reading = sensor.readings.order_by("-timestamp").first()
                        value = float(reading.value) if reading else 0.0

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

                    self.stdout.write(
                        "SYNC OK | "
                        + " | ".join(
                            [
                                f"{label}={float(Sensor.objects.get(device__slug=slug, variable=variable).readings.order_by('-timestamp').first().value):.2f}"
                                if Sensor.objects.get(device__slug=slug, variable=variable).readings.order_by("-timestamp").first()
                                else f"{label}=0.00"
                                for slug, variable, _, label in REAL_SIGNALS
                            ]
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
