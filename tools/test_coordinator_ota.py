"""Offline checks against the actual C5/C6 build artifacts. Run in ESP-IDF."""
import contextlib
import hashlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

import coordinator_ota as ota
import check_ota_migration as migration

sys.path.insert(0, str(Path(os.environ["IDF_PATH"]) / "components/partition_table"))
from gen_esp32part import PartitionTable


class BuildChecks(unittest.TestCase):
    def test_builds_and_migration(self):
        for name in ("esp32c5", "esp32c6"):
            with self.subTest(chip=name), tempfile.TemporaryDirectory() as tmp:
                build = ota.ROOT / ("build-" + name[-2:])
                app = (build / (ota.PRODUCT+".bin")).read_bytes()
                elf = (build / (ota.PRODUCT+".elf")).read_bytes()
                self.assertEqual(ota.image_info(app)[::2], (name, hashlib.sha256(elf).hexdigest()))
                self.assertLessEqual(len(app), ota.SLOT_SIZE)
                self.assertIn("#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1",
                              (build/"bootloader/config/sdkconfig.h").read_text())
                pub = serialization.load_pem_public_key((ota.ROOT/"main/ota_public.pem").read_bytes())
                sig = (build/(ota.PRODUCT+".bin.sig")).read_bytes()
                pub.verify(sig, app, ec.ECDSA(hashes.SHA256()))
                with self.assertRaises(InvalidSignature):
                    pub.verify(sig, app[:-1], ec.ECDSA(hashes.SHA256()))
                csv = "\n".join(f"{label},{t},{s},{off},{size}" for label, (t,s,off,size) in migration.OLD.items())
                old = PartitionTable.from_csv(csv).to_binary()
                flash = bytearray(b"\xff" * 0x400000)
                flash[0x8000:0x8000+len(old)] = old
                flash[0x40000:0x40000+len(app)] = app
                flash[0x2a000] = 0x55
                backup = Path(tmp)/"legacy.bin"
                backup.write_bytes(flash)
                with contextlib.redirect_stdout(io.StringIO()):
                    migration.check(backup, build)
                # A HomeKit C5 uses the same chip. Its product or layout must fail.
                flash[0x40000+80:0x40000+112] = b"homekit_ethernet_bridge".ljust(32,b"\0")
                backup.write_bytes(flash)
                with self.assertRaisesRegex(ValueError, "coordinador"):
                    migration.check(backup, build)
                flash[0x8000:0x8000+3072] = (build/"partition_table/partition-table.bin").read_bytes()
                backup.write_bytes(flash)
                with self.assertRaisesRegex(ValueError, "factory"):
                    migration.check(backup, build)

    def test_upload_protocol_and_confirmation(self):
        build = ota.ROOT / "build-c6"
        app = (build / (ota.PRODUCT+".bin")).read_bytes()
        key = ec.generate_private_key(ec.SECP256R1())
        sig = key.sign(app, ec.ECDSA(hashes.SHA256()))
        digest = hashlib.sha256(app).digest()
        chip, _, elf = ota.image_info(app)
        before = dict(product=ota.PRODUCT, chip=chip, confirmed=True, slot="ota_0", nonce="03"*32)
        after = dict(before, slot="ota_1", elf_sha256=elf)
        network = dict(ext_pan_id="ab", pan_id=3, coordinator_ieee="cd", channel=14)
        checker = self

        class Connection:
            def __init__(self, host, port, timeout):
                checker.assertEqual(port, 8082)
                self.headers = {}
                self.body = bytearray()
            def putrequest(self, method, uri):
                checker.assertEqual((method, uri), ("POST", "/ota"))
            def putheader(self, name, value):
                self.headers[name] = value
            def endheaders(self):
                pass
            def send(self, chunk):
                self.body.extend(chunk)
            def getresponse(self):
                import base64
                h = self.headers
                checker.assertEqual(int(h["Content-Length"]), len(app))
                checker.assertEqual(self.body, app)
                checker.assertEqual(base64.b64decode(h["X-Image-SHA256"]), digest)
                pub = key.public_key()
                pub.verify(base64.b64decode(h["X-Image-Signature"]), app, ec.ECDSA(hashes.SHA256()))
                proof = base64.b64decode(h["X-Upload-Signature"])
                pub.verify(proof, b"esp32_zigbee3/upload/v1"+bytes.fromhex(before["nonce"])+digest,
                           ec.ECDSA(hashes.SHA256()))
                with checker.assertRaises(InvalidSignature):
                    pub.verify(proof, b"esp32_zigbee3/upload/v1"+bytes(32)+digest, ec.ECDSA(hashes.SHA256()))
                return self
            status = 200
            def read(self):
                return b"Rebooting"
            def close(self):
                pass

        for devices in ({"one"}, set()):
            with patch.object(ota, "info", side_effect=(before, after)), \
                 patch.object(ota, "network", side_effect=((network, {"one"}), (network, devices))), \
                 patch.object(ota.http.client, "HTTPConnection", Connection), \
                 patch.object(ota.time, "sleep"), contextlib.redirect_stdout(io.StringIO()):
                if devices:
                    ota.install("gateway", app, sig, key)
                else:
                    with self.assertRaisesRegex(RuntimeError, "Faltan dispositivos"):
                        ota.install("gateway", app, sig, key)


if __name__ == "__main__":
    unittest.main()
