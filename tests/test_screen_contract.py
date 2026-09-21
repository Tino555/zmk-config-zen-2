import os
import re
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ICONS = ROOT / "config/widgets/icons"


class ScreenContractTest(unittest.TestCase):
    def test_minute_counter_tracks_completed_windows_and_resets_after_sleep(self):
        program = r'''
#include <assert.h>
#include <stdint.h>
#include "zen_minute_counter.h"
int main(void) {
    struct zen_minute_counter counter = {0};
    zen_minute_counter_reset(&counter, 87); /* restored settings */
    assert(zen_minute_counter_complete(&counter, 90) == 3);
    assert(zen_minute_counter_complete(&counter, 90) == 0);
    zen_minute_counter_reset(&counter, 91); /* sleep/wake: no stale window */
    assert(zen_minute_counter_complete(&counter, 92) == 1);
    zen_minute_counter_reset(&counter, UINT32_MAX - 1);
    assert(zen_minute_counter_complete(&counter, 1) == 3);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "main.c"
            executable = Path(directory) / "counter"
            source.write_text(program)
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "config"),
                 str(source), "-o", str(executable)], check=True, capture_output=True,
            )
            subprocess.run([str(executable)], check=True, capture_output=True)

    def test_screen_and_build_have_new_components(self):
        screen = (ROOT / "config/custom_status_screen.c").read_text()
        self.assertIn("zmk_widget_battery_status_init", screen)
        self.assertIn('"Keys %lu"', screen)
        self.assertIn('"+%lu"', screen)
        self.assertNotIn("full_refresh_work", screen)
        self.assertNotIn("locks_label", screen)
        self.assertIn("lv_txt_get_width", screen)
        self.assertIn("lv_obj_get_style_text_font", screen)
        self.assertIn("ev->state == ZMK_ACTIVITY_ACTIVE && !atomic_get(&minute_awake)", screen)
        for side in ("left", "right"):
            conf = (ROOT / f"config/corneish_zen_v2_{side}.conf").read_text()
            self.assertIn("CONFIG_LV_FONT_MONTSERRAT_10=y", conf)
            self.assertIn("CONFIG_LV_FONT_MONTSERRAT_8=y", conf)

    def test_battery_icons_render_without_usb(self):
        battery = (ROOT / "config/widgets/battery_status.c").read_text()
        self.assertIn("bool charging = false;", battery)
        self.assertIn("lv_img_set_src(icon, charging ? &batt_100_chg : &batt_100);", battery)
        workflow = (ROOT / ".github/workflows/build.yml").read_text()
        self.assertIn("widgets/battery_status.c", workflow)

    def test_workflow_installs_every_asset_and_behavior(self):
        workflow = (ROOT / ".github/workflows/build.yml").read_text()
        step = workflow.split("      - name: Install custom Corne-ish Zen screen and assets", 1)[1]
        script = step.split("        run: |\n", 1)[1].split("\n      - name:", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "zmk/app"
            board = target / "boards/arm/corneish_zen"
            (board / "widgets/icons").mkdir(parents=True)
            (target / "dts/bindings/behaviors").mkdir(parents=True)
            (board / "CMakeLists.txt").write_text("zephyr_library()\n")
            subprocess.run(["sh", "-e", "-c", textwrap.dedent(script)], cwd=directory,
                           env={**os.environ, "GITHUB_WORKSPACE": str(ROOT)}, check=True,
                           capture_output=True)
            names = [*(f"bluetooth_connected_{i}" for i in range(1, 6)),
                     *(f"bluetooth_advertising_{i}" for i in range(1, 6)),
                     "bluetooth_connected_right", "bluetooth_disconnected_right", "zenlogo"]
            for name in names:
                self.assertEqual((board / "widgets/icons" / f"{name}.c").read_bytes(),
                                 (ICONS / f"{name}.c").read_bytes())
            for name in ("custom_status_screen.c", "custom_status_screen.h",
                         "zen_minute_behavior.c", "zen_minute_counter.h"):
                self.assertEqual((board / name).read_bytes(), (ROOT / "config" / name).read_bytes())
            self.assertEqual((board / "widgets/battery_status.c").read_bytes(),
                             (ROOT / "config/widgets/battery_status.c").read_bytes())
            self.assertEqual((target / "dts/bindings/behaviors/zmk,behavior-zen-minute.yaml").read_bytes(),
                             (ROOT / "config/dts/bindings/behaviors/zmk,behavior-zen-minute.yaml").read_bytes())
            self.assertIn("zephyr_library_sources_ifdef(CONFIG_CUSTOM_WIDGET_PERIPHERAL_STATUS zen_minute_behavior.c)",
                          (board / "CMakeLists.txt").read_text())

    def test_bluetooth_assets_are_small_1bit_images(self):
        names = [
            *(f"bluetooth_connected_{n}" for n in range(1, 6)),
            *(f"bluetooth_advertising_{n}" for n in range(1, 6)),
            "bluetooth_connected_right",
            "bluetooth_disconnected_right",
        ]
        for name in names:
            with self.subTest(name=name):
                source = (ICONS / f"{name}.c").read_text()
                self.assertRegex(source, r"\.header\.w\s*=\s*36,")
                self.assertRegex(source, r"\.header\.h\s*=\s*16,")
                self.assertRegex(source, r"\.header\.cf\s*=\s*LV_IMG_CF_INDEXED_1BIT,")
                self.assertRegex(source, r"\.data_size\s*=\s*88,")
                body = source.split("_map[] = {", 1)[1].split("};", 1)[0]
                self.assertEqual(len(re.findall(r"0x[0-9a-fA-F]{2}\b", body)), 88)

    def test_shift_control_positions_and_sleep_timeout(self):
        keymap = (ROOT / "config/corneish_zen.keymap").read_text()
        default = keymap.split("default_layer {", 1)[1].split("lower_layer {", 1)[0]
        rows = default.split("bindings = <", 1)[1].split(">;", 1)[0].splitlines()
        self.assertEqual(rows[2].split()[0], "&td0")
        self.assertEqual(rows[3].split()[0:2], ["&kp", "LCTRL"])
        conf = (ROOT / "config/corneish_zen.conf").read_text()
        self.assertIn("CONFIG_ZMK_IDLE_SLEEP_TIMEOUT=1800000", conf)


if __name__ == "__main__":
    unittest.main()
