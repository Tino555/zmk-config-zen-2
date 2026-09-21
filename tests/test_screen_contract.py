import hashlib
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
    def test_screen_layout_distributes_edge_and_internal_gaps(self):
        program = r'''
#include <assert.h>
#include <stdint.h>
#include "zen_screen_layout.h"

static void check_gaps(const int32_t *heights, const int32_t *tops, size_t count) {
    int32_t smallest = 128;
    int32_t largest = 0;
    int32_t end = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t gap = tops[i] - end;
        assert(gap >= 0);
        if (gap < smallest) smallest = gap;
        if (gap > largest) largest = gap;
        end = tops[i] + heights[i];
    }
    int32_t bottom = 128 - end;
    if (bottom < smallest) smallest = bottom;
    if (bottom > largest) largest = bottom;
    assert(largest - smallest <= 1);
}

int main(void) {
    const int32_t right_heights[] = {31, 16, 14, 38};
    int32_t right_tops[4];
    zen_screen_layout(128, right_heights, 4, right_tops);
    assert(right_tops[0] == 5 && right_tops[1] == 42);
    assert(right_tops[2] == 64 && right_tops[3] == 84);
    check_gaps(right_heights, right_tops, 4);

    const int32_t left_heights[] = {31, 16, 14, 12, 16};
    int32_t left_tops[5];
    zen_screen_layout(128, left_heights, 5, left_tops);
    assert(left_tops[0] == 6 && left_tops[1] == 44);
    assert(left_tops[2] == 66 && left_tops[3] == 87 && left_tops[4] == 105);
    check_gaps(left_heights, left_tops, 5);

    const int32_t usb_heights[] = {31, 31, 14, 38};
    int32_t usb_tops[4];
    zen_screen_layout(128, usb_heights, 4, usb_tops);
    check_gaps(usb_heights, usb_tops, 4);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "main.c"
            executable = Path(directory) / "layout"
            source.write_text(program)
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "config"),
                 str(source), "-o", str(executable)], check=True, capture_output=True,
            )
            subprocess.run([str(executable)], check=True, capture_output=True)

    def test_screen_reflows_when_widget_size_changes(self):
        screen = (ROOT / "config/custom_status_screen.c").read_text()
        self.assertIn("LV_EVENT_SIZE_CHANGED", screen)
        self.assertIn("lv_obj_update_layout", screen)
        self.assertIn("lv_obj_get_content_height(status_layout_screen)", screen)
        self.assertIn("lv_obj_get_content_width(status_layout_screen)", screen)
        self.assertIn("lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN)", screen)
        self.assertIn("zen_screen_layout", screen)
        self.assertNotIn("LV_ALIGN_TOP_MID, 0, 58", screen)

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
                         "zen_minute_behavior.c", "zen_minute_counter.h", "zen_screen_layout.h"):
                self.assertEqual((board / name).read_bytes(), (ROOT / "config" / name).read_bytes())
            self.assertEqual((board / "widgets/battery_status.c").read_bytes(),
                             (ROOT / "config/widgets/battery_status.c").read_bytes())
            self.assertFalse((target / "dts/bindings/behaviors/zmk,behavior-zen-minute.yaml").exists())
            self.assertTrue((ROOT / "config/dts/bindings/behaviors/zmk,behavior-zen-minute.yaml").exists())
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

    def test_signature_crops_blank_rows_without_resampling(self):
        source = (ICONS / "zenlogo.c").read_text()
        self.assertRegex(source, r"\.header\.w\s*=\s*80,")
        self.assertRegex(source, r"\.header\.h\s*=\s*26,")
        self.assertRegex(source, r"\.data_size\s*=\s*268,")
        body = source.split("zenlogo_map[] = {", 1)[1].split("};", 1)[0]
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        data = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", body))
        self.assertEqual(len(data), 268)
        self.assertTrue(any(data[8:18]))
        self.assertTrue(any(data[-10:]))
        self.assertEqual(hashlib.sha256(data[8:]).hexdigest(),
                         "aaeffbee7fcbe8befe4e80e6c3aa3e4af82362783e4d7eacc177189eb60cb6d9")

    def test_build_patches_lvgl_invalidation_for_untransformed_widgets(self):
        workflow = (ROOT / ".github/workflows/build.yml").read_text()
        self.assertIn("8a6a2d1d29d17d1e4bdc94c243c146a39d635fdd", workflow)
        self.assertIn("apply --reverse --check", workflow)
        self.assertIn("git -C modules/lib/gui/lvgl apply --check", workflow)
        self.assertIn("git -C modules/lib/gui/lvgl apply", workflow)
        patch = (ROOT / "patches/lvgl-avoid-static-invalidation-padding.patch").read_text()
        self.assertIn("src/core/lv_obj_pos.c", patch)
        self.assertIn("LV_LAYER_TYPE_TRANSFORM", patch)

        # The upstream function's tail is enough to exercise the source-level patch.
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "src/core/lv_obj_pos.c"
            target.parent.mkdir(parents=True)
            target.write_text("""void lv_obj_get_transformed_area(const lv_obj_t * obj, lv_area_t * area, bool recursive,
                                 bool inv)
{
    lv_point_t p[4] = {
        {area->x1, area->y1},
        {area->x1, area->y2},
        {area->x2, area->y1},
        {area->x2, area->y2},
    };

    lv_obj_transform_point(obj, &p[0], recursive, inv);
    lv_obj_transform_point(obj, &p[1], recursive, inv);
    lv_obj_transform_point(obj, &p[2], recursive, inv);
    lv_obj_transform_point(obj, &p[3], recursive, inv);

    area->x1 = LV_MIN4(p[0].x, p[1].x, p[2].x, p[3].x);
    area->x2 = LV_MAX4(p[0].x, p[1].x, p[2].x, p[3].x);
    area->y1 = LV_MIN4(p[0].y, p[1].y, p[2].y, p[3].y);
    area->y2 = LV_MAX4(p[0].y, p[1].y, p[2].y, p[3].y);
    lv_area_increase(area, 5, 5);
}

""")
            subprocess.run(["git", "apply", "--check", str(ROOT / "patches/lvgl-avoid-static-invalidation-padding.patch")],
                           cwd=directory, check=True, capture_output=True)
            subprocess.run(["git", "apply", str(ROOT / "patches/lvgl-avoid-static-invalidation-padding.patch")],
                           cwd=directory, check=True, capture_output=True)
            subprocess.run(["git", "apply", "--reverse", "--check",
                            str(ROOT / "patches/lvgl-avoid-static-invalidation-padding.patch")],
                           cwd=directory, check=True, capture_output=True)
            patched = target.read_text()
            self.assertIn("if(_lv_obj_get_layer_type(current) == LV_LAYER_TYPE_TRANSFORM)", patched)
            self.assertEqual(patched.count("lv_area_increase(area, 5, 5)"), 1)

            program = Path(directory) / "main.c"
            program.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
typedef struct lv_obj_t { int layer; struct lv_obj_t *parent; } lv_obj_t;
typedef struct { int x; int y; } lv_point_t;
typedef struct { int x1; int y1; int x2; int y2; } lv_area_t;
#define LV_LAYER_TYPE_TRANSFORM 1
static int min4(int a, int b, int c, int d) {
    int result = a < b ? a : b;
    result = result < c ? result : c;
    return result < d ? result : d;
}
static int max4(int a, int b, int c, int d) {
    int result = a > b ? a : b;
    result = result > c ? result : c;
    return result > d ? result : d;
}
#define LV_MIN4(a, b, c, d) min4(a, b, c, d)
#define LV_MAX4(a, b, c, d) max4(a, b, c, d)
static void lv_area_increase(lv_area_t *area, int x, int y) {
    area->x1 -= x; area->x2 += x; area->y1 -= y; area->y2 += y;
}
static int _lv_obj_get_layer_type(const lv_obj_t *obj) { return obj->layer; }
static const lv_obj_t *lv_obj_get_parent(const lv_obj_t *obj) { return obj->parent; }
static void lv_obj_transform_point(const lv_obj_t *obj, lv_point_t *p, bool recursive, bool inv) {
    (void)obj; (void)p; (void)recursive; (void)inv;
}
''' + patched + r'''
int main(void) {
    lv_obj_t parent = {LV_LAYER_TYPE_TRANSFORM, NULL};
    lv_obj_t child = {0, &parent};
    lv_area_t area = {10, 20, 29, 34};
    lv_obj_get_transformed_area(&child, &area, false, false);
    assert(area.x1 == 10 && area.y1 == 20 && area.x2 == 29 && area.y2 == 34);
    area = (lv_area_t){10, 20, 29, 34};
    lv_obj_get_transformed_area(&child, &area, true, false);
    assert(area.x1 == 5 && area.y1 == 15 && area.x2 == 34 && area.y2 == 39);
    child.layer = LV_LAYER_TYPE_TRANSFORM;
    area = (lv_area_t){10, 20, 29, 34};
    lv_obj_get_transformed_area(&child, &area, false, false);
    assert(area.x1 == 5 && area.y1 == 15 && area.x2 == 34 && area.y2 == 39);
    return 0;
}
''')
            executable = Path(directory) / "invalidation"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(program),
                            "-o", str(executable)], check=True, capture_output=True)
            subprocess.run([str(executable)], check=True, capture_output=True)

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
