/*
 * brick - a falling-blocks game for the Windows console.
 *
 *   A / Left   move left        S / Down   soft drop
 *   D / Right  move right       Space      hard drop
 *   W / Up     rotate           P          pause
 *   Esc        quit
 *
 * The game runs on a console screen buffer of its own, so the terminal comes
 * back untouched (scrollback included) when it exits. The high score is kept
 * in brick.dat, next to brick.exe.
 *
 * The file is organized in three layers: the game rules (pieces, field,
 * scoring), the drawing (a frame buffer written to the console in one call)
 * and the screens/main loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <wchar.h>
#include <ctype.h>
#include <time.h>
#include <conio.h>
#include <windows.h>

#define FIELD_W          11     /* playable columns */
#define FIELD_H          20     /* playable rows */
#define NUM_PIECES        7
#define FALL_MS_START   500     /* gravity interval at level 1 */
#define FALL_MS_STEP     40     /* faster by this much per level */
#define FALL_MS_MIN      80
#define LINES_PER_LEVEL  10
#define FLASH_FRAMES      6     /* line-clear animation */
#define FLASH_MS         60
#define FRAME_MS         16
#define SAVE_FILE       L"brick.dat"

#define SCREEN_W         60
#define SCREEN_H         24
#define PANEL_X          ((FIELD_W + 2) * 2 + 3)    /* first column of the side panel */

#define ATTR_NORMAL      (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define ATTR_BRIGHT      (ATTR_NORMAL | FOREGROUND_INTENSITY)
#define ATTR_DIM         FOREGROUND_INTENSITY

/* ================================================================== */
/* Game rules                                                         */
/* ================================================================== */

/* a shape in its own size x size box, which is also the box it rotates in */
typedef struct {
	int size;
	int cells[4][4];
} Shape;

static const Shape SHAPES[NUM_PIECES] = {
	{ 4, { {0,0,0,0}, {1,1,1,1}, {0,0,0,0}, {0,0,0,0} } },    /* I */
	{ 2, { {1,1}, {1,1} } },                                   /* O */
	{ 3, { {0,1,0}, {1,1,1}, {0,0,0} } },                      /* T */
	{ 3, { {0,1,1}, {1,1,0}, {0,0,0} } },                      /* S */
	{ 3, { {1,1,0}, {0,1,1}, {0,0,0} } },                      /* Z */
	{ 3, { {1,0,0}, {1,1,1}, {0,0,0} } },                      /* J */
	{ 3, { {0,0,1}, {1,1,1}, {0,0,0} } }                       /* L */
};

/* a piece on the field: x, y is the top-left corner of its box (y may be negative) */
typedef struct {
	int size;
	int cells[4][4];
	int x, y;
} Piece;

typedef struct {
	int   field[FIELD_H][FIELD_W];      /* 1 = locked block */
	Piece cur, next;
	int   score, lines, level;
	int   fall_ms;
	int   paused;
} Game;

/* puts the shape in its spawn position: centered, topmost block on row 0 */
static void piece_init (Piece *p, int shape) {

	int top = 0;

	p->size = SHAPES[shape].size;
	memcpy(p->cells, SHAPES[shape].cells, sizeof p->cells);

	while (top < p->size) {
		int used = 0;
		for (int x = 0; x < p->size; x++)
			used |= p->cells[top][x];
		if (used)
			break;
		top++;
	}
	p->x = (FIELD_W - p->size) / 2;
	p->y = -top;
}

/* the piece turned 90 degrees clockwise inside its own box */
static Piece piece_rotated (const Piece *p) {

	Piece r = *p;

	memset(r.cells, 0, sizeof r.cells);
	for (int y = 0; y < p->size; y++)
		for (int x = 0; x < p->size; x++)
			r.cells[x][p->size - 1 - y] = p->cells[y][x];
	return r;
}

/* is there a block of the piece at (x, y) inside its box? */
static int piece_has (const Piece *p, int x, int y) {

	return x >= 0 && x < p->size && y >= 0 && y < p->size && p->cells[y][x];
}

/* does the piece fit? Blocks above the field are allowed, walls and the floor are not */
static int fits (const Game *g, const Piece *p) {

	for (int y = 0; y < p->size; y++) {
		for (int x = 0; x < p->size; x++) {
			if (!p->cells[y][x])
				continue;
			int fx = p->x + x;
			int fy = p->y + y;
			if (fx < 0 || fx >= FIELD_W || fy >= FIELD_H)
				return 0;
			if (fy >= 0 && g->field[fy][fx])
				return 0;
		}
	}
	return 1;
}

static int try_move (Game *g, int dx, int dy) {

	Piece moved = g->cur;

	moved.x += dx;
	moved.y += dy;
	if (!fits(g, &moved))
		return 0;
	g->cur = moved;
	return 1;
}

/* rotates, sliding sideways a little if the plain rotation does not fit */
static int try_rotate (Game *g) {

	static const int KICKS[] = { 0, -1, 1, -2, 2 };
	Piece r = piece_rotated(&g->cur);

	for (int i = 0; i < (int) (sizeof KICKS / sizeof *KICKS); i++) {
		Piece t = r;
		t.x += KICKS[i];
		if (fits(g, &t)) {
			g->cur = t;
			return 1;
		}
	}
	return 0;
}

/* moves the piece to the floor; returns the rows it fell */
static int hard_drop (Game *g) {

	int rows = 0;

	while (try_move(g, 0, 1))
		rows++;
	return rows;
}

/* the row the current piece would land on (its y), for the ghost */
static int landing_y (const Game *g) {

	Piece p = g->cur;

	for (;;) {
		p.y++;
		if (!fits(g, &p))
			return p.y - 1;
	}
}

/* next becomes the current piece and a new next is drawn; 0 if the new piece does not fit */
static int spawn (Game *g) {

	g->cur = g->next;
	piece_init(&g->next, rand() % NUM_PIECES);
	return fits(g, &g->cur);
}

static void new_game (Game *g) {

	memset(g, 0, sizeof *g);
	g->level   = 1;
	g->fall_ms = FALL_MS_START;
	piece_init(&g->next, rand() % NUM_PIECES);
	spawn(g);
}

/* writes the current piece into the field; 1 if some block ended above the field (game over) */
static int lock_piece (Game *g) {

	int overflow = 0;

	for (int y = 0; y < g->cur.size; y++) {
		for (int x = 0; x < g->cur.size; x++) {
			if (!g->cur.cells[y][x])
				continue;
			int fy = g->cur.y + y;
			if (fy < 0)
				overflow = 1;
			else
				g->field[fy][g->cur.x + x] = 1;
		}
	}
	return overflow;
}

/* the full rows, from top to bottom; returns how many */
static int full_rows (const Game *g, int rows[FIELD_H]) {

	int n = 0;

	for (int y = 0; y < FIELD_H; y++) {
		int x = 0;
		while (x < FIELD_W && g->field[y][x])
			x++;
		if (x == FIELD_W)
			rows[n++] = y;
	}
	return n;
}

/* removes the rows (top to bottom, as full_rows returns them), dropping what is above */
static void remove_rows (Game *g, const int *rows, int n) {

	for (int i = 0; i < n; i++) {
		memmove(g->field[1], g->field[0], rows[i] * sizeof g->field[0]);
		memset(g->field[0], 0, sizeof g->field[0]);
	}
}

/* scoring: 100/300/500/800 for 1-4 lines times the level, plus 2 per row of a hard drop */
static void award (Game *g, int cleared, int dropped) {

	static const int POINTS[5] = { 0, 100, 300, 500, 800 };

	g->score  += POINTS[cleared] * g->level + dropped * 2;
	g->lines  += cleared;
	g->level   = g->lines / LINES_PER_LEVEL + 1;
	g->fall_ms = FALL_MS_START - (g->level - 1) * FALL_MS_STEP;
	if (g->fall_ms < FALL_MS_MIN)
		g->fall_ms = FALL_MS_MIN;
}

/* ================================================================== */
/* Console output                                                     */
/* ================================================================== */

static HANDLE    con_out;                   /* the game's own screen buffer */
static HANDLE    con_saved;                 /* the buffer that was active before */
static CHAR_INFO screen[SCREEN_H][SCREEN_W];    /* the frame, written to the console in one call */

static BOOL WINAPI on_ctrl (DWORD type) {

	(void) type;
	SetConsoleActiveScreenBuffer(con_saved);
	return FALSE;                           /* let the default handler end the process */
}

static int screen_open (void) {

	CONSOLE_CURSOR_INFO ci = { 1, FALSE };

	con_saved = GetStdHandle(STD_OUTPUT_HANDLE);
	con_out = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
	if (con_out == INVALID_HANDLE_VALUE)
		return 0;
	SetConsoleCursorInfo(con_out, &ci);
	SetConsoleActiveScreenBuffer(con_out);
	SetConsoleCtrlHandler(on_ctrl, TRUE);
	return 1;
}

static void screen_close (void) {

	SetConsoleActiveScreenBuffer(con_saved);
	CloseHandle(con_out);
}

static void screen_clear (void) {

	for (int y = 0; y < SCREEN_H; y++) {
		for (int x = 0; x < SCREEN_W; x++) {
			screen[y][x].Char.UnicodeChar = L' ';
			screen[y][x].Attributes = ATTR_NORMAL;
		}
	}
}

static void present (void) {

	COORD size = { SCREEN_W, SCREEN_H };
	COORD origin = { 0, 0 };
	SMALL_RECT area = { 0, 0, SCREEN_W - 1, SCREEN_H - 1 };

	WriteConsoleOutputW(con_out, &screen[0][0], size, origin, &area);
}

static void put_char (int x, int y, WORD attr, wchar_t c) {

	if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H)
		return;
	screen[y][x].Char.UnicodeChar = c;
	screen[y][x].Attributes = attr;
}

static void put (int x, int y, WORD attr, const wchar_t *text) {

	for (; *text; text++)
		put_char(x++, y, attr, *text);
}

static void putf (int x, int y, WORD attr, const wchar_t *fmt, ...) {

	wchar_t buf[SCREEN_W + 1];
	va_list ap;

	va_start(ap, fmt);
	vswprintf(buf, SCREEN_W + 1, fmt, ap);
	va_end(ap);
	put(x, y, attr, buf);
}

/* plain ASCII text */
static void put_ascii (int x, int y, WORD attr, const char *text) {

	for (; *text; text++)
		put_char(x++, y, attr, (wchar_t) *text);
}

/* big-letter art: '#' becomes a solid block, anything else a space */
static void put_art (int x, int y, WORD attr, const char *row) {

	for (; *row; row++)
		put_char(x++, y, attr, *row == '#' ? L'█' : L' ');
}

/* a block is two screen columns wide */
static void put_cell (int sx, int sy, WORD attr, wchar_t c) {

	put_char(sx, sy, attr, c);
	put_char(sx + 1, sy, attr, c);
}

/*
 * The field with its walls. blink_mask is a bit mask of rows to blink during
 * the line-clear animation (flash_on: the rows are lit this frame).
 */
static void draw_field (const Game *g, unsigned blink_mask, int flash_on) {

	int ghost_y = landing_y(g);

	for (int y = 0; y < FIELD_H; y++) {
		put_cell(0, y, ATTR_NORMAL, L'█');
		put_cell((FIELD_W + 1) * 2, y, ATTR_NORMAL, L'█');

		for (int x = 0; x < FIELD_W; x++) {
			wchar_t c = g->field[y][x] ? L'█' : L' ';
			WORD attr = ATTR_NORMAL;

			if (blink_mask & (1u << y)) {
				c = flash_on ? L'░' : L' ';
			} else if (piece_has(&g->cur, x - g->cur.x, y - g->cur.y)) {
				c = L'█';
				attr = ATTR_BRIGHT;
			} else if (!g->field[y][x] && piece_has(&g->cur, x - g->cur.x, y - ghost_y)) {
				c = L'░';
				attr = ATTR_DIM;
			}
			put_cell((x + 1) * 2, y, attr, c);
		}
	}
	for (int x = 0; x < FIELD_W + 2; x++)
		put_cell(x * 2, FIELD_H, ATTR_NORMAL, L'█');
}

static void draw_panel (const Game *g, int highscore) {

	put_ascii(PANEL_X, 1, ATTR_BRIGHT, "[ BRICK ]");
	putf(PANEL_X, 2, ATTR_NORMAL, L"Record: %d", highscore);
	putf(PANEL_X, 3, ATTR_NORMAL, L" Score: %d", g->score);
	putf(PANEL_X, 4, ATTR_NORMAL, L" Level: %d", g->level);
	putf(PANEL_X, 5, ATTR_NORMAL, L" Lines: %d", g->lines);

	put_ascii(PANEL_X, 7, ATTR_NORMAL, "NEXT:");
	for (int y = 0; y < g->next.size; y++)
		for (int x = 0; x < g->next.size; x++)
			if (g->next.cells[y][x])
				put_cell(PANEL_X + x * 2, 8 + y, ATTR_NORMAL, L'█');

	put_ascii(PANEL_X, 13, ATTR_NORMAL, "Controls:");
	put_ascii(PANEL_X, 14, ATTR_NORMAL, "A D   move");
	put_ascii(PANEL_X, 15, ATTR_NORMAL, "S     down");
	put_ascii(PANEL_X, 16, ATTR_NORMAL, "W     rotate");
	put_ascii(PANEL_X, 17, ATTR_NORMAL, "SPC   drop");
	put_ascii(PANEL_X, 18, ATTR_NORMAL, "P     pause");
	put_ascii(PANEL_X, 19, ATTR_NORMAL, "ESC   exit");

	if (g->paused)
		put_ascii(PANEL_X, 21, ATTR_BRIGHT, "** PAUSED **");
}

static void draw_game (const Game *g, int highscore, unsigned blink_mask, int flash_on) {

	screen_clear();
	draw_field(g, blink_mask, flash_on);
	draw_panel(g, highscore);
	present();
}

/* ================================================================== */
/* High score                                                         */
/* ================================================================== */

/* brick.dat in the folder of the executable, whatever the current folder is */
static int save_path (wchar_t *path, DWORD size) {

	DWORD n = GetModuleFileNameW(NULL, path, size);
	wchar_t *slash;

	if (n == 0 || n >= size)
		return 0;
	slash = wcsrchr(path, L'\\');
	if (!slash || (size_t) (slash - path) + 1 + wcslen(SAVE_FILE) >= size)
		return 0;
	wcscpy(slash + 1, SAVE_FILE);
	return 1;
}

static int load_highscore (void) {

	wchar_t path[MAX_PATH];
	int score = 0;
	FILE *f;

	if (!save_path(path, MAX_PATH) || !(f = _wfopen(path, L"rb")))
		return 0;
	if (fread(&score, sizeof score, 1, f) != 1 || score < 0)
		score = 0;
	fclose(f);
	return score;
}

static void save_highscore (int score) {

	wchar_t path[MAX_PATH];
	FILE *f;

	if (!save_path(path, MAX_PATH) || !(f = _wfopen(path, L"wb")))
		return;
	fwrite(&score, sizeof score, 1, f);
	fclose(f);
}

/* ================================================================== */
/* Keyboard                                                           */
/* ================================================================== */

typedef enum { ACT_NONE, ACT_LEFT, ACT_RIGHT, ACT_DOWN, ACT_ROTATE, ACT_DROP, ACT_PAUSE, ACT_QUIT } Action;

/* the next pending key as an action, without waiting */
static Action read_action (void) {

	int ch;

	if (!_kbhit())
		return ACT_NONE;
	ch = _getch();
	if (ch == 0 || ch == 0xE0) {                /* arrow keys come as a prefix plus a scan code */
		switch (_getch()) {
		case 75: return ACT_LEFT;
		case 77: return ACT_RIGHT;
		case 80: return ACT_DOWN;
		case 72: return ACT_ROTATE;
		}
		return ACT_NONE;
	}
	switch (tolower(ch)) {
	case 'a': return ACT_LEFT;
	case 'd': return ACT_RIGHT;
	case 's': return ACT_DOWN;
	case 'w': return ACT_ROTATE;
	case ' ': return ACT_DROP;
	case 'p': return ACT_PAUSE;
	case 27:  return ACT_QUIT;
	}
	return ACT_NONE;
}

/* waits for a key; 0 if it was Esc, 1 for any other */
static int wait_key (void) {

	while (_kbhit())
		_getch();
	for (;;) {
		if (_kbhit())
			return _getch() != 27;
		Sleep(50);
	}
}

/* ================================================================== */
/* Playing                                                            */
/* ================================================================== */

/* blinks the rows that are about to disappear */
static void blink_rows (const Game *g, int highscore, const int *rows, int n) {

	unsigned mask = 0;

	for (int i = 0; i < n; i++)
		mask |= 1u << rows[i];
	for (int f = 0; f < FLASH_FRAMES; f++) {
		draw_game(g, highscore, mask, f % 2 == 0);
		Sleep(FLASH_MS);
	}
}

/* locks the piece, clears and scores the lines, brings the next piece; 0 on game over */
static int settle (Game *g, int highscore, int dropped) {

	int rows[FIELD_H];
	int overflow = lock_piece(g);
	int n = full_rows(g, rows);

	if (n > 0) {
		blink_rows(g, highscore, rows, n);
		remove_rows(g, rows, n);
	}
	award(g, n, dropped);
	return !overflow && spawn(g);
}

/* one game, until it ends or the player quits */
static void play (Game *g, int highscore) {

	DWORD last_fall = GetTickCount();

	new_game(g);
	while (_kbhit())
		_getch();

	for (;;) {
		Action a;

		while ((a = read_action()) != ACT_NONE) {
			if (a == ACT_QUIT)
				return;
			if (a == ACT_PAUSE) {
				g->paused = !g->paused;
				last_fall = GetTickCount();     /* no free fall after resuming */
				continue;
			}
			if (g->paused)
				continue;

			switch (a) {
			case ACT_LEFT:   try_move(g, -1, 0); break;
			case ACT_RIGHT:  try_move(g, 1, 0);  break;
			case ACT_DOWN:   try_move(g, 0, 1);  break;
			case ACT_ROTATE: try_rotate(g);      break;
			case ACT_DROP: {
				int dropped = hard_drop(g);
				if (!settle(g, highscore, dropped))
					return;
				last_fall = GetTickCount();
				break;
			}
			default: break;
			}
		}

		if (!g->paused && GetTickCount() - last_fall >= (DWORD) g->fall_ms) {
			if (!try_move(g, 0, 1) && !settle(g, highscore, 0))
				return;
			last_fall = GetTickCount();
		}

		draw_game(g, highscore, 0, 0);
		Sleep(FRAME_MS);
	}
}

/* ================================================================== */
/* Screens                                                            */
/* ================================================================== */

/* 0 if the player chose to quit */
static int title_screen (int highscore) {

	const int x = 6;

	screen_clear();
	put_ascii(x, 2, ATTR_BRIGHT, " ____  ____  ___ ____ _  __");
	put_ascii(x, 3, ATTR_BRIGHT, "| __ )|  _ \\|_ _/ ___| |/ /");
	put_ascii(x, 4, ATTR_BRIGHT, "|  _ \\| |_) || | |   | ' / ");
	put_ascii(x, 5, ATTR_BRIGHT, "| |_) |  _ < | | |___| . \\ ");
	put_ascii(x, 6, ATTR_BRIGHT, "|____/|_| \\_\\___\\____|_|\\_\\");

	putf(x + 2, 9, ATTR_NORMAL, L"Last Record : %d", highscore);

	put_ascii(x + 2, 11, ATTR_NORMAL, "Controls:");
	put_ascii(x + 2, 12, ATTR_NORMAL, "  A / Left key   - move left");
	put_ascii(x + 2, 13, ATTR_NORMAL, "  D / Right key  - move right");
	put_ascii(x + 2, 14, ATTR_NORMAL, "  S / Down key   - move down");
	put_ascii(x + 2, 15, ATTR_NORMAL, "  W / Up key     - rotate");
	put_ascii(x + 2, 16, ATTR_NORMAL, "  Space          - drop");
	put_ascii(x + 2, 17, ATTR_NORMAL, "  P              - pause");
	put_ascii(x + 2, 18, ATTR_NORMAL, "  ESC            - exit");

	put_ascii(x + 4, 20, ATTR_BRIGHT, "Any key to start!");
	present();
	return wait_key();
}

/* 5-row letters for the game-over box */
static const char *const WORD_GAME[5] = {
	" ##   ##  #   # ####",
	"#    #  # ## ## #   ",
	"# ## #### # # # ### ",
	"#  # #  # #   # #   ",
	" ##  #  # #   # ####"
};
static const char *const WORD_OVER[5] = {
	" ##  #   # #### ### ",
	"#  # #   # #    #  #",
	"#  # #   # ###  ### ",
	"#  #  # #  #    # # ",
	" ##    #   #### #  #"
};

/* shows the final numbers over the field; 0 if the player chose to quit */
static int game_over_screen (const Game *g, int highscore, int new_record) {

	const int left = 2;                         /* the box sits between the field walls */
	const int width = FIELD_W * 2;              /* including its side bars */
	const int height = 1 + 5 + 1 + 5 + 1 + 4 + (new_record ? 1 : 0) + 1;
	const int top = (FIELD_H + 1 - height) / 2;
	int y = top;

	/* the field stays visible around the box */
	screen_clear();
	draw_field(g, 0, 0);

	for (int row = 0; row < height; row++) {
		int edge = (row == 0 || row == height - 1);

		put_char(left, top + row, ATTR_NORMAL, edge ? L'+' : L'|');
		for (int x = 1; x < width - 1; x++)
			put_char(left + x, top + row, ATTR_NORMAL, edge ? L'-' : L' ');
		put_char(left + width - 1, top + row, ATTR_NORMAL, edge ? L'+' : L'|');
	}

	y++;
	for (int i = 0; i < 5; i++)
		put_art(left + 1, y++, ATTR_BRIGHT, WORD_GAME[i]);
	y++;
	for (int i = 0; i < 5; i++)
		put_art(left + 1, y++, ATTR_BRIGHT, WORD_OVER[i]);
	y++;
	putf(left + 2, y++, ATTR_NORMAL, L"Score  : %d", g->score);
	putf(left + 2, y++, ATTR_NORMAL, L"Level  : %d", g->level);
	putf(left + 2, y++, ATTR_NORMAL, L"Lines  : %d", g->lines);
	putf(left + 2, y++, ATTR_NORMAL, L"Record : %d", highscore);
	if (new_record)
		put_ascii(left + 3, y, ATTR_BRIGHT, "== NEW RECORD ==");

	present();
	return wait_key();
}

int main (void) {

	int highscore;

	if (!screen_open()) {
		fputs("brick: this game needs a console window\n", stderr);
		return 1;
	}
	srand((unsigned) time(NULL));
	highscore = load_highscore();

	while (title_screen(highscore)) {
		Game g;
		int new_record;

		play(&g, highscore);
		new_record = g.score > highscore;
		if (new_record) {
			highscore = g.score;
			save_highscore(highscore);
		}
		if (!game_over_screen(&g, highscore, new_record))
			break;
	}

	screen_close();
	printf("Record: %d\n", highscore);
	return 0;
}
