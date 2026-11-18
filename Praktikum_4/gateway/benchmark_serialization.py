
import json
import time
import cbor2
import telemetry_pb2

# ==========================================
# KONFIGURASI BENCHMARK
# ==========================================

ITERATIONS = 10000

# Data telemetri contoh dari ESP32
sample_data = {
    "node_id": "ESP32_UNIB_01",
    "timestamp": 123456,
    "seq": 100,
    "temp": 28.5,
    "hum": 65.5,
    "lux": 750,
    "battery": 3.9,
    "status": "NORMAL"
}

# ==========================================
# 1. BENCHMARK JSON
# ==========================================

start = time.perf_counter()

for _ in range(ITERATIONS):
    json_payload = json.dumps(sample_data).encode("utf-8")

json_time = (time.perf_counter() - start) / ITERATIONS
json_size = len(json_payload)

# ==========================================
# 2. BENCHMARK CBOR
# ==========================================

start = time.perf_counter()

for _ in range(ITERATIONS):
    cbor_payload = cbor2.dumps(sample_data)

cbor_time = (time.perf_counter() - start) / ITERATIONS
cbor_size = len(cbor_payload)

# ==========================================
# 3. BENCHMARK PROTOBUF
# ==========================================

packet = telemetry_pb2.TelemetryPacket(
    node_id=sample_data["node_id"],
    timestamp=sample_data["timestamp"],
    seq=sample_data["seq"],
    temp=sample_data["temp"],
    hum=sample_data["hum"],
    lux=sample_data["lux"],
    battery=sample_data["battery"],
    status=sample_data["status"]
)

start = time.perf_counter()

for _ in range(ITERATIONS):
    protobuf_payload = packet.SerializeToString()

protobuf_time = (time.perf_counter() - start) / ITERATIONS
protobuf_size = len(protobuf_payload)

# ==========================================
# 4. HASIL BENCHMARK
# ==========================================

def reduction(json_bytes, other_bytes):
    return ((json_bytes - other_bytes) / json_bytes) * 100

print("\n" + "=" * 65)
print("       UNIB - BENCHMARK SERIALISASI DATA IoT")
print("=" * 65)

print(f"Jumlah iterasi : {ITERATIONS:,} kali")
print(f"Ukuran JSON    : {json_size} byte")
print(f"Ukuran CBOR    : {cbor_size} byte")
print(f"Ukuran Protobuf: {protobuf_size} byte")

print("\n" + "-" * 65)
print(f"{'FORMAT':<15}{'UKURAN':<15}{'ENCODE (us)':<18}{'PENGHEMATAN'}")
print("-" * 65)

print(
    f"{'JSON':<15}"
    f"{json_size:<15}"
    f"{json_time * 1_000_000:<18.3f}"
    f"{'0.00%'}"
)

print(
    f"{'CBOR':<15}"
    f"{cbor_size:<15}"
    f"{cbor_time * 1_000_000:<18.3f}"
    f"{reduction(json_size, cbor_size):.2f}%"
)

print(
    f"{'Protobuf':<15}"
    f"{protobuf_size:<15}"
    f"{protobuf_time * 1_000_000:<18.3f}"
    f"{reduction(json_size, protobuf_size):.2f}%"
)

print("=" * 65)
print("[SELESAI] Benchmark berhasil dijalankan.")
