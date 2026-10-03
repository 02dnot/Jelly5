/*
 * Jelly5 bring-up probe: the smallest app the toolchain can make. It shows a
 * system notification and sends one UDP log line, then idles. If this runs
 * where the full app does not, the full app dies in its own start-up.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
int sceSystemServiceHideSplashScreen(void);

static void notify(const char *t)
{
    struct { char pad[45]; char msg[3075]; } n;
    memset(&n, 0, sizeof n);
    snprintf(n.msg, sizeof n.msg, "%s", t);
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
}

static void udp_log(const char *t)
{
#ifdef JELLY5_LOG_HOST
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(JELLY5_LOG_PORT);
    inet_pton(AF_INET, JELLY5_LOG_HOST, &a.sin_addr);
    sendto(fd, t, strlen(t), 0, (struct sockaddr *)&a, sizeof a);
    close(fd);
#else
    (void)t;
#endif
}

int main(void)
{
    notify("Jelly5 probe: main kjører");
    udp_log("[probe] main reached\n");
    sceSystemServiceHideSplashScreen();
    for (int i = 0; i < 30; i++) {
        char line[64];
        snprintf(line, sizeof line, "[probe] alive %d\n", i);
        udp_log(line);
        sleep(1);
    }
    return 0;
}

/* The runtime shims log through the engine; the probe has no engine. */
void evo_boot_log(const char *fmt, ...) { (void)fmt; }

/* The packager expects a .data.rel.ro section, which a program this small
 * would otherwise not have: one relocated constant pointer makes it. */
__attribute__((used)) const char *const jelly5_probe_tag[] = {"jelly5-probe", "PPSA99506"};
