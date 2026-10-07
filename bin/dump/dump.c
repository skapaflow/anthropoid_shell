#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <getopt.h>
#include <inttypes.h>

#define MAX_COLS 256
#define USAGE \
	"\n Usage:\n\tdump [-c columns] [-s jump] [-n range] file\n"

void fatal (const char *fmt, ...) {

	va_list ap;

	fflush(stdout);
	va_start(ap, fmt);
	fputs("\n ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(EXIT_FAILURE);
}

void usage (FILE *out, int status) {

	fputs(USAGE, out);
	exit(status);
}

uint64_t number (int opt, const char *s) {

	char *end;
	unsigned long long n;

	if (!isdigit((unsigned char) *s))
		fatal("invalid value for -%c: %s", opt, s);
	errno = 0;
	n = strtoull(s, &end, 0);
	if (*end || errno)
		fatal("invalid value for -%c: %s", opt, s);
	return n;
}

void print_line (uint64_t address, const unsigned char *buff, size_t n, size_t cols) {

	size_t i;

	printf("%08" PRIx64 "  ", address);
	for (i = 0; i < cols; i++) {
		if (i < n)
			printf("%02x ", buff[i]);
		else
			fputs("   ", stdout);
		if (i + 1 == cols / 2)
			putchar(' ');
	}
	fputs(" |", stdout);
	for (i = 0; i < n; i++)
		putchar(isprint(buff[i]) ? buff[i] : '.');
	puts("|");
}

int main (int argc, char *argv[]) {

	FILE *file = NULL;
	unsigned char buff[MAX_COLS];

	int c;
	size_t n, want;
	size_t cols = 16;
	uint64_t cols_arg;
	uint64_t skip = 0;
	uint64_t length = 0;
	uint64_t done = 0;

	while ((c = getopt(argc, argv, "c:s:n:h")) != -1) {

		switch (c) {

			case 'c':
				cols_arg = number(c, optarg);
				if (cols_arg < 1 || cols_arg > MAX_COLS)
					fatal("columns must be between 1 and %d", MAX_COLS);
				cols = (size_t) cols_arg;
				break;
			case 's': skip = number(c, optarg); break;
			case 'n': length = number(c, optarg); break;
			case 'h': usage(stdout, EXIT_SUCCESS); break;
			default: usage(stderr, EXIT_FAILURE);
		}
	}

	if (optind != argc - 1)
		usage(stderr, EXIT_FAILURE);

	if (skip > INT64_MAX)
		fatal("offset too large");

	if (!(file = fopen(argv[optind], "rb")))
		fatal("%s: file not found or not readable", argv[optind]);

	if (_fseeki64(file, (long long) skip, SEEK_SET))
		fatal("unable to seek through file");

	setvbuf(stdout, NULL, _IOFBF, 1 << 16);

	while (!length || done < length) {

		want = cols;
		if (length && length - done < want)
			want = (size_t) (length - done);

		n = fread(buff, 1, want, file);
		if (n)
			print_line(skip + done, buff, n, cols);
		done += n;

		if (n < want)
			break;
	}

	if (ferror(file))
		fatal("error reading file");

	fclose(file);

	return 0;
}