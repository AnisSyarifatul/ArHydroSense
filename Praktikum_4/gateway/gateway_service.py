
import os
import json
import time
import signal
import serial
import cbor2
import paho.mqtt.client as mqtt

from datetime import datetime

# ==========================================
# KONFIGURASI
# ==========================================

SERIAL_URL = os.getenv(
    "SERIAL_URL",
    "rfc2217://host.docker.internal:4000"
)

BAUDRATE = int(os.getenv("SERIAL_BAUDRATE", "115200"))

MQTT_BROKER = os.getenv("MQTT_BROKER", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_TOPIC = os.getenv("MQTT_TOPIC", "unib/iot/telemetry")

ARCHIVE_FILE = "telemetry_archive.cbor"

RECONNECT_DELAY = 3
MAX_LINE_SIZE = 4096

REQUIRED_FIELDS = {
    "node_id",
    "timestamp",
    "seq",
    "temp",
    "hum",
    "lux",
    "battery",
    "status"
}

running = True
mqtt_client = None


# ==========================================
# VALIDASI JSON
# ==========================================

def validate_data(data):

    if not isinstance(data, dict):
        raise ValueError("Payload bukan JSON object")

    missing = REQUIRED_FIELDS - data.keys()

    if missing:
        raise ValueError(f"Field tidak lengkap: {missing}")

    if not isinstance(data["node_id"], str):
        raise ValueError("node_id harus berupa string")

    if not isinstance(data["timestamp"], (int, float)):
        raise ValueError("timestamp harus berupa angka")

    if not isinstance(data["seq"], int) or isinstance(data["seq"], bool):
        raise ValueError("seq harus berupa integer")

    for field in ("temp", "hum", "battery"):
        if not isinstance(data[field], (int, float)):
            raise ValueError(f"{field} harus berupa angka")

    if not isinstance(data["lux"], int) or isinstance(data["lux"], bool):
        raise ValueError("lux harus berupa integer")

    if not isinstance(data["status"], str):
        raise ValueError("status harus berupa string")


# ==========================================
# ARSIP CBOR
# ==========================================

def save_archive(data):

    archive_data = {
        "received_at": datetime.now().isoformat(timespec="seconds"),
        "telemetry": data
    }

    payload = cbor2.dumps(archive_data)

    with open(ARCHIVE_FILE, "ab") as file:
        file.write(len(payload).to_bytes(4, byteorder="big"))
        file.write(payload)

    print(f"[ARCHIVE] Data tersimpan ({len(payload)} byte)")


# ==========================================
# MQTT
# ==========================================

def connect_mqtt():

    global mqtt_client

    mqtt_client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id="unib-edge-gateway"
    )

    while running:

        try:
            print(
                f"[MQTT] Menghubungkan ke "
                f"{MQTT_BROKER}:{MQTT_PORT}..."
            )

            mqtt_client.connect(
                MQTT_BROKER,
                MQTT_PORT,
                keepalive=60
            )

            mqtt_client.loop_start()

            print("[MQTT] Berhasil terhubung!")
            print(f"[MQTT] Topic: {MQTT_TOPIC}")

            return True

        except (OSError, mqtt.MqttException) as error:

            print(f"[MQTT ERROR] {error}")
            print(f"[MQTT] Mencoba kembali dalam {RECONNECT_DELAY} detik...")

            time.sleep(RECONNECT_DELAY)

    return False


def publish_mqtt(data):

    if mqtt_client is None or not mqtt_client.is_connected():
        print("[MQTT ERROR] Broker belum terhubung")
        return

    payload = json.dumps(data)

    result = mqtt_client.publish(
        MQTT_TOPIC,
        payload,
        qos=1
    )

    if result.rc == mqtt.MQTT_ERR_SUCCESS:
        print("[MQTT] Data berhasil dikirim")
    else:
        print(f"[MQTT ERROR] Gagal publish: {result.rc}")


# ==========================================
# PROSES TELEMETRI
# ==========================================

def process_payload(raw_line):

    try:
        text = raw_line.decode("utf-8").strip()

        if not text:
            return

        data = json.loads(text)

        validate_data(data)

        print("\n" + "=" * 60)
        print("[GATEWAY] DATA TELEMETRI DITERIMA")
        print("=" * 60)

        print(f"Node ID    : {data['node_id']}")
        print(f"Sequence   : {data['seq']}")
        print(f"Suhu       : {data['temp']} °C")
        print(f"Kelembapan : {data['hum']} %")
        print(f"Lux        : {data['lux']}")
        print(f"Baterai    : {data['battery']} V")
        print(f"Status     : {data['status']}")

        save_archive(data)

        publish_mqtt(data)

        print("[VALIDASI] Data JSON valid")
        print("=" * 60)

    except (
        UnicodeDecodeError,
        json.JSONDecodeError,
        ValueError
    ) as error:

        print(f"[PARSER ERROR] {error}")
        print(f"[RAW DATA] {raw_line!r}")


# ==========================================
# SERIAL RECEIVER
# ==========================================

def receive_serial():

    while running:

        ser = None

        try:

            print(f"\n[SERIAL] Menghubungkan ke {SERIAL_URL}...")

            ser = serial.serial_for_url(
                SERIAL_URL,
                baudrate=BAUDRATE,
                timeout=5
            )

            print("[SERIAL] Berhasil terhubung!")
            print("[SERIAL] Menunggu data ESP32...")

            buffer = bytearray()

            while running:

                chunk = ser.read(256)

                if not chunk:
                    continue

                buffer.extend(chunk)

                if len(buffer) > MAX_LINE_SIZE:
                    print("[FRAMING ERROR] Ukuran paket terlalu besar")
                    buffer.clear()
                    continue

                while b"\n" in buffer:

                    raw_line, _, remaining = buffer.partition(b"\n")
                    buffer = bytearray(remaining)

                    if raw_line:
                        process_payload(bytes(raw_line) + b"\n")

        except (serial.SerialException, OSError) as error:

            print(f"[SERIAL ERROR] {error}")
            print(f"[RECONNECT] Mencoba kembali dalam {RECONNECT_DELAY} detik...")

        finally:

            if ser is not None:
                try:
                    ser.close()
                except Exception:
                    pass

        if running:
            time.sleep(RECONNECT_DELAY)


# ==========================================
# SHUTDOWN
# ==========================================

def stop_program(signum, frame):

    global running

    print("\n[SYSTEM] Menghentikan Edge Gateway...")
    running = False


# ==========================================
# PROGRAM UTAMA
# ==========================================

def main():

    print("=" * 60)
    print("       UNIB - PYTHON EDGE GATEWAY")
    print("=" * 60)

    signal.signal(signal.SIGTERM, stop_program)
    signal.signal(signal.SIGINT, stop_program)

    if not connect_mqtt():
        return

    try:
        receive_serial()

    finally:

        if mqtt_client is not None:
            mqtt_client.loop_stop()
            mqtt_client.disconnect()

        print("[SYSTEM] Edge Gateway berhenti.")


if __name__ == "__main__":
    main()
