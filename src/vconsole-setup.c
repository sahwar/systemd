/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Kay Sievers

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdarg.h>
#include <limits.h>
#include <locale.h>
#include <langinfo.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/tiocl.h>
#include <linux/kd.h>

#include "util.h"
#include "log.h"
#include "macro.h"

static bool is_vconsole(int fd) {
        unsigned char data[1];

        data[0] = TIOCL_GETFGCONSOLE;
        return ioctl(fd, TIOCLINUX, data) >= 0;
}

static bool is_locale_utf8(void) {
        const char *set;

        if (!setlocale(LC_ALL, ""))
                return true;

        set = nl_langinfo(CODESET);
        if (!set)
                return true;

        return streq(set, "UTF-8");
}

static int disable_utf8(int fd) {
        int r = 0, k;

        if (ioctl(fd, KDSKBMODE, K_XLATE) < 0)
                r = -errno;

        if (loop_write(fd, "\033%@", 3, false) < 0)
                r = -errno;

        if ((k = write_one_line_file("/sys/module/vt/parameters/default_utf8", "0")) < 0)
                r = k;

        if (r < 0)
                log_warning("Failed to disable UTF-8: %s", strerror(errno));

        return r;
}

static int load_keymap(const char *vc, const char *map, const char *map_toggle, bool utf8, pid_t *_pid) {
        const char *args[8];
        int i = 0;
        pid_t pid;

        args[i++] = KBD_LOADKEYS;
        args[i++] = "-q";
        args[i++] = "-C";
        args[i++] = vc;
        if (utf8)
                args[i++] = "-u";
        args[i++] = map;
        if (map_toggle)
                args[i++] = map_toggle;
        args[i++] = NULL;

        if ((pid = fork()) < 0) {
                log_error("Failed to fork: %m");
                return -errno;
        } else if (pid == 0) {
                execv(args[0], (char **) args);
                _exit(EXIT_FAILURE);
        }

        *_pid = pid;
        return 0;
}

static int load_font(const char *vc, const char *font, const char *map, const char *unimap, pid_t *_pid) {
        const char *args[9];
        int i = 0;
        pid_t pid;

        args[i++] = KBD_SETFONT;
        args[i++] = "-C";
        args[i++] = vc;
        args[i++] = font;
        if (map) {
                args[i++] = "-m";
                args[i++] = map;
        }
        if (unimap) {
                args[i++] = "-u";
                args[i++] = unimap;
        }
        args[i++] = NULL;

        if ((pid = fork()) < 0) {
                log_error("Failed to fork: %m");
                return -errno;
        } else if (pid == 0) {
                execv(args[0], (char **) args);
                _exit(EXIT_FAILURE);
        }

        *_pid = pid;
        return 0;
}

int main(int argc, char **argv) {
        const char *vc;
        char *vc_keymap = NULL;
        char *vc_keymap_toggle = NULL;
        char *vc_font = NULL;
        char *vc_font_map = NULL;
        char *vc_font_unimap = NULL;
#ifdef TARGET_GENTOO
        char *vc_unicode = NULL;
#endif
        int fd = -1;
        bool utf8;
        int r = EXIT_FAILURE;
        pid_t font_pid = 0, keymap_pid = 0;

        log_set_target(LOG_TARGET_SYSLOG_OR_KMSG);
        log_parse_environment();
        log_open();

        if (argv[1])
                vc = argv[1];
        else
                vc = "/dev/tty0";

        if ((fd = open(vc, O_RDWR|O_CLOEXEC)) < 0) {
                log_error("Failed to open %s: %m", vc);
                goto finish;
        }

        if (!is_vconsole(fd)) {
                log_error("Device %s is not a virtual console.", vc);
                goto finish;
        }

        utf8 = is_locale_utf8();

        if ((r = parse_env_file("/proc/cmdline", WHITESPACE,
#ifdef TARGET_FEDORA
                                "SYSFONT", &vc_font,
                                "KEYTABLE", &vc_keymap,
#endif
                                "vconsole.keymap", &vc_keymap,
                                "vconsole.keymap.toggle", &vc_keymap_toggle,
                                "vconsole.font", &vc_font,
                                "vconsole.font.map", &vc_font_map,
                                "vconsole.font.unimap", &vc_font_unimap,
                                NULL)) < 0) {

                if (r != -ENOENT)
                        log_warning("Failed to read /proc/cmdline: %s", strerror(-r));
        }

        /* Hmm, nothing set on the kernel cmd line? Then let's
         * try /etc/vconsole.conf */
        if (r <= 0 &&
            (r = parse_env_file("/etc/vconsole.conf", NEWLINE,
                                "KEYMAP", &vc_keymap,
                                "KEYMAP_TOGGLE", &vc_keymap_toggle,
                                "FONT", &vc_font,
                                "FONT_MAP", &vc_font_map,
                                "FONT_UNIMAP", &vc_font_unimap,
                                NULL)) < 0) {

                if (r != -ENOENT)
                        log_warning("Failed to read /etc/vconsole.conf: %s", strerror(-r));
        }

        if (r <= 0) {
#ifdef TARGET_FEDORA
                if ((r = parse_env_file("/etc/sysconfig/i18n", NEWLINE,
                                        "SYSFONT", &vc_font,
                                        "SYSFONTACM", &vc_font_map,
                                        "UNIMAP", &vc_font_unimap,
                                        NULL)) < 0) {

                        if (r != -ENOENT)
                                log_warning("Failed to read /etc/sysconfig/i18n: %s", strerror(-r));
                }

                if ((r = parse_env_file("/etc/sysconfig/keyboard", NEWLINE,
                                        "KEYTABLE", &vc_keymap,
                                        "KEYMAP", &vc_keymap,
                                        NULL)) < 0) {

                        if (r != -ENOENT)
                                log_warning("Failed to read /etc/sysconfig/i18n: %s", strerror(-r));
                }

                if (access("/etc/sysconfig/console/default.kmap", F_OK) >= 0) {
                        char *t;

                        if (!(t = strdup("/etc/sysconfig/console/default.kmap"))) {
                                log_error("Out of memory.");
                                goto finish;
                        }

                        free(vc_keymap);
                        vc_keymap = t;
                }

#elif defined(TARGET_SUSE)
                if ((r = parse_env_file("/etc/sysconfig/keyboard", NEWLINE,
                                        "KEYTABLE", &vc_keymap,
                                        NULL)) < 0) {

                        if (r != -ENOENT)
                                log_warning("Failed to read /etc/sysconfig/keyboard: %s", strerror(-r));
                }

                if ((r = parse_env_file("/etc/sysconfig/console", NEWLINE,
                                        "CONSOLE_FONT", &vc_font,
                                        "CONSOLE_SCREENMAP", &vc_font_map,
                                        "CONSOLE_UNICODEMAP", &vc_font_unimap,
                                        NULL)) < 0) {

                        if (r != -ENOENT)
                                log_warning("Failed to read /etc/sysconfig/console: %s", strerror(-r));
                }

#elif defined(TARGET_ARCH)
                if ((r = parse_env_file("/etc/rc.conf", NEWLINE,
                                        "KEYMAP", &vc_keymap,
                                        "CONSOLEFONT", &vc_font,
                                        "CONSOLEMAP", &vc_font_map,
                                        NULL)) < 0) {

                        if (r != -ENOENT)
                                log_warning("Failed to read /etc/rc.conf: %s", strerror(-r));
                }

#elif defined(TARGET_FRUGALWARE)
                if ((r = parse_env_file("/etc/sysconfig/keymap", NEWLINE,
                                        "keymap", &vc_keymap,
                                        NULL)) < 0) {
                        if (r != -ENOENT)
                                log_warning("Failed to read /etc/sysconfig/keymap: %s", strerror(-r));
                }
                if ((r = parse_env_file("/etc/sysconfig/font", NEWLINE,
                                        "font", &vc_font,
                                        NULL)) < 0) {
                        if (r != -ENOENT)
                                log_warning("Failed to read /etc/sysconfig/font: %s", strerror(-r));
                }

#elif defined(TARGET_ALTLINUX)
                if ((r = parse_env_file("/etc/sysconfig/keyboard", NEWLINE,
                                        "KEYTABLE", &vc_keymap,
                                        NULL)) < 0) {

                        if (r != -ENOENT)
                                log_warning("Failed to read /etc/sysconfig/keyboard: %s", strerror(-r));
                }

                if ((r = parse_env_file("/etc/sysconfig/consolefont", NEWLINE,
                                        "SYSFONT", &vc_font,
                                        NULL)) < 0) {

                        if (r != -ENOENT)
                                log_warning("Failed to read /etc/sysconfig/console: %s", strerror(-r));
                }

#elif defined(TARGET_GENTOO)
                if ((r = parse_env_file("/etc/rc.conf", NEWLINE,
                                        "unicode", &vc_unicode,
                                        NULL)) < 0) {
                        if (r != -ENOENT)
                                log_warning("Failed to read /etc/rc.conf: %s", strerror(-r));
                }

                if (vc_unicode) {
                        int rc_unicode;

                        if ((rc_unicode = parse_boolean(vc_unicode)) < 0)
                                log_error("Unknown value for /etc/rc.conf unicode=%s", vc_unicode);
                        else {
                                if (rc_unicode && !utf8)
                                        log_warning("/etc/rc.conf wants unicode, but current locale is not UTF-8 capable!");
                                else if (!rc_unicode && utf8) {
                                        log_debug("/etc/rc.conf does not want unicode, leave it on in kernel but does not apply to vconsole.");
                                        utf8 = false;
                                }
                        }
                }

                /* /etc/conf.d/consolefont comments and gentoo
                 * documentation mention uppercase, but the actual
                 * contents are lowercase.  the existing
                 * /etc/init.d/consolefont tries both
                 */
                if ((r = parse_env_file("/etc/conf.d/consolefont", NEWLINE,
                                        "CONSOLEFONT", &vc_font,
                                        "consolefont", &vc_font,
                                        "consoletranslation", &vc_font_map,
                                        "CONSOLETRANSLATION", &vc_font_map,
                                        "unicodemap", &vc_font_unimap,
                                        "UNICODEMAP", &vc_font_unimap,
                                        NULL)) < 0) {
                        if (r != -ENOENT)
                                log_warning("Failed to read /etc/conf.d/consolefont: %s", strerror(-r));
                }

                if ((r = parse_env_file("/etc/conf.d/keymaps", NEWLINE,
                                        "keymap", &vc_keymap,
                                        "KEYMAP", &vc_keymap,
                                        NULL)) < 0) {
                        if (r != -ENOENT)
                                log_warning("Failed to read /etc/conf.d/keymaps: %s", strerror(-r));
                }
#endif
        }

        if (!vc_keymap)
                vc_keymap = strdup("us");
        if (!vc_font)
                vc_font = strdup(DEFAULT_FONT);

        if (!vc_keymap || !vc_font) {
                log_error("Failed to allocate strings.");
                goto finish;
        }

        if (!utf8)
                disable_utf8(fd);

        if (load_keymap(vc, vc_keymap, vc_keymap_toggle, utf8, &keymap_pid) >= 0 &&
            load_font(vc, vc_font, vc_font_map, vc_font_unimap, &font_pid) >= 0)
                r = EXIT_SUCCESS;

finish:
        if (keymap_pid > 0)
                wait_for_terminate_and_warn(KBD_LOADKEYS, keymap_pid);

        if (font_pid > 0)
                wait_for_terminate_and_warn(KBD_SETFONT, font_pid);

        free(vc_keymap);
        free(vc_font);
        free(vc_font_map);
        free(vc_font_unimap);

        if (fd >= 0)
                close_nointr_nofail(fd);

        return r;
}
