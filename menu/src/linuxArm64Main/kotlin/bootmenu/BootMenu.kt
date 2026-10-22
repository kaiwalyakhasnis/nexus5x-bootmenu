package bootmenu

import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import kotlinx.cinterop.pointed
import kotlinx.cinterop.toKString
import kotlinx.coroutines.delay
import sys.BM_CHECKING
import sys.BM_DONE
import sys.BM_FAILED
import sys.BM_IMAGE
import sys.BM_KEEP
import sys.bm_battery
import sys.bm_boot
import sys.bm_charger
import sys.bm_default
import sys.bm_flash_error
import sys.bm_flash_percent
import sys.bm_flash_start
import sys.bm_flash_state
import sys.bm_get
import sys.bm_log
import sys.bm_scan
import sys.bm_timeout
import ui.Alignment
import ui.Arrangement
import ui.Box
import ui.CircleShape
import ui.Color
import ui.Column
import ui.Device
import ui.Modifier
import ui.Offset
import ui.PointerEventType
import ui.RoundedCornerShape
import ui.Row
import ui.Spacer
import ui.Text
import ui.background
import ui.border
import ui.dp
import ui.fillMaxSize
import ui.fillMaxWidth
import ui.height
import ui.padding
import ui.pointerInput
import ui.size
import ui.weight
import ui.width
import kotlin.math.abs
import kotlin.time.Duration.Companion.seconds
import kotlin.time.TimeSource

internal object Menu {
    val Background = Color(0xff0d1117)
    val Card = Color(0xff171c26)
    val CardSelected = Color(0xff1e2a40)
    val Accent = Color(0xff4c8dff)
    val Text = Color(0xffe8ecf2)
    val Muted = Color(0xff8b95a7)
    val Bad = Color(0xffef6b64)
    val Good = Color(0xff5fd08a)
    val Track = Color(0xff262d3a)
    val Badges = listOf(Color(0xff1fa79a), Color(0xff7cc576), Color(0xffffb74d), Color(0xff4fa3f7), Color(0xff9d7fe0), Color(0xfff06292))
}

/** One line of the menu, from the native side (native/bootmenu_sys.h). */
internal class Entry(val index: Int, val kind: Int, val label: String, val error: String, val size: Long, val current: Boolean) {
    val image get() = kind == BM_IMAGE.toInt() || kind == BM_KEEP.toInt()
    val color get() = if (kind == BM_KEEP.toInt()) Menu.Muted else Menu.Badges[index % Menu.Badges.size]
}

private fun loadEntries(): List<Entry> = (0 until bm_scan()).map { i ->
    val e = bm_get(i)!!.pointed
    Entry(i, e.kind, e.label.toKString(), e.error.toKString(), e.size, e.current != 0)
}

private const val KEY_ENTER = 28
private const val KEY_SPACE = 57
private const val KEY_UP = 103
private const val KEY_DOWN = 108
private const val KEY_VOLUMEDOWN = 114
private const val KEY_VOLUMEUP = 115
private const val KEY_POWER = 116

/**
 * The menu. Volume keys (or arrows) move, Power (or Enter) starts, a tap
 * starts what it hits. The image in the recovery slot is the default and
 * starts by itself after bootmenu.timeout seconds unless a key is pressed.
 *
 * Selection is the app's own state, not the toolkit's key focus: it has to
 * start on the default entry. So rows take taps through pointerInput (which
 * key focus ignores) and keys come through Device.onKey.
 */
@Composable
internal fun BootMenu() {
    var entries by remember { mutableStateOf(loadEntries()) }
    val charger = remember { bm_charger() != 0 }
    val dflt = remember { bm_default() }
    var sel by remember { mutableIntStateOf(if (dflt >= 0) dflt else entries.indexOfFirst { it.error.isEmpty() }.coerceAtLeast(0)) }
    val total = remember { if (!charger && dflt >= 0) bm_timeout() * 10 else 0 }
    var left by remember { mutableIntStateOf(if (total > 0) total else -1) }   // tenths of a second, -1: no countdown
    var switching by remember { mutableStateOf<Entry?>(null) }                 // being written into the slot
    var starting by remember { mutableStateOf<Entry?>(null) }                  // rebooting into it
    var phase by remember { mutableIntStateOf(0) }
    var percent by remember { mutableIntStateOf(0) }
    var note by remember { mutableStateOf("") }
    var battery by remember { mutableIntStateOf(bm_battery()) }
    var lastInput by remember { mutableStateOf(TimeSource.Monotonic.markNow()) }

    fun choose(e: Entry) {
        if (switching != null || starting != null) return
        left = -1
        bm_log("selected ${e.label}")
        when {
            e.error.isNotEmpty() -> note = "${e.label}: ${e.error}"
            e.kind == BM_IMAGE.toInt() && !e.current ->
                if (bm_flash_start(e.index) == 0) switching = e else note = "${e.label}: can't write it"
            else -> starting = e
        }
    }

    DisposableEffect(Unit) {
        Device.onInput = {
            lastInput = TimeSource.Monotonic.markNow()
        }
        Device.onKey = onKey@{ code, value ->
            if (value == 0) return@onKey
            if (!Device.screenOn) {                         // a key on a dark screen only wakes it
                if (value == 1) Device.setScreen(true)
                return@onKey
            }
            if (switching != null || starting != null) return@onKey
            if (value == 1) bm_log("key $code")
            left = -1
            when (code) {
                KEY_VOLUMEUP, KEY_UP -> sel = (sel - 1).mod(entries.size)
                KEY_VOLUMEDOWN, KEY_DOWN -> sel = (sel + 1).mod(entries.size)
                KEY_POWER, KEY_ENTER, KEY_SPACE -> if (value == 1) {
                    note = ""
                    choose(entries[sel])
                }
            }
        }
        onDispose {
            Device.onKey = null
            Device.onInput = null
        }
    }

    LaunchedEffect(Unit) {
        bm_log("menu ready: ${entries.size} entries, default $dflt, timeout ${total / 10} s${if (charger) ", charger" else ""}")
        while (true) {                                      // battery, and the screen off when idle
            delay(1000)
            battery = bm_battery()
            if (Device.screenOn && left < 0 && switching == null && starting == null && lastInput.elapsedNow() > 60.seconds)
                Device.setScreen(false)
        }
    }

    LaunchedEffect(left >= 0) {
        while (left > 0) {
            delay(100)
            if (left > 0) left--
        }
        if (left == 0) {
            bm_log("timeout: ${entries[dflt].label}")
            choose(entries[dflt])
        }
    }

    LaunchedEffect(switching) {
        val e = switching ?: return@LaunchedEffect
        while (true) {
            phase = bm_flash_state()
            percent = bm_flash_percent()
            if (phase == BM_DONE.toInt()) {
                switching = null
                starting = e
                return@LaunchedEffect
            }
            if (phase == BM_FAILED.toInt()) {
                note = "Failed: ${bm_flash_error()?.toKString()}"
                entries = loadEntries()                     // the slot may hold part of an image now
                sel = sel.coerceAtMost(entries.size - 1)
                switching = null
                return@LaunchedEffect
            }
            delay(50)
        }
    }

    LaunchedEffect(starting) {
        val e = starting ?: return@LaunchedEffect
        delay(200)                                          // let the "Starting" screen reach the panel
        bm_boot(e.index)
        note = "Couldn't reboot"
        starting = null
    }

    val busy = switching ?: starting
    if (busy != null) {
        SwitchScreen(busy, writing = switching != null, phase, percent)
        return
    }
    Column(Modifier.fillMaxSize().padding(horizontal = 18.dp, vertical = 28.dp)) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text("Boot menu", color = Menu.Text, scale = 2, bold = true)
            Spacer(Modifier.weight(1f))
            if (battery >= 0) Text("$battery %", color = Menu.Muted)
        }
        Spacer(Modifier.height(6.dp))
        Text("Nexus 5X - choose what to start", color = Menu.Muted)
        Spacer(Modifier.height(22.dp))
        // There is no scrolling: more than five rows get shorter.
        val compact = entries.count { it.image } > 5
        Column(verticalArrangement = Arrangement.SpacedBy(if (compact) 6.dp else 10.dp)) {
            for (e in entries) if (e.image) EntryRow(e, e.index == sel, compact) {
                sel = e.index
                choose(e)
            }
        }
        Spacer(Modifier.weight(1f))
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpacedBy(10.dp)) {
            for (e in entries) if (!e.image) SystemButton(e, e.index == sel, Modifier.weight(1f)) {
                sel = e.index
                choose(e)
            }
        }
        Spacer(Modifier.height(20.dp))
        Footer(
            when {
                note.isNotEmpty() -> note
                left >= 0 -> "Starting ${entries[dflt].label} in ${(left + 9) / 10} s"
                charger -> "Charging" + if (battery >= 0) ": $battery %" else ""
                else -> "Volume: move    Power: start    or tap"
            },
            color = if (note.isNotEmpty()) Menu.Bad else if (left >= 0) Menu.Text else Menu.Muted,
            fraction = if (left >= 0 && total > 0) left.toFloat() / total else null,
        )
    }
}

/** Remembers where a press started, across recompositions (pointerInput's element is re-created by them). */
private class Press(var at: Offset? = null)

/** A tap: press and release without moving away. */
private fun Modifier.tap(press: Press, onTap: () -> Unit) = then(pointerInput { ev ->
    when (ev.type) {
        PointerEventType.Press -> press.at = ev.position
        PointerEventType.Release -> {
            val p = press.at
            press.at = null
            if (p != null && abs(ev.position.x - p.x) < 40 && abs(ev.position.y - p.y) < 40) onTap()
        }
        PointerEventType.Move -> {}
    }
})

private fun Modifier.selectedBorder(selected: Boolean, radius: Int) =
    if (selected) then(Modifier.border(2.dp, Menu.Accent, RoundedCornerShape(radius.dp))) else this

@Composable
private fun Badge(e: Entry, size: Int, scale: Int) {
    Box(Modifier.size(size.dp).background(e.color, CircleShape), contentAlignment = Alignment.Center) {
        Text(e.label.take(1).uppercase(), color = Menu.Background, scale = scale, bold = true)
    }
}

@Composable
private fun EntryRow(e: Entry, selected: Boolean, compact: Boolean, onTap: () -> Unit) {
    val press = remember { Press() }
    val ok = e.error.isEmpty()
    Row(
        Modifier.fillMaxWidth().height(if (compact) 56.dp else 68.dp)
            .background(if (selected) Menu.CardSelected else Menu.Card, RoundedCornerShape(16.dp))
            .selectedBorder(selected, 16)
            .tap(press, onTap)
            .padding(horizontal = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Badge(e, 42, 2)
        Spacer(Modifier.width(14.dp))
        Column(Modifier.weight(1f)) {
            Text(e.label, color = if (ok) Menu.Text else Menu.Muted, scale = 2)
            Spacer(Modifier.height(if (compact) 0.dp else 4.dp))
            Text(
                when {
                    !ok -> e.error
                    e.kind == BM_KEEP.toInt() -> "whatever is in recovery now"
                    else -> "${(e.size + (1 shl 19)) shr 20} MB"
                },
                color = if (ok) Menu.Muted else Menu.Bad,
            )
        }
        if (e.current) {
            Box(Modifier.background(Menu.Good, RoundedCornerShape(10.dp)).padding(horizontal = 8.dp, vertical = 4.dp)) {
                Text("CURRENT", color = Menu.Background, bold = true)
            }
        }
    }
}

@Composable
private fun SystemButton(e: Entry, selected: Boolean, modifier: Modifier, onTap: () -> Unit) {
    val press = remember { Press() }
    Box(
        modifier.height(54.dp)
            .background(if (selected) Menu.CardSelected else Menu.Card, RoundedCornerShape(14.dp))
            .selectedBorder(selected, 14)
            .tap(press, onTap),
        contentAlignment = Alignment.Center,
    ) {
        Text(e.label, color = Menu.Text)
    }
}

@Composable
private fun Bar(fraction: Float, modifier: Modifier) {
    Box(modifier.height(8.dp).background(Menu.Track, RoundedCornerShape(4.dp))) {
        if (fraction > 0f) Box(Modifier.fillMaxWidth(fraction.coerceAtMost(1f)).height(8.dp).background(Menu.Accent, RoundedCornerShape(4.dp)))
    }
}

@Composable
private fun Footer(text: String, color: Color, fraction: Float?) {
    Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
        Text(text, color = color)
        if (fraction != null) {
            Spacer(Modifier.height(12.dp))
            Bar(fraction, Modifier.fillMaxWidth())
        }
    }
}

/** Writing the image into the recovery slot, then rebooting into it. */
@Composable
private fun SwitchScreen(e: Entry, writing: Boolean, phase: Int, percent: Int) {
    Column(
        Modifier.fillMaxSize().padding(horizontal = 40.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Badge(e, 96, 4)
        Spacer(Modifier.height(24.dp))
        Text(e.label, color = Menu.Text, scale = 2, bold = true)
        Spacer(Modifier.height(12.dp))
        // Writing is the first half of the bar, reading it back the second.
        val checking = phase == BM_CHECKING.toInt()
        val done = if (!writing) 1f else if (checking) 0.5f + percent / 200f else percent / 200f
        Text(
            when {
                !writing -> "Starting"
                checking -> "Checking the recovery slot  ${(done * 100).toInt()} %"
                else -> "Writing the recovery slot  ${(done * 100).toInt()} %"
            },
            color = Menu.Muted,
        )
        Spacer(Modifier.height(24.dp))
        Bar(done, Modifier.fillMaxWidth())
    }
}
