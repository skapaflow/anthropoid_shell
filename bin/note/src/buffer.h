#ifndef NOTE_BUFFER_H
#define NOTE_BUFFER_H

#include <wchar.h>
#include <stddef.h>
#include <stdbool.h>

/* one line of text, UTF-16, without the line break */
typedef struct {
	wchar_t      *s;
	int           len, cap;
	unsigned char state;    /* syntax state at the start of the line (valid below BUFFER.valid) */
} LINE;

/* encodings: ANSI is the Windows code page, for files that are not valid UTF-8 */
enum { ENC_UTF8, ENC_ANSI, ENC_UTF16LE, ENC_UTF16BE };

/* the whole file: always at least one line */
typedef struct {
	LINE *lines;
	int   count, cap;
	int   valid;     /* lines whose syntax state is known */
	int   encoding;  /* ENC_*: read and written the same way */
	bool  bom;       /* UTF-8 with BOM (UTF-16 always has one) */
	bool  crlf;      /* line break of the file: CRLF or LF */
} BUFFER;

extern void buffer_init (BUFFER *);
extern void buffer_free (BUFFER *);
extern int  buffer_load (BUFFER *, const wchar_t *);
extern bool buffer_save (BUFFER *, const wchar_t *);
extern bool buffer_fits (BUFFER *);
extern const wchar_t *buffer_encoding_name (const BUFFER *);
extern void buffer_insert (BUFFER *, int, int, const wchar_t *, int, int *, int *);
extern void buffer_delete (BUFFER *, int, int, int, int);
extern wchar_t *buffer_text (BUFFER *, int, int, int, int, int *);
extern void text_end (int, int, const wchar_t *, int, int *, int *);
extern void *xrealloc (void *, size_t);

#endif
