#include <time.h>
#include <math.h>
#include <stdio.h>
#include <stdbool.h>
#include <windows.h>

typedef struct {float x, y;} vec2f;

typedef struct {
	int lot;
	int step;
	vec2f v[256];
} VEC_TAIL;

HANDLE hconsole;
char *ascii_buffer = NULL;
int screen_w, screen_h;

const int width  = 30;
const int height = 20;

/* ball */
vec2f ball_pos   = {width / 2, height / 2};
vec2f ball_dir   = {-1.0f, 0.0f};
float ball_speed = 0.1f;

/* rackets in general */
int  score_left = 0;
int score_right = 0;
const float  racket_width = 1;
const float racket_height = 4;
float racket_speed = 0.3f;
vec2f  racket_left = {2,8};
vec2f racket_right = {width-2, 8};

/* Clamp float value */
float clamp (float value, float min, float max) {

	return __min(max, __max(value, min));
}

/* Calculate linear interpolation between two floats */
float lerp (float start, float end, float time) {

	return start + time*(end - start);
}

/* normalize input value within input range, normalize(50, 0, 100) == 0.500000 */
float normalize (float value, float start, float end) {

	return (value - start)/(end - start);
}

void vec2_norm (float *x, float *y) {
	/* sets a vectors length to 1 (which means that x + y == 1) */
	float length = sqrt((*x * *x) + (*y * *y));
	if (length != 0.0f) {
		length = 1.0f / length;
		*x *= length;
		*y *= length;
	}
}

void graph_bar (char *bar, float value, int max, int size, const char *glyph) {

	for (int i = 0; i < size; i++)
		bar[i] = (i < (value/max)*size ? glyph[0] : glyph[1]);
}

void hide_cursor (void) {

	CONSOLE_CURSOR_INFO ci = {1, FALSE};
	SetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci);
}

void init (void) {

	/* get terminal size */
	CONSOLE_SCREEN_BUFFER_INFO info;
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
	screen_w = info.dwMaximumWindowSize.X;
	screen_h = info.srWindow.Bottom;

	ascii_buffer = (char *) malloc(sizeof(char) * screen_w * screen_h);

	srand(time(NULL));
	hconsole = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, 0, NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
	SetConsoleActiveScreenBuffer(hconsole);
	for (int i = 0; i < (screen_w * screen_h); i++) ascii_buffer[i] = 32;
}

void render (void) {

	COORD cxy = {0,0};
	DWORD dwbyteswritten = 0;
	WriteConsoleOutputCharacter(hconsole, ascii_buffer, screen_w * screen_h, cxy, &dwbyteswritten);
	for (int i = 0; i < (screen_w * screen_h); i++) ascii_buffer[i] = 32;
}

void pixel (int x, int y, int type) {

	if ((x >= 0 && y >= 0) && (x < screen_w && y < screen_h))
		ascii_buffer[x + (y * screen_w)] = type;
}

void line (int x0, int y0, int x1, int y1, int type) {

	int dx  = abs(x1 - x0);
	int dy  = abs(y1 - y0);
	int sx  = (x0 < x1 ? 1 : -1);
	int sy  = (y0 < y1 ? 1 : -1);
	int err = dx - dy;
	for (;;) {
		pixel(x0, y0, type);
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * err;
		if (e2 > -dy) {err -= dy; x0 += sx;}
		if (e2 < dx) {err += dx; y0 += sy;}
	}
}

void rect (int x, int y, int x2, int y2, int type) {

	/* horizontal */
	for (int i = 0; i < x2-x; i++) {
		pixel(x+i, y, type);
		pixel(x+i, y2, type);
	}
	/* vertical */
	for (int i = 0; i <= y2-y; i++) {
		pixel(x, y+i, type);
		pixel(x2, y+i, type);
	}
}

void vector_tail (VEC_TAIL *vt, vec2f pos, int size, float time, const char *glyph) {

	/* limite do vetor em VEC_TAIL */
	size = __min(size, (sizeof(vt->v) / sizeof((vt->v)[0])));

	/* draw */
	for (int i = 0; i < (vt->lot - 1); i++) {

		if (vt->v[i   % vt->lot].x > 0 &&
			vt->v[i   % vt->lot].y > 0 &&
			vt->v[i+1 % vt->lot].x > 0 &&
			vt->v[i+1 % vt->lot].y > 0) {
			vec2f v1 = {((int)vt->v[i   % vt->lot].x), ((int)vt->v[i   % vt->lot].y)};
			vec2f v2 = {((int)vt->v[i+1 % vt->lot].x), ((int)vt->v[i+1 % vt->lot].y)};
			int type = 2;
			if (i < vt->lot/3) type = 0;
			if (i >= vt->lot/3 && i <= vt->lot-(vt->lot/3)) type = 1;
			line(v1.x, v1.y, v2.x, v2.y, glyph[type]);
		}
	}

	/* add */
	if (vt->step++ > time) {
		vt->step = 0;
		/* expand list */
		vt->lot += (vt->lot < size ? 1 : -1);
		vt->lot = __max(vt->lot, 0);
		/* Move the elements to the right */
		for (int j = (vt->lot - 1); j > 0; j--)
			vt->v[j] = vt->v[j - 1];
		/* adicionar no inicio */
		if (vt->lot <= size) {
			vt->v[0].x = pos.x;
			vt->v[0].y = pos.y;
		} else {
			vt->v[0].x = 0;
			vt->v[0].y = 0;
		}
	}
}

int print (int x, int y, const char *fmt, ...) {

	va_list ap;
	char buf[1024];
	va_start(ap, fmt);
	int n = vsprintf(buf, fmt, ap);
	va_end(ap);
	for (int i = x, j = 0; j < n; i++, j++)
		if ((x >= 0 && y >= 0) && (x < screen_w && y < screen_h))
			pixel(i, y, buf[j]);
	return n;
}

int key (int k) {

	return (GetAsyncKeyState(k) & 0x8000);
}

void update_ball (void) {

	/* fly a bit */
	ball_pos.x += ball_dir.x * ball_speed;
	ball_pos.y += ball_dir.y * ball_speed;

	/* hit by left racket? */
	if (ball_pos.x > racket_left.x &&
		ball_pos.x < racket_left.x + racket_width &&
		ball_pos.y < racket_left.y + racket_height &&
		(int)ball_pos.y >= (int)racket_left.y) {
		// set fly direction depending on where it hit the racket
		// (t is 0.5 if hit at top, 0 at center, -0.5 at bottom)
		float t = ((ball_pos.y - racket_left.y) / racket_height) - 0.5f;
		ball_dir.x = fabs(ball_dir.x); // force it to be positive
		ball_dir.y = t;
	}
   
	/* hit by right racket? */
	if (ball_pos.x > racket_right.x &&
		ball_pos.x < racket_right.x + racket_width &&
		ball_pos.y < racket_right.y + racket_height &&
		(int)ball_pos.y >= (int)racket_right.y) {
		// set fly direction depending on where it hit the racket
		// (t is 0.5 if hit at top, 0 at center, -0.5 at bottom)
		float t = ((ball_pos.y - racket_right.y) / racket_height) - 0.5f;
		ball_dir.x = -fabs(ball_dir.x); // force it to be negative
		ball_dir.y = t;
	}

	// hit left wall?
	if (ball_pos.x < 1) {
		++score_right;
		ball_speed = 0.1f;
		ball_pos.x = width / 2;
		ball_pos.y = height / 2;
		ball_dir.x = fabs(ball_dir.x); // force it to be positive
		ball_dir.y = 0;
	}

	// hit right wall?
	if (ball_pos.x > width) {
		++score_left;
		ball_speed = 0.1f;
		ball_pos.x = width / 2;
		ball_pos.y = height / 2;
		ball_dir.x = -fabs(ball_dir.x); // force it to be negative
		ball_dir.y = 0;
	}

	/* hit bottom wall? */
	if (ball_pos.y > height) {
		ball_dir.y = -fabs(ball_dir.y); // force it to be negative
	}

	/* hit top wall? */
	if (ball_pos.y < 1) {
		ball_dir.y = fabs(ball_dir.y); // force it to be positive
	}

	/* make sure that length of dir stays at 1 */
	vec2_norm(&ball_dir.x, &ball_dir.y);
}

int main (int argc, char const *argv[]) {

	init();
	hide_cursor();

	VEC_TAIL vt = {0};

	while (!key(VK_ESCAPE)) {
		
		/* increase the ball speed */
		ball_speed = clamp(ball_speed + 0.0001f, 0.1f, 0.4f);

		/* player 1 */
		if (key(   VK_UP)) racket_left.y -= racket_speed;
		if (key( VK_DOWN)) racket_left.y += racket_speed;
		if (key( VK_LEFT)) racket_left.x -= racket_speed;
		if (key(VK_RIGHT)) racket_left.x += racket_speed;
		/* limitrar raquete */
		racket_left.x = fmaxf(racket_left.x, 1);
		racket_left.x = fminf(racket_left.x, 13);
		racket_left.y = clamp(racket_left.y, 1, (height - racket_height - 1));

		/* player 2*/
		racket_right.y = lerp(racket_right.y, ball_pos.y-1, 0.06f);
		racket_right.y = clamp(racket_right.y, 1, (height - racket_height - 1));

		/* middle line */
		line(width/2, 0, width/2, height, 250);
		/* quadra */
		rect(0, 0, width, height, 219);

		/* draw rackets */
		line(racket_left.x, racket_left.y, racket_left.x, (racket_left.y + racket_height), 219);
		line(racket_right.x, racket_right.y, racket_right.x, (racket_right.y + racket_height), 219);

		/* placar */
		print((width/2)-3, 1, "%2.2d", score_left);
		print((width/2)+2, 1, "%2.2d", score_right);

		/* ball speed */
		static char bar[128];
		float value = normalize(ball_speed, 0.1f, 0.4f) * 100;
		graph_bar(bar, value, 100, 21, "\xdb\xb0");/* █xdb ░xb0 */
		print(5, height+2, "%s", bar);

		/* ball and its tail */
		vector_tail(&vt, (vec2f){ball_pos.x, ball_pos.y}, 25, 0, "%+-");
		pixel(ball_pos.x, ball_pos.y, '\xdb');
		update_ball();

		render();
		Sleep(1);
	}

	free(ascii_buffer);

	return 0;
}