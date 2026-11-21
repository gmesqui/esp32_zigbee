#!/usr/bin/env python3
"""Sign, install and verify coordinator application images over Ethernet."""
import argparse
import base64
import hashlib
import http.client
import json
import struct
import sys
import time
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT = Path(__file__).resolve().parents[1]
KEY = Path.home() / ".config/esp32-zigbee-ota/signing.pem"
PRODUCT = "esp32_zigbee3"
CHIPS = {0x17: "esp32c5", 0x0D: "esp32c6"}
SLOT_SIZE = 0x1E0000


def image_info(data):
    if len(data) < 288 or data[0] != 0xe9 or struct.unpack_from("<I", data, 32)[0] != 0xabcd5432:
        raise ValueError("Se esperaba un binario de aplicación ESP-IDF.")
    chip = CHIPS.get(struct.unpack_from("<H", data, 12)[0])
    product = data[80:112].split(b"\0")[0]
    if chip is None or product != PRODUCT.encode():
        raise ValueError("La imagen no es de un coordinador Zigbee C5/C6.")
    return chip, data[48:80].split(b"\0")[0].decode(), data[176:208].hex()


def signing_key(path):
    key = serialization.load_pem_private_key(path.read_bytes(), password=None)
    pub = serialization.load_pem_public_key((ROOT / "main/ota_public.pem").read_bytes())
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.public_key().public_numbers() != pub.public_numbers():
        raise ValueError("La clave de firma no corresponde a la clave pública del firmware.")
    return key


def request(host, port, method, route, data=None, headers=None):
    conn = http.client.HTTPConnection(host, port, timeout=10)
    try:
        conn.request(method, route, body=data, headers=headers or {})
        response = conn.getresponse()
        body = response.read()
        if response.status != 200:
            raise ValueError(f"{method} {route}: HTTP {response.status} {body[:150]!r}")
        return body
    finally:
        conn.close()


def info(host):
    return json.loads(request(host, 8082, "GET", "/ota"))


def network(host):
    obj = json.loads(request(host, 8080, "GET", "/api/status"))
    zb = obj["zigbee"]
    if not zb["ready"] or not all(k in zb for k in ("pan_id", "ext_pan_id", "coordinator_ieee", "channel")):
        raise ValueError("La identidad Zigbee aún no está disponible.")
    devices = {item["ieee"] for item in obj["devices"]}
    if len(devices) != obj["device_count"]:
        raise ValueError("El inventario Zigbee recibido está incompleto.")
    return zb, devices


def install(host, data, signature, key):
    chip, version, elf = image_info(data)
    before = info(host)
    if before["chip"] != chip or before["product"] != PRODUCT or not before["confirmed"]:
        raise ValueError("El coordinador no está confirmado o no corresponde a la imagen.")
    original, devices = network(host)
    digest = hashlib.sha256(data).digest()
    nonce = bytes.fromhex(before["nonce"])
    if len(nonce) != 32:
        raise ValueError("Desafío no válido.")
    credentials = key.sign(b"esp32_zigbee3/upload/v1" + nonce + digest, ec.ECDSA(hashes.SHA256()))
    headers = {k: base64.b64encode(v).decode("ascii") for k, v in (
        ("X-Image-SHA256", digest), ("X-Image-Signature", signature), ("X-Upload-Signature", credentials))}
    expected_slot = {"ota_0": "ota_1", "ota_1": "ota_0"}[before["slot"]]
    print(f"Enviando {len(data)} bytes a {host} ({chip}, versión {version}, {expected_slot})", flush=True)
    try:
        # Keep the normal WebSocket/status service available during this transfer.
        conn = http.client.HTTPConnection(host, 8082, timeout=190)
        try:
            conn.putrequest("POST", "/ota")
            conn.putheader("Content-Length", str(len(data)))
            for name, value in headers.items():
                conn.putheader(name, value)
            conn.endheaders()
            for start in range(0, len(data), 65536):
                chunk = data[start:start+65536]
                conn.send(chunk)
                print(f"\rEnviado {(start+len(chunk))*100//len(data)} %", end="", flush=True)
            response = conn.getresponse()
            body = response.read()
            if response.status != 200:
                raise ValueError(f"HTTP {response.status}: {body[:150]!r}")
        finally:
            conn.close()
    except (OSError, http.client.HTTPException) as exc:
        print(f"\nTransferencia interrumpida ({exc}). Comprobando qué imagen arrancó.", flush=True)

    print("\nEsperando el arranque y la confirmación por Ethernet…", flush=True)
    deadline = time.monotonic() + 180
    last = "sin respuesta"
    while time.monotonic() < deadline:
        time.sleep(2)
        try:
            live = info(host)
            last = f"{live['slot']}, ELF {live['elf_sha256']}, confirmada={live['confirmed']}"
            if live["slot"] != expected_slot or live["elf_sha256"] != elf or not live["confirmed"]:
                continue
            restored, current = network(host)
            if any(restored[k] != original[k] for k in ("ext_pan_id", "pan_id", "coordinator_ieee", "channel")):
                raise RuntimeError("La identidad de la red Zigbee ha cambiado.")
            if not devices.issubset(current):
                raise RuntimeError("Faltan dispositivos del inventario anterior.")
            print(f"Confirmada {version} en {expected_slot}. Red y {len(devices)} dispositivos conservados.")
            return
        except (OSError, http.client.HTTPException, ValueError, KeyError) as exc:
            last = str(exc)
    raise RuntimeError(f"No se pudo confirmar la actualización. Último estado: {last}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    get = sub.add_parser("status")
    get.add_argument("host")
    for verb in ("sign", "upload"):
        cmd = sub.add_parser(verb)
        if verb == "upload":
            cmd.add_argument("host")
        cmd.add_argument("image", type=Path)
        cmd.add_argument("--key", type=Path, default=KEY)
    args = parser.parse_args()
    if args.action == "status":
        print(json.dumps(info(args.host), indent=2))
        return
    data = args.image.read_bytes()
    chip, version, _ = image_info(data)
    if not 4096 <= len(data) <= SLOT_SIZE:
        raise ValueError("La imagen no cabe en la partición OTA.")
    key = signing_key(args.key)
    signature_path = Path(str(args.image) + ".sig")
    if args.action == "sign":
        signature_path.write_bytes(key.sign(data, ec.ECDSA(hashes.SHA256())))
        print(f"{chip} {version}: {signature_path}")
        return
    signature = signature_path.read_bytes()
    key.public_key().verify(signature, data, ec.ECDSA(hashes.SHA256()))
    install(args.host, data, signature, key)


if __name__ == "__main__":
    try:
        main()
    except InvalidSignature:
        sys.exit("Firma de imagen incorrecta.")
    except (OSError, ValueError, KeyError, RuntimeError, http.client.HTTPException) as exc:
        sys.exit(str(exc))
