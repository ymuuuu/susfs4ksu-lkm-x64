// SPDX-License-Identifier: GPL-2.0
/*
 * susfs_sc_x64 - x86_64 port of tools/susfs_sc.c, same contract:
 *
 *     susfs_sc_x64 <cmd-hex> [payload-hex]
 *
 * sends reboot(KSU_INSTALL_MAGIC1, SUSFS_MAGIC, cmd, payload) through the
 * raw syscall interface and reports the return value plus the `err` field
 * the kernel writes back at the end of the payload struct.  After the call
 * it also dumps the whole payload as hex, which is how the string answers
 * (CMD_SUSFS_SHOW_VERSION, SHOW_VARIANT, SHOW_ENABLED_FEATURES) become
 * readable without guessing endianness or offsets.
 *
 * Freestanding like the arm64 original: Android refuses non-PIE
 * executables and there is no bionic sysroot in the build container, so
 * _start and the syscall wrappers are hand written.  Differences from the
 * arm64 file are exactly the ones the ABI forces: syscall numbers
 * (write 1, exit 60, reboot 169), register binding (rax/rdi/rsi/rdx/r10),
 * the syscall instruction, and the stack layout at process entry
 * (argc at [rsp], argv right above it).
 *
 * Build (in the DDK container, same clang that builds the module):
 *
 *     clang --target=x86_64-linux-gnu -O2 -nostdlib -static-pie \
 *           -fno-stack-protector -fuse-ld=lld -Wl,-e,_start \
 *           -o susfs_sc_x64 tools/susfs_sc_x64.c
 */

typedef unsigned long u64;
typedef long s64;

#define SYS_write 1
#define SYS_exit 60
#define SYS_reboot 169

#define KSU_INSTALL_MAGIC1 0xDEADBEEF
#define SUSFS_MAGIC 0xFAFAFAFA

/* Largest payload struct in the ABI (st_susfs_spoof_cmdline_or_bootconfig
 * and st_susfs_enabled_features are 8196). */
#define PAYLOAD_MAX 8196

/* Reply structs end with `int err` as the LAST field; seeded with 126
 * (ERR_CMD_NOT_SUPPORTED) so an untouched buffer means "not supported". */
#define ERR_SEED 126

static char payload[PAYLOAD_MAX] __attribute__((aligned(16)));
static char out[512];

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

static void puts_len(const char *s, unsigned long n)
{
	sys5(SYS_write, 1, (long)s, (long)n, 0, 0);
}

static void say(const char *s)
{
	puts_len(s, slen(s));
}

static void say_hex(unsigned long v)
{
	static const char d[] = "0123456789abcdef";
	char *p = out;
	int i;

	p[0] = '0';
	p[1] = 'x';
	p += 2;
	for (i = 60; i >= 0; i -= 4)
		*p++ = d[(v >> i) & 0xf];
	*p++ = '\n';
	puts_len(out, p - out);
}

static void say_dec(long v)
{
	char tmp[24];
	char *p = out;
	int n = 0;
	unsigned long u;

	if (v < 0) {
		*p++ = '-';
		u = (unsigned long)(-v);
	} else {
		u = (unsigned long)v;
	}
	if (!u) {
		tmp[n++] = '0';
	}
	while (u) {
		tmp[n++] = (char)('0' + (u % 10));
		u /= 10;
	}
	while (n)
		*p++ = tmp[--n];
	puts_len(out, p - out);
}

static void dump_payload(long len)
{
	static const char d[] = "0123456789abcdef";
	char *p = out;
	long i;

	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)payload[i];

		if (i && (i & 15) == 0) {
			*p++ = '\n';
			puts_len(out, p - out);
			p = out;
		}
		*p++ = d[c >> 4];
		*p++ = d[c & 0xf];
		if ((i & 15) == 15)
			*p++ = ' ';
	}
	if (p != out) {
		*p++ = '\n';
		puts_len(out, p - out);
	}
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static long parse_hex(const char *s, char *dst, long cap)
{
	long n = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	while (s[0] && s[1] && n < cap) {
		int hi = hexval(s[0]);
		int lo = hexval(s[1]);

		if (hi < 0 || lo < 0)
			return -1;
		dst[n++] = (char)((hi << 4) | lo);
		s += 2;
	}
	if (s[0] == ' ' || s[0] == '\n')
		return n;
	if (s[0])
		return -1;
	return n;
}

static long parse_cmd(const char *s)
{
	long v = 0;
	int i;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	for (i = 0; s[i]; i++) {
		int d = hexval(s[i]);

		if (d < 0)
			return -1;
		v = (v << 4) | d;
	}
	return v;
}

long sc_main(long argc, char **argv)
{
	long cmd, len;
	long rc;
	int err;

	if (argc < 2) {
		say("usage: susfs_sc_x64 <cmd-hex> [payload-hex]\n");
		return 2;
	}
	cmd = parse_cmd(argv[1]);
	if (cmd < 0) {
		say("susfs_sc_x64: bad command number\n");
		return 2;
	}

	len = 0;
	if (argc > 2) {
		len = parse_hex(argv[2], payload, PAYLOAD_MAX);
		if (len < 0) {
			say("susfs_sc_x64: bad or oversized payload hex\n");
			return 2;
		}
	}

	if (len >= 4)
		*(int *)(payload + len - 4) = ERR_SEED;

	say("susfs_sc_x64: cmd ");
	say_hex((unsigned long)cmd);
	rc = sys5(SYS_reboot, KSU_INSTALL_MAGIC1, SUSFS_MAGIC, cmd,
		  (long)payload, 0);
	say("susfs_sc_x64: reboot() returned ");
	say_hex((unsigned long)rc);
	err = (len >= 4) ? *(int *)(payload + len - 4) : 0;
	say("susfs_sc_x64: err=");
	say_dec(err);
	say(" (");
	say_hex((unsigned long)(long)err);
	say(")\n");
	if (len >= 4 && err == ERR_SEED)
		say("susfs_sc_x64: err is still the 126 sentinel: this kernel did not answer that command\n");
	if (len > 0) {
		say("susfs_sc_x64: payload after call:\n");
		dump_payload(len);
	}
	return (rc == 0 && err == 0) ? 0 : 1;
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
