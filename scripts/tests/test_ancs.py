import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD_CONFIG = ROOT / "main/boards/lckfb/szpi-esp32s3/config.json"


class AncsBuildConfigurationTests(unittest.TestCase):
    def test_dedicated_variant_does_not_change_standard_firmware(self):
        config = json.loads(BOARD_CONFIG.read_text(encoding="utf-8"))
        builds = {entry["name"]: entry for entry in config["builds"]}
        self.assertEqual(
            builds["lichuang-dev"]["sdkconfig_append"],
            ["CONFIG_USE_DEVICE_AEC=y"],
        )
        ancs = builds["lichuang-dev-ancs"]["sdkconfig_append"]
        self.assertIn("CONFIG_ENABLE_IOS_ANCS_RELAY=y", ancs)
        self.assertIn("CONFIG_BT_NIMBLE_NVS_PERSIST=y", ancs)
        self.assertIn("CONFIG_BT_NIMBLE_MAX_BONDS=1", ancs)
        self.assertIn("CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1", ancs)
        self.assertIn("CONFIG_BT_NIMBLE_SM_LVL=2", ancs)
        self.assertIn("CONFIG_BT_NIMBLE_SPS_SERVICE=y", ancs)
        self.assertIn("CONFIG_BT_NIMBLE_BAS_SERVICE=y", ancs)
        self.assertIn("CONFIG_BT_NIMBLE_HID_SERVICE=y", ancs)
        self.assertIn("CONFIG_BT_NIMBLE_SVC_HID_MAX_INSTANCES=1", ancs)
        self.assertIn("CONFIG_BT_NIMBLE_SVC_HID_MAX_RPTS=1", ancs)
        self.assertIn("CONFIG_BT_NIMBLE_SVC_DIS_PNP_ID=y", ancs)
        self.assertIn("CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING=n", ancs)

    def test_hid_pairing_carrier_keeps_ancs_solicitation(self):
        client = (ROOT / "main/bluetooth/ancs_client.cc").read_text(encoding="utf-8")
        relay = (ROOT / "main/bluetooth/notification_relay.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("BLE_UUID16_INIT(0x1812)", client)
        self.assertIn("kGenericHidAppearance = 0x03c0", client)
        self.assertIn("fields.sol_uuids128 = &kAncsServiceUuid", client)
        self.assertIn("#include <services/hid/ble_svc_hid.h>", client)
        self.assertIn("ble_svc_hid_add(parameters)", client)
        self.assertIn("ble_svc_hid_init()", client)
        self.assertIn("ble_svc_bas_init()", client)
        self.assertIn("ble_svc_sps_init(0, 0)", client)
        self.assertIn("ble_hs_cfg.store_status_cb = ble_store_util_status_rr", client)
        self.assertIn("BLE_GAP_REPEAT_PAIRING_RETRY", client)
        self.assertIn("case BLE_GAP_EVENT_ENC_CHANGE", client)
        self.assertIn("client->HandleEncryptedConnection", client)
        self.assertIn("xTaskCreateWithCaps", relay)
        self.assertIn("MALLOC_CAP_SPIRAM", relay)

    def test_kconfig_restricts_ancs_to_lichuang_s3_without_blufi(self):
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
        block = kconfig.split("config ENABLE_IOS_ANCS_RELAY", 1)[1].split(
            "config AUDIO_DEBUG_UDP_SERVER", 1
        )[0]
        self.assertIn(
            "depends on IDF_TARGET_ESP32S3 && BOARD_TYPE_LICHUANG_DEV_S3",
            block,
        )
        self.assertIn("depends on !USE_ESP_BLUFI_WIFI_PROVISIONING", block)
        self.assertIn("depends on BT_NIMBLE_ENABLED", block)


class AncsNativeTests(unittest.TestCase):
    def test_parser_and_retry_policy(self):
        compiler = next(
            (path for name in ("c++", "g++", "clang++") if (path := shutil.which(name))),
            None,
        )
        if compiler is None:
            self.skipTest("No host C++ compiler is installed")
        with tempfile.TemporaryDirectory() as temp_dir:
            executable = Path(temp_dir) / "ancs_host_test"
            cjson_object = Path(temp_dir) / "cJSON.o"
            cjson_dir = ROOT / "managed_components/espressif__cjson/cJSON"
            c_compiler = shutil.which("gcc") or shutil.which("clang")
            if c_compiler is None:
                self.skipTest("No host C compiler is installed")
            subprocess.run(
                [
                    c_compiler,
                    "-std=c11",
                    "-I",
                    str(cjson_dir),
                    "-c",
                    str(cjson_dir / "cJSON.c"),
                    "-o",
                    str(cjson_object),
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run(
                [
                    compiler,
                    "-std=c++14",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "main/bluetooth"),
                    "-I",
                    str(cjson_dir),
                    str(ROOT / "main/bluetooth/ancs_attribute_parser.cc"),
                    str(ROOT / "main/bluetooth/ancs_webhook.cc"),
                    str(ROOT / "main/bluetooth/tests/ancs_host_test.cc"),
                    str(cjson_object),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
            )
            subprocess.run([str(executable)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()
