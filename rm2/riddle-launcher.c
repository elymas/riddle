// riddle-launcher — watch the reMarkable 2 touchscreen for a 5-finger tap and
// launch the diary. Runs while xochitl is up; reads the touch device WITHOUT
// grabbing it, so normal use is unaffected. When riddle itself runs it grabs
// the device (EVIOCGRAB), so this watcher goes quiet until riddle exits.
//
// Build (rM2 SDK): $CC riddle-launcher.c -O2 -o riddle-launcher
// Install: see rm2/install-launcher.sh
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_SLOTS 32
#define FINGERS 5
#define LAUNCH "/home/root/riddle/riddle-takeover.sh"

// 32-bit kernel input_event: 8-byte timeval, then type/code/value.
// (struct input_event from linux/input.h already matches the running kernel's
// word size when built with the device SDK, so we use it directly.)

static int find_touch(void) {
    for (int i = 0; i < 16; i++) {
        char path[64], name[128] = {0};
        snprintf(path, sizeof path, "/sys/class/input/event%d/device/name", i);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f)) {
            for (char* p = name; *p; p++) *p = (*p >= 'A' && *p <= 'Z') ? *p + 32 : *p;
            if (strstr(name, "pt_mt") || strstr(name, "touch")) {
                fclose(f);
                char dev[32];
                snprintf(dev, sizeof dev, "/dev/input/event%d", i);
                int fd = open(dev, O_RDONLY);
                if (fd >= 0) return fd;
            }
        }
        fclose(f);
    }
    return -1;
}

static int riddle_running(void) {
    // Cheap check: is a process named "riddle" alive?
    DIR* d = opendir("/proc");
    if (!d) return 0;
    struct dirent* e;
    int found = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char p[64], comm[64] = {0};
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        FILE* f = fopen(p, "r");
        if (!f) continue;
        if (fgets(comm, sizeof comm, f)) {
            comm[strcspn(comm, "\n")] = 0;
            if (strcmp(comm, "riddle") == 0) found = 1;
        }
        fclose(f);
        if (found) break;
    }
    closedir(d);
    return found;
}

static void launch(void) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        // Detach fully; riddle-takeover.sh stops xochitl, runs riddle, and
        // restores xochitl on exit.
        execl("/bin/sh", "sh", "-c", LAUNCH " >/tmp/riddle-launcher-run.log 2>&1", (char*)NULL);
        _exit(127);
    }
}

int main(void) {
    int fd = -1;
    int active[MAX_SLOTS] = {0};   // slot -> tracking id != -1
    int cur = 0;
    int armed = 1;                 // rising-edge guard: re-arm when fingers < 5

    for (;;) {
        if (fd < 0) {
            fd = find_touch();
            if (fd < 0) { sleep(2); continue; }
        }
        struct input_event ev;
        ssize_t n = read(fd, &ev, sizeof ev);
        if (n != (ssize_t)sizeof ev) {
            // Device vanished (e.g. transient) — reopen after a beat.
            close(fd); fd = -1;
            memset(active, 0, sizeof active); cur = 0; armed = 1;
            sleep(1);
            continue;
        }
        if (ev.type == EV_ABS && ev.code == ABS_MT_SLOT) {
            cur = ev.value; if (cur < 0) cur = 0; if (cur >= MAX_SLOTS) cur = MAX_SLOTS - 1;
        } else if (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID) {
            active[cur] = (ev.value != -1);
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            int count = 0;
            for (int i = 0; i < MAX_SLOTS; i++) count += active[i];
            if (count < FINGERS) armed = 1;
            // ponytail: rising-edge on >=5 simultaneous slots. A resting palm
            // can occasionally trip this; if false positives bite, gate on a
            // brief tap (all 5 down then up within ~700ms, low motion).
            if (count >= FINGERS && armed && !riddle_running()) {
                armed = 0;
                launch();
            }
        }
    }
    return 0;
}
