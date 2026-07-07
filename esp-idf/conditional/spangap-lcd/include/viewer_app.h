/**
 * viewer_app.h — the document viewer's LCD launcher program as a boot-registered
 * Service.
 *
 * ViewerApp is an LcdApp (hence a Service): the straddle's `services:` entry
 * points the generated boot code at this header, which constructs a ViewerApp
 * and registers it. LcdApp::onInit installs its launcher tile; appInit() does
 * the boot-task wiring (nav worker, `lcdview` CLI verb, Settings pane, once_lcd
 * auto-open).
 *
 * The class is declared here (global, no namespace — the codebase disambiguates
 * by the viewer* symbol prefix) so the trampoline TU can `new ViewerApp()`; the
 * methods are defined out-of-line in viewer_lcd.cpp, where the file-static
 * viewer state (page, address bar, nav worker) lives. Compiled only under
 * conditional/spangap-lcd/, so it exists only when the lcd straddle is staged —
 * matching the entry's `when: spangap/spangap-lcd` gate.
 */
#pragma once

#include "lcd_app.h"   /* LcdApp (a Service) */
#include "lvgl.h"      /* lv_obj_t */

/** The document viewer. onCreate builds the page + address bar once; onShow runs
 *  the manual-launch home navigation; onClose nulls the widget handles so the
 *  next open rebuilds. appInit() (boot task) spawns the nav worker, registers
 *  the `lcdview` CLI verb + Settings pane, and fires any once_lcd auto-open. */
class ViewerApp : public LcdApp {
public:
    ViewerApp();
    void onCreate(lv_obj_t* root) override;
    void onShow() override;
    void onClose() override;

protected:
    void appInit() override;
};
