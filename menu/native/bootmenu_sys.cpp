// The system side of the boot menu: see bootmenu_sys.h.
//
// The bullhead kernel has no devtmpfs: block device nodes are made here from
// sysfs (embedded-compose's init makes the display and input ones).

#include "bootmenu_sys.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
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
#include <atomic>
#include <string>
#include <thread>
#include <vector>

static const char* kImageDir = "/cache/bootmenu";
static const char* kSlotName = "recovery";              // the only partition ever written
static const char* kCacheName = "cache";
static const uint64_t kSlotMin = 8ull << 20;            // bullhead's recovery is 32 MiB;
static const uint64_t kSlotMax = 64ull << 20;           // anything else is not that partition
static const size_t kHdrSize = 1648;                    // boot image header v0 (bootimg.h)
static const size_t kChunk = 1 << 20;

// ---------------------------------------------------------------- logging

static void klog(const char* fmt, ...) {
  static int kmsg = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
  char buf[512];
  int n = snprintf(buf, sizeof(buf), "<6>bootmenu: ");
  va_list ap;
  va_start(ap, fmt);
  n += vsnprintf(buf + n, sizeof(buf) - n, fmt, ap);
  va_end(ap);
  if (n >= int(sizeof(buf)) - 1) n = sizeof(buf) - 2;
  buf[n++] = '\n';
  if (kmsg >= 0 && write(kmsg, buf, n) < 0) {}   // nowhere left to report it
}

void bm_log(const char* msg) { klog("%s", msg); }

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

// ---------------------------------------------------------------- partitions

struct Part {
  std::string dev;
  uint64_t size = 0;
};

// What ueventd would do, for block devices: /dev/block/<name>.
static void make_block_nodes() {
  mkdir("/dev/block", 0755);
  for (auto& n : list_dir("/sys/class/block")) {
    unsigned ma, mi;
    if (sscanf(read_file("/sys/class/block/" + n + "/dev").c_str(), "%u:%u", &ma, &mi) != 2) continue;
    std::string node = "/dev/block/" + n;
    struct stat st;
    if (stat(node.c_str(), &st) != 0) mknod(node.c_str(), S_IFBLK | 0600, makedev(ma, mi));
  }
}

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

struct Entry {
  bm_entry pub;
  std::string path;
  unsigned char hdr[kHdrSize];
};

static Part g_slot;
static bool g_have_slot, g_have_cache;
static std::vector<Entry> g_entries;
static int g_default = -1;
static std::string g_cmdline;

static void set_str(char* dst, size_t cap, const std::string& s) { snprintf(dst, cap, "%s", s.c_str()); }

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
// Returns the error, empty if it can be booted; *size and hdr are filled in.
static std::string check_image(const std::string& path, uint64_t slot_size, uint64_t* size, unsigned char* hdr) {
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  struct stat st;
  if (fd < 0 || fstat(fd, &st) != 0) {
    if (fd >= 0) close(fd);
    return "can't read";
  }
  *size = st.st_size;
  std::string err;
  if (!read_full(fd, hdr, kHdrSize, 0) || memcmp(hdr, "ANDROID!", 8) != 0) {
    err = "not a boot image";
  } else {
    uint64_t page = le32(hdr + 36);
    auto pages = [page](uint64_t n) { return (n + page - 1) / page * page; };
    uint64_t need = page + pages(le32(hdr + 8)) + pages(le32(hdr + 16)) + pages(le32(hdr + 24));
    if (page < 2048 || page > 16384 || (page & (page - 1)) || le32(hdr + 8) == 0)
      err = "bad header";
    else if (need > *size)
      err = "truncated";
    else if (*size > slot_size)
      err = "too big for recovery";
  }
  close(fd);
  return err;
}

// Does the slot start with exactly this file? Reads both in chunks.
static bool same_content(const std::string& path, const Part& slot, uint64_t size,
                         std::atomic<int>* percent = nullptr) {
  int a = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  int b = open(slot.dev.c_str(), O_RDONLY | O_CLOEXEC);
  bool same = a >= 0 && b >= 0;
  std::vector<unsigned char> x(kChunk), y(kChunk);
  for (uint64_t off = 0; same && off < size; off += kChunk) {
    size_t n = std::min<uint64_t>(kChunk, size - off);
    same = read_full(a, x.data(), n, off) && read_full(b, y.data(), n, off) && memcmp(x.data(), y.data(), n) == 0;
    if (percent) *percent = int((off + n) * 100 / size);
  }
  if (a >= 0) close(a);
  if (b >= 0) close(b);
  return same;
}

void bm_init() {
  // Started without a console, fds 0-2 can be closed: hold them with /dev/null,
  // or the next open() (the kernel log, a partition) becomes "stdin" or "stdout".
  int fd;
  while ((fd = open("/dev/null", O_RDWR)) >= 0 && fd <= 2) {}
  if (fd > 2) close(fd);
  g_cmdline = read_file("/proc/cmdline");
  // The eMMC can still be probing when the app starts.
  for (int64_t until = now_ms() + 10000;;) {
    make_block_nodes();
    g_have_slot = g_have_slot || find_part(kSlotName, &g_slot);
    Part cache;
    if (!g_have_cache && find_part(kCacheName, &cache)) {
      g_have_cache = true;
      mkdir("/cache", 0755);
      if (mount(cache.dev.c_str(), "/cache", "ext4", MS_RDONLY | MS_NOATIME, "noload") != 0)
        klog("mount cache: %s", strerror(errno));
      klog("cache %s", cache.dev.c_str());
    }
    if ((g_have_slot && g_have_cache) || now_ms() > until) break;
    usleep(100 * 1000);
  }
  if (g_have_slot && (g_slot.size < kSlotMin || g_slot.size > kSlotMax)) {
    klog("recovery partition %s is %llu bytes: not touching it", g_slot.dev.c_str(), (unsigned long long)g_slot.size);
    g_have_slot = false;
  }
  klog("recovery %s, cache %s", g_have_slot ? g_slot.dev.c_str() : "missing", g_have_cache ? "mounted" : "missing");
}

int bm_scan() {
  g_entries.clear();
  g_default = -1;
  unsigned char slot_hdr[kHdrSize];
  bool have_slot_hdr = false;
  if (g_have_slot) {
    int fd = open(g_slot.dev.c_str(), O_RDONLY | O_CLOEXEC);
    have_slot_hdr = fd >= 0 && read_full(fd, slot_hdr, kHdrSize, 0);
    if (fd >= 0) close(fd);
  }
  for (auto& n : list_dir(kImageDir)) {
    if (n.size() < 5 || n.compare(n.size() - 4, 4, ".img") != 0) continue;
    Entry e = {};
    e.pub.kind = BM_IMAGE;
    e.path = std::string(kImageDir) + "/" + n;
    set_str(e.pub.label, sizeof(e.pub.label), label_of(n));
    uint64_t size = 0;
    std::string err = g_have_slot ? check_image(e.path, g_slot.size, &size, e.hdr) : "no recovery partition";
    e.pub.size = size;
    set_str(e.pub.error, sizeof(e.pub.error), err);
    // The header (with its SHA-1 id) picks the candidate, the full compare confirms it.
    if (err.empty() && have_slot_hdr && g_default < 0 && memcmp(e.hdr, slot_hdr, kHdrSize) == 0 &&
        same_content(e.path, g_slot, size)) {
      e.pub.current = 1;
      g_default = g_entries.size();
    }
    klog("image %s: %s%s, %llu bytes", n.c_str(), err.empty() ? "ok" : err.c_str(),
         e.pub.current ? ", in the recovery slot" : "", (unsigned long long)size);
    g_entries.push_back(e);
  }
  if (g_have_slot && g_default < 0) {
    Entry e = {};
    e.pub.kind = BM_KEEP;
    set_str(e.pub.label, sizeof(e.pub.label), "Recovery slot as is");
    if (!have_slot_hdr || memcmp(slot_hdr, "ANDROID!", 8) != 0)
      set_str(e.pub.error, sizeof(e.pub.error), "no boot image there");
    g_entries.push_back(e);
  }
  Entry b = {};
  b.pub.kind = BM_BOOTLOADER;
  set_str(b.pub.label, sizeof(b.pub.label), "Bootloader");
  g_entries.push_back(b);
  Entry p = {};
  p.pub.kind = BM_POWEROFF;
  set_str(p.pub.label, sizeof(p.pub.label), "Power off");
  g_entries.push_back(p);
  return g_entries.size();
}

const bm_entry* bm_get(int i) { return i >= 0 && i < int(g_entries.size()) ? &g_entries[i].pub : nullptr; }
int bm_default() { return g_default; }

int bm_timeout() {
  std::string t = kv_value(g_cmdline, "bootmenu.timeout", ' ');
  return t.empty() ? 5 : atoi(t.c_str());
}

int bm_charger() { return kv_value(g_cmdline, "androidboot.mode", ' ') == "charger"; }

int bm_battery() {
  std::string s = read_file("/sys/class/power_supply/battery/capacity");
  return s.empty() ? -1 : atoi(s.c_str());
}

// ---------------------------------------------------------------- writing

static std::atomic<int> g_state{BM_IDLE}, g_percent{0};
static std::string g_error;

// Copies the image into the recovery slot, then reads the slot back.
static bool flash(const Entry& e, std::string* err) {
  // Checked once more right here, whatever the menu showed.
  Part again;
  if (!find_part(kSlotName, &again) || again.dev != g_slot.dev || again.size != g_slot.size) {
    *err = "recovery partition changed";
    return false;
  }
  if (g_slot.size < kSlotMin || g_slot.size > kSlotMax) {
    *err = "recovery partition has an unexpected size";
    return false;
  }
  uint64_t size;
  unsigned char hdr[kHdrSize];
  *err = check_image(e.path, g_slot.size, &size, hdr);
  if (!err->empty()) return false;

  klog("writing %s (%llu bytes) to %s", e.path.c_str(), (unsigned long long)size, g_slot.dev.c_str());
  int in = open(e.path.c_str(), O_RDONLY | O_CLOEXEC);
  int out = open(g_slot.dev.c_str(), O_WRONLY | O_CLOEXEC);
  if (in < 0 || out < 0) {
    *err = "can't open";
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    return false;
  }
  std::vector<unsigned char> buf(kChunk);
  bool ok = true;
  for (uint64_t off = 0; ok && off < size; off += kChunk) {
    size_t n = std::min<uint64_t>(kChunk, size - off);
    ok = read_full(in, buf.data(), n, off);
    for (size_t w = 0; ok && w < n;) {
      ssize_t r = pwrite(out, buf.data() + w, n - w, off + w);
      if (r <= 0) ok = false;
      else w += r;
    }
    g_percent = int((off + n) * 100 / size);
  }
  ok = ok && fsync(out) == 0;
  ioctl(out, BLKFLSBUF, 0);    // drop the cached blocks, so the check reads the eMMC
  close(out);
  close(in);
  if (!ok) {
    *err = std::string("write failed: ") + strerror(errno);
    return false;
  }
  g_state = BM_CHECKING;
  g_percent = 0;
  if (!same_content(e.path, g_slot, size, &g_percent)) {
    *err = "read-back differs";
    return false;
  }
  klog("written and verified");
  return true;
}

int bm_flash_start(int i) {
  if (i < 0 || i >= int(g_entries.size()) || g_entries[i].pub.kind != BM_IMAGE || g_entries[i].pub.error[0] ||
      !g_have_slot || g_state == BM_WRITING || g_state == BM_CHECKING)
    return -1;
  g_state = BM_WRITING;
  g_percent = 0;
  Entry e = g_entries[i];
  std::thread([e, i] {
    std::string err;
    if (flash(e, &err)) {
      for (auto& x : g_entries) x.pub.current = 0;
      g_entries[i].pub.current = 1;
      g_default = i;
      g_state = BM_DONE;
    } else {
      klog("not written: %s", err.c_str());
      g_error = err;
      g_state = BM_FAILED;
    }
  }).detach();
  return 0;
}

int bm_flash_state() { return g_state; }
int bm_flash_percent() { return g_percent; }
const char* bm_flash_error() { return g_error.c_str(); }

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
}

int bm_boot(int i) {
  const bm_entry* e = bm_get(i);
  if (!e || e->error[0]) return -1;
  switch (e->kind) {
    case BM_IMAGE:
      if (!e->current) return -1;    // write it first (bm_flash_start)
      reboot_to("recovery");
      break;
    case BM_KEEP:
      reboot_to("recovery");
      break;
    case BM_BOOTLOADER:
      reboot_to("bootloader");
      break;
    case BM_POWEROFF:
      klog("power off");
      sync();
      umount2("/cache", MNT_DETACH);
      reboot(RB_POWER_OFF);
      break;
  }
  return -1;
}
