/*
 * THE CITY: NO SIGNAL - Enhanced PSP / PPSSPP homebrew prototype
 *
 * Features:
 * - Expanded 32x32 city grid with streets, building shells and interiors
 * - Perspective ray-cast renderer with wall details, windows and fog
 * - Entity stalking logic: approaches through the city and retreats from view
 * - Procedural ambient drone / radio-like noise through PSP audio output
 * - Environmental clue locations and radio transmissions
 * - Unlimited flashlight battery
 *
 * Build with PSPSDK:
 *   make
 *
 * Note: the renderer is a software ray-caster (perspective 3D-style view),
 * not a polygonal 3D model engine. It is designed to be lightweight on PSP.
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspaudio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

PSP_MODULE_INFO("THE CITY: NO SIGNAL", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define W 480
#define H 272
#define MAP_W 32
#define MAP_H 32
#define PI 3.14159265358979323846f
#define FOV 1.0471975512f
#define MOVE_SPEED 0.060f
#define TURN_SPEED 0.043f
#define AUDIO_SAMPLES 1024

static unsigned int frame[W * H];
static unsigned char city[MAP_H][MAP_W];
static unsigned int oldButtons = 0;
static float px = 3.5f, py = 3.5f, angle = 0.0f;
static float ex = 26.5f, ey = 26.5f;
static int flashlight = 1, paused = 0, stepTicks = 0;
static int radioTicks = 0, clueCount = 0, entityMood = 0;
static int audioChannel = -1;
static short audioBuffer[AUDIO_SAMPLES];
static char statusLine[112] = "RADIO: NO SIGNAL";
static char clueLine[180] = "The city has gone quiet.";
static char districtLine[48] = "NORTH RESIDENTIAL";
static unsigned int noiseSeed = 0x12345678;

static const char *clues[] = {
    "Apartment 04: dinner is still on the table. Every family photo is face-down.",
    "A child's drawing shows a tall, featureless shape outside the bedroom window.",
    "Emergency screen: CONTAINMENT FAILURE. DO NOT LOOK DIRECTLY AT IT.",
    "Police radio: 'We hear footsteps in rooms we already cleared.'",
    "A subway train is frozen here. A small stuffed animal rests on a seat.",
    "Rooftop notes: the telescope keeps turning toward the same dark tower.",
    "Office memo: evacuation order received 19 hours after the last employee vanished.",
    "Graffiti: IT'S ALREADY INSIDE."
};
#define CLUE_COUNT (sizeof(clues) / sizeof(clues[0]))

static unsigned int rgb(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return 0xFF000000u | ((unsigned int)r << 16) |
           ((unsigned int)g << 8) | (unsigned int)b;
}
static int inside(int x, int y) {
    return x >= 0 && y >= 0 && x < MAP_W && y < MAP_H;
}
static int blocked(float x, float y) {
    int mx = (int)x, my = (int)y;
    if (!inside(mx, my)) return 1;
    return city[my][mx] == 1;
}
static void setCell(int x, int y, unsigned char v) {
    if (inside(x, y)) city[y][x] = v;
}

/* Generate a connected street grid with several enterable building interiors. */
static void buildCity(void) {
    int x, y, bx, by;
    for (y = 0; y < MAP_H; ++y)
        for (x = 0; x < MAP_W; ++x)
            city[y][x] = 1;

    /* Main roads and cross streets. */
    for (y = 1; y < MAP_H - 1; ++y)
        for (x = 1; x < MAP_W - 1; ++x)
            if ((x == 2 || x == 3 || x == 10 || x == 11 ||
                 x == 18 || x == 19 || x == 26 || x == 27 ||
                 y == 2 || y == 3 || y == 10 || y == 11 ||
                 y == 18 || y == 19 || y == 26 || y == 27))
                city[y][x] = 0;

    /* Building blocks with hollow interiors and door openings. */
    for (by = 5; by <= 21; by += 8) {
        for (bx = 5; bx <= 21; bx += 8) {
            int xx, yy;
            for (yy = by; yy < by + 5; ++yy)
                for (xx = bx; xx < bx + 5; ++xx)
                    city[yy][xx] = 0;

            for (xx = bx; xx < bx + 5; ++xx) {
                city[by][xx] = 1;
                city[by + 4][xx] = 1;
            }
            for (yy = by; yy < by + 5; ++yy) {
                city[yy][bx] = 1;
                city[yy][bx + 4] = 1;
            }

            /* Entrance openings on alternating sides. */
            city[by + ((bx + by) % 3) + 1][bx] = 0;
            city[by + 4][bx + 2] = 0;

            /* Interior partitions create rooms and narrow corridors. */
            city[by + 2][bx + 1] = 1;
            city[by + 2][bx + 2] = 1;
            city[by + 2][bx + 3] = 1;
            city[by + 2][bx + 2] = 0;
        }
    }

    /* Landmarks / larger connected structures in the south and east. */
    for (y = 22; y <= 24; ++y)
        for (x = 5; x <= 24; ++x)
            city[y][x] = 0;
    for (x = 5; x <= 24; ++x) {
        city[22][x] = 1; city[24][x] = 1;
    }
    city[22][14] = 0; city[24][8] = 0; city[24][20] = 0;

    /* Subway access corridor and station platform. */
    for (x = 12; x <= 24; ++x) {
        city[28][x] = 0;
        city[29][x] = 0;
    }
    city[27][16] = 0;
    city[27][17] = 0;
    city[27][18] = 0;

    /* Rooftop / office and apartment clue points. */
    setCell(8, 7, 2);
    setCell(15, 8, 2);
    setCell(23, 7, 2);
    setCell(7, 15, 2);
    setCell(16, 16, 2);
    setCell(23, 15, 2);
    setCell(8, 23, 2);
    setCell(20, 23, 2);
}

static void putPixel(int x, int y, unsigned int c) {
    if ((unsigned)x < W && (unsigned)y < H)
        frame[y * W + x] = c;
}

/* DDA ray-caster: each screen column traces one ray through the city grid. */
static void renderWorld(void) {
    int x, y;
    for (y = 0; y < H / 2; ++y) {
        int shade = 3 + y / 30;
        for (x = 0; x < W; ++x) putPixel(x, y, rgb(shade, shade + 1, shade + 4));
    }
    for (y = H / 2; y < H; ++y) {
        int shade = 7 + (y - H/2) / 10;
        for (x = 0; x < W; ++x) putPixel(x, y, rgb(shade, shade, shade + 2));
    }

    for (x = 0; x < W; ++x) {
        float cameraX = 2.0f * x / (float)W - 1.0f;
        float rayA = angle + atanf(cameraX * tanf(FOV * 0.5f));
        float rayX = cosf(rayA), rayY = sinf(rayA);
        int mapX = (int)px, mapY = (int)py;
        float deltaX = fabsf(1.0f / (rayX == 0 ? 0.00001f : rayX));
        float deltaY = fabsf(1.0f / (rayY == 0 ? 0.00001f : rayY));
        float sideX, sideY, perpDist = 0.0f;
        int stepX, stepY, side = 0, hit = 0, wallType = 1;
        if (rayX < 0) { stepX = -1; sideX = (px - mapX) * deltaX; }
        else { stepX = 1; sideX = (mapX + 1.0f - px) * deltaX; }
        if (rayY < 0) { stepY = -1; sideY = (py - mapY) * deltaY; }
        else { stepY = 1; sideY = (mapY + 1.0f - py) * deltaY; }

        while (!hit && sideX < 24.0f && sideY < 24.0f) {
            if (sideX < sideY) {
                sideX += deltaX; mapX += stepX; side = 0;
            } else {
                sideY += deltaY; mapY += stepY; side = 1;
            }
            if (!inside(mapX, mapY)) { hit = 1; wallType = 1; break; }
            if (city[mapY][mapX] != 0) {
                hit = 1; wallType = city[mapY][mapX];
            }
        }
        if (!hit) continue;
        perpDist = (side == 0) ? (mapX - px + (1 - stepX) * 0.5f) / (rayX == 0 ? 0.00001f : rayX)
                               : (mapY - py + (1 - stepY) * 0.5f) / (rayY == 0 ? 0.00001f : rayY);
        if (perpDist < 0.05f) perpDist = 0.05f;
        {
            int wallH = (int)(H / perpDist);
            int top = H/2 - wallH/2, bottom = H/2 + wallH/2;
            float wallX = (side == 0) ? py + perpDist * rayY : px + perpDist * rayX;
            int texture = ((int)(wallX * 8.0f)) & 7;
            int base = (int)(68.0f / (1.0f + perpDist * 0.35f));
            float beam = 1.0f - fabsf(cameraX);
            if (side) base = base * 3 / 4;
            if (flashlight && beam > 0.48f) base += 15;
            if (base > 75) base = 75;
            if (wallType == 2) base += 4;

            for (y = top; y < bottom; ++y) {
                if (y < 0 || y >= H) continue;
                int localY = (y - top) * 16 / (wallH > 0 ? wallH : 1);
                int shade = base;
                /* Concrete seams, window bands and broken facade details. */
                if ((localY == 3 || localY == 8 || localY == 13) && texture < 6) shade /= 2;
                if ((texture == 2 || texture == 5) && localY >= 4 && localY <= 10)
                    shade = (shade * 2) / 3;
                if (wallType == 2 && ((x + y) % 7 < 2)) shade += 12;
                putPixel(x, y, rgb(shade, shade + 1, shade + 4));
            }
        }
    }

    /* Presence: only a distant, incomplete silhouette, never a full reveal. */
    {
        float dx = ex - px, dy = ey - py;
        float distance = sqrtf(dx*dx + dy*dy);
        float relative = atan2f(dy, dx) - angle;
        while (relative > PI) relative -= 2*PI;
        while (relative < -PI) relative += 2*PI;
        if (distance < 11.0f && fabsf(relative) < 0.20f && distance > 2.8f) {
            int cx = W/2 + (int)(relative * 180.0f);
            int scale = (int)(36.0f / distance);
            int top = H/2 - scale*2, bottom = H/2 + scale*2;
            int half = scale / 2;
            for (y = top; y < bottom; ++y)
                for (x = cx-half; x <= cx+half; ++x)
                    if (((x + y + stepTicks) & 3) != 0)
                        putPixel(x, y, rgb(2, 2, 4));
        }
    }

    /* Vignette and heavy darkness at the edges. */
    for (y = 0; y < H; y += 2) {
        for (x = 0; x < W; x += 2) {
            int dx = abs(x - W/2), dy = abs(y - H/2);
            if (dx > 190 || dy > 108) {
                int i = y * W + x;
                unsigned int c = frame[i];
                int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
                frame[i] = rgb(r/2, g/2, b/2);
            }
        }
    }
}

static void copyFrame(void) {
    void *fb = NULL; int pitch = 0, format = 0;
    sceDisplayGetFrameBuf(&fb, &pitch, &format, 0);
    if (!fb || pitch < W) return;
    unsigned int *dst = (unsigned int *)fb;
    for (int y = 0; y < H; ++y)
        memcpy(dst + y * pitch, frame + y * W, W * sizeof(unsigned int));
}

static void updateDistrict(void) {
    if (py < 9) strcpy(districtLine, "NORTH RESIDENTIAL");
    else if (px > 20 && py < 20) strcpy(districtLine, "EAST OFFICE DISTRICT");
    else if (py > 25) strcpy(districtLine, "UNDERGROUND / SUBWAY");
    else if (px < 12) strcpy(districtLine, "WEST INDUSTRIAL");
    else strcpy(districtLine, "CENTRAL CITY");
}

static void inspect(void) {
    int mx = (int)(px + cosf(angle) * 1.35f);
    int my = (int)(py + sinf(angle) * 1.35f);
    if (inside(mx, my) && city[my][mx] == 2) {
        snprintf(clueLine, sizeof(clueLine), "%s", clues[clueCount % CLUE_COUNT]);
        clueCount++;
        snprintf(statusLine, sizeof(statusLine), "FIELD EVIDENCE RECORDED");
    } else {
        snprintf(clueLine, sizeof(clueLine), "Dust covers everything. Something moved when the light turned away.");
        snprintf(statusLine, sizeof(statusLine), "NO INTERACTABLE CLUE");
    }
}

static void useRadio(void) {
    static const char *messages[] = {
        "RADIO: [STATIC] ...unit 7, do you copy?",
        "RADIO: ...the streets are empty. We are not alone.",
        "RADIO: ...do not follow the footsteps underground.",
        "RADIO: [BREATHING] ...you should not have come back.",
        "RADIO: ...signal source is directly behind you..."
    };
    radioTicks++;
    snprintf(statusLine, sizeof(statusLine), "%s", messages[radioTicks % 5]);
}

/* A low generated drone with occasional noisy pulses; no external audio files needed. */
static int audioThread(SceSize args, void *argp) {
    int i;
    (void)args; (void)argp;
    audioChannel = sceAudioChReserve(-1, AUDIO_SAMPLES, PSP_AUDIO_FORMAT_MONO);
    if (audioChannel < 0) return 0;
    while (1) {
        for (i = 0; i < AUDIO_SAMPLES; ++i) {
            float t = (float)(stepTicks + i) * 0.018f;
            int hum = (int)(sinf(t) * 650.0f + sinf(t * 0.37f) * 380.0f);
            noiseSeed = noiseSeed * 1664525u + 1013904223u;
            int noise = ((int)((noiseSeed >> 20) & 0x7ff) - 1024);
            int pulse = ((stepTicks / 900) % 4 == 0 && (i % 240) < 5) ? noise / 2 : 0;
            audioBuffer[i] = (short)(hum + noise / 9 + pulse);
        }
        sceAudioOutputBlocking(audioChannel, PSP_AUDIO_VOLUME_MAX / 3, audioBuffer);
    }
    return 0;
}

static void updateEntity(void) {
    float dx = px - ex, dy = py - ey;
    float d = sqrtf(dx*dx + dy*dy);
    if (d < 0.001f) return;

    /* It advances only occasionally. Looking toward it makes it retreat. */
    if (stepTicks % 95 == 0) {
        float toEntity = atan2f(ey - py, ex - px) - angle;
        while (toEntity > PI) toEntity -= 2*PI;
        while (toEntity < -PI) toEntity += 2*PI;

        if (fabsf(toEntity) < 0.42f && d < 13.0f) {
            entityMood = 1;
            ex += (dx / d) * 1.4f;
            ey += (dy / d) * 1.4f;
            snprintf(statusLine, sizeof(statusLine), "THE FOOTSTEPS STOP WHEN YOU TURN.");
        } else if (d > 3.6f) {
            float nx = ex + (dx / d) * 0.34f;
            float ny = ey + (dy / d) * 0.34f;
            if (!blocked(nx, ey)) ex = nx;
            if (!blocked(ex, ny)) ey = ny;
            entityMood = 2;
        } else {
            entityMood = 3;
            snprintf(statusLine, sizeof(statusLine), "A COLD DRAFT PASSES THROUGH THE ROOM.");
        }
    }
    /* Never allow the entity to occupy the player's exact tile. */
    if (blocked(ex, ey)) { ex = 26.5f; ey = 26.5f; }
}

static void drawHud(void) {
    pspDebugScreenSetTextColor(0xFFB5BBC4);
    pspDebugScreenSetXY(1, 1);
    pspDebugScreenPrintf("THE CITY: NO SIGNAL");
    pspDebugScreenSetXY(1, 2);
    pspDebugScreenPrintf("%s", districtLine);
    pspDebugScreenSetXY(1, 3);
    pspDebugScreenPrintf("FLASHLIGHT: %s  [UNLIMITED]", flashlight ? "ON" : "OFF");
    pspDebugScreenSetXY(1, 4);
    pspDebugScreenPrintf("%s", statusLine);
    pspDebugScreenSetXY(1, 31);
    pspDebugScreenPrintf("D-PAD MOVE  ANALOG TURN  X INSPECT  [] LIGHT  TRIANGLE RADIO");
    pspDebugScreenSetXY(1, 32);
    pspDebugScreenPrintf("START PAUSE  |  EVIDENCE: %d", clueCount);
    pspDebugScreenSetXY(1, 6);
    pspDebugScreenSetTextColor(0xFF9299A3);
    pspDebugScreenPrintf("%s", clueLine);
}

int main(void) {
    SceCtrlData pad;
    int audioTid;
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    buildCity();

    audioTid = sceKernelCreateThread("CityAmbientAudio", audioThread, 0x18, 0x1000, PSP_THREAD_ATTR_USER, NULL);
    if (audioTid >= 0) sceKernelStartThread(audioTid, 0, NULL);

    while (1) {
        sceCtrlReadBufferPositive(&pad, 1);
        unsigned int pressed = pad.Buttons & ~oldButtons;
        oldButtons = pad.Buttons;

        if (pressed & PSP_CTRL_START) paused = !paused;

        if (!paused) {
            float forward = 0.0f, strafe = 0.0f;
            if (pad.Buttons & PSP_CTRL_UP) forward += MOVE_SPEED;
            if (pad.Buttons & PSP_CTRL_DOWN) forward -= MOVE_SPEED;
            if (pad.Buttons & PSP_CTRL_LEFT) strafe -= MOVE_SPEED;
            if (pad.Buttons & PSP_CTRL_RIGHT) strafe += MOVE_SPEED;
            if (pad.Lx < 105) angle -= TURN_SPEED;
            if (pad.Lx > 150) angle += TURN_SPEED;

            {
                float nx = px + cosf(angle) * forward - sinf(angle) * strafe;
                float ny = py + sinf(angle) * forward + cosf(angle) * strafe;
                if (!blocked(nx, py)) px = nx;
                if (!blocked(px, ny)) py = ny;
            }

            if (pressed & PSP_CTRL_SQUARE) flashlight = !flashlight;
            if (pressed & PSP_CTRL_CROSS) inspect();
            if (pressed & PSP_CTRL_TRIANGLE) useRadio();

            stepTicks++;
            updateEntity();
            updateDistrict();

            if (stepTicks % 620 == 0)
                snprintf(statusLine, sizeof(statusLine), "A DOOR CREAKS SOMEWHERE BEHIND YOU.");
            if (stepTicks % 1100 == 0)
                snprintf(statusLine, sizeof(statusLine), "RADIO: [STATIC] ...IT IS STILL HERE...");
        }

        renderWorld();
        pspDebugScreenClear();
        copyFrame();
        drawHud();

        if (paused) {
            pspDebugScreenSetXY(18, 15);
            pspDebugScreenSetTextColor(0xFFD0D3D8);
            pspDebugScreenPrintf("PAUSED - PRESS START");
        }

        sceDisplayWaitVblankStart();
        sceKernelDelayThread(1000);
    }
    return 0;
}
