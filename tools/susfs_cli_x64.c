// SPDX-License-Identifier: GPL-2.0-only
/* susfs_cli_x64 - the classic `susfs` command for the x86_64 LKM.
 *
 * Manager modules written for arm64 phones drive a built-in susfs through a
 * CLI named `susfs` / `ksu_susfs` (BRENE, sidex15's module, and friends).
 * Those binaries are arm64 and speak to kernels with susfs compiled in.
 * On this port the same command surface is served by routing each command
 * to wherever the LKM implements it: table commands go to the /proc
 * control nodes, the rest go through the reboot-ABI supercall
 * (reboot(KSU_INSTALL_MAGIC1, SUSFS_MAGIC, cmd, payload)), which ksud
 * itself speaks.
 *
 * Freestanding like tools/susfs_sc_x64.c: no bionic sysroot in the build
 * container, own _start, raw syscalls.
 *
 * Build (in the DDK container):
 *
 *     clang --target=x86_64-linux-gnu -O2 -nostdlib -static-pie \
 *           -fno-stack-protector -fuse-ld=lld -Wl,-e,_start \
 *           -o susfs_cli_x64 tools/susfs_cli_x64.c
 *
 * Exit status: 0 on success, 1 when the kernel or a node refused, 2 usage.
 */

typedef unsigned long u64;

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_reboot 169

#define O_RDONLY 0
#define O_WRONLY 1
#define O_TRUNC 01000

#define KSU_INSTALL_MAGIC1 0xDEADBEEF
#define SUSFS_MAGIC 0xFAFAFAFA

/* susfs_abi.h command ids */
#define CMD_SUSFS_ADD_SUS_PATH                  0x55550
#define CMD_SUSFS_ADD_SUS_PATH_LOOP             0x55553
#define CMD_SUSFS_HIDE_SUS_MNTS_FOR_NON_SU_PROCS 0x55561
#define CMD_SUSFS_SET_UNAME                     0x55590
#define CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG     0x555b0
#define CMD_SUSFS_SHOW_VERSION                  0x555e1
#define CMD_SUSFS_SHOW_ENABLED_FEATURES         0x555e2
#define CMD_SUSFS_SHOW_VARIANT                  0x555e3
#define CMD_SUSFS_ENABLE_AVC_LOG_SPOOFING       0x60010
#define CMD_SUSFS_ADD_SUS_MAP                   0x60020

#define PATH_MAX_LEN 256
#define UNAME_FIELD 65			/* __NEW_UTS_LEN + 1 */
#define FEATURES_SIZE 8192
#define ERR_SEED 126

static char sc_buf[FEATURES_SIZE + 4] __attribute__((aligned(16)));
static char line[FEATURES_SIZE];

/* Freestanding: clang rewrites zeroing and copying loops into memset/memcpy
 * calls (the CI link failed on exactly that), so both must exist here. */
void *memset(void *dst, int c, unsigned long n)
{
	unsigned char *p = dst;
	unsigned long i;

	for (i = 0; i < n; i++)
		p[i] = (unsigned char)c;
	return dst;
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;
	unsigned long i;

	for (i = 0; i < n; i++)
		d[i] = s[i];
	return dst;
}

static long sys5(long n, long a, long b, long c, long d, long e)
{
	register long rax __asm__("rax") = n;
	register long rdi __asm__("rdi") = a;
	register long rsi __asm__("rsi") = b;
	register long rdx __asm__("rdx") = c;
	register long r10 __asm__("r10") = d;
	register long r8 __asm__("r8") = e;

	__asm__ volatile("syscall"
			 : "+r"(rax)
			 : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8)
			 : "memory", "cc", "rcx", "r11");
	return rax;
}

static unsigned long slen(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	return n;
}

static void out(const char *s, unsigned long n)
{
	sys5(SYS_write, 1, (long)s, (long)n, 0, 0);
}

static void say(const char *s)
{
	out(s, slen(s));
}

static int sequal(const char *a, const char *b)
{
	unsigned long i;

	for (i = 0; a[i] && a[i] == b[i]; i++)
		;
	return a[i] == b[i];
}

static void scopy(char *dst, const char *src, unsigned long cap)
{
	unsigned long i;

	for (i = 0; i + 1 < cap && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

/* reboot-ABI supercall with sc_buf as the payload.  Returns the kernel's
 * err field (0 = success), or -999 when the syscall itself failed. */
static long supercall(unsigned long cmd, unsigned long len)
{
	long rc;
	long err;

	if (len >= 4)
		*(int *)(sc_buf + len - 4) = ERR_SEED;
	rc = sys5(SYS_reboot, KSU_INSTALL_MAGIC1, SUSFS_MAGIC, (long)cmd,
		  (long)sc_buf, 0);
	if (rc != 0)
		return -999;
	err = (len >= 4) ? *(int *)(sc_buf + len - 4) : 0;
	return err;
}

/* Write one command line to a /proc node.  proc handlers answer `len` on
 * success and a negative errno on failure. */
static long proc_write(const char *node)
{
	long fd = sys5(SYS_open, (long)node, O_WRONLY, 0, 0, 0);
	long rc;

	if (fd < 0)
		return fd;
	rc = sys5(SYS_write, fd, (long)line, (long)slen(line), 0, 0);
	sys5(SYS_close, fd, 0, 0, 0, 0);
	return rc;
}

static int fail(const char *what, long err)
{
	say("susfs: ");
	say(what);
	say(" failed");
	if (err < 0 && err > -1000) {
		say(" (kernel err ");
		/* small negative numbers only; print as decimal */
		{
			char tmp[8];
			int n = 0;
			unsigned long u = (unsigned long)(-err);

			while (u) {
				tmp[n++] = (char)('0' + (u % 10));
				u /= 10;
			}
			while (n)
				out(&tmp[--n], 1);
		}
		say(")");
	}
	say("\n");
	return 1;
}

static void print_sc_string(void)
{
	out(sc_buf, slen(sc_buf));
	if (slen(sc_buf) && sc_buf[slen(sc_buf) - 1] != '\n')
		say("\n");
}

/* join argv[from..to) into line as "cmd arg arg" */
static int join(char **argv, long from, long to)
{
	unsigned long n = 0;
	long i;

	for (i = from; i <= to; i++) {
		unsigned long l = slen(argv[i]);

		if (n + l + 2 >= sizeof(line))
			return -1;
		if (i > from)
			line[n++] = ' ';
		scopy(line + n, argv[i], l + 1);
		n += l;
	}
	line[n] = '\0';
	return 0;
}

/* 260-byte path struct: char[256] + int err */
static void fill_path(const char *p)
{
	unsigned long i;

	for (i = 0; i < sizeof(sc_buf); i++)
		sc_buf[i] = 0;
	scopy(sc_buf, p, PATH_MAX_LEN);
}

int sc_main(long argc, char **argv)
{
	long err;
	long got;

	if (argc < 2)
		goto usage;

	if (sequal(argv[1], "show") && argc == 3 && sequal(argv[2], "version")) {
		for (err = 0; err < 20; err++)
			sc_buf[err] = 0;
		err = supercall(CMD_SUSFS_SHOW_VERSION, 20);
		if (err)
			return fail("show version", err);
		print_sc_string();
		return 0;
	}
	if (sequal(argv[1], "show") && argc == 3 && sequal(argv[2], "variant")) {
		for (err = 0; err < 20; err++)
			sc_buf[err] = 0;
		err = supercall(CMD_SUSFS_SHOW_VARIANT, 20);
		if (err)
			return fail("show variant", err);
		print_sc_string();
		return 0;
	}
	if (sequal(argv[1], "show") && argc == 3 && sequal(argv[2], "enabled_features")) {
		unsigned long i;

		for (i = 0; i < sizeof(sc_buf); i++)
			sc_buf[i] = 0;
		err = supercall(CMD_SUSFS_SHOW_ENABLED_FEATURES, FEATURES_SIZE + 4);
		if (err)
			return fail("show enabled_features", err);
		print_sc_string();
		return 0;
	}
	if (sequal(argv[1], "enable_log") && argc == 3) {
		if (join(argv, 2, 2))
			goto usage;
		got = proc_write("/proc/susfs_enable_log");
		return got < 0 ? fail("enable_log", got) : 0;
	}
	if (sequal(argv[1], "enable_avc_log_spoofing") && argc == 3) {
		if (join(argv, 2, 2))
			goto usage;
		got = proc_write("/proc/susfs_avc_spoof");
		return got < 0 ? fail("enable_avc_log_spoofing", got) : 0;
	}
	if (sequal(argv[1], "hide_sus_mnts_for_non_su_procs") && argc == 3) {
		sc_buf[0] = argv[2][0] - '0';
		sc_buf[1] = 0;
		err = supercall(CMD_SUSFS_HIDE_SUS_MNTS_FOR_NON_SU_PROCS, 8);
		return err ? fail("hide_sus_mnts_for_non_su_procs", err) : 0;
	}
	if (sequal(argv[1], "add_sus_path") && argc == 3) {
		fill_path(argv[2]);
		err = supercall(CMD_SUSFS_ADD_SUS_PATH, PATH_MAX_LEN + 4);
		return err ? fail("add_sus_path", err) : 0;
	}
	if (sequal(argv[1], "add_sus_path_loop") && argc == 3) {
		fill_path(argv[2]);
		err = supercall(CMD_SUSFS_ADD_SUS_PATH_LOOP, PATH_MAX_LEN + 4);
		return err ? fail("add_sus_path_loop", err) : 0;
	}
	if (sequal(argv[1], "add_sus_map") && argc == 3) {
		fill_path(argv[2]);
		err = supercall(CMD_SUSFS_ADD_SUS_MAP, PATH_MAX_LEN + 4);
		return err ? fail("add_sus_map", err) : 0;
	}
	if (sequal(argv[1], "add_sus_kstat") && argc == 3) {
		if (join(argv, 1, 2))
			goto usage;
		got = proc_write("/proc/susfs_kstat");
		return got < 0 ? fail("add_sus_kstat", got) : 0;
	}
	if (sequal(argv[1], "add_sus_kstat_statically") && argc == 15) {
		if (join(argv, 1, 14))
			goto usage;
		got = proc_write("/proc/susfs_kstat");
		return got < 0 ? fail("add_sus_kstat_statically", got) : 0;
	}
	if (sequal(argv[1], "update_sus_kstat") && argc == 3) {
		if (join(argv, 1, 2))
			goto usage;
		got = proc_write("/proc/susfs_kstat");
		return got < 0 ? fail("update_sus_kstat", got) : 0;
	}
	if (sequal(argv[1], "update_sus_kstat_full_clone") && argc == 3) {
		if (join(argv, 1, 2))
			goto usage;
		got = proc_write("/proc/susfs_kstat");
		return got < 0 ? fail("update_sus_kstat_full_clone", got) : 0;
	}
	if (sequal(argv[1], "add_open_redirect") && argc == 5) {
		if (join(argv, 1, 4))
			goto usage;
		got = proc_write("/proc/susfs_open_redirect");
		return got < 0 ? fail("add_open_redirect", got) : 0;
	}
	if (sequal(argv[1], "set_uname") && argc == 4) {
		unsigned long i;

		for (i = 0; i < 136; i++)
			sc_buf[i] = 0;
		scopy(sc_buf, argv[2], UNAME_FIELD);
		scopy(sc_buf + UNAME_FIELD, argv[3], UNAME_FIELD);
		err = supercall(CMD_SUSFS_SET_UNAME, 136);
		return err ? fail("set_uname", err) : 0;
	}
	if (sequal(argv[1], "set_cmdline_or_bootconfig") && argc == 3) {
		long fd = sys5(SYS_open, (long)argv[2], O_RDONLY, 0, 0, 0);
		unsigned long i;

		if (fd < 0)
			return fail("open file", fd);
		got = sys5(SYS_read, fd, (long)sc_buf, FEATURES_SIZE, 0, 0);
		sys5(SYS_close, fd, 0, 0, 0, 0);
		if (got < 0)
			return fail("read file", got);
		for (i = got; i < FEATURES_SIZE + 4; i++)
			sc_buf[i] = 0;
		err = supercall(CMD_SUSFS_SET_CMDLINE_OR_BOOTCONFIG, FEATURES_SIZE + 4);
		return err ? fail("set_cmdline_or_bootconfig", err) : 0;
	}

usage:
	say("usage: susfs show version|variant|enabled_features\n"
	    "       susfs enable_log <0|1>\n"
	    "       susfs enable_avc_log_spoofing <0|1>\n"
	    "       susfs hide_sus_mnts_for_non_su_procs <0|1>\n"
	    "       susfs add_sus_path <path>\n"
	    "       susfs add_sus_path_loop <path>\n"
	    "       susfs add_sus_map <path>\n"
	    "       susfs add_sus_kstat <path>\n"
	    "       susfs add_sus_kstat_statically <path> <12 values or default>\n"
	    "       susfs update_sus_kstat <path>\n"
	    "       susfs update_sus_kstat_full_clone <path>\n"
	    "       susfs add_open_redirect <target> <redirected> <uid_scheme 0-4>\n"
	    "       susfs set_uname <release> <version>\n"
	    "       susfs set_cmdline_or_bootconfig <file>\n");
	return 2;
}

__asm__(
".text\n"
".global _start\n"
".type _start,@function\n"
"_start:\n"
"	xor	%rbp, %rbp\n"
"	movq	(%rsp), %rdi\n"
"	leaq	8(%rsp), %rsi\n"
"	call	sc_main\n"
"	movl	%eax, %edi\n"
"	movl	$60, %eax\n"
"	syscall\n"
);
