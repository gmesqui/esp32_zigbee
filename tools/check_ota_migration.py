#!/usr/bin/env python3
"""Read-only preflight for migration from the legacy factory layout."""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

from coordinator_ota import image_info, SLOT_SIZE

DATA = {
    "nvs": (1, 2, 0x9000, 0x20000),
    "phy_init": (1, 1, 0x29000, 0x1000),
    "zb_storage": (1, 0x81, 0x2a000, 0x10000),
    "zb_fct": (1, 0x81, 0x3a000, 0x1000),
}
OLD = {**DATA, "factory": (0, 0, 0x40000, 0x3c0000)}
NEW = {**DATA, "otadata": (1, 0, 0x3c000, 0x2000),
       "ota_0": (0, 0x10, 0x40000, SLOT_SIZE), "ota_1": (0, 0x11, 0x220000, SLOT_SIZE)}


def layout(raw):
    from gen_esp32part import PartitionTable
    parts = PartitionTable.from_binary(raw)
    actual = {p.name: (p.type, p.subtype, p.offset, p.size) for p in parts}
    if len(parts) != len(actual) or any(p.encrypted or p.readonly for p in parts):
        raise ValueError("Tabla inesperada o particiones protegidas.")
    return actual


def check(backup, build):
    data = backup.read_bytes()
    if len(data) != 0x400000 or layout(data[0x8000:0x9000]) != OLD:
        raise ValueError("Se requiere una copia completa de la tabla factory original de 4 MiB.")
    chip, _, _ = image_info(data[0x40000:])
    cfg = json.loads((build / "flasher_args.json").read_text())
    if cfg["extra_esptool_args"]["chip"] != chip or cfg["flash_settings"]["flash_size"] != "4MB":
        raise ValueError("Destino de compilación incompatible.")
    if layout((build / cfg["partition-table"]["file"]).read_bytes()) != NEW:
        raise ValueError("La tabla OTA no conserva las particiones de datos.")
    boot = 0x2000 if chip == "esp32c5" else 0
    ranges = {boot: 0x8000, 0x8000: 0x9000, 0x3c000: 0x3e000, 0x40000: 0x40000 + SLOT_SIZE}
    images = {int(k, 0): (build / v).read_bytes() for k, v in cfg["flash_files"].items()}
    if images.keys() != ranges.keys():
        raise ValueError("Conjunto de escrituras inesperado.")
    for start, payload in images.items():
        erase_end = start + ((len(payload)+4095)//4096)*4096
        if not payload or erase_end > ranges[start]:
            raise ValueError(f"Escritura fuera del rango previsto en {start:#x}.")
    if images[0x3c000] != b"\xff"*8192 or layout(images[0x8000]) != NEW or image_info(images[0x40000])[0] != chip:
        raise ValueError("Los archivos de carga no coinciden con la migración.")
    if data[0x2a000:0x3a000] == b"\xff"*0x10000:
        raise ValueError("La copia no contiene almacenamiento Zigbee inicializado.")
    print(f"Coordinador {chip}. SHA-256 de la copia: {hashlib.sha256(data).hexdigest()}")
    for name, (_, _, offset, size) in DATA.items():
        print(f"{name}: {hashlib.sha256(data[offset:offset+size]).hexdigest()}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("backup", type=Path)
    parser.add_argument("build", type=Path)
    args = parser.parse_args()
    if not os.environ.get("IDF_PATH"):
        sys.exit("Activa primero el entorno ESP-IDF.")
    sys.path.insert(0, str(Path(os.environ["IDF_PATH"]) / "components/partition_table"))
    try:
        check(args.backup, args.build)
    except (OSError, ValueError, RuntimeError, KeyError) as exc:
        sys.exit(str(exc))
