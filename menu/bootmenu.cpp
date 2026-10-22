// bootmenu: PID 1 of the boot menu image, which lives in the boot partition.
//
// Lists the boot images in /cache/bootmenu/, writes the chosen one into the
// recovery partition and reboots with the reason "recovery", so aboot (LK)
// boots it. The next normal power-on starts this menu again.
//
// What it writes: only the partition whose GPT name is exactly "recovery",
// and only a file that is a boot image and fits. /cache is mounted read-only
// without journal replay; nothing else is opened for writing.
//
// The bullhead kernel has no devtmpfs, so /dev is a tmpfs filled from sysfs.

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/input.h>
#include <linux/reboot.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "minui/minui.h"

static const char* kImageDir = "/cache/bootmenu";
static const char* kSlotName = "recovery";              // the only partition ever written
static const char* kCacheName = "cache";
static const uint64_t kSlotMin = 8ull << 20;            // bullhead's recovery is 32 MiB;
static const uint64_t kSlotMax = 64ull << 20;           // anything else is not that partition
static const size_t kHdrSize = 1648;                    // boot image header v0 (bootimg.h)
static const size_t kChunk = 1 << 20;
static const int kIdleBlankMs = 60 * 1000;

// ---------------------------------------------------------------- logging

static int g_kmsg = -1;

// To the kernel log: the serial console in QEMU, dmesg on the phone.
static void klog(const char* fmt, ...) {
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "<6>bootmenu: ");
  va_list ap;
  va_start(ap, fmt);
  n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);
  if (n >= int(sizeof(buf)) - 1) n = sizeof(buf) - 2;
  buf[n++] = '\n';
  if (g_kmsg >= 0 && write(g_kmsg, buf, n) < 0) {}   // nowhere left to report it
}

static int64_t now_ms() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

static std::string read_file(const std::string& path) {
  std::string s;
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return s;
  char buf[4096];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) s.append(buf, n);
  close(fd);
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s;
}

static std::vector<std::string> list_dir(const char* path) {
  std::vector<std::string> v;
  DIR* d = opendir(path);
  if (!d) return v;
  while (dirent* de = readdir(d))
    if (de->d_name[0] != '.') v.push_back(de->d_name);
  closedir(d);
  std::sort(v.begin(), v.end());
  return v;
}

// "KEY=value" from a uevent file or the kernel command line.
static std::string kv_value(const std::string& text, const char* key, char sep) {
  std::string k = std::string(key) + "=";
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find(sep, pos);
    if (end == std::string::npos) end = text.size();
    if (text.compare(pos, k.size(), k) == 0) return text.substr(pos + k.size(), end - pos - k.size());
    pos = end + 1;
  }
  return "";
}

// ---------------------------------------------------------------- /dev

static void mount_api() {
  mkdir("/proc", 0755);
  mkdir("/sys", 0755);
  mkdir("/dev", 0755);
  mount("proc", "/proc", "proc", 0, nullptr);
  mount("sysfs", "/sys", "sysfs", 0, nullptr);
  mount("tmpfs", "/dev", "tmpfs", MS_NOSUID, "mode=0755");
  mknod("/dev/kmsg", S_IFCHR | 0600, makedev(1, 11));
  mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
  mkdir("/dev/input", 0755);
  mkdir("/dev/block", 0755);
  mkdir("/cache", 0755);
}

static void mknod_from_sysfs(const std::string& sysdir, const std::string& node, mode_t type) {
  unsigned ma, mi;
  if (sscanf(read_file(sysdir + "/dev").c_str(), "%u:%u", &ma, &mi) != 2) return;
  struct stat st;
  if (stat(node.c_str(), &st) == 0) return;
  mknod(node.c_str(), type | 0600, makedev(ma, mi));
}

// What ueventd would do, for the three kinds of device the menu needs.
static void make_nodes() {
  for (auto& n : list_dir("/sys/class/graphics"))
    if (n.compare(0, 2, "fb") == 0) mknod_from_sysfs("/sys/class/graphics/" + n, "/dev/" + n, S_IFCHR);
  for (auto& n : list_dir("/sys/class/input"))
    if (n.compare(0, 5, "event") == 0) mknod_from_sysfs("/sys/class/input/" + n, "/dev/input/" + n, S_IFCHR);
  for (auto& n : list_dir("/sys/class/block"))
    mknod_from_sysfs("/sys/class/block/" + n, "/dev/block/" + n, S_IFBLK);
}

// ---------------------------------------------------------------- partitions

struct Part {
  std::string dev;
  uint64_t size = 0;
};

// The one partition (not a whole disk) whose GPT name is exactly `name`.
// Zero or several matches both count as not found.
static bool find_part(const char* name, Part* out) {
  int found = 0;
  for (auto& n : list_dir("/sys/class/block")) {
    std::string s = "/sys/class/block/" + n;
    if (access((s + "/partition").c_str(), F_OK) != 0) continue;
    if (kv_value(read_file(s + "/uevent"), "PARTNAME", '\n') != name) continue;
    out->dev = "/dev/block/" + n;
    found++;
  }
  if (found != 1) return false;
  int fd = open(out->dev.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  bool ok = ioctl(fd, BLKGETSIZE64, &out->size) == 0;
  close(fd);
  return ok;
}

// ---------------------------------------------------------------- images

enum Kind { IMAGE, KEEP, BOOTLOADER, POWEROFF };

struct Entry {
  Kind kind;
  std::string label;
  std::string path;
  std::string error;      // why it can't be booted; empty if it can
  uint64_t size = 0;
  bool current = false;   // already in the recovery slot
  unsigned char hdr[kHdrSize];
};

static uint32_t le32(const unsigned char* p) {
  return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
}

static bool read_full(int fd, void* buf, size_t len, off_t off) {
  unsigned char* p = static_cast<unsigned char*>(buf);
  while (len) {
    ssize_t n = pread(fd, p, len, off);
    if (n <= 0) return false;
    p += n; off += n; len -= n;
  }
  return true;
}

// "2-Hike_Buddy.img" -> "Hike Buddy": the number only orders the list.
static std::string label_of(std::string name) {
  name.erase(name.size() - 4);
  size_t i = 0;
  while (i < name.size() && isdigit((unsigned char)name[i])) i++;
  if (i > 0 && i < name.size() && (name[i] == '-' || name[i] == '_')) name.erase(0, i + 1);
  std::replace(name.begin(), name.end(), '_', ' ');
  return name;
}

// A header v0 boot image whose sections are all in the file, and that fits the slot.
static void check_image(Entry* e, uint64_t slot_size) {
  int fd = open(e->path.c_str(), O_RDONLY | O_CLOEXEC);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0) {
    e->error = "can't read";
    if (fd >= 0) close(fd);
    return;
  }
  e->size = st.st_size;
  if (!read_full(fd, e->hdr, kHdrSize, 0) || memcmp(e->hdr, "ANDROID!", 8) != 0) {
    e->error = "not a boot image";
  } else {
    uint64_t page = le32(e->hdr + 36);
    auto pages = [page](uint64_t n) { return (n + page - 1) / page * page; };
    uint64_t need = page + pages(le32(e->hdr + 8)) + pages(le32(e->hdr + 16)) + pages(le32(e->hdr + 24));
    if (page < 2048 || page > 16384 || (page & (page - 1)) || le32(e->hdr + 8) == 0)
      e->error = "bad header";
    else if (need > e->size)
      e->error = "truncated";
    else if (e->size > slot_size)
      e->error = "too big for recovery";
  }
  close(fd);
}

// Does the slot start with exactly this file? Reads both in chunks.
static bool same_content(const std::string& path, const Part& slot, uint64_t size,
                         void (*progress)(uint64_t done, uint64_t total) = nullptr) {
  int a = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  int b = open(slot.dev.c_str(), O_RDONLY | O_CLOEXEC);
  bool same = a >= 0 && b >= 0;
  std::vector<unsigned char> x(kChunk), y(kChunk);
  for (uint64_t off = 0; same && off < size; off += kChunk) {
    size_t n = std::min<uint64_t>(kChunk, size - off);
    same = read_full(a, x.data(), n, off) && read_full(b, y.data(), n, off) && memcmp(x.data(), y.data(), n) == 0;
    if (progress) progress(off + n, size);
  }
  if (a >= 0) close(a);
  if (b >= 0) close(b);
  return same;
}

static std::vector<Entry> scan(const Part* slot) {
  std::vector<Entry> v;
  unsigned char slot_hdr[kHdrSize];
  bool have_slot_hdr = false;
  if (slot) {
    int fd = open(slot->dev.c_str(), O_RDONLY | O_CLOEXEC);
    have_slot_hdr = fd >= 0 && read_full(fd, slot_hdr, kHdrSize, 0);
    if (fd >= 0) close(fd);
  }
  bool any_current = false;
  for (auto& n : list_dir(kImageDir)) {
    if (n.size() < 5 || n.compare(n.size() - 4, 4, ".img") != 0) continue;
    Entry e;
    e.kind = IMAGE;
    e.label = label_of(n);
    e.path = std::string(kImageDir) + "/" + n;
    if (!slot) e.error = "no recovery partition";
    else check_image(&e, slot->size);
    // The header (with its SHA-1 id) picks the candidate, the full compare confirms it.
    if (e.error.empty() && have_slot_hdr && !any_current && memcmp(e.hdr, slot_hdr, kHdrSize) == 0)
      e.current = any_current = same_content(e.path, *slot, e.size);
    klog("image %s: %s%s, %llu bytes", n.c_str(), e.error.empty() ? "ok" : e.error.c_str(),
         e.current ? ", in the recovery slot" : "", (unsigned long long)e.size);
    v.push_back(e);
  }
  if (slot && !any_current) {
    Entry e;
    e.kind = KEEP;
    e.label = "Recovery slot as is";
    if (!have_slot_hdr || memcmp(slot_hdr, "ANDROID!", 8) != 0) e.error = "no boot image there";
    v.push_back(e);
  }
  Entry b;
  b.kind = BOOTLOADER;
  b.label = "Bootloader (fastboot)";
  v.push_back(b);
  Entry p;
  p.kind = POWEROFF;
  p.label = "Power off";
  v.push_back(p);
  return v;
}

// ---------------------------------------------------------------- reboot

static void reboot_to(const char* reason) {
  klog("reboot %s", reason ? reason : "(normal)");
  sync();
  umount2("/cache", MNT_DETACH);
  if (reason)
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, reason);
  else
    reboot(RB_AUTOBOOT);
  klog("reboot failed: %s", strerror(errno));
  for (;;) pause();    // PID 1 must not exit
}

static void power_off() {
  klog("power off");
  sync();
  umount2("/cache", MNT_DETACH);
  reboot(RB_POWER_OFF);
  for (;;) pause();
}

// ---------------------------------------------------------------- drawing

static int g_scale = 3;             // 10x18 font scaled 3x on the 1080 px panel
static int g_cw = 10, g_ch = 18;

static void color(uint32_t rgb) { gr_color(rgb >> 16, (rgb >> 8) & 0xff, rgb & 0xff, 255); }

static void text(int x, int y, const std::string& s, int scale) {
  gr_text_scaled(gr_sys_font(), x, y, s.c_str(), scale);
}

static int text_w(const std::string& s, int scale) { return int(s.size()) * g_cw * scale; }

static void text_center(int y, const std::string& s, int scale) {
  text((gr_fb_width() - text_w(s, scale)) / 2, y, s, scale);
}

static const uint32_t kBg = 0x101418, kFg = 0xf0f0f0, kDim = 0x8a949e, kAccent = 0x2d6cdf,
                      kBad = 0xe0605a, kGood = 0x5ec27a;

static void draw_menu(const std::vector<Entry>& v, int sel, const std::string& status) {
  const int W = gr_fb_width(), H = gr_fb_height(), s = g_scale;
  const int margin = g_cw * s * 2, line = g_ch * s, row = line * 2;
  color(kBg);
  gr_clear();

  int y = H / 10;
  color(kFg);
  text_center(y, "BOOT MENU", s + 1);
  y += g_ch * (s + 1) + line / 2;
  color(kDim);
  text_center(y, "Volume: move    Power: boot", s - 1);
  y += line * 2;

  for (size_t i = 0; i < v.size(); i++) {
    const Entry& e = v[i];
    if (e.kind == BOOTLOADER) y += line / 2;    // gap before the system entries
    if (int(i) == sel) {
      color(kAccent);
      gr_fill(margin / 2, y, W - margin / 2, y + row);
    }
    const int ty = y + (row - line) / 2;
    color(e.error.empty() ? kFg : kDim);
    text(margin, ty, e.label, s);
    std::string tag;
    uint32_t tag_color = kDim;
    if (!e.error.empty()) tag = e.error, tag_color = kBad;
    else if (e.current) tag = "current", tag_color = kGood;
    else if (e.kind == IMAGE) tag = std::to_string((e.size + (1 << 19)) >> 20) + " MB";
    if (!tag.empty()) {
      color(int(i) == sel && e.error.empty() ? kFg : tag_color);
      text(W - margin - text_w(tag, s - 1), ty + (line - g_ch * (s - 1)) / 2, tag, s - 1);
    }
    y += row;
  }

  color(kDim);
  if (!status.empty()) text_center(H - H / 8, status, s - 1);
  gr_flip();
}

static void draw_message(const std::string& title, const std::string& line1, const std::string& line2,
                         double fraction = -1) {
  const int W = gr_fb_width(), H = gr_fb_height(), s = g_scale;
  color(kBg);
  gr_clear();
  color(kFg);
  text_center(H / 3, title, s);
  color(kDim);
  text_center(H / 3 + g_ch * s * 2, line1, s - 1);
  text_center(H / 3 + g_ch * s * 3, line2, s - 1);
  if (fraction >= 0) {
    int x0 = W / 8, x1 = W - W / 8, y0 = H / 2, y1 = y0 + g_ch * s / 2;
    color(0x2a3038);
    gr_fill(x0, y0, x1, y1);
    color(kAccent);
    gr_fill(x0, y0, x0 + int((x1 - x0) * std::min(fraction, 1.0)), y1);
  }
  gr_flip();
}

// ---------------------------------------------------------------- writing

static std::string g_phase;
static int64_t g_last_draw;

static void progress(uint64_t done, uint64_t total) {
  int64_t now = now_ms();
  if (done < total && now - g_last_draw < 100) return;
  g_last_draw = now;
  int pct = int(done * 100 / total);
  draw_message(g_phase, std::to_string(pct) + " %", "", double(done) / total);
}

// Copies the image into the recovery slot, then reads the slot back.
static bool flash(const Entry& e, const Part& slot, std::string* err) {
  // Checked once more right here, whatever the menu showed.
  Part again;
  if (!find_part(kSlotName, &again) || again.dev != slot.dev || again.size != slot.size) {
    *err = "recovery partition changed";
    return false;
  }
  if (slot.size < kSlotMin || slot.size > kSlotMax) {
    *err = "recovery partition has an unexpected size";
    return false;
  }
  Entry check = e;
  check.error.clear();
  check_image(&check, slot.size);
  if (!check.error.empty()) {
    *err = check.error;
    return false;
  }

  klog("writing %s (%llu bytes) to %s", e.path.c_str(), (unsigned long long)check.size, slot.dev.c_str());
  int in = open(e.path.c_str(), O_RDONLY | O_CLOEXEC);
  int out = open(slot.dev.c_str(), O_WRONLY | O_CLOEXEC);
  if (in < 0 || out < 0) {
    *err = "can't open";
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    return false;
  }
  g_phase = "Writing " + e.label;
  std::vector<unsigned char> buf(kChunk);
  bool ok = true;
  for (uint64_t off = 0; ok && off < check.size; off += kChunk) {
    size_t n = std::min<uint64_t>(kChunk, check.size - off);
    ok = read_full(in, buf.data(), n, off);
    for (size_t w = 0; ok && w < n;) {
      ssize_t r = pwrite(out, buf.data() + w, n - w, off + w);
      if (r <= 0) ok = false;
      else w += r;
    }
    progress(off + n, check.size);
  }
  ok = ok && fsync(out) == 0;
  ioctl(out, BLKFLSBUF, 0);    // drop the cached blocks, so the check reads the eMMC
  close(out);
  close(in);
  if (!ok) {
    *err = std::string("write failed: ") + strerror(errno);
    return false;
  }
  g_phase = "Checking " + e.label;
  if (!same_content(e.path, slot, check.size, progress)) {
    *err = "read-back differs";
    return false;
  }
  klog("written and verified");
  return true;
}

// ---------------------------------------------------------------- input

static int g_key = -1;

static int on_input(int fd, uint32_t epevents) {
  input_event ev;
  if (ev_get_input(fd, epevents, &ev) != 0) return 0;
  if (ev.type == EV_KEY && (ev.value == 1 || ev.value == 2)) g_key = ev.code;
  return 0;
}

static int wait_key(int timeout_ms) {
  g_key = -1;
  if (ev_wait(timeout_ms) == 0) ev_dispatch();
  return g_key;
}

// ---------------------------------------------------------------- main

int main() {
  mount_api();
  g_kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
  klog("start");
  std::string cmdline = read_file("/proc/cmdline");
  bool charger = kv_value(cmdline, "androidboot.mode", ' ') == "charger";
  std::string t = kv_value(cmdline, "bootmenu.timeout", ' ');
  int timeout_s = t.empty() ? 5 : atoi(t.c_str());

  // The eMMC and the display can still be probing when PID 1 starts.
  Part slot, cache;
  bool have_slot = false, have_cache = false;
  for (int64_t until = now_ms() + 10000;;) {
    make_nodes();
    have_slot = have_slot || find_part(kSlotName, &slot);
    have_cache = have_cache || find_part(kCacheName, &cache);
    if ((have_slot && have_cache && access("/dev/fb0", F_OK) == 0) || now_ms() > until) break;
    usleep(100 * 1000);
  }
  if (have_slot && (slot.size < kSlotMin || slot.size > kSlotMax)) {
    klog("recovery partition %s is %llu bytes: not touching it", slot.dev.c_str(), (unsigned long long)slot.size);
    have_slot = false;
  }
  klog("recovery %s, cache %s", have_slot ? slot.dev.c_str() : "missing", have_cache ? cache.dev.c_str() : "missing");
  if (have_cache && mount(cache.dev.c_str(), "/cache", "ext4", MS_RDONLY | MS_NOATIME, "noload") != 0)
    klog("mount cache: %s", strerror(errno));

  if (gr_init() != 0) {
    klog("no display: booting the recovery slot as it is");
    reboot_to("recovery");
  }
  gr_font_size(gr_sys_font(), &g_cw, &g_ch);
  g_scale = std::max(2, gr_fb_width() / 360);
  if (ev_init(on_input) != 0) klog("no input devices");

  std::vector<Entry> v = scan(have_slot ? &slot : nullptr);
  int sel = 0, dflt = -1;
  for (size_t i = 0; i < v.size(); i++)
    if (v[i].current) dflt = i;
  if (dflt >= 0) sel = dflt;
  else
    for (size_t i = 0; i < v.size(); i++)
      if (v[i].error.empty()) { sel = i; break; }
  if (charger || dflt < 0) timeout_s = 0;    // no auto-boot: plugged in while off, or nothing to default to
  klog("menu ready: %zu entries, default %d, timeout %d s%s", v.size(), dflt, timeout_s, charger ? ", charger" : "");

  int64_t deadline = timeout_s > 0 ? now_ms() + timeout_s * 1000 : -1;
  int64_t last_key = now_ms();
  bool blank = false;
  std::string note;
  for (;;) {
    std::string status = note;
    if (deadline >= 0) {
      int left = int((deadline - now_ms() + 999) / 1000);
      status = "Booting " + v[dflt].label + " in " + std::to_string(left) + " s";
    } else if (charger) {
      status = "Charging: " + read_file("/sys/class/power_supply/battery/capacity") + " %";
    } else if (status.empty() && !have_cache) {
      status = "No cache partition";
    }
    if (!blank) draw_menu(v, sel, status);

    int key = wait_key(deadline >= 0 ? 200 : 1000);
    int64_t now = now_ms();
    if (key < 0) {
      if (deadline >= 0 && now >= deadline) {
        klog("timeout: %s", v[dflt].label.c_str());
        draw_message("Starting " + v[dflt].label, "", "");
        reboot_to("recovery");
      }
      if (!blank && now - last_key > kIdleBlankMs) {
        gr_fb_blank(true);
        blank = true;
      }
      continue;
    }
    klog("key %d", key);
    last_key = now;
    deadline = -1;
    if (blank) {    // the first key only wakes the screen
      gr_fb_blank(false);
      blank = false;
      continue;
    }
    note.clear();
    if (key == KEY_VOLUMEUP || key == KEY_UP) {
      sel = (sel + v.size() - 1) % v.size();
    } else if (key == KEY_VOLUMEDOWN || key == KEY_DOWN) {
      sel = (sel + 1) % v.size();
    } else if (key == KEY_POWER || key == KEY_ENTER || key == KEY_SPACE) {
      Entry& e = v[sel];
      klog("selected %s", e.label.c_str());
      if (!e.error.empty()) {
        note = e.label + ": " + e.error;
      } else if (e.kind == BOOTLOADER) {
        reboot_to("bootloader");
      } else if (e.kind == POWEROFF) {
        draw_message("Powering off", "", "");
        power_off();
      } else if (e.kind == KEEP || e.current) {
        draw_message("Starting " + e.label, "", "");
        reboot_to("recovery");
      } else {
        std::string err;
        if (flash(e, slot, &err)) {
          draw_message("Starting " + e.label, "", "");
          reboot_to("recovery");
        }
        klog("not written: %s", err.c_str());
        // The slot may now hold part of an image: re-read what is there.
        v = scan(&slot);
        sel = std::min<int>(sel, v.size() - 1);
        note = "Failed: " + err;
      }
    }
  }
}
