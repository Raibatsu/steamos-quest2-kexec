// q2memprobe v2: map physical RAM windows that hang a core when touched (kexec-leftover hyp/TZ protection).
// The parent forks one child per window, pinned to its own core. The child reads one word per page
// (stage 1), then does an atomic +0 per page (stage 2). If the child has not finished within the timeout,
// its core is stuck: the window is recorded as BAD with the stage it died in, that core is retired,
// and the sweep continues on the remaining cores (so one boot can map up to NCPU-1 bad windows).
// Progress goes to stdout (the caller redirects it to /run/probe.log) and UDP 10.42.0.1:6666.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int usock = -1;
static struct sockaddr_in dst;

static void say(const char *fmt, ...)
{
	char buf[300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	char line[340];
	int n = snprintf(line, sizeof(line), "q2probe: %s\n", buf);
	fputs(line, stdout);
	fflush(stdout);
	if (usock >= 0)
		sendto(usock, line, n, 0, (struct sockaddr *)&dst, sizeof(dst));
}

struct shared { volatile int stage; };

int main(int argc, char **argv)
{
	uint64_t ra[512], rb[512];
	int nr = 0;
	uint64_t win = 2ULL << 20;
	int timeout_ms = 1500;
	uint64_t lo = 0, hi = ~0ULL;

	if (argc >= 3) { lo = strtoull(argv[1], 0, 16); hi = strtoull(argv[2], 0, 16); }
	if (argc >= 4) win = strtoull(argv[3], 0, 10) << 10;

	usock = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons(6666);
	inet_pton(AF_INET, "10.42.0.1", &dst.sin_addr);

	FILE *f = fopen("/proc/iomem", "r");
	if (!f) { say("cannot open /proc/iomem"); return 1; }
	char l[256];
	while (fgets(l, sizeof(l), f)) {
		if (l[0] == ' ' || !strstr(l, "System RAM"))
			continue;
		uint64_t a, b;
		if (sscanf(l, "%lx-%lx", &a, &b) != 2)
			continue;
		b += 1;
		if (b <= lo || a >= hi)
			continue;
		if (a < lo) a = lo;
		if (b > hi) b = hi;
		if (nr < 512) { ra[nr] = a; rb[nr] = b; nr++; }
	}
	fclose(f);

	int ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	int dead[64] = {0};
	int alive = ncpu;
	say("%d System RAM ranges, %d cpus, window %lu KiB, timeout %d ms", nr, ncpu, (unsigned long)(win >> 10), timeout_ms);

	int mfd = open("/dev/mem", O_RDWR);
	if (mfd < 0) { say("cannot open /dev/mem"); return 1; }
	struct shared *sh = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	int nbad = 0, cpu = 0;
	uint64_t total = 0, done = 0, nextprog = 0;
	for (int i = 0; i < nr; i++)
		total += rb[i] - ra[i];

	for (int i = 0; i < nr && alive > 1; i++) {
		for (uint64_t w = ra[i] & ~(win - 1); w < rb[i] && alive > 1; w += win) {
			uint64_t a = w < ra[i] ? ra[i] : w;
			uint64_t e = w + win > rb[i] ? rb[i] : w + win;
			a = (a + 0xfff) & ~0xfffULL;
			e &= ~0xfffULL;
			if (a >= e)
				continue;

			while (dead[cpu % ncpu])
				cpu++;
			int c = cpu % ncpu;
			cpu++;

			sh->stage = 0;
			say("touching %#lx-%#lx", (unsigned long)a, (unsigned long)e);
			pid_t pid = fork();
			if (pid == 0) {
				cpu_set_t one;
				CPU_ZERO(&one);
				CPU_SET(c, &one);
				sched_setaffinity(0, sizeof(one), &one);
				sh->stage = 1;
				volatile uint64_t *m = mmap(0, e - a, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, a);
				if (m == MAP_FAILED)
					_exit(3);
				for (uint64_t off = 0; off < e - a; off += 4096)
					(void)m[off / 8];
				sh->stage = 2;
				for (uint64_t off = 0; off < e - a; off += 4096)
					__sync_fetch_and_add((uint64_t *)&m[off / 8], 0);
				sh->stage = 3;
				_exit(0);
			}
			int status = 0, waited = 0, ok = 0;
			struct timespec ts = { 0, 5 * 1000 * 1000 };
			while (waited < timeout_ms) {
				pid_t r = waitpid(pid, &status, WNOHANG);
				if (r == pid) { ok = 1; break; }
				nanosleep(&ts, 0);
				waited += 5;
			}
			if (!ok) {
				nbad++;
				dead[c] = 1;
				alive--;
				say("BAD window %#lx-%#lx hung cpu%d in stage %d (%s); %d cpus left",
				    (unsigned long)a, (unsigned long)e, c, sh->stage,
				    sh->stage == 1 ? "READ" : sh->stage == 2 ? "ATOMIC WRITE" : "?", alive);
			} else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
				say("window %#lx-%#lx child failed status=%#x", (unsigned long)a, (unsigned long)e, status);
			}
			done += e - a;
			if (done >= nextprog) {
				say("progress %lu/%lu MiB (at %#lx), bad so far %d", (unsigned long)(done >> 20),
				    (unsigned long)(total >> 20), (unsigned long)e, nbad);
				nextprog = done + (256ULL << 20);
			}
		}
	}
	say("SWEEP END: %d bad window(s), %d/%d cpus still alive, covered %lu of %lu MiB",
	    nbad, alive, ncpu, (unsigned long)(done >> 20), (unsigned long)(total >> 20));
	return 0;
}
