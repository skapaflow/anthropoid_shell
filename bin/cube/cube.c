#include <time.h>
#include <math.h>
#include <conio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <windows.h>

#define PI 3.14159265f
#define FPS 60

typedef struct {float x, y, z;} vec3f;

static int SCREEN_W;
static int SCREEN_H;
static wchar_t *screen;

/* light intensity (0..0.5 in steps of 0.1) to character; 0 stays '.' */
static const wchar_t shade[] = {L'*', L'#', 0x2591, 0x2592, 0x2588};

static inline void pixel (int x, int y, wchar_t c) {

	if (x >= 0 && y >= 0 && x < SCREEN_W && y < SCREEN_H)
		screen[x + y * SCREEN_W] = c;
}

static void line (int x1, int y1, int x2, int y2, wchar_t c) {

	int i, dx = x2 - x1, dy = y2 - y1;
	int dxabs = abs(dx), dyabs = abs(dy);
	int sdx = (dx < 0) ? -1 : (dx > 0);
	int sdy = (dy < 0) ? -1 : (dy > 0);
	int x = dyabs >> 1, y = dxabs >> 1;
	int px = x1, py = y1;
	if (dxabs >= dyabs) {
		for (i = 0; i < dxabs; i++) {
			y += dyabs;
			if (y >= dxabs)
				y -= dxabs, py += sdy;
			px += sdx;
			pixel(px, py, c);
		}
	} else {
		for (i = 0; i < dyabs; i++) {
			x += dxabs;
			if (x >= dyabs)
				x -= dyabs, px += sdx;
			py += sdy;
			pixel(px, py, c);
		}
	}
}

/*
 * Orientation is a rotation matrix applied to the original vertices every
 * frame, so errors do not pile up in the vertices (the cube never deforms).
 * Rotating the columns by a step about axis c is the old rotate().
 */
static void rotate (float m[3][3], float angle, int c) {

	int a = (c == 0) ? 1 : 0, b = (c == 1) ? 2 : (c == 0 ? 2 : 1);
	float cs = cosf(angle), sn = sinf(angle);
	for (int j = 0; j < 3; j++) {
		float va = m[a][j], vb = m[b][j];
		m[a][j] = va * cs - vb * sn;
		m[b][j] = va * sn + vb * cs;
	}
}

/* Gram-Schmidt on the rows, to drop the float error of the accumulated steps */
static void orthonormalize (float m[3][3]) {

	float d, l;
	l = sqrtf(m[0][0]*m[0][0] + m[0][1]*m[0][1] + m[0][2]*m[0][2]);
	for (int j = 0; j < 3; j++) m[0][j] /= l;
	d = m[1][0]*m[0][0] + m[1][1]*m[0][1] + m[1][2]*m[0][2];
	for (int j = 0; j < 3; j++) m[1][j] -= d * m[0][j];
	l = sqrtf(m[1][0]*m[1][0] + m[1][1]*m[1][1] + m[1][2]*m[1][2]);
	for (int j = 0; j < 3; j++) m[1][j] /= l;
	m[2][0] = m[0][1]*m[1][2] - m[0][2]*m[1][1];
	m[2][1] = m[0][2]*m[1][0] - m[0][0]*m[1][2];
	m[2][2] = m[0][0]*m[1][1] - m[0][1]*m[1][0];
}

static HANDLE init (void) {

	HANDLE orig = GetStdHandle(STD_OUTPUT_HANDLE);
	CONSOLE_SCREEN_BUFFER_INFO w;
	GetConsoleScreenBufferInfo(orig, &w);
	SCREEN_W = w.srWindow.Right - w.srWindow.Left + 1;
	SCREEN_H = w.srWindow.Bottom - w.srWindow.Top + 1;

	HANDLE h = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, 0, NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
	COORD size = {(SHORT) SCREEN_W, (SHORT) SCREEN_H};
	SetConsoleScreenBufferSize(h, size);
	SetConsoleActiveScreenBuffer(h);
	screen = malloc(sizeof(wchar_t) * SCREEN_W * SCREEN_H);
	for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
		screen[i] = L' ';
	srand((unsigned) time(NULL));
	return h;
}

int main (void) {

	HANDLE orig = GetStdHandle(STD_OUTPUT_HANDLE);
	HANDLE con = init();

	const char dir[] = "xyz";
	int r1 = 1, r2 = 1;

	/* 8 vertices of the cube (side 20, centered on the origin) */
	static const vec3f v0[8] = {
		{ 10, -10, -10}, { 10, -10,  10}, {-10, -10,  10}, {-10, -10, -10},
		{ 10,  10, -10}, { 10,  10,  10}, {-10,  10,  10}, {-10,  10, -10}
	};
	/* the 12 triangles, counter-clockwise on screen when facing the viewer */
	static const int f[12][3] = {
		{1, 3, 0}, {7, 5, 4}, {4, 1, 0}, {5, 2, 1}, {2, 7, 3}, {0, 7, 4},
		{1, 2, 3}, {7, 6, 5}, {4, 5, 1}, {5, 6, 2}, {2, 6, 7}, {0, 3, 7}
	};

	float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	vec3f c[8];
	float light_deg = 0;
	unsigned frame = 0;

	LARGE_INTEGER freq, last, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&last);

	while (!kbhit()) {

		QueryPerformanceCounter(&now);
		float dt = (float) (now.QuadPart - last.QuadPart) / freq.QuadPart;
		last = now;
		if (dt > 0.1f) dt = 0.1f;

		/* now and then pick another axis (1 in 600 frames, about every 10 s) */
		if (rand() % 600 == 0) r1 = rand() % 3;
		if (rand() % 600 == 0) r2 = rand() % 3;

		/* 36 and 24 degrees per second, the old 0.09 and 0.06 per frame at 400 fps */
		rotate(m, 36 * dt * PI / 180, dir[r1] - 'x');
		rotate(m, 24 * dt * PI / 180, dir[r2] - 'x');
		if (++frame % 512 == 0) orthonormalize(m);

		light_deg += 20 * dt;
		if (light_deg >= 360) light_deg -= 360;

		/* rotate, move 60 away from the viewer and project (distance 50) */
		for (int i = 0; i < 8; i++) {
			float x = m[0][0]*v0[i].x + m[0][1]*v0[i].y + m[0][2]*v0[i].z;
			float y = m[1][0]*v0[i].x + m[1][1]*v0[i].y + m[1][2]*v0[i].z;
			float z = m[2][0]*v0[i].x + m[2][1]*v0[i].y + m[2][2]*v0[i].z + 60;
			c[i] = (vec3f) {(50 / z) * x + SCREEN_W / 2, (50 / z) * y + SCREEN_H / 2, z};
		}

		vec3f L = {
			SCREEN_W / 2 + 20 * cosf(light_deg * PI / 180),
			SCREEN_H / 2 + 20 * sinf(light_deg * PI / 180),
			-5
		};
		pixel((int) L.x, (int) L.y, L'+');

		for (int i = 0; i < 12; i++) {

			const vec3f *s0 = &c[f[i][0]], *s1 = &c[f[i][1]], *s2 = &c[f[i][2]];

			/* back-face culling: signed area of the screen triangle */
			float ax = s1->x - s0->x, ay = s1->y - s0->y;
			float bx = s2->x - s0->x, by = s2->y - s0->y;
			if (ax * by - ay * bx >= 0)
				continue;

			float xmaxf = fmaxf(s0->x, fmaxf(s1->x, s2->x)), xminf = fminf(s0->x, fminf(s1->x, s2->x));
			float ymaxf = fmaxf(s0->y, fmaxf(s1->y, s2->y)), yminf = fminf(s0->y, fminf(s1->y, s2->y));
			if (xmaxf <= 0 || ymaxf <= 0 || xminf >= SCREEN_W || yminf >= SCREEN_H)
				continue;
			int xmax = xmaxf >= SCREEN_W ? SCREEN_W - 1 : (int) xmaxf;
			int ymax = ymaxf >= SCREEN_H ? SCREEN_H - 1 : (int) ymaxf;
			int xmin = xminf <= 0 ? 0 : (int) xminf;
			int ymin = yminf <= 0 ? 0 : (int) yminf;

			/* face normal, once per triangle */
			vec3f n = {
				ay * (s2->z - s0->z) - (s1->z - s0->z) * by,
				(s1->z - s0->z) * bx - ax * (s2->z - s0->z),
				ax * by - ay * bx
			};
			float len = sqrtf(n.x*n.x + n.y*n.y + n.z*n.z);
			n.x /= len, n.y /= len, n.z /= len;

			/* edge functions at the first pixel, then one add per step */
			float e0dx = s1->y - s0->y, e0dy = -(s1->x - s0->x);
			float e1dx = s2->y - s1->y, e1dy = -(s2->x - s1->x);
			float e2dx = s0->y - s2->y, e2dy = -(s0->x - s2->x);
			float r0 = (xmin - s0->x) * e0dx + (ymin - s0->y) * e0dy;
			float r1e = (xmin - s1->x) * e1dx + (ymin - s1->y) * e1dy;
			float r2e = (xmin - s2->x) * e2dx + (ymin - s2->y) * e2dy;

			for (int y = ymin; y <= ymax; y++, r0 += e0dy, r1e += e1dy, r2e += e2dy) {
				float w0 = r0, w1 = r1e, w2 = r2e;
				for (int x = xmin; x <= xmax; x++, w0 += e0dx, w1 += e1dx, w2 += e2dx) {
					if (!(w0 > 0 && w1 > 0 && w2 > 0))
						continue;
					float lx = L.x - x, ly = L.y - y, lz = L.z;
					float dot = n.x * lx + n.y * ly + n.z * lz;
					if (dot <= 0) {
						pixel(x, y, L'.');
						continue;
					}
					int k = (int) (dot / sqrtf(lx*lx + ly*ly + lz*lz) * 5);
					pixel(x, y, shade[k > 4 ? 4 : k]);
				}
			}

			/* wireframe: blank out the edges */
			line((int) s0->x, (int) s0->y, (int) s1->x, (int) s1->y, L' ');
			line((int) s1->x, (int) s1->y, (int) s2->x, (int) s2->y, L' ');
			line((int) s2->x, (int) s2->y, (int) s0->x, (int) s0->y, L' ');
		}

		COORD home = {0, 0};
		DWORD written;
		WriteConsoleOutputCharacterW(con, screen, SCREEN_W * SCREEN_H, home, &written);
		for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
			screen[i] = L' ';

		/* hold the frame rate instead of spinning a core */
		QueryPerformanceCounter(&now);
		int spent = (int) ((now.QuadPart - last.QuadPart) * 1000 / freq.QuadPart);
		if (spent < 1000 / FPS)
			Sleep(1000 / FPS - spent);
	}

	_getch();
	SetConsoleActiveScreenBuffer(orig);
	CloseHandle(con);
	free(screen);

	return 0;
}
