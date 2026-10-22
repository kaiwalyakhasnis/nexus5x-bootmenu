@file:OptIn(ExperimentalNativeApi::class)

import bootmenu.BootMenu
import bootmenu.Menu
import sys.bm_init
import ui.runApp
import kotlin.experimental.ExperimentalNativeApi

/** The app's entry point, called from the backend's main() (embedded-compose core/hal/ec_hal.h). */
@CName("ec_app_main")
fun kotlinMain(): Int {
    bm_init()
    return runApp(background = Menu.Background) { BootMenu() }
}
